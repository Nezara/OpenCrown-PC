// Spike log for the [timing] block: which guest thread made a slow frame. Every guest thread that waits for the next
// frame (vblank waits, the AddEntry queue wait, the display queue's idle wait) has "work segments" between those
// waits; each closed segment records its wall time, the thread's CPU time, the time it spent in blocking imports
// (waits / locks / sleeps, guest/hle.cpp) and, when it went over the frame budget, its top imports and (with
// DCP_GUEST_PROF=1) its most sampled guest blocks. Flips end frame intervals: they feed a histogram, and the worst
// intervals of each log window keep, per thread, the busiest segment that overlapped them. A frame is judged one
// flip later, so segments still open at its flip (the display thread's) have closed by then.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#include "gpu/draw_stats.h"
#include "hle/threads.h"
#include "guest/guest_prof.h"
#include "guest/hle.h"

namespace hle {
namespace {

using Clock = std::chrono::steady_clock;
constexpr uint64_t BUDGET_NS = 16683350;  // one 59.94 Hz vblank
constexpr size_t SEGMENTS = 512;          // ~3 s of three threads' segments at 60 fps
constexpr size_t WORST = 4;               // frames listed per window

uint64_t now_ns() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}
const uint64_t t_origin = now_ns();

// CPU time of the calling thread in ns. Windows: TSC cycles of this thread over a once-measured cycle rate.
uint64_t thread_cpu_ns() {
#ifdef _WIN32
  static const double cycles_per_ns = [] {
    ULONG64 c0 = 0, c1 = 0;
    HANDLE me = GetCurrentThread();
    auto t0 = Clock::now();
    QueryThreadCycleTime(me, &c0);
    while (Clock::now() - t0 < std::chrono::milliseconds(10)) {
    }
    QueryThreadCycleTime(me, &c1);
    double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
    return ns > 0 && c1 > c0 ? double(c1 - c0) / ns : 1.0;
  }();
  ULONG64 c = 0;
  QueryThreadCycleTime(GetCurrentThread(), &c);
  return uint64_t(double(c) / cycles_per_ns);
#else
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#endif
}

struct Segment {
  std::string thread;
  uint64_t start = 0, end = 0, cpu = 0, blocked = 0;
  std::string imports, guest, gpu;  // only for segments over budget (gpu: draw-path sub-timers, if it drew)
  bool over = false;
  uint64_t busy() const { return end - start - std::min(blocked, end - start); }
};

struct Local {
  bool in_work = false;
  uint64_t start = 0, cpu0 = 0;
  gpu::DrawStatsSnapshot gpu0;  // draw-path counters at the segment start
  uint64_t gpu_calls0 = 0;      // this thread's draw / scene calls then
};
thread_local Local local;

struct Frame {
  uint64_t start = 0, end = 0;
  std::vector<Segment> threads;  // busiest overlapping segment per thread
};

std::mutex mu;
std::deque<Segment> segments;
uint64_t last_flip = 0, pend_start = 0, pend_end = 0;
uint64_t hist[4] = {};  // flip intervals in whole ms <= 17, <= 34, <= 50, more: 1, 2, 3, 4+ vblanks
std::vector<Frame> worst;  // longest first

// mu held.
void judge(uint64_t start, uint64_t end) {
  uint64_t len = end - start;
  if (worst.size() >= WORST && len <= worst.back().end - worst.back().start) return;
  Frame f{start, end, {}};
  for (const Segment& s : segments) {
    if (s.end <= start || s.start >= end) continue;
    auto it = std::find_if(f.threads.begin(), f.threads.end(), [&](const Segment& o) { return o.thread == s.thread; });
    if (it == f.threads.end())
      f.threads.push_back(s);
    else if (s.busy() > it->busy())
      *it = s;
  }
  std::sort(f.threads.begin(), f.threads.end(), [](const Segment& a, const Segment& b) { return a.busy() > b.busy(); });
  worst.push_back(std::move(f));
  std::sort(worst.begin(), worst.end(), [](const Frame& a, const Frame& b) { return a.end - a.start > b.end - b.start; });
  if (worst.size() > WORST) worst.pop_back();
}

}  // namespace

void frame_wait_begin() {
  if (!local.in_work) return;
  local.in_work = false;
  Segment s;
  s.end = now_ns();
  s.start = local.start;
  s.cpu = thread_cpu_ns() - local.cpu0;
  bool long_wall = s.end - s.start > BUDGET_NS;
  guest::SegmentImports imp = guest::svc_segment_end(long_wall);
  s.blocked = imp.blocked_ns;
  s.over = s.busy() > BUDGET_NS || s.cpu > BUDGET_NS;
  if (s.over) {
    s.imports = std::move(imp.text);
    s.guest = guest::prof::recent_hot(s.start, 4);
    if (gpu::thread_gpu_calls != local.gpu_calls0) s.gpu = gpu::draw_stats_text(local.gpu0, gpu::draw_stats_snapshot(), 0);
  }
  s.thread = current_thread_name();
  std::lock_guard lk(mu);
  segments.push_back(std::move(s));
  if (segments.size() > SEGMENTS) segments.pop_front();
}

void frame_wait_end() {
  local.in_work = true;
  local.start = now_ns();
  local.cpu0 = thread_cpu_ns();
  local.gpu0 = gpu::draw_stats_snapshot();
  local.gpu_calls0 = gpu::thread_gpu_calls;
  guest::svc_segment_start();
}

void frame_presented() {
  static std::atomic<uint32_t> flips{0};
  if (++flips % 30 == 0) sample_cpu_clocks();  // a few sysfs reads every ~0.5 s (thread_perf_report)
  uint64_t t = now_ns();
  std::lock_guard lk(mu);
  if (last_flip) {
    uint64_t ms = (t - last_flip) / 1000000;
    hist[ms <= 17 ? 0 : ms <= 34 ? 1 : ms <= 50 ? 2 : 3]++;
    if (pend_end) judge(pend_start, pend_end);
    pend_start = last_flip, pend_end = t;
  }
  last_flip = t;
}

std::string frame_stats_report() {
  std::lock_guard lk(mu);
  uint64_t n = hist[0] + hist[1] + hist[2] + hist[3];
  if (!n) return {};
  std::string out;
  char b[256];
  std::snprintf(b, sizeof b,
                "  flip intervals: %llu <18 ms (1 vblank), %llu <35 (2), %llu <51 (3), %llu longer | worst frames (per thread its busiest "
                "segment; work = wall - blocked; * = over 16.7 ms work or CPU):\n",
                (unsigned long long)hist[0], (unsigned long long)hist[1], (unsigned long long)hist[2],
                (unsigned long long)hist[3]);
  out += b;
  for (const Frame& f : worst) {
    std::snprintf(b, sizeof b, "  spike %.1f ms at %.2f s:", (f.end - f.start) / 1e6, (f.end - t_origin) / 1e9);
    out += b;
    if (f.threads.empty()) out += " (no thread segments)";
    for (size_t i = 0; i < f.threads.size() && i < 4; i++) {
      const Segment& s = f.threads[i];
      std::snprintf(b, sizeof b, "%s %s%s work %.1f (wall %.1f, cpu %.1f, blocked %.1f)", i ? ";" : "",
                    s.over ? "*" : "", s.thread.c_str(), s.busy() / 1e6, (s.end - s.start) / 1e6, s.cpu / 1e6,
                    s.blocked / 1e6);
      out += b;
      if (!s.imports.empty()) out += " imports [" + s.imports + "]";
      if (!s.guest.empty()) out += " guest [" + s.guest + "]";
      if (!s.gpu.empty()) out += " gpu [" + s.gpu + "]";
    }
    out += "\n";
  }
  hist[0] = hist[1] = hist[2] = hist[3] = 0;
  worst.clear();
  return out;
}

}  // namespace hle
