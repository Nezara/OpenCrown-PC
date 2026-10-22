#include "gpu/renderer.h"
#include "gpu/overlay.h"
#include "gpu/draw_stats.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

#include "guest/memory.h"

namespace gpu {
namespace {

// Headless: counts what it is given so runs can show the game is rendering.
class NullRenderer final : public Renderer {
 public:
  const char* name() const override { return "null"; }
  void begin_scene(guest::Memory&, const Scene&) override { scenes_++; }
  void draw(guest::Memory&, const Draw&) override { draws_++; }
  void end_scene(guest::Memory&) override {}
  void present(guest::Memory&, const Frame& f) override {
    if (++frames_ % 600 == 1)
      std::fprintf(stderr, "[gpu] frame %llu: %ux%u fb 0x%08x, %llu scenes / %llu draws so far\n",
                   (unsigned long long)frames_.load(), f.width, f.height, f.base, (unsigned long long)scenes_.load(),
                   (unsigned long long)draws_.load());
  }

 private:
  std::atomic<uint64_t> scenes_{0}, draws_{0}, frames_{0};
};

std::unique_ptr<Renderer> current = std::make_unique<NullRenderer>();

}  // namespace

std::atomic<uint64_t> vblank_wait_us{0};
std::atomic<uint64_t> draw_stats[kStatCount];

double ticks_per_us() {
  static const double rate = [] {
#if defined(__aarch64__)
    uint64_t f;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return double(f) / 1e6;
#else
    using C = std::chrono::steady_clock;
    auto t0 = C::now();
    uint64_t k0 = ticks();
    while (C::now() - t0 < std::chrono::milliseconds(20)) {
    }
    double us = double(std::chrono::duration_cast<std::chrono::nanoseconds>(C::now() - t0).count()) / 1000.0;
    uint64_t k1 = ticks();
    return k1 > k0 && us > 0 ? double(k1 - k0) / us : 1.0;
#endif
  }();
  return rate;
}

DrawStatsSnapshot draw_stats_snapshot() {
  DrawStatsSnapshot s;
  for (int i = 0; i < kStatCount; i++) s.v[i] = draw_stats[i].load(std::memory_order_relaxed);
  return s;
}

std::string draw_stats_text(const DrawStatsSnapshot& a, const DrawStatsSnapshot& b, double per_frame) {
  uint64_t d[kStatCount];
  for (int i = 0; i < kStatCount; i++) d[i] = b.v[i] - a.v[i];
  double k = 1.0 / (ticks_per_us() * 1000.0);  // ticks -> ms
  char buf[768];
  if (per_frame > 0) {
    double n = per_frame, ms = k / n;
    std::snprintf(buf, sizeof buf,
                  "  draw path: %.0f draws %.2f ms/frame (gxm %.2f, %.2f us/draw) | programs %.2f (translated %llu, %.1f ms) "
                  "| pipelines %.2f (built %llu, %.1f ms) | textures %.2f (hash %.0f / %.2f MB + %.0f sampled %.2f, decode %llu %.1f ms, "
                  "upload %llu / %.1f MB %.1f ms, rt copies %.1f) | copies %.2f (%.0f KB) | descriptors %.2f (%.0f written) "
                  "| vkCmd %.2f | scenes %.1f %.2f (fence %.2f) ms/frame\n",
                  d[kDraws] / n, d[kDrawTicks] * ms, d[kGxmDrawTicks] * ms,
                  d[kDraws] ? d[kGxmDrawTicks] * k * 1000.0 / double(d[kDraws]) : 0.0, d[kProgramTicks] * ms,
                  (unsigned long long)d[kTranslations], d[kTranslateTicks] * k, d[kPipelineTicks] * ms,
                  (unsigned long long)d[kPipelineBuilds], d[kPipelineBuildTicks] * k, d[kTextureTicks] * ms,
                  d[kTextureHashes] / n, d[kTextureHashBytes] / n / 1048576.0, d[kTextureSamples] / n,
                  d[kTextureHashTicks] * ms,
                  (unsigned long long)d[kTextureDecodes], d[kTextureDecodeTicks] * k,
                  (unsigned long long)d[kTextureUploads], d[kTextureUploadBytes] / 1048576.0, d[kTextureUploadTicks] * k,
                  d[kTargetCopies] / n, d[kCopyTicks] * ms, d[kCopyBytes] / n / 1024.0, d[kDescriptorTicks] * ms,
                  d[kDescriptorAllocs] / n, d[kCmdTicks] * ms, d[kScenes] / n, d[kSceneTicks] * ms,
                  d[kSceneFenceTicks] * ms);
    return buf;
  }
  if (!d[kDraws] && !d[kScenes]) return {};
  std::snprintf(buf, sizeof buf,
                "%llu draws %.1f ms: prog %.1f (%llu new %.1f), pipe %.1f (%llu new %.1f), tex %.1f (hash %.1f, decode "
                "%llu %.1f, upload %llu %.1f), copy %.1f, desc %.1f, cmd %.1f, scene %.1f (fence %.1f)",
                (unsigned long long)d[kDraws], d[kGxmDrawTicks] * k, d[kProgramTicks] * k,
                (unsigned long long)d[kTranslations], d[kTranslateTicks] * k, d[kPipelineTicks] * k,
                (unsigned long long)d[kPipelineBuilds], d[kPipelineBuildTicks] * k, d[kTextureTicks] * k,
                d[kTextureHashTicks] * k, (unsigned long long)d[kTextureDecodes], d[kTextureDecodeTicks] * k,
                (unsigned long long)d[kTextureUploads], d[kTextureUploadTicks] * k, d[kCopyTicks] * k,
                d[kDescriptorTicks] * k, d[kCmdTicks] * k, d[kSceneTicks] * k, d[kSceneFenceTicks] * k);
  return buf;
}

std::string (*frame_log_extra)(double frames) = nullptr;

uint64_t gxp_hash(guest::Memory& mem, uint32_t gxp) {
  uint32_t size = mem.read<uint32_t>(gxp + 8);
  if (!mem.valid(gxp, size)) return 0;
  const uint8_t* p = mem.ptr(gxp);
  uint64_t h = 0xcbf29ce484222325ull;
  for (uint32_t i = 0; i < size; i++) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

Renderer& renderer() { return *current; }
void set_renderer(std::unique_ptr<Renderer> r) { current = std::move(r); }
std::unique_ptr<Renderer> make_null_renderer() { return std::make_unique<NullRenderer>(); }

namespace {
std::mutex overlay_lock;
std::vector<OverlayVertex> overlay_tris;
}  // namespace

void set_overlay(std::vector<OverlayVertex> tris) {
  std::lock_guard lk(overlay_lock);
  overlay_tris = std::move(tris);
}
std::vector<OverlayVertex> overlay() {
  std::lock_guard lk(overlay_lock);
  return overlay_tris;
}

}  // namespace gpu
