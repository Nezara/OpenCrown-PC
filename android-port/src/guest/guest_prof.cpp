#include "guest/guest_prof.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "guest/cpu.h"
#include "guest/memory.h"

#ifdef _WIN32
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#endif
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace guest::prof {

namespace {

constexpr uint32_t SLOT_STRIDE = 64;  // one cache line per thread: the JIT stores to it on every block entry
constexpr uint32_t SLOT_AREA = 0x4000;
constexpr uint64_t IMPORT_KEY = 1ull << 32;

// One live host thread running guest code.
struct Rec {
  std::string name;
  uint32_t slot = 0;
  std::atomic<int32_t> import{-1};
  std::atomic<bool> dead{false};
  bool has_clock = false;
#ifdef _WIN32
  HANDLE thread = nullptr;
#else
  clockid_t clock{};
  int stat_fd = -1;  // /proc/self/task/<tid>/stat: run state at the sample instant
#endif
  uint64_t last_cpu = 0;
  bool first = true;
  uint32_t serial = 0;  // identifies the record in the recent-sample ring (slots are reused)
};

// Recent busy samples of every thread, for per-frame queries (recent_hot).
struct Recent {
  uint64_t t_ns;
  uint64_t key;
  uint32_t serial;
};
constexpr size_t RECENT = 1 << 15;

struct Agg {
  uint64_t cpu = 0, samples = 0, busy = 0;
  std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> keys;  // key -> busy, idle samples
};

struct State {
  std::mutex mu;
  std::string out = "guest_prof.txt";
  std::function<std::string(uint32_t)> import_name;
  Memory* mem = nullptr;
  uint32_t area = 0;
  std::vector<uint32_t> free_slots;
  std::vector<Rec*> live;
  std::map<std::string, Agg> agg;
  bool started = false;
  unsigned hz = 1000;
  double units_per_ns = 1.0;  // thread CPU clock units (ns on POSIX, cycles on Windows)
  uint64_t ticks = 0;
  std::chrono::steady_clock::time_point t0;
  std::vector<Recent> recent;  // ring of RECENT
  size_t recent_next = 0;
  uint32_t next_serial = 1;
};

State& st() {
  static State s;
  return s;
}

// The calling thread's record; marked dead when the host thread ends (the sampler frees it).
struct Holder {
  Rec* rec = nullptr;
  ~Holder() {
    if (rec) rec->dead.store(true);
  }
};
thread_local Holder holder;

uint64_t cpu_now(Rec& r) {
#ifdef _WIN32
  ULONG64 c = 0;
  QueryThreadCycleTime(r.thread, &c);
  return c;
#else
  timespec ts{};
  clock_gettime(r.clock, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#endif
}

#ifdef _WIN32
// Cycles per ns of this thread's cycle counter (QueryThreadCycleTime counts TSC cycles while the thread runs).
double calibrate_cycles() {
  ULONG64 c0 = 0, c1 = 0;
  HANDLE self = GetCurrentThread();
  auto t0 = std::chrono::steady_clock::now();
  QueryThreadCycleTime(self, &c0);
  while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(20)) {
  }
  QueryThreadCycleTime(self, &c1);
  double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  return ns > 0 && c1 > c0 ? double(c1 - c0) / ns : 1.0;
}
#endif

std::string clean(std::string s) {
  for (char& c : s)
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  return s;
}

void write_file(State& s) {
  std::string text;
  {
    std::lock_guard lk(s.mu);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.t0).count();
    char b[256];
    std::snprintf(b, sizeof b, "# OpenCrown guest profile v1 (tools/guest_prof.py)\nhz\t%u\nseconds\t%.2f\nticks\t%llu\n",
                  s.hz, secs, (unsigned long long)s.ticks);
    text += b;
    for (auto& [name, a] : s.agg) {
      std::snprintf(b, sizeof b, "T\t%s\t%.1f\t%llu\t%llu\n", name.c_str(), a.cpu / s.units_per_ns / 1e6,
                    (unsigned long long)a.samples, (unsigned long long)a.busy);
      text += b;
      std::vector<std::pair<uint64_t, std::pair<uint32_t, uint32_t>>> rows(a.keys.begin(), a.keys.end());
      std::sort(rows.begin(), rows.end(), [](const auto& x, const auto& y) {
        return x.second.first + x.second.second > y.second.first + y.second.second;
      });
      for (auto& [key, n] : rows) {
        if (key & IMPORT_KEY) {
          uint32_t imm = uint32_t(key);
          std::string desc = s.import_name ? clean(s.import_name(imm)) : "";
          std::snprintf(b, sizeof b, "I\t%s\t%u\t%u\t%u\t", name.c_str(), imm, n.first, n.second);
          text += b + desc + "\n";
        } else {
          std::snprintf(b, sizeof b, "G\t%s\t%08x\t%u\t%u\n", name.c_str(), uint32_t(key), n.first, n.second);
          text += b;
        }
      }
    }
  }
  std::string tmp = s.out + ".tmp";
  if (std::FILE* f = std::fopen(tmp.c_str(), "wb")) {
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    std::remove(s.out.c_str());
    std::rename(tmp.c_str(), s.out.c_str());
  }
}

// Is the thread on the CPU right now? It used no CPU during the last tick: no. Linux / Android: its run state
// (R = running or runnable). Elsewhere: in guest code, yes; in an import, when it used at least half the tick
// (undercounts short bursts). Threads also block outside imports (ULT workers between jobs, fibers), so guest-code
// samples are checked too.
bool on_cpu(Rec& r, bool in_import, uint64_t delta, uint64_t period_units) {
  if (!r.has_clock) return true;
  if (!delta) return false;
#ifndef _WIN32
  if (r.stat_fd >= 0) {
    char b[512];
    ssize_t n = pread(r.stat_fd, b, sizeof b - 1, 0);
    if (n > 0) {
      b[n] = 0;
      const char* p = std::strrchr(b, ')');
      if (p && p[1] == ' ') return p[2] == 'R';
    }
  }
#endif
  return !in_import || delta * 2 >= period_units;
}

uint64_t steady_ns() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}

void sample(State& s, uint64_t period_units) {
  std::lock_guard lk(s.mu);
  s.ticks++;
  const uint64_t t_now = steady_ns();
  if (s.recent.empty()) s.recent.assign(RECENT, Recent{0, 0, 0});
  for (size_t i = 0; i < s.live.size();) {
    Rec& r = *s.live[i];
    uint64_t now = r.has_clock ? cpu_now(r) : 0;
    uint64_t delta = r.first ? 0 : now - r.last_cpu;
    r.last_cpu = now;
    bool first = r.first;
    r.first = false;
    Agg& a = s.agg[r.name];
    a.cpu += delta;
    if (r.dead.load()) {
#ifdef _WIN32
      CloseHandle(r.thread);
#else
      if (r.stat_fd >= 0) close(r.stat_fd);
#endif
      s.free_slots.push_back(r.slot);
      delete &r;
      s.live.erase(s.live.begin() + long(i));
      continue;
    }
    i++;
    if (first) continue;
    int32_t imm = r.import.load(std::memory_order_relaxed);
    uint64_t key = imm >= 0 ? IMPORT_KEY | uint32_t(imm)
                            : __atomic_load_n(reinterpret_cast<uint32_t*>(s.mem->ptr(r.slot)), __ATOMIC_RELAXED);
    bool busy = on_cpu(r, imm >= 0, delta, period_units);
    auto& n = a.keys[key];
    (busy ? n.first : n.second)++;
    a.samples++;
    a.busy += busy;
    if (busy) {
      s.recent[s.recent_next] = {t_now, key, r.serial};
      s.recent_next = (s.recent_next + 1) % RECENT;
    }
  }
}

void sampler() {
  State& s = st();
#ifdef _WIN32
  s.units_per_ns = calibrate_cycles();
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#else
  pthread_setname_np(pthread_self(), "guest profiler");
#ifdef __linux__
  // Started from the first registered guest thread, so it inherited that thread's core pinning (the fast cores):
  // move to the other cores so the sampler does not compete with the frame-critical threads.
  cpu_set_t mine, other;
  CPU_ZERO(&other);
  if (sched_getaffinity(0, sizeof mine, &mine) == 0) {
    long n = sysconf(_SC_NPROCESSORS_CONF);
    for (long c = 0; c < n && c < CPU_SETSIZE; c++)
      if (!CPU_ISSET(c, &mine)) CPU_SET(c, &other);
    if (CPU_COUNT(&other)) sched_setaffinity(0, sizeof other, &other);
  }
#endif
#endif
  const auto period = std::chrono::nanoseconds(1000000000ll / s.hz);
  const uint64_t period_units = uint64_t(double(period.count()) * s.units_per_ns);
  auto next = std::chrono::steady_clock::now();
  auto next_write = next + std::chrono::seconds(5);
  for (;;) {
    next += period;
    auto now = std::chrono::steady_clock::now();
    if (next < now) next = now;  // fell behind: skip ticks rather than burst
#ifdef _WIN32
    if (timer) {
      LARGE_INTEGER due;
      due.QuadPart = -std::max<long long>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(next - now).count() / 100);
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, INFINITE);
    } else {
      std::this_thread::sleep_until(next);
    }
#else
    std::this_thread::sleep_until(next);
#endif
    sample(s, period_units);
    if (std::chrono::steady_clock::now() >= next_write) {
      write_file(s);
      next_write += std::chrono::seconds(5);
    }
  }
}

}  // namespace

bool enabled() {
  static const bool on = [] {
    const char* e = std::getenv("DCP_GUEST_PROF");
    return e && *e == '1';
  }();
  return on;
}

void set_default_output(const std::string& path) {
  std::lock_guard lk(st().mu);
  st().out = path;
}

void set_import_names(std::function<std::string(uint32_t)> fn) {
  std::lock_guard lk(st().mu);
  st().import_name = std::move(fn);
}

void register_thread(const std::string& name, Cpu& cpu, Memory& mem) {
  if (!enabled()) return;
  State& s = st();
  auto* r = new Rec;
  r->name = clean(name);
#ifdef _WIN32
  r->thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
  r->has_clock = r->thread != nullptr;
#else
  r->has_clock = pthread_getcpuclockid(pthread_self(), &r->clock) == 0;
#ifdef __linux__
  char path[64];
  std::snprintf(path, sizeof path, "/proc/self/task/%d/stat", int(gettid()));
  r->stat_fd = open(path, O_RDONLY | O_CLOEXEC);
#endif
#endif
  bool start = false;
  {
    std::lock_guard lk(s.mu);
    if (!s.area) {
      s.mem = &mem;
      s.area = mem.alloc(SLOT_AREA, "guest profiler slots");
      for (uint32_t off = SLOT_AREA; s.area && off >= SLOT_STRIDE; off -= SLOT_STRIDE)
        s.free_slots.push_back(s.area + off - SLOT_STRIDE);
    }
    if (s.free_slots.empty()) {  // more live threads than slots: this one is not profiled
      delete r;
      return;
    }
    r->slot = s.free_slots.back();
    r->serial = s.next_serial++;
    s.free_slots.pop_back();
    mem.write<uint32_t>(r->slot, 0);
    s.live.push_back(r);
    if (!s.started) {
      s.started = start = true;
      if (const char* e = std::getenv("DCP_GUEST_PROF_OUT")) s.out = e;
      if (const char* e = std::getenv("DCP_GUEST_PROF_HZ")) s.hz = std::clamp(unsigned(std::atoi(e)), 10u, 20000u);
      s.t0 = std::chrono::steady_clock::now();
    }
  }
  if (holder.rec) holder.rec->dead.store(true);  // a thread adopted twice: the newer record counts
  holder.rec = r;
  cpu.prof_slot = r->slot;
  if (start) {
    std::fprintf(stderr, "[prof] guest profiler on: %u Hz -> %s (rewritten every 5 s)\n", s.hz, s.out.c_str());
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "OpenCrown", "[prof] guest profiler on: %u Hz -> %s", s.hz, s.out.c_str());
#endif
    std::thread(sampler).detach();
  }
}

std::string recent_hot(uint64_t since_ns, size_t top) {
  Rec* me = holder.rec;
  if (!enabled() || !me) return {};
  State& s = st();
  std::unordered_map<uint64_t, uint32_t> n;
  {
    std::lock_guard lk(s.mu);
    if (s.recent.empty()) return {};
    // Newest first, until older than `since_ns` (ring order = time order).
    for (size_t k = 0; k < RECENT; k++) {
      const Recent& e = s.recent[(s.recent_next + RECENT - 1 - k) % RECENT];
      if (!e.t_ns || e.t_ns < since_ns) break;
      if (e.serial == me->serial) n[e.key]++;
    }
  }
  std::vector<std::pair<uint64_t, uint32_t>> v(n.begin(), n.end());
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  std::string out;
  char b[64];
  for (size_t j = 0; j < v.size() && j < top; j++) {
    if (v[j].first & IMPORT_KEY) {
      std::string name = s.import_name ? s.import_name(uint32_t(v[j].first)) : std::to_string(uint32_t(v[j].first));
      if (size_t sp = name.rfind(' '); sp != std::string::npos) name = name.substr(sp + 1);
      out += (j ? ", " : "") + name;
      std::snprintf(b, sizeof b, " x%u", v[j].second);
    } else {
      std::snprintf(b, sizeof b, "%s%08x x%u", j ? ", " : "", uint32_t(v[j].first) & ~1u, v[j].second);
    }
    out += b;
  }
  return out;
}

Scope::Scope(int32_t imm) : rec_(holder.rec), prev_(-1) {
  if (Rec* r = static_cast<Rec*>(rec_)) prev_ = r->import.exchange(imm, std::memory_order_relaxed);
}

Scope::~Scope() {
  if (Rec* r = static_cast<Rec*>(rec_)) r->import.store(prev_, std::memory_order_relaxed);
}

}  // namespace guest::prof
