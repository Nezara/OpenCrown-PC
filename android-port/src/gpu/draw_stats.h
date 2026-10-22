// Draw-path sub-timers (always on): where the rendering thread's time inside sceGxmDraw / BeginScene goes. Counters
// are cumulative since start; the rendering thread is the only writer (a relaxed load + store, no locked add), any
// thread may read a snapshot. Time comes from the CPU's own counter (arm64 CNTVCT_EL0, x86 TSC), a few ns per read,
// so a draw can afford ~10 stamps. Printed as the `draw path:` line of the [timing] block (vulkan.cpp log_timing) and,
// for spike segments that drew, as `gpu [...]` (hle/frame_stats.cpp).
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace gpu {

inline uint64_t ticks() {
#if defined(__aarch64__)
  uint64_t v;
  asm volatile("mrs %0, cntvct_el0" : "=r"(v));
  return v;
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
  return __rdtsc();
#else
  return 0;
#endif
}
double ticks_per_us();  // measured once (x86) or read from CNTFRQ_EL0 (arm64)

enum DrawStat : int {
  kDraws,          // renderer draw() calls
  kGxmDrawTicks,   // whole sceGxmDraw HLE (gxm lock, state copy, renderer draw)
  kDrawTicks,      // renderer draw() incl. its lock
  kProgramTicks,   // program lookups (GXP hash once per frame, map) incl. translations
  kTranslations, kTranslateTicks,  // USSE -> SPIR-V + shader module (or loaded from the disk cache)
  kPipelineTicks,  // pipeline lookups incl. builds
  kPipelineBuilds, kPipelineBuildTicks,  // vkCreateGraphicsPipelines on the rendering thread
  kTextureTicks,   // texture lookups incl. everything below
  kTextureHashes, kTextureHashTicks, kTextureHashBytes,  // full hashes of a texture's guest bytes (ticks: all checks)
  kTextureSamples,                                       // sample checks (texture.h sample_key)
  kTextureDecodes, kTextureDecodeTicks,                  // unswizzle / convert / CPU BC decompression
  kTextureUploads, kTextureUploadTicks, kTextureUploadBytes,  // image + memory + staging + submit + fence wait
  kTargetCopies,   // render target -> texture copies recorded
  kCopyTicks, kCopyBytes,  // index scan + index / vertex / uniform copies into the host ring
  kDescriptorTicks,        // sampler lookup + descriptor set allocate / update (or cache hit)
  kDescriptorAllocs,       // descriptor sets written (the rest were reused)
  kCmdTicks,               // vkCmd* recording for the draw
  kScenes, kSceneTicks, kSceneFenceTicks,  // BeginScene: whole call / waiting for the frame slot's fence
  kStatCount
};

extern std::atomic<uint64_t> draw_stats[kStatCount];
// sceGxmDraw / BeginScene calls made by the calling thread: the spike log attaches the counters only to segments of
// the thread that drew.
inline thread_local uint64_t thread_gpu_calls = 0;
inline void stat_add(DrawStat s, uint64_t v) {
  draw_stats[s].store(draw_stats[s].load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}
// Adds the ticks since `t0` to `s` and returns now.
inline uint64_t stat_time(DrawStat s, uint64_t t0) {
  uint64_t t = ticks();
  stat_add(s, t - t0);
  return t;
}

struct DrawStatsSnapshot {
  uint64_t v[kStatCount] = {};
};
DrawStatsSnapshot draw_stats_snapshot();
// Difference b - a as text. per_frame > 0: the [timing] line (values divided by it); 0: a compact spike summary
// (empty when nothing was drawn).
std::string draw_stats_text(const DrawStatsSnapshot& a, const DrawStatsSnapshot& b, double per_frame);

}  // namespace gpu
