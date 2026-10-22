// SceDisplay: the shown framebuffer and the vblank clock. Vblanks tick at 59.94 Hz from process start, so every
// sceDisplayWaitVblankStart* paces the caller like the hardware does; a new framebuffer goes to the renderer.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gpu/renderer.h"
#include "hle/common.h"
#include "hle/ctrl.h"
#include "hle/kobj.h"
#include "hle/threads.h"
#include "guest/hle.h"

namespace hle {
namespace {

constexpr uint32_t DISPLAY_ERROR_INVALID_VALUE = 0x80290001;  // vitasdk SCE_DISPLAY_ERROR_INVALID_VALUE
constexpr uint32_t DISPLAY_ERROR_INVALID_ADDR = 0x80290002;

using Clock = std::chrono::steady_clock;
const Clock::time_point t0 = Clock::now();
constexpr std::chrono::nanoseconds VBLANK{16683350};  // 1001/60000 s

std::mutex display_lock;
gpu::Frame shown;
std::atomic<uint64_t> flips{0};  // sceDisplaySetFrameBuf calls (input script clock)

uint64_t vcount_now() { return uint64_t((Clock::now() - t0) / VBLANK); }

void wait_next_vblank() { std::this_thread::sleep_until(t0 + VBLANK * (vcount_now() + 1)); }

// int sceDisplaySetFrameBuf(const SceDisplayFrameBuf* {size, base, pitch, pixelformat, width, height}, sync)
void SetFrameBuf(Cpu& c) {
  uint32_t p = arg(c, 0);
  gpu::Frame f;
  if (p) {
    Memory& m = c.mem();
    f = {m.read<uint32_t>(p + 4), m.read<uint32_t>(p + 8), m.read<uint32_t>(p + 12), m.read<uint32_t>(p + 16),
         m.read<uint32_t>(p + 20)};
    if (f.base & 0xff) return ret(c, DISPLAY_ERROR_INVALID_ADDR);
    if (f.pitch < f.width || f.width == 0 || f.height == 0) return ret(c, DISPLAY_ERROR_INVALID_VALUE);
  }
  {
    std::lock_guard lk(display_lock);
    shown = f;
  }
  // The game flips only from its display queue callback (0x813b7ad6, run on our display queue thread after
  // AddEntry flushed the frame). A flip from any other thread shows that thread's work so far, as before.
  if (!gxm_on_display_queue_thread()) gpu::renderer().flush(c.mem());
  flips++;
  frame_presented();
  gpu::renderer().present(c.mem(), f);
  ret(c, 0);
}

// Who waits: guest return address (LR) of each sceDisplayWaitVblankStart* call, listed in the [timing] block.
struct CallerStat { uint64_t calls = 0, us = 0; };
std::mutex callers_lock;
std::map<uint32_t, CallerStat> callers;

void WaitVblankStart(Cpu& c) {
  uint32_t lr = c.reg(14) & ~1u;
  auto t = Clock::now();
  frame_wait_begin();
  wait_next_vblank();
  frame_wait_end();
  uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t).count());
  gpu::vblank_wait_us += us;
  note_vblank_wait(us);
  {
    std::lock_guard lk(callers_lock);
    auto& s = callers[lr];
    s.calls++, s.us += us;
  }
  ret(c, 0);
}

std::string frame_times(double frames) {
  std::string out = thread_times(frames) + thread_perf_report(frames) + lock_stats_report(frames) +
                    gxm_display_queue_stats(frames) + frame_stats_report() + guest::import_callers_report(frames);
  std::vector<std::pair<uint32_t, CallerStat>> v;
  {
    std::lock_guard lk(callers_lock);
    v.assign(callers.begin(), callers.end());
    callers.clear();
  }
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.us > b.second.us; });
  if (v.empty()) return out;
  out += "  vblank callers:";
  char buf[64];
  for (size_t i = 0; i < v.size() && i < 4; i++) {
    std::snprintf(buf, sizeof buf, " %08x %.2fx %.2fms", v[i].first, v[i].second.calls / frames,
                  v[i].second.us / frames / 1000.0);
    out += buf;
  }
  return out + " (per frame)\n";
}

}  // namespace

uint64_t display_flip_count() { return flips.load(); }

void register_display() {
  gpu::frame_log_extra = frame_times;
  REG(sceDisplaySetFrameBuf, SetFrameBuf);
  REG(sceDisplayWaitVblankStart, WaitVblankStart);
  REG(sceDisplayWaitVblankStartCB, WaitVblankStart);  // no callbacks registered by the game: same wait
}

}  // namespace hle
