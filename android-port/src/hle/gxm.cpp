// SceGxm: our own implementation of the Vita GPU API. It keeps the API state (contexts, render targets, shader
// patcher programs, surfaces, textures, fixed-function state), validates calls the way the game relies on, and hands
// complete scene / draw / flip records to the renderer backend (src/gpu/renderer.h).
//
// Structures the game owns and may copy (SceGxmTexture, SceGxmColorSurface, SceGxmDepthStencilSurface) are encoded
// in place: textures with the hardware control-word layout (vitasdk psp2/gxm.h), surfaces with our own encoding
// (the game treats them as opaque). Everything else the game only holds as a pointer: those are small guest slots we
// hand out, mapped back to host objects. GXP program headers are read in guest memory (layout: Vita3K's gxm
// types.h, read as reference only).
//
// The display queue works like the hardware's: our "GXM display queue" host thread (a guest thread of its own, see
// adopt_host_thread) runs the game's display callback for each entry in order, on a copy of its callback data in a
// guest ring of maxPendingSwaps slots. AddEntry first waits until fewer than maxPendingSwaps entries are queued or
// running and no queued entry still shows a buffer this frame rendered into (fragment sync objects of the frame's
// scenes vs the entries' new buffers), then flushes the frame's GPU work (Renderer::flush) and queues the entry, so
// the callback's flip (sceDisplaySetFrameBuf -> Renderer::present) and its sceDisplayWaitVblankStart run beside the
// rendering thread. DisplayQueueFinish waits until the queue is empty. Game side (1.09 asm): sceGxmInitialize at
// 0x813b65dc (maxPendingSwaps 2, callback 0x813b7aa3, 4-byte data = framebuffer address), AddEntry 0x813b73cc
// (old / new = the two display buffers' sync objects, swapped every frame), BeginScene 0x813b729e (fragment sync =
// the buffer it renders into).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "gpu/draw_stats.h"
#include "gpu/renderer.h"
#include "gpu/texture.h"
#include "hle/common.h"
#include "hle/threads.h"

namespace hle {
namespace {

enum : uint32_t {
  GXM_ERROR_UNINITIALIZED = 0x805B0000,
  GXM_ERROR_ALREADY_INITIALIZED = 0x805B0001,
  GXM_ERROR_OUT_OF_MEMORY = 0x805B0002,
  GXM_ERROR_INVALID_VALUE = 0x805B0003,
  GXM_ERROR_INVALID_POINTER = 0x805B0004,
  GXM_ERROR_INVALID_ALIGNMENT = 0x805B0005,
  GXM_ERROR_NOT_WITHIN_SCENE = 0x805B0006,
  GXM_ERROR_WITHIN_SCENE = 0x805B0007,
  GXM_ERROR_NULL_PROGRAM = 0x805B0008,
  GXM_ERROR_RESERVE_FAILED = 0x805B000B,
};

std::recursive_mutex gxm_lock;

// --- guest slots for opaque handles ------------------------------------------------------------------------------
// Small zeroed guest records (contexts use the game's own host memory instead). Never reused: the game creates its
// programs, targets and sync objects once.
uint32_t slot(Cpu& c, uint32_t size) {
  static uint32_t chunk = 0, used = 0;
  size = (size + 15) & ~15u;
  if (!chunk || used + size > 0x10000) {
    chunk = c.mem().alloc(0x10000, "GXM handles");
    used = 0;
    if (!chunk) return 0;
  }
  uint32_t va = chunk + used;
  used += size;
  std::memset(c.mem().ptr(va), 0, size);
  return va;
}

// --- GXP program headers -----------------------------------------------------------------------------------------
// Offsets: magic 0 ("GXP\0"), program_flags 0x14 (bit 0 = fragment), parameter_count 0x24, parameters_offset 0x28
// (relative to that field), default_uniform_buffer_count 0x64 (32-bit words). Parameters are 16 bytes: name offset,
// category:4 type:4, component_count:4 container:4, semantic, semantic index, array size, resource index.
constexpr uint32_t GXP_MAGIC = 0x00505847;
bool is_gxp(Cpu& c, uint32_t p) { return p && c.mem().valid(p, 0x70) && c.mem().read<uint32_t>(p) == GXP_MAGIC; }
uint32_t gxp_param_count(Cpu& c, uint32_t p) { return c.mem().read<uint32_t>(p + 0x24); }
uint32_t gxp_params(Cpu& c, uint32_t p) { return p + 0x28 + c.mem().read<uint32_t>(p + 0x28); }
uint32_t gxp_default_uniform_bytes(Cpu& c, uint32_t p) { return c.mem().read<uint32_t>(p + 0x64) * 4; }

// --- state -------------------------------------------------------------------------------------------------------
struct Patcher {
  std::map<std::vector<uint8_t>, uint32_t> vertex_cache, fragment_cache;  // key -> program slot (deduplicated)
};
struct ProgramRef {
  int refs = 0;
};

struct Context {
  uint32_t vertex_ring = 0, vertex_ring_size = 0, vertex_ring_used = 0;
  uint32_t fragment_ring = 0, fragment_ring_size = 0, fragment_ring_used = 0;
  bool in_scene = false;
  uint32_t vertex_program = 0, fragment_program = 0;  // slots
  const gpu::VertexProgram* vp = nullptr;             // their objects (never destroyed)
  const gpu::FragmentProgram* fp = nullptr;
  uint32_t vertex_uniforms = 0, fragment_uniforms = 0;
  std::array<uint32_t, 16> streams{};
  std::array<std::array<uint32_t, 4>, 16> textures{};
  gpu::RenderState state;
};
struct RenderTarget {
  uint32_t width, height, scenes_per_frame, flags;
};

bool initialized = false;
uint32_t display_callback = 0, display_data_size = 0;
std::map<uint32_t, std::unique_ptr<Context>> contexts;
std::map<uint32_t, RenderTarget> targets;
std::map<uint32_t, std::unique_ptr<Patcher>> patchers;
std::map<uint32_t, uint32_t> program_ids;  // id slot -> GXP address
std::map<uint32_t, std::unique_ptr<gpu::VertexProgram>> vertex_programs;
std::map<uint32_t, std::unique_ptr<gpu::FragmentProgram>> fragment_programs;
std::map<uint32_t, ProgramRef> program_refs;
std::map<uint32_t, int> sync_objects;

Context* find_context(uint32_t h) {
  auto it = contexts.find(h);
  return it == contexts.end() ? nullptr : it->second.get();
}
#define CONTEXT_OR(fail)                     \
  std::lock_guard lk(gxm_lock);              \
  Context* ctx = find_context(arg(c, 0));    \
  if (!ctx) return fail

// --- display queue -----------------------------------------------------------------------------------------------
struct DisplayEntry {
  uint32_t old_sync = 0, new_sync = 0, data = 0;  // data = our copy of the callback data (a ring slot)
};
std::mutex dq_lock;
std::condition_variable dq_cv;  // entry queued / entry done
std::deque<DisplayEntry> dq;    // queued entries; the front one stays until its callback has returned
uint64_t dq_added = 0;
uint32_t dq_max = 1, dq_slot_size = 16, dq_ring = 0, dq_stack_top = 0;
bool dq_started = false;  // the thread lives as long as the process (Terminate leaves it idle)
std::set<uint32_t> frame_syncs;  // fragment sync objects of the scenes since the last AddEntry (gxm_lock)
thread_local bool on_display_thread = false;
// [timing] (display.cpp): microseconds AddEntry waited for the queue, entries added, queue depth summed at each add.
std::atomic<uint64_t> dq_wait_us{0}, dq_adds{0}, dq_depth_sum{0};

void display_thread() {
  guest::Cpu& k = adopt_host_thread("GXM display queue");
  on_display_thread = true;
  for (;;) {
    DisplayEntry e;
    {
      frame_wait_begin();  // idle until the next flip is queued (spike log)
      std::unique_lock lk(dq_lock);
      dq_cv.wait(lk, [] { return !dq.empty(); });
      e = dq.front();
      lk.unlock();
      frame_wait_end();
    }
    if (display_callback && !k.invoke(display_callback, {e.data}, dq_stack_top)) die(k, "GXM display queue callback");
    {
      std::lock_guard lk(dq_lock);
      dq.pop_front();
    }
    dq_cv.notify_all();
  }
}

// gxm_lock held.
bool start_display_queue(Cpu& c, uint32_t max_pending, uint32_t data_size) {
  std::lock_guard lk(dq_lock);
  dq_max = std::max(max_pending, 1u);
  dq_slot_size = (std::max(data_size, 16u) + 15) & ~15u;
  dq_ring = c.mem().alloc(dq_slot_size * dq_max, "GXM display queue data");
  constexpr uint32_t STACK = 0x10000;
  uint32_t stack = c.mem().alloc(STACK, "GXM display queue stack");
  if (!dq_ring || !stack) return false;
  dq_stack_top = stack + STACK;
  if (!dq_started) std::thread(display_thread).detach();
  dq_started = true;
  return true;
}

// --- initialisation / memory -------------------------------------------------------------------------------------
// sceGxmInitialize(SceGxmInitializeParams* {flags, maxPending, callback, callbackDataSize, pbSize})
void Initialize(Cpu& c) {
  uint32_t p = arg(c, 0);
  if (!p) return ret(c, GXM_ERROR_INVALID_POINTER);
  std::lock_guard lk(gxm_lock);
  if (initialized) return ret(c, GXM_ERROR_ALREADY_INITIALIZED);
  uint32_t max_pending = c.mem().read<uint32_t>(p + 4);
  display_callback = c.mem().read<uint32_t>(p + 8);
  display_data_size = c.mem().read<uint32_t>(p + 12);
  if (display_data_size > 0x200) return ret(c, GXM_ERROR_INVALID_VALUE);
  if (!start_display_queue(c, max_pending, display_data_size)) return ret(c, GXM_ERROR_OUT_OF_MEMORY);
  initialized = true;
  HLE_NOTE("sceGxmInitialize: display callback 0x%08x (%u-byte data), %u pending swaps, parameter buffer 0x%x\n",
           display_callback, display_data_size, max_pending, c.mem().read<uint32_t>(p + 16));
  ret(c, 0);
}

void Terminate(Cpu& c) {
  std::lock_guard lk(gxm_lock);
  initialized = false;
  ret(c, 0);
}

// Map/unmap only register memory for GPU access on hardware; every guest address is already reachable here.
void MapMemory(Cpu& c) {
  if (!arg(c, 0)) return ret(c, GXM_ERROR_INVALID_POINTER);
  ret(c, 0);
}
void UnmapMemory(Cpu& c) { ret(c, arg(c, 0) ? 0u : uint32_t(GXM_ERROR_INVALID_POINTER)); }
// sceGxm{Vertex,Fragment}UsseMemory(base, size, unsigned* offset): USSE offset = the address (unused by us).
void MapUsseMemory(Cpu& c) {
  if (!arg(c, 0) || !arg(c, 2)) return ret(c, GXM_ERROR_INVALID_POINTER);
  c.mem().write<uint32_t>(arg(c, 2), arg(c, 0));
  ret(c, 0);
}
void UnmapUsseMemory(Cpu& c) { ret(c, arg(c, 0) ? 0u : uint32_t(GXM_ERROR_INVALID_POINTER)); }

void PadHeartbeat(Cpu& c) { ret(c, 0); }
void Finish(Cpu& c) { ret(c, 0); }  // nothing the game reads back depends on the host GPU's progress

// sceGxmDisplayQueueFinish(): returns once every queued display callback has run.
void DisplayQueueFinish(Cpu& c) {
  std::unique_lock lk(dq_lock);
  dq_cv.wait(lk, [] { return dq.empty(); });
  ret(c, 0);
}

void SyncObjectCreate(Cpu& c) {
  if (!arg(c, 0)) return ret(c, GXM_ERROR_INVALID_POINTER);
  std::lock_guard lk(gxm_lock);
  uint32_t h = slot(c, 16);
  sync_objects[h] = 0;
  c.mem().write<uint32_t>(arg(c, 0), h);
  ret(c, 0);
}
void SyncObjectDestroy(Cpu& c) {
  std::lock_guard lk(gxm_lock);
  ret(c, sync_objects.erase(arg(c, 0)) ? 0u : uint32_t(GXM_ERROR_INVALID_POINTER));
}

// sceGxmDisplayQueueAddEntry(oldBuffer, newBuffer, const void* callbackData)
void DisplayQueueAddEntry(Cpu& c) {
  uint32_t old_sync = arg(c, 0), new_sync = arg(c, 1), data = arg(c, 2), size;
  std::set<uint32_t> written;
  {
    std::lock_guard lk(gxm_lock);
    if (!initialized) return ret(c, GXM_ERROR_UNINITIALIZED);
    if (!data && display_data_size) return ret(c, GXM_ERROR_INVALID_POINTER);
    size = display_data_size;
    written.swap(frame_syncs);
  }
  {
    // Room in the queue, and no queued flip still to show a buffer this frame drew into: only then may the frame's
    // GPU work be submitted (a flip copies its buffer when its present is submitted, in queue order).
    auto t0 = std::chrono::steady_clock::now();
    frame_wait_begin();  // the rendering thread's frame ends here (spike log); flush below is next frame's work
    std::unique_lock lk(dq_lock);
    dq_depth_sum += dq.size();
    dq_adds++;
    dq_cv.wait(lk, [&] {
      if (dq.size() >= dq_max) return false;
      for (const DisplayEntry& e : dq)
        if (written.count(e.new_sync)) return false;
      return true;
    });
    dq_wait_us += uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
  }
  frame_wait_end();
  gpu::renderer().flush(c.mem());
  {
    std::lock_guard lk(dq_lock);
    // Fewer than dq_max entries are queued and only this call adds any: slot dq_added % dq_max is free.
    uint32_t copy = dq_ring + uint32_t(dq_added++ % dq_max) * dq_slot_size;
    if (size) std::memcpy(c.mem().ptr(copy), c.mem().ptr(data), size);
    dq.push_back({old_sync, new_sync, copy});
  }
  dq_cv.notify_all();
  ret(c, 0);
}

// --- contexts / targets / surfaces ---------------------------------------------------------------------------------
// sceGxmCreateContext(SceGxmContextParams* {hostMem, size, vdm, size, vertex ring, size, fragment ring, size, ...})
void CreateContext(Cpu& c) {
  uint32_t p = arg(c, 0), out = arg(c, 1);
  if (!p || !out) return ret(c, GXM_ERROR_INVALID_POINTER);
  Memory& m = c.mem();
  uint32_t host = m.read<uint32_t>(p), host_size = m.read<uint32_t>(p + 4);
  if (!host || host_size < 2048) return ret(c, GXM_ERROR_INVALID_VALUE);
  auto ctx = std::make_unique<Context>();
  ctx->vertex_ring = m.read<uint32_t>(p + 16);
  ctx->vertex_ring_size = m.read<uint32_t>(p + 20);
  ctx->fragment_ring = m.read<uint32_t>(p + 24);
  ctx->fragment_ring_size = m.read<uint32_t>(p + 28);
  std::lock_guard lk(gxm_lock);
  if (!initialized) return ret(c, GXM_ERROR_UNINITIALIZED);
  contexts[host] = std::move(ctx);  // the handle is the game's host memory block
  m.write<uint32_t>(out, host);
  ret(c, 0);
}

void DestroyContext(Cpu& c) {
  std::lock_guard lk(gxm_lock);
  ret(c, contexts.erase(arg(c, 0)) ? 0u : uint32_t(GXM_ERROR_INVALID_POINTER));
}

// sceGxmCreateRenderTarget(SceGxmRenderTargetParams* {flags, u16 width, u16 height, u16 scenes, ...}, target*)
void CreateRenderTarget(Cpu& c) {
  uint32_t p = arg(c, 0), out = arg(c, 1);
  if (!p || !out) return ret(c, GXM_ERROR_INVALID_POINTER);
  Memory& m = c.mem();
  RenderTarget t{m.read<uint16_t>(p + 4), m.read<uint16_t>(p + 6), m.read<uint16_t>(p + 8), m.read<uint32_t>(p)};
  if (!t.width || !t.height || t.width > 4096 || t.height > 4096) return ret(c, GXM_ERROR_INVALID_VALUE);
  std::lock_guard lk(gxm_lock);
  uint32_t h = slot(c, 16);
  targets[h] = t;
  m.write<uint32_t>(out, h);
  HLE_NOTE("sceGxmCreateRenderTarget: %ux%u, %u scenes/frame -> 0x%08x\n", t.width, t.height, t.scenes_per_frame, h);
  ret(c, 0);
}

void DestroyRenderTarget(Cpu& c) {
  std::lock_guard lk(gxm_lock);
  ret(c, targets.erase(arg(c, 0)) ? 0u : uint32_t(GXM_ERROR_INVALID_POINTER));
}

// Colour surface (0x30 bytes, opaque to the game), our encoding: [0] format, [1] type, [2] scale, [3] width,
// [4] height, [5] stride, [6] data, [7] output register size, [8] 'CSUF' marker.
constexpr uint32_t CSURF_MARK = 0x46555343;
// sceGxmColorSurfaceInit(surface, format, type, scale, outputRegisterSize, width, height, stride, data)
void ColorSurfaceInit(Cpu& c) {
  uint32_t s = arg(c, 0);
  uint32_t w = arg(c, 5), h = arg(c, 6), stride = arg(c, 7), data = arg(c, 8);
  if (!s || !data) return ret(c, GXM_ERROR_INVALID_POINTER);
  if (!w || !h || w > 4096 || h > 4096 || stride < w) return ret(c, GXM_ERROR_INVALID_VALUE);
  uint32_t words[12] = {arg(c, 1), arg(c, 2), arg(c, 3), w, h, stride, data, arg(c, 4), CSURF_MARK, 0, 0, 0};
  std::memcpy(c.mem().ptr(s), words, sizeof words);
  ret(c, 0);
}
gpu::ColorSurface read_color_surface(Cpu& c, uint32_t s) {
  gpu::ColorSurface r;
  if (!s || c.mem().read<uint32_t>(s + 32) != CSURF_MARK) return r;
  uint32_t w[8];
  std::memcpy(w, c.mem().ptr(s), sizeof w);
  r = {true, w[0], w[1], w[2], w[7], w[3], w[4], w[5], w[6]};
  return r;
}

// Depth/stencil surface {zlsControl, depthData, stencilData, backgroundDepth, backgroundControl}: zlsControl =
// format, backgroundControl = type | stride << 8 (our encoding; the game never reads them back).
// sceGxmDepthStencilSurfaceInit(surface, format, type, strideInSamples, depthData, stencilData)
void DepthStencilSurfaceInit(Cpu& c) {
  uint32_t s = arg(c, 0);
  if (!s) return ret(c, GXM_ERROR_INVALID_POINTER);
  Memory& m = c.mem();
  m.write<uint32_t>(s, arg(c, 1));
  m.write<uint32_t>(s + 4, arg(c, 4));
  m.write<uint32_t>(s + 8, arg(c, 5));
  m.write<float>(s + 12, 1.0f);
  m.write<uint32_t>(s + 16, (arg(c, 2) & 0xff) | arg(c, 3) << 8);
  ret(c, 0);
}
gpu::DepthStencilSurface read_depth_surface(Cpu& c, uint32_t s) {
  gpu::DepthStencilSurface r;
  if (!s) return r;
  Memory& m = c.mem();
  uint32_t bg = m.read<uint32_t>(s + 16);
  r = {true, m.read<uint32_t>(s), bg & 0xff, bg >> 8, m.read<uint32_t>(s + 4), m.read<uint32_t>(s + 8)};
  return r;
}

// --- shader patcher ------------------------------------------------------------------------------------------------
void ShaderPatcherCreate(Cpu& c) {
  if (!arg(c, 0) || !arg(c, 1)) return ret(c, GXM_ERROR_INVALID_POINTER);
  std::lock_guard lk(gxm_lock);
  uint32_t h = slot(c, 16);
  patchers[h] = std::make_unique<Patcher>();
  c.mem().write<uint32_t>(arg(c, 1), h);
  ret(c, 0);
}
void ShaderPatcherDestroy(Cpu& c) {
  std::lock_guard lk(gxm_lock);
  ret(c, patchers.erase(arg(c, 0)) ? 0u : uint32_t(GXM_ERROR_INVALID_POINTER));
}

// Debug (local only): DCP_DUMP_GXP=<dir> writes each registered GXP to <dir>/<hash>.gxp and lists its parameters
// in <dir>/programs.txt. These are game files: keep them out of the repository (Work/).
void dump_program(Cpu& c, uint32_t gxp) {
  static const char* dir = std::getenv("DCP_DUMP_GXP");
  if (!dir || !*dir) return;
  Memory& m = c.mem();
  uint64_t h = gpu::gxp_hash(m, gxp);
  uint32_t size = m.read<uint32_t>(gxp + 8);
  char path[512];
  std::snprintf(path, sizeof path, "%s/%016llx.gxp", dir, (unsigned long long)h);
  if (std::FILE* f = std::fopen(path, "wb")) {
    std::fwrite(m.ptr(gxp), 1, size, f);
    std::fclose(f);
  }
  std::snprintf(path, sizeof path, "%s/programs.txt", dir);
  std::FILE* f = std::fopen(path, "a");
  if (!f) return;
  bool frag = m.read<uint32_t>(gxp + 0x14) & 1;
  std::fprintf(f, "%016llx %s size %u, default uniforms %u words\n", (unsigned long long)h, frag ? "fragment" : "vertex",
               size, m.read<uint32_t>(gxp + 0x64));
  uint32_t params = gxp_params(c, gxp);
  static const char* cat[] = {"attribute", "uniform", "sampler", "aux", "buffer"};
  for (uint32_t i = 0; i < gxp_param_count(c, gxp); i++) {
    uint32_t p = params + 16 * i;
    uint8_t b4 = m.read<uint8_t>(p + 4), b5 = m.read<uint8_t>(p + 5);
    std::string name = str(c, p + m.read<uint32_t>(p), 64);
    std::fprintf(f, "    %-10s %-24s type %u comps %u container %u semantic %u/%u array %u resource %u\n",
                 (b4 & 15) < 5 ? cat[b4 & 15] : "?", name.c_str(), b4 >> 4, b5 & 15, b5 >> 4, m.read<uint8_t>(p + 6),
                 m.read<uint8_t>(p + 7), m.read<uint32_t>(p + 8), m.read<uint32_t>(p + 12));
  }
  std::fclose(f);
}

// sceGxmShaderPatcherRegisterProgram(patcher, const SceGxmProgram* header, SceGxmShaderPatcherId* id)
void ShaderPatcherRegisterProgram(Cpu& c) {
  uint32_t gxp = arg(c, 1), out = arg(c, 2);
  if (!arg(c, 0) || !out) return ret(c, GXM_ERROR_INVALID_POINTER);
  if (!is_gxp(c, gxp)) return ret(c, GXM_ERROR_INVALID_VALUE);
  std::lock_guard lk(gxm_lock);
  uint32_t id = slot(c, 16);
  c.mem().write<uint32_t>(id, gxp);
  program_ids[id] = gxp;
  c.mem().write<uint32_t>(out, id);
  dump_program(c, gxp);
  ret(c, 0);
}
void ShaderPatcherUnregisterProgram(Cpu& c) {
  std::lock_guard lk(gxm_lock);
  ret(c, program_ids.erase(arg(c, 1)) ? 0u : uint32_t(GXM_ERROR_INVALID_VALUE));
}
// const SceGxmProgram* sceGxmShaderPatcherGetProgramFromId(id)
void ShaderPatcherGetProgramFromId(Cpu& c) {
  std::lock_guard lk(gxm_lock);
  auto it = program_ids.find(arg(c, 0));
  ret(c, it == program_ids.end() ? 0u : it->second);
}

// sceGxmShaderPatcherCreateVertexProgram(patcher, id, attributes*, count, streams*, count, out*)
void ShaderPatcherCreateVertexProgram(Cpu& c) {
  uint32_t attrs = arg(c, 2), nattr = arg(c, 3), streams = arg(c, 4), nstreams = arg(c, 5), out = arg(c, 6);
  if (!out || (nattr && !attrs) || (nstreams && !streams)) return ret(c, GXM_ERROR_INVALID_POINTER);
  if (nattr > 16 || nstreams > 16) return ret(c, GXM_ERROR_INVALID_VALUE);
  std::lock_guard lk(gxm_lock);
  auto pit = patchers.find(arg(c, 0));
  auto iit = program_ids.find(arg(c, 1));
  if (pit == patchers.end() || iit == program_ids.end()) return ret(c, GXM_ERROR_INVALID_POINTER);
  Memory& m = c.mem();
  std::vector<uint8_t> key(8 + 8 * nattr + 4 * nstreams);
  std::memcpy(key.data(), &iit->first, 4);
  std::memcpy(key.data() + 4, &nattr, 4);
  if (nattr) std::memcpy(key.data() + 8, m.ptr(attrs), 8 * nattr);
  if (nstreams) std::memcpy(key.data() + 8 + 8 * nattr, m.ptr(streams), 4 * nstreams);
  auto& cache = pit->second->vertex_cache;
  if (auto hit = cache.find(key); hit != cache.end()) {
    program_refs[hit->second].refs++;
    m.write<uint32_t>(out, hit->second);
    return ret(c, 0);
  }
  auto vp = std::make_unique<gpu::VertexProgram>();
  vp->gxp = iit->second;
  for (uint32_t i = 0; i < nattr; i++) {
    uint32_t a = attrs + 8 * i;
    vp->attributes.push_back({m.read<uint16_t>(a), m.read<uint16_t>(a + 2), m.read<uint8_t>(a + 4),
                              m.read<uint8_t>(a + 5), m.read<uint16_t>(a + 6)});
  }
  for (uint32_t i = 0; i < nstreams; i++)
    vp->streams.push_back({m.read<uint16_t>(streams + 4 * i), m.read<uint16_t>(streams + 4 * i + 2)});
  uint32_t h = slot(c, 16);
  m.write<uint32_t>(h, vp->gxp);
  vertex_programs[h] = std::move(vp);
  program_refs[h].refs = 1;
  cache[key] = h;
  m.write<uint32_t>(out, h);
  ret(c, 0);
}

// sceGxmShaderPatcherCreateFragmentProgram(patcher, id, outputFormat, multisample, blend*, vertexGxp*, out*)
void ShaderPatcherCreateFragmentProgram(Cpu& c) {
  uint32_t blend = arg(c, 4), vgxp = arg(c, 5), out = arg(c, 6);
  if (!out) return ret(c, GXM_ERROR_INVALID_POINTER);
  std::lock_guard lk(gxm_lock);
  auto pit = patchers.find(arg(c, 0));
  auto iit = program_ids.find(arg(c, 1));
  if (pit == patchers.end() || iit == program_ids.end()) return ret(c, GXM_ERROR_INVALID_POINTER);
  Memory& m = c.mem();
  auto fp = std::make_unique<gpu::FragmentProgram>();
  fp->gxp = iit->second;
  fp->output_format = arg(c, 2);
  fp->multisample = arg(c, 3);
  fp->has_blend = blend != 0;
  fp->blend = blend ? m.read<uint32_t>(blend) : 0;
  fp->vertex_gxp = vgxp;
  uint32_t kv[6] = {iit->first, fp->output_format, fp->multisample, fp->has_blend, fp->blend, vgxp};
  std::vector<uint8_t> key(sizeof kv);
  std::memcpy(key.data(), kv, sizeof kv);
  auto& cache = pit->second->fragment_cache;
  if (auto hit = cache.find(key); hit != cache.end()) {
    program_refs[hit->second].refs++;
    m.write<uint32_t>(out, hit->second);
    return ret(c, 0);
  }
  uint32_t h = slot(c, 16);
  m.write<uint32_t>(h, fp->gxp);
  fragment_programs[h] = std::move(fp);
  program_refs[h].refs = 1;
  cache[key] = h;
  m.write<uint32_t>(out, h);
  ret(c, 0);
}

// Programs stay alive (the patcher caches them); releasing only drops a reference.
void ShaderPatcherReleaseProgram(Cpu& c) {
  std::lock_guard lk(gxm_lock);
  auto it = program_refs.find(arg(c, 1));
  if (it == program_refs.end()) return ret(c, GXM_ERROR_INVALID_POINTER);
  if (it->second.refs > 0) it->second.refs--;
  ret(c, 0);
}

// --- program queries -----------------------------------------------------------------------------------------------
void ProgramGetParameterCount(Cpu& c) {
  uint32_t p = arg(c, 0);
  ret(c, is_gxp(c, p) ? gxp_param_count(c, p) : 0u);
}
// const SceGxmProgramParameter* sceGxmProgramGetParameter(program, index)
void ProgramGetParameter(Cpu& c) {
  uint32_t p = arg(c, 0), i = arg(c, 1);
  if (!is_gxp(c, p) || i >= gxp_param_count(c, p)) return ret(c, 0);
  ret(c, gxp_params(c, p) + 16 * i);
}
void ProgramParameterGetResourceIndex(Cpu& c) {
  uint32_t p = arg(c, 0);
  ret(c, p ? c.mem().read<uint32_t>(p + 12) : 0u);
}

// --- per-context state ---------------------------------------------------------------------------------------------
void SetVertexProgram(Cpu& c) {
  CONTEXT_OR(void());
  if (auto it = vertex_programs.find(arg(c, 1)); it != vertex_programs.end())
    ctx->vertex_program = arg(c, 1), ctx->vp = it->second.get();
}
void SetFragmentProgram(Cpu& c) {
  CONTEXT_OR(void());
  if (auto it = fragment_programs.find(arg(c, 1)); it != fragment_programs.end())
    ctx->fragment_program = arg(c, 1), ctx->fp = it->second.get();
}

// sceGxmSetVertexStream(ctx, index, data)
void SetVertexStream(Cpu& c) {
  CONTEXT_OR(ret(c, GXM_ERROR_INVALID_POINTER));
  uint32_t i = arg(c, 1);
  if (i >= 16) return ret(c, GXM_ERROR_INVALID_VALUE);
  if (!arg(c, 2)) return ret(c, GXM_ERROR_INVALID_POINTER);
  ctx->streams[i] = arg(c, 2);
  ret(c, 0);
}

// DEBUG (env DCP_LOG_TEXTURES=1): log each distinct format / layout / size / mip count / palette combination once,
// decoded from the control words (layout: see the textures section below).
void log_texture(const std::array<uint32_t, 4>& t) {
  static const bool on = std::getenv("DCP_LOG_TEXTURES") != nullptr;
  static std::set<std::array<uint32_t, 4>> seen;
  if (!on) return;
  uint32_t type = t[1] >> 29, base = (t[0] & 0x80000000) | (t[1] & 0x1f000000), swz = (t[3] >> 28) & 7;
  bool pot = type == 0 || type == 2;
  uint32_t w = pot ? 1u << ((t[1] >> 16) & 15) : ((t[1] >> 12) & 0xfff) + 1;
  uint32_t h = pot ? 1u << (t[1] & 15) : (t[1] & 0xfff) + 1;
  uint32_t mips = type == 6 ? 1 : ((t[0] >> 17) & 15) + 1;  // linear strided reuses the mip bits for the stride
  bool palette = (t[3] & 0x3ffffff) != 0;
  if (!seen.insert({base | swz << 12, type << 16 | mips, w << 16 | h, palette}).second) return;
  static const char* types[8] = {"swizzled", "?1", "cube", "linear", "tiled", "swizzled-arb", "strided", "cube-arb"};
  std::fprintf(stderr, "[gxm tex] base %08x swizzle %u %-12s %4ux%-4u mips %2u%s\n", base, swz, types[type], w, h, mips,
               palette ? " palette" : "");
}

// DEBUG (local only): DCP_DUMP_TEXTURES=<dir> writes each distinct texture once: <key>.raw (guest bytes), <key>.dec
// (our decoded levels) and a line in <dir>/textures.txt. Game data: keep it in Work/.
void dump_texture(Cpu& c, const std::array<uint32_t, 4>& t) {
  static const char* dir = std::getenv("DCP_DUMP_TEXTURES");
  static std::set<uint64_t> seen;
  if (!dir || !*dir) return;
  gpu::tex::Words w = {t[0], t[1], t[2], t[3]};
  Memory& m = c.mem();
  auto read = [&](uint32_t va, uint32_t n) -> const uint8_t* { return m.valid(va, n) ? (const uint8_t*)m.ptr(va) : nullptr; };
  uint64_t key = gpu::tex::cache_key(w, read);
  if (!key || !seen.insert(key).second) return;
  gpu::tex::Decoded d;
  bool ok = gpu::tex::decode(w, read, d);
  char path[512];
  auto put = [&](const char* ext, const void* p, size_t n) {
    std::snprintf(path, sizeof path, "%s/%016llx.%s", dir, (unsigned long long)key, ext);
    if (std::FILE* f = std::fopen(path, "wb")) { std::fwrite(p, 1, n, f); std::fclose(f); }
  };
  if (const uint8_t* src = read(d.desc.data, d.desc.source_bytes)) put("raw", src, d.desc.source_bytes);
  if (ok) put("dec", d.data.data(), d.data.size());
  std::snprintf(path, sizeof path, "%s/textures.txt", dir);
  if (std::FILE* f = std::fopen(path, "a")) {
    std::fprintf(f, "%016llx %08x %08x %08x %08x fmt %d type %d %ux%u mips %u %s\n", (unsigned long long)key, t[0], t[1],
                 t[2], t[3], int(d.desc.format), int(d.desc.type), d.desc.width, d.desc.height, d.desc.mips,
                 ok ? "ok" : d.desc.reason.c_str());
    std::fclose(f);
  }
}

// sceGxmSetFragmentTexture(ctx, unit, const SceGxmTexture*): copied now, as hardware latches it.
void SetFragmentTexture(Cpu& c) {
  CONTEXT_OR(ret(c, GXM_ERROR_INVALID_POINTER));
  uint32_t i = arg(c, 1), t = arg(c, 2);
  if (i >= 16) return ret(c, GXM_ERROR_INVALID_VALUE);
  if (!t) return ret(c, GXM_ERROR_INVALID_POINTER);
  for (int w = 0; w < 4; w++) ctx->textures[i][w] = c.mem().read<uint32_t>(t + 4 * w);
  log_texture(ctx->textures[i]);
  dump_texture(c, ctx->textures[i]);
  ret(c, 0);
}

// Default uniform buffers come from the context's vertex / fragment ring (sized by the bound program's GXP).
uint32_t reserve(Cpu& c, uint32_t ring, uint32_t ring_size, uint32_t& used, uint32_t bytes) {
  used = (used + 15) & ~15u;
  if (used + bytes > ring_size) used = 0;
  if (bytes > ring_size) return 0;
  uint32_t va = ring + used;
  used += bytes;
  (void)c;
  return va;
}
void ReserveVertexDefaultUniformBuffer(Cpu& c) {
  CONTEXT_OR(ret(c, GXM_ERROR_INVALID_POINTER));
  if (!arg(c, 1)) return ret(c, GXM_ERROR_INVALID_POINTER);
  if (!ctx->vertex_program) return ret(c, GXM_ERROR_NULL_PROGRAM);
  uint32_t bytes = gxp_default_uniform_bytes(c, vertex_programs[ctx->vertex_program]->gxp);
  uint32_t va = bytes ? reserve(c, ctx->vertex_ring, ctx->vertex_ring_size, ctx->vertex_ring_used, bytes) : 0;
  if (bytes && !va) return ret(c, GXM_ERROR_RESERVE_FAILED);
  ctx->vertex_uniforms = va;
  c.mem().write<uint32_t>(arg(c, 1), va);
  ret(c, 0);
}
void ReserveFragmentDefaultUniformBuffer(Cpu& c) {
  CONTEXT_OR(ret(c, GXM_ERROR_INVALID_POINTER));
  if (!arg(c, 1)) return ret(c, GXM_ERROR_INVALID_POINTER);
  if (!ctx->fragment_program) return ret(c, GXM_ERROR_NULL_PROGRAM);
  uint32_t bytes = gxp_default_uniform_bytes(c, fragment_programs[ctx->fragment_program]->gxp);
  uint32_t va = bytes ? reserve(c, ctx->fragment_ring, ctx->fragment_ring_size, ctx->fragment_ring_used, bytes) : 0;
  if (bytes && !va) return ret(c, GXM_ERROR_RESERVE_FAILED);
  ctx->fragment_uniforms = va;
  c.mem().write<uint32_t>(arg(c, 1), va);
  ret(c, 0);
}

uint16_t to_half(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  uint32_t sign = (x >> 16) & 0x8000;
  int32_t exp = int32_t((x >> 23) & 0xff) - 127 + 15;
  uint32_t mant = x & 0x7fffff;
  if (((x >> 23) & 0xff) == 0xff) return uint16_t(sign | 0x7c00 | (mant ? 0x200 : 0));
  if (exp >= 31) return uint16_t(sign | 0x7c00);
  if (exp <= 0) {
    if (exp < -10) return uint16_t(sign);
    mant |= 0x800000;
    uint32_t shift = uint32_t(14 - exp);
    uint32_t h = mant >> shift;
    if ((mant >> (shift - 1)) & 1) h++;
    return uint16_t(sign | h);
  }
  uint32_t h = sign | uint32_t(exp) << 10 | (mant >> 13);
  if (mant & 0x1000) h++;  // round half up
  return uint16_t(h);
}

// sceGxmSetUniformDataF(buffer, parameter, componentOffset, componentCount, const float* data). Placement: scalars
// and single vectors are packed from resource_index (in 32-bit words); vector arrays put each element on an
// 8-byte boundary for float types, 4 bytes for integer types. Values are converted to the parameter's type.
void SetUniformDataF(Cpu& c) {
  uint32_t buf = arg(c, 0), param = arg(c, 1), off = arg(c, 2), count = arg(c, 3), src = arg(c, 4);
  if (!buf || !param || !src) return ret(c, GXM_ERROR_INVALID_POINTER);
  Memory& m = c.mem();
  uint8_t b4 = m.read<uint8_t>(param + 4), b5 = m.read<uint8_t>(param + 5);
  uint32_t category = b4 & 15, type = b4 >> 4, comps = b5 & 15;
  uint32_t array_size = m.read<uint32_t>(param + 8), res = m.read<uint32_t>(param + 12);
  if (category != 1) return ret(c, GXM_ERROR_INVALID_VALUE);  // SCE_GXM_PARAMETER_CATEGORY_UNIFORM
  // F32, F16, C10, U32, S32, U16, S16, U8, S8
  static const uint32_t type_size[9] = {4, 2, 2, 4, 4, 2, 2, 1, 1};
  if (type >= 9 || type == 2) return ret(c, GXM_ERROR_INVALID_VALUE);
  uint32_t size = type_size[type];
  bool is_float = type <= 1;
  if (comps == 0) comps = 1;
  auto put = [&](uint32_t dst, float v) {
    switch (type) {
      case 0: m.write<float>(dst, v); break;
      case 1: m.write<uint16_t>(dst, to_half(v)); break;
      case 3: m.write<uint32_t>(dst, uint32_t(v)); break;
      case 4: m.write<int32_t>(dst, int32_t(v)); break;
      case 5: m.write<uint16_t>(dst, uint16_t(v)); break;
      case 6: m.write<int16_t>(dst, int16_t(v)); break;
      case 7: m.write<uint8_t>(dst, uint8_t(v)); break;
      case 8: m.write<int8_t>(dst, int8_t(v)); break;
    }
  };
  uint32_t base = buf + res * 4;
  if (array_size == 1 || comps == 1) {
    uint32_t step = comps == 1 ? 4 : size;  // scalar arrays use one word per element
    for (uint32_t i = 0; i < count; i++) put(base + (off + i) * step, m.read<float>(src + 4 * i));
  } else {
    uint32_t elem = comps * size;
    uint32_t align = is_float ? 8 : 4;
    elem = (elem + align - 1) & ~(align - 1);
    for (uint32_t i = 0; i < count; i++) {
      uint32_t k = off + i;
      put(base + (k / comps) * elem + (k % comps) * size, m.read<float>(src + 4 * i));
    }
  }
  ret(c, 0);
}

// sceGxmSetViewport(ctx, xOffset, xScale, yOffset, yScale, zOffset, zScale): floats in s0..s5.
void SetViewport(Cpu& c) {
  CONTEXT_OR(void());
  for (int i = 0; i < 6; i++) ctx->state.viewport[i] = c.sreg(i);
}
void SetViewportEnable(Cpu& c) {
  CONTEXT_OR(void());
  ctx->state.viewport_enabled = arg(c, 1) == 0;  // SCE_GXM_VIEWPORT_ENABLED = 0
}
void SetCullMode(Cpu& c) {
  CONTEXT_OR(void());
  ctx->state.cull = arg(c, 1);
}
void SetFrontPolygonMode(Cpu& c) {
  CONTEXT_OR(void());
  ctx->state.front_polygon = arg(c, 1);
}
void SetBackPolygonMode(Cpu& c) {
  CONTEXT_OR(void());
  ctx->state.back_polygon = arg(c, 1);
}
void SetFrontDepthFunc(Cpu& c) {
  CONTEXT_OR(void());
  ctx->state.front_depth_func = arg(c, 1);
}
void SetFrontDepthWriteEnable(Cpu& c) {
  CONTEXT_OR(void());
  ctx->state.front_depth_write = arg(c, 1);
}
void SetFrontStencilRef(Cpu& c) {
  CONTEXT_OR(void());
  ctx->state.front_stencil.ref = arg(c, 1) & 0xff;
}
// sceGxmSetFrontStencilFunc(ctx, func, stencilFail, depthFail, depthPass, compareMask, writeMask)
void SetFrontStencilFunc(Cpu& c) {
  CONTEXT_OR(void());
  gpu::StencilState& s = ctx->state.front_stencil;
  s.func = arg(c, 1);
  s.fail = arg(c, 2);
  s.depth_fail = arg(c, 3);
  s.depth_pass = arg(c, 4);
  s.compare_mask = uint8_t(arg(c, 5));
  s.write_mask = uint8_t(arg(c, 6));
}
void SetFrontPointLineWidth(Cpu& c) {
  CONTEXT_OR(void());
  ctx->state.point_line_width = arg(c, 1);
}

// --- scenes and draws ----------------------------------------------------------------------------------------------
// sceGxmBeginScene(ctx, flags, target, validRegion*, vertexSync, fragmentSync, colorSurface*, depthStencil*)
void BeginScene(Cpu& c) {
  gpu::thread_gpu_calls++;
  std::lock_guard lk(gxm_lock);
  Context* ctx = find_context(arg(c, 0));
  if (!ctx) return ret(c, GXM_ERROR_INVALID_POINTER);
  if (ctx->in_scene) return ret(c, GXM_ERROR_WITHIN_SCENE);
  auto t = targets.find(arg(c, 2));
  if (t == targets.end()) return ret(c, GXM_ERROR_INVALID_POINTER);
  gpu::Scene s;
  s.render_target = t->first;
  s.width = t->second.width;
  s.height = t->second.height;
  s.flags = arg(c, 1);
  uint32_t region = arg(c, 3);
  s.valid_x_max = region ? c.mem().read<uint32_t>(region) : s.width - 1;
  s.valid_y_max = region ? c.mem().read<uint32_t>(region + 4) : s.height - 1;
  s.color = read_color_surface(c, arg(c, 6));
  s.depth = read_depth_surface(c, arg(c, 7));
  if (uint32_t sync = arg(c, 5)) frame_syncs.insert(sync);
  ctx->in_scene = true;
  gpu::renderer().begin_scene(c.mem(), s);
  ret(c, 0);
}

// sceGxmEndScene(ctx, SceGxmNotification* vertexNotification, SceGxmNotification* fragmentNotification): the work
// is complete when we return, so both notifications are signalled at once.
void EndScene(Cpu& c) {
  std::lock_guard lk(gxm_lock);
  Context* ctx = find_context(arg(c, 0));
  if (!ctx) return ret(c, GXM_ERROR_INVALID_POINTER);
  if (!ctx->in_scene) return ret(c, GXM_ERROR_NOT_WITHIN_SCENE);
  ctx->in_scene = false;
  gpu::renderer().end_scene(c.mem());
  for (int i = 1; i <= 2; i++)
    if (uint32_t n = arg(c, i)) {
      uint32_t addr = c.mem().read<uint32_t>(n);
      if (addr) c.mem().write<uint32_t>(addr, c.mem().read<uint32_t>(n + 4));
    }
  ret(c, 0);
}

// sceGxmDraw(ctx, primType, indexType, const void* indices, indexCount)
void Draw(Cpu& c) {
  struct Timed {  // draw-path sub-timer (gpu/draw_stats.h): the whole HLE call
    uint64_t t0 = gpu::ticks();
    ~Timed() { gpu::stat_time(gpu::kGxmDrawTicks, t0); }
  } timed;
  gpu::thread_gpu_calls++;
  std::lock_guard lk(gxm_lock);
  Context* ctx = find_context(arg(c, 0));
  if (!ctx) return ret(c, GXM_ERROR_INVALID_POINTER);
  if (!ctx->in_scene) return ret(c, GXM_ERROR_NOT_WITHIN_SCENE);
  if (!ctx->vertex_program || !ctx->fragment_program) return ret(c, GXM_ERROR_NULL_PROGRAM);
  if (!arg(c, 3)) return ret(c, GXM_ERROR_INVALID_POINTER);
  gpu::Draw d;
  d.vertex = ctx->vp;
  d.fragment = ctx->fp;
  d.streams = ctx->streams;
  d.primitive = arg(c, 1);
  d.index_format = arg(c, 2);
  d.indices = arg(c, 3);
  d.index_count = arg(c, 4);
  d.vertex_uniforms = ctx->vertex_uniforms;
  d.fragment_uniforms = ctx->fragment_uniforms;
  d.textures = ctx->textures;
  d.state = ctx->state;
  gpu::renderer().draw(c.mem(), d);
  ret(c, 0);
}

// --- textures ------------------------------------------------------------------------------------------------------
// Control words (vitasdk SceGxmTexture): word0 vaddr 3-5, uaddr 6-8, mip filter 9, min 10-11, mag 12-13,
// mip count 17-20, format0 31; word1 height-1 0-11 / width-1 12-23 (power-of-two layouts: log2 h 0-3, log2 w 16-19),
// base format 24-28, type 29-31; word2 data address; word3 palette >> 6 in 0-25, swizzle 28-30.
constexpr uint32_t TEX_SWIZZLED = 0x00000000, TEX_CUBE = 0x40000000;
bool pot_layout(uint32_t type) { return type == TEX_SWIZZLED || type == TEX_CUBE; }
uint32_t log2u(uint32_t v) {
  uint32_t r = 0;
  while ((1u << r) < v) r++;
  return r;
}

uint32_t texture_init(Cpu& c, uint32_t type) {
  uint32_t t = arg(c, 0), data = arg(c, 1), fmt = arg(c, 2), w = arg(c, 3), h = arg(c, 4), mips = arg(c, 5);
  if (!t) return GXM_ERROR_INVALID_POINTER;
  if (data & 3) return GXM_ERROR_INVALID_ALIGNMENT;
  if (!w || !h || w > 4096 || h > 4096 || mips > 13) return GXM_ERROR_INVALID_VALUE;
  if (pot_layout(type) && ((w & (w - 1)) || (h & (h - 1)))) return GXM_ERROR_INVALID_VALUE;
  uint32_t w0 = (fmt >> 31) << 31 | ((mips ? mips - 1 : 0) & 15) << 17;
  uint32_t size = pot_layout(type) ? (log2u(h) | log2u(w) << 16) : ((h - 1) | (w - 1) << 12);
  uint32_t w1 = size | ((fmt >> 24) & 0x1f) << 24 | type;
  uint32_t w3 = ((fmt >> 12) & 7) << 28;
  Memory& m = c.mem();
  m.write<uint32_t>(t, w0);
  m.write<uint32_t>(t + 4, w1);
  m.write<uint32_t>(t + 8, data & ~3u);
  m.write<uint32_t>(t + 12, w3);
  return 0;
}
void TextureInitLinear(Cpu& c) { ret(c, texture_init(c, 0x60000000)); }
void TextureInitSwizzled(Cpu& c) { ret(c, texture_init(c, TEX_SWIZZLED)); }
void TextureInitSwizzledArbitrary(Cpu& c) { ret(c, texture_init(c, 0xA0000000)); }
void TextureInitCube(Cpu& c) { ret(c, texture_init(c, TEX_CUBE)); }
void TextureInitCubeArbitrary(Cpu& c) { ret(c, texture_init(c, 0xE0000000)); }

// Read-modify-write of one field of control word `w`.
uint32_t texture_field(Cpu& c, uint32_t w, uint32_t shift, uint32_t bits, uint32_t value) {
  uint32_t t = arg(c, 0);
  if (!t) return GXM_ERROR_INVALID_POINTER;
  uint32_t mask = ((1u << bits) - 1) << shift;
  uint32_t v = c.mem().read<uint32_t>(t + 4 * w);
  c.mem().write<uint32_t>(t + 4 * w, (v & ~mask) | ((value << shift) & mask));
  return 0;
}
void TextureSetUAddrMode(Cpu& c) { ret(c, texture_field(c, 0, 6, 3, arg(c, 1))); }
void TextureSetVAddrMode(Cpu& c) { ret(c, texture_field(c, 0, 3, 3, arg(c, 1))); }
void TextureSetMinFilter(Cpu& c) { ret(c, texture_field(c, 0, 10, 2, arg(c, 1))); }
void TextureSetMagFilter(Cpu& c) { ret(c, texture_field(c, 0, 12, 2, arg(c, 1))); }
void TextureSetMipFilter(Cpu& c) { ret(c, texture_field(c, 0, 9, 1, arg(c, 1) ? 1 : 0)); }  // enum = 0x200
void TextureSetMipmapCount(Cpu& c) { ret(c, texture_field(c, 0, 17, 4, arg(c, 1) ? arg(c, 1) - 1 : 0)); }
void TextureSetPalette(Cpu& c) {
  if (arg(c, 1) & 63) return ret(c, GXM_ERROR_INVALID_ALIGNMENT);
  ret(c, texture_field(c, 3, 0, 26, arg(c, 1) >> 6));
}

uint32_t texture_word(Cpu& c, int w) { return arg(c, 0) ? c.mem().read<uint32_t>(arg(c, 0) + 4 * w) : 0; }
void TextureGetWidth(Cpu& c) {
  uint32_t w1 = texture_word(c, 1);
  ret(c, pot_layout(w1 & 0xE0000000) ? 1u << ((w1 >> 16) & 15) : ((w1 >> 12) & 0xfff) + 1);
}
void TextureGetHeight(Cpu& c) {
  uint32_t w1 = texture_word(c, 1);
  ret(c, pot_layout(w1 & 0xE0000000) ? 1u << (w1 & 15) : (w1 & 0xfff) + 1);
}
void TextureGetFormat(Cpu& c) {
  uint32_t w0 = texture_word(c, 0), w1 = texture_word(c, 1), w3 = texture_word(c, 3);
  ret(c, (w0 & 0x80000000) | (w1 & 0x1f000000) | ((w3 >> 28) & 7) << 12);
}

}  // namespace

bool gxm_on_display_queue_thread() { return on_display_thread; }

std::string gxm_display_queue_stats(double frames) {
  uint64_t adds = dq_adds.exchange(0), depth = dq_depth_sum.exchange(0), wait = dq_wait_us.exchange(0);
  if (!adds) return {};
  char buf[128];
  std::snprintf(buf, sizeof buf, "  display queue: AddEntry waited %.2f ms/frame, %.2f entries queued at add (max %u)\n",
                wait / frames / 1000.0, double(depth) / double(adds), dq_max);
  return buf;
}

void register_gxm() {
  REG(sceGxmInitialize, Initialize);
  REG(sceGxmTerminate, Terminate);
  REG(sceGxmMapMemory, MapMemory);
  REG(sceGxmUnmapMemory, UnmapMemory);
  REG(sceGxmMapVertexUsseMemory, MapUsseMemory);
  REG(sceGxmMapFragmentUsseMemory, MapUsseMemory);
  REG(sceGxmUnmapVertexUsseMemory, UnmapUsseMemory);
  REG(sceGxmUnmapFragmentUsseMemory, UnmapUsseMemory);
  REG(sceGxmPadHeartbeat, PadHeartbeat);
  REG(sceGxmFinish, Finish);
  REG(sceGxmDisplayQueueFinish, DisplayQueueFinish);
  REG(sceGxmDisplayQueueAddEntry, DisplayQueueAddEntry);
  REG(sceGxmSyncObjectCreate, SyncObjectCreate);
  REG(sceGxmSyncObjectDestroy, SyncObjectDestroy);
  REG(sceGxmCreateContext, CreateContext);
  REG(sceGxmDestroyContext, DestroyContext);
  REG(sceGxmCreateRenderTarget, CreateRenderTarget);
  REG(sceGxmDestroyRenderTarget, DestroyRenderTarget);
  REG(sceGxmColorSurfaceInit, ColorSurfaceInit);
  REG(sceGxmDepthStencilSurfaceInit, DepthStencilSurfaceInit);
  REG(sceGxmShaderPatcherCreate, ShaderPatcherCreate);
  REG(sceGxmShaderPatcherDestroy, ShaderPatcherDestroy);
  REG(sceGxmShaderPatcherRegisterProgram, ShaderPatcherRegisterProgram);
  REG(sceGxmShaderPatcherUnregisterProgram, ShaderPatcherUnregisterProgram);
  REG(sceGxmShaderPatcherGetProgramFromId, ShaderPatcherGetProgramFromId);
  REG(sceGxmShaderPatcherCreateVertexProgram, ShaderPatcherCreateVertexProgram);
  REG(sceGxmShaderPatcherCreateFragmentProgram, ShaderPatcherCreateFragmentProgram);
  REG(sceGxmShaderPatcherReleaseVertexProgram, ShaderPatcherReleaseProgram);
  REG(sceGxmShaderPatcherReleaseFragmentProgram, ShaderPatcherReleaseProgram);
  REG(sceGxmProgramGetParameterCount, ProgramGetParameterCount);
  REG(sceGxmProgramGetParameter, ProgramGetParameter);
  REG(sceGxmProgramParameterGetResourceIndex, ProgramParameterGetResourceIndex);
  REG(sceGxmSetVertexProgram, SetVertexProgram);
  REG(sceGxmSetFragmentProgram, SetFragmentProgram);
  REG(sceGxmSetVertexStream, SetVertexStream);
  REG(sceGxmSetFragmentTexture, SetFragmentTexture);
  REG(sceGxmReserveVertexDefaultUniformBuffer, ReserveVertexDefaultUniformBuffer);
  REG(sceGxmReserveFragmentDefaultUniformBuffer, ReserveFragmentDefaultUniformBuffer);
  REG(sceGxmSetUniformDataF, SetUniformDataF);
  REG(sceGxmSetViewport, SetViewport);
  REG(sceGxmSetViewportEnable, SetViewportEnable);
  REG(sceGxmSetCullMode, SetCullMode);
  REG(sceGxmSetFrontPolygonMode, SetFrontPolygonMode);
  REG(sceGxmSetBackPolygonMode, SetBackPolygonMode);
  REG(sceGxmSetFrontDepthFunc, SetFrontDepthFunc);
  REG(sceGxmSetFrontDepthWriteEnable, SetFrontDepthWriteEnable);
  REG(sceGxmSetFrontStencilRef, SetFrontStencilRef);
  REG(sceGxmSetFrontStencilFunc, SetFrontStencilFunc);
  REG(sceGxmSetFrontPointLineWidth, SetFrontPointLineWidth);
  REG(sceGxmBeginScene, BeginScene);
  REG(sceGxmEndScene, EndScene);
  REG(sceGxmDraw, Draw);
  REG(sceGxmTextureInitLinear, TextureInitLinear);
  REG(sceGxmTextureInitSwizzled, TextureInitSwizzled);
  REG(sceGxmTextureInitSwizzledArbitrary, TextureInitSwizzledArbitrary);
  REG(sceGxmTextureInitCube, TextureInitCube);
  REG(sceGxmTextureInitCubeArbitrary, TextureInitCubeArbitrary);
  REG(sceGxmTextureSetUAddrMode, TextureSetUAddrMode);
  REG(sceGxmTextureSetVAddrMode, TextureSetVAddrMode);
  REG(sceGxmTextureSetMinFilter, TextureSetMinFilter);
  REG(sceGxmTextureSetMagFilter, TextureSetMagFilter);
  REG(sceGxmTextureSetMipFilter, TextureSetMipFilter);
  REG(sceGxmTextureSetMipmapCount, TextureSetMipmapCount);
  REG(sceGxmTextureSetPalette, TextureSetPalette);
  REG(sceGxmTextureGetWidth, TextureGetWidth);
  REG(sceGxmTextureGetHeight, TextureGetHeight);
  REG(sceGxmTextureGetFormat, TextureGetFormat);
}

}  // namespace hle
