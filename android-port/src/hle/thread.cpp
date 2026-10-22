// SceLibKernel / SceThreadmgr threads: create, start, exit (entry return), wait-end, delete, delay, id, priority and
// affinity. Priorities and affinity masks are stored and reported back but not applied to host threads yet.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#ifndef _WIN32
#include <pthread.h>
#include <time.h>
#endif
#ifdef __linux__
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include <fstream>
#endif

#include <dynarmic/interface/exclusive_monitor.h>

#include "hle/kobj.h"
#include "hle/threads.h"
#include "guest/guest_prof.h"
#include "guest/hle.h"

namespace hle {
namespace {

constexpr uint32_t TLS_SIZE = 0x1000;

struct Thread {
  uint32_t uid = 0;
  std::string name;
  uint32_t entry = 0, priority = 0, stack_size = 0, attr = 0, affinity = 0;
  uint32_t stack = 0, tls = 0;
  enum State { DORMANT, RUNNING, DEAD } state = DORMANT;
  std::thread::id host_id;  // the host thread running it (import statistics)
  uint32_t exit_status = 0;
  std::unique_ptr<guest::Cpu> cpu;
  int slot = -1;  // exclusive monitor processor id held by `cpu`
  std::thread host;
  std::condition_variable ended;
  std::atomic<uint64_t> vblank_us{0};  // frame timing (thread_times)
  uint64_t cpu_seen_us = 0;
  uint64_t jit_seen_blocks = 0, jit_seen_insts = 0;  // translations reported so far (thread_perf_report)
#ifndef _WIN32
  clockid_t clock{};
  bool has_clock = false;
#endif
  ~Thread();
};

Process proc;
std::map<uint32_t, std::unique_ptr<Thread>> threads;  // guarded by kernel_lock()
thread_local Thread* self = nullptr;
Thread* main_thread = nullptr;

// sceKernelStartThread split timer ([timing] "thread starts" line), kernel lock: calls, whole call, make_core part.
struct StartStats { uint32_t n = 0; uint64_t ns = 0, core_ns = 0, max = 0, core_max = 0; } starts;

uint64_t now_ns() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}

// JIT code cache of a guest thread's core: 64 MiB. DCP_CODE_CACHE_MB=n (4-128) sets it for every core but the main
// thread's, for an A/B of the core build cost (Dynarmic's arm64 prelude invalidates the whole cache: R2 in
// notes/android_perf_strategy.md); their call() child cores take min(16 MiB, that).
size_t code_cache_size(bool main) {
  static const size_t env = [] {
    const char* e = std::getenv("DCP_CODE_CACHE_MB");
    long v = e ? std::atol(e) : 0;
    if (v <= 0) return size_t(0);
    v = std::clamp(v, 4L, 128L);
    std::fprintf(stderr, "[threads] code cache %ld MiB for every core but the main thread's (DCP_CODE_CACHE_MB)\n", v);
    return size_t(v) << 20;
  }();
  return main || !env ? size_t(64) << 20 : env;
}

#ifdef __linux__
// Big.LITTLE hosts (most phones): the cores with the highest cpufreq maximum, and the rest. Empty when every core is
// the same (desktop Linux, homogeneous SoCs): then nothing is pinned.
struct CoreSets { cpu_set_t big, rest; bool split = false; };
const CoreSets& core_sets() {
  static const CoreSets s = [] {
    CoreSets c;
    CPU_ZERO(&c.big);
    CPU_ZERO(&c.rest);
    std::vector<long> f;
    // cpu_capacity (the scheduler's per-core performance) first: max clocks can be equal across clusters (Unisoc
    // T618: A75 and A55 both 2.0 GHz). cpufreq maximum where the kernel has no capacity entry.
    for (const char* file : {"/cpu_capacity", "/cpufreq/cpuinfo_max_freq"}) {
      for (int i = 0; i < CPU_SETSIZE; i++) {
        std::ifstream in("/sys/devices/system/cpu/cpu" + std::to_string(i) + file);
        long v = 0;
        if (!(in >> v)) break;
        f.push_back(v);
      }
      if (!f.empty()) break;
    }
    if (f.empty()) return c;
    long top = *std::max_element(f.begin(), f.end());
    int nbig = 0;
    for (size_t i = 0; i < f.size(); i++) {
      if (f[i] == top) {
        CPU_SET(i, &c.big);
        nbig++;
      } else {
        CPU_SET(i, &c.rest);
      }
    }
    c.split = nbig < int(f.size());
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "OpenCrown", "[threads] %zu cores, %d fast%s", f.size(), nbig,
                        c.split ? ": main thread + render thread pinned to them" : ", no pinning");
#endif
    std::fprintf(stderr, "[threads] %zu cores, %d fast (capacity / kHz %ld)%s\n", f.size(), nbig, top,
                 c.split ? ": main thread + render thread pinned to them" : ", no pinning");
    return c;
  }();
  return s;
}

// The game's two frame-critical threads (they set the frame rate) get the fast cores to themselves; every other
// guest thread (CRI file / audio servers, workers) runs on the remaining ones.
void place(const std::string& name) {
  const CoreSets& s = core_sets();
  if (!s.split) return;
  bool critical = name == "main thread" || name == "RenderDLThread";
  sched_setaffinity(0, sizeof(cpu_set_t), critical ? &s.big : &s.rest);
}
#endif

// The calling host thread becomes `t`: named after it (top / simpleperf show guest names) and its CPU clock noted.
void enter(Thread* t) {
  self = t;
  t->host_id = std::this_thread::get_id();
#ifndef _WIN32
  pthread_setname_np(pthread_self(), t->name.substr(0, 15).c_str());
  t->has_clock = pthread_getcpuclockid(pthread_self(), &t->clock) == 0;
#endif
#ifdef __linux__
  place(t->name);
#endif
  guest::prof::register_thread(t->name, *t->cpu, *proc.mem);
}

// Exclusive monitor processor ids: handed out when a thread gets its core and returned when the thread is deleted,
// so live cores never share a reservation slot (the game creates many short-lived fiber threads over a session).
std::mutex slot_lock;
std::vector<bool> slot_used(MAX_CORES);

void make_core(Thread& t, bool main = false) {
  const size_t cache = code_cache_size(main);
  int core = -1;
  {
    std::lock_guard lk(slot_lock);
    for (size_t i = 0; i < MAX_CORES && core < 0; i++)
      if (!slot_used[i]) slot_used[i] = true, core = int(i);
  }
  if (core < 0) {  // more than MAX_CORES live threads: share one (ldrex/strex between the two are not exclusive)
    static std::atomic<uint32_t> shared{0};
    std::fprintf(stderr, "[thread] more than %zu live guest cores; exclusive monitor slots are shared\n", MAX_CORES);
    t.cpu = std::make_unique<guest::Cpu>(*proc.mem, *proc.monitor, shared++ % MAX_CORES, proc.svc, cache);
    return;
  }
  proc.monitor->ClearProcessor(core);  // no reservation left over from the slot's previous thread
  t.slot = core;
  t.cpu = std::make_unique<guest::Cpu>(*proc.mem, *proc.monitor, core, proc.svc, cache);
}

Thread::~Thread() {
  if (host.joinable()) host.join();
  cpu.reset();
  if (slot >= 0) {
    std::lock_guard lk(slot_lock);
    slot_used[slot] = false;
  }
}

Thread* find(uint32_t uid) {  // kernel lock held; uid 0 = the calling thread
  if (uid == 0) return self;
  auto it = threads.find(uid);
  return it == threads.end() ? nullptr : it->second.get();
}

void print_regs(guest::Cpu& c) {
  std::fprintf(stderr, "      pc=%08x lr=%08x sp=%08x cpsr=%08x r0=%08x r1=%08x r2=%08x r3=%08x\n", c.pc(), c.reg(14),
               c.reg(13), c.cpsr(), c.reg(0), c.reg(1), c.reg(2), c.reg(3));
  std::fprintf(stderr, "      r4=%08x r5=%08x r6=%08x r7=%08x r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x\n", c.reg(4),
               c.reg(5), c.reg(6), c.reg(7), c.reg(8), c.reg(9), c.reg(10), c.reg(11), c.reg(12));
}

// Runs a started thread's core until its entry returns (or it faults).
void run(Thread* t) {
  enter(t);
  if (!t->cpu->run() || !t->cpu->exited) die(*t->cpu, "thread \"" + t->name + "\"");
  std::lock_guard lk(kernel_lock());
  t->exit_status = t->cpu->exit_code;
  t->state = Thread::DEAD;
  t->ended.notify_all();
}

// --- imports ---------------------------------------------------------------------------------------------------
// SceUID sceKernelCreateThread(name, entry, initPriority, stackSize, attr, cpuAffinityMask, option*)
void CreateThread(Cpu& c) {
  auto t = std::make_unique<Thread>();
  t->name = str(c, arg(c, 0), 32);
  t->entry = arg(c, 1);
  t->priority = arg(c, 2);
  t->stack_size = (std::max<uint32_t>(arg(c, 3), 0x1000) + 0xfff) & ~0xfffu;
  t->attr = arg(c, 4);
  t->affinity = arg(c, 5);
  t->stack = proc.mem->alloc(t->stack_size, "thread stack");
  t->tls = proc.mem->alloc(TLS_SIZE, "thread TLS");
  if (!t->stack || !t->tls) return ret(c, SCE_KERNEL_ERROR_NO_MEMORY);
  std::lock_guard lk(kernel_lock());
  t->uid = new_uid();
  uint32_t uid = t->uid;
  HLE_NOTE("thread \"%s\" created: uid 0x%08x entry 0x%08x prio 0x%x stack 0x%x affinity 0x%x\n", t->name.c_str(), uid,
           t->entry, t->priority, t->stack_size, t->affinity);
  threads[uid] = std::move(t);
  ret(c, uid);
}

// int sceKernelStartThread(uid, argSize, argBlock*): argBlock is copied to the top of the new stack and passed as
// (argSize, pointer to the copy).
void StartThread(Cpu& c) {
  uint32_t uid = arg(c, 0), size = arg(c, 1), block = arg(c, 2);
  std::lock_guard lk(kernel_lock());
  uint64_t t0 = now_ns();
  Thread* t = find(uid);
  if (!t) return ret(c, SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
  if (t->state != Thread::DORMANT) return ret(c, SCE_KERNEL_ERROR_NOT_DORMANT);
  uint32_t sp = t->stack + t->stack_size, argp = 0;
  if (block && size) {
    sp = (sp - size) & ~7u;
    if (!c.mem().valid(block, size)) return ret(c, SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    std::memmove(c.mem().ptr(sp), c.mem().ptr(block), size);
    argp = sp;
  }
  uint64_t t1 = now_ns();
  make_core(*t);
  uint64_t core_ns = now_ns() - t1;
  guest::Cpu& k = *t->cpu;
  k.tpidruro = t->tls;
  k.set_reg(0, size);
  k.set_reg(1, argp);
  k.set_reg(13, sp & ~7u);
  k.set_reg(14, guest::Cpu::call_return - 8);  // the SVC_THREAD_RETURN trampoline sits just before call_return
  k.set_pc(t->entry);
  t->state = Thread::RUNNING;
  if (t->host.joinable()) t->host.join();
  t->host = std::thread(run, t);
  uint64_t ns = now_ns() - t0;
  starts.n++, starts.ns += ns, starts.core_ns += core_ns;
  starts.max = std::max(starts.max, ns), starts.core_max = std::max(starts.core_max, core_ns);
  ret(c, 0);
}

// int sceKernelWaitThreadEnd(uid, int* exitStatus, SceUInt* timeout) and the CB variant (no callbacks exist).
void WaitThreadEnd(Cpu& c) {
  uint32_t uid = arg(c, 0), stat = arg(c, 1), timeout = arg(c, 2);
  std::unique_lock lk(kernel_lock());
  Thread* t = find(uid);
  if (!t) return ret(c, SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
  if (t->state == Thread::DORMANT) return ret(c, SCE_KERNEL_ERROR_DORMANT);
  uint32_t r = wait_until(c, lk, t->ended, timeout, [&] { return t->state == Thread::DEAD; });
  if (r == 0 && stat) c.mem().write<uint32_t>(stat, t->exit_status);
  ret(c, r);
}

// int sceKernelGetThreadExitStatus(uid, int* status): the exit status of an ended thread; NOT_DORMANT while it runs.
void GetThreadExitStatus(Cpu& c) {
  std::lock_guard lk(kernel_lock());
  Thread* t = find(arg(c, 0));
  if (!t) return ret(c, SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
  if (t->state == Thread::RUNNING) return ret(c, SCE_KERNEL_ERROR_NOT_DORMANT);
  if (arg(c, 1)) c.mem().write<uint32_t>(arg(c, 1), t->exit_status);
  ret(c, 0);
}

// int sceKernelDeleteThread(uid): only dormant or ended threads.
void DeleteThread(Cpu& c) {
  std::unique_lock lk(kernel_lock());
  auto it = threads.find(arg(c, 0));
  if (it == threads.end()) return ret(c, SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
  if (it->second->state == Thread::RUNNING) return ret(c, SCE_KERNEL_ERROR_NOT_DORMANT);
  std::unique_ptr<Thread> t = std::move(it->second);
  threads.erase(it);
  lk.unlock();
  if (t->host.joinable()) t->host.join();
  ret(c, 0);
}

void GetThreadId(Cpu& c) { ret(c, current_thread_uid()); }

// int sceKernelDelayThread(SceUInt usec) and the CB variant.
void DelayThread(Cpu& c) {
  std::this_thread::sleep_for(std::chrono::microseconds(arg(c, 0)));
  ret(c, 0);
}

// int sceKernelChangeThreadPriority(uid, priority)
void ChangeThreadPriority(Cpu& c) {
  std::lock_guard lk(kernel_lock());
  Thread* t = find(arg(c, 0));
  if (!t) return ret(c, SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
  t->priority = arg(c, 1);
  ret(c, 0);
}

// int sceKernelChangeThreadCpuAffinityMask(uid, mask)
void ChangeThreadCpuAffinityMask(Cpu& c) {
  std::lock_guard lk(kernel_lock());
  Thread* t = find(arg(c, 0));
  if (!t) return ret(c, SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
  t->affinity = arg(c, 1);
  ret(c, 0);
}


}  // namespace

// --- shared kernel state -------------------------------------------------------------------------------------------
std::mutex& kernel_lock() {
  static std::mutex m;
  // DCP_LOCK_STATS: probe for contention and time the wait; the caller then takes the lock as usual (a thread that
  // slips in between the probe and that lock is not counted).
  if (lock_stats_on()) {
    if (m.try_lock()) {
      m.unlock();
    } else {
      uint64_t t0 = now_ns();
      m.lock();
      m.unlock();
      note_kernel_lock_wait(now_ns() - t0, is_main_thread());
    }
  }
  return m;
}

bool is_main_thread() { return self && self == main_thread; }

bool probe_thread(uint32_t uid, ThreadProbe& p) {
  auto it = uid ? threads.find(uid) : threads.end();
  if (it == threads.end()) return false;
  Thread& t = *it->second;
  p.name = t.name;
  if (t.state != Thread::RUNNING) return true;
#ifndef _WIN32
  timespec ts{};
  if (t.has_clock && clock_gettime(t.clock, &ts) == 0) p.cpu_ns = uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#endif
  p.import = guest::svc_current_import(t.host_id, &p.blocking);
  return true;
}

bool is_fast_core(int cpu) {
#ifdef __linux__
  const CoreSets& s = core_sets();
  return s.split && cpu >= 0 && cpu < CPU_SETSIZE && CPU_ISSET(cpu, &s.big);
#else
  (void)cpu;
  return false;
#endif
}

uint32_t new_uid() {
  static std::atomic<uint32_t> next{0x40010001};
  return next += 2;
}

uint32_t current_thread_uid() { return self ? self->uid : 0; }

void init_threads(const Process& p) { proc = p; }

void die(guest::Cpu& c, const std::string& where) {
  static std::mutex m;
  std::lock_guard lk(m);
  std::fflush(stdout);
  std::fprintf(stderr, "[run] stopped in %s: %s\n", where.c_str(), c.fault().empty() ? "halted" : c.fault().c_str());
  print_regs(c);
  // The stack around sp (saved registers of the frames being returned through).
  uint32_t sp = c.reg(13);
  for (uint32_t a = (sp & ~15u) - 0x40; a < (sp & ~15u) + 0x40; a += 16)
    if (c.mem().valid(a, 16))
      std::fprintf(stderr, "      %08x: %08x %08x %08x %08x\n", a, c.mem().read<uint32_t>(a),
                   c.mem().read<uint32_t>(a + 4), c.mem().read<uint32_t>(a + 8), c.mem().read<uint32_t>(a + 12));
  // What the registers point at (hex + text), for registers holding mapped addresses.
  for (int r = 0; r <= 12; r++) {
    uint32_t a = c.reg(r);
    if (a < 0x81000000 || !c.mem().valid(a, 24)) continue;
    char hex[80] = {}, txt[25] = {};
    for (int i = 0; i < 24; i++) {
      uint8_t b = c.mem().read<uint8_t>(a + i);
      std::snprintf(hex + 3 * i, 4, "%02x ", b);
      txt[i] = b >= 32 && b < 127 ? char(b) : '.';
    }
    std::fprintf(stderr, "      [r%d %08x] %s %s\n", r, a, hex, txt);
  }
  std::fflush(stderr);
  std::_Exit(3);
}

uint32_t run_main_thread(uint32_t entry, uint32_t stack_size, uint32_t r0, uint32_t r1) {
  auto t = std::make_unique<Thread>();
  t->name = "main thread";
  t->entry = entry;
  t->stack_size = stack_size;
  t->stack = proc.mem->alloc(stack_size, "main thread stack");
  t->tls = proc.mem->alloc(TLS_SIZE, "main thread TLS");
  if (!t->stack || !t->tls) {
    std::fprintf(stderr, "[run] cannot allocate the main thread\n");
    std::_Exit(1);
  }
  make_core(*t, true);
  main_thread = t.get();
  guest::Cpu& k = *t->cpu;
  k.tpidruro = t->tls;
  k.set_reg(0, r0);
  k.set_reg(1, r1);
  k.set_reg(13, (t->stack + stack_size) & ~7u);
  k.set_reg(14, guest::Cpu::call_return - 8);
  k.set_pc(entry);
  t->state = Thread::RUNNING;
  Thread* tp = t.get();
  {
    std::lock_guard lk(kernel_lock());
    t->uid = new_uid();
    threads[t->uid] = std::move(t);
  }
  std::printf("[run] main thread: uid 0x%08x stack 0x%08x (+0x%x), TLS 0x%08x, entry 0x%08x\n", tp->uid, tp->stack,
              stack_size, tp->tls, entry);
  enter(tp);
  if (!k.run() || !k.exited) die(k, "main thread");
  return k.exit_code;
}

const std::string& current_thread_name() {
  static const std::string none = "?";
  return self ? self->name : none;
}

void note_vblank_wait(uint64_t us) {
  if (self) self->vblank_us += us;
}

std::string thread_times(double frames) {
  struct Row { std::string name; double cpu, vblank; std::thread::id id; };
  std::vector<Row> rows;
  {
    std::lock_guard lk(kernel_lock());
    for (auto& [uid, t] : threads) {
      if (t->state != Thread::RUNNING) continue;
      uint64_t cpu = 0;
#ifndef _WIN32
      timespec ts{};
      if (t->has_clock && clock_gettime(t->clock, &ts) == 0) cpu = uint64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
#endif
      uint64_t d = cpu - std::min(cpu, t->cpu_seen_us);
      t->cpu_seen_us = cpu;
      rows.push_back({t->name, d / frames / 1000.0, t->vblank_us.exchange(0) / frames / 1000.0, t->host_id});
    }
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.cpu + a.vblank > b.cpu + b.vblank; });
  std::string out;
  char buf[96];
  for (size_t i = 0; i < rows.size() && i < 6; i++) {
    std::snprintf(buf, sizeof buf, "  %-16s cpu %5.2f vblank %5.2f ms/frame", rows[i].name.c_str(), rows[i].cpu,
                  rows[i].vblank);
    out += buf;
    std::string calls = guest::svc_report_for(rows[i].id, frames);
    if (!calls.empty()) out += " | " + calls;
    out += "\n";
  }
  return out;
}

// Current clock of every core (scaling_cur_freq, Linux / Android where readable), summed per sample.
namespace {
struct ClockSamples {
  std::mutex mu;
  std::vector<int> fds;  // per cpu, -1 = not readable
  std::vector<uint64_t> sum_khz, min_khz;
  std::vector<uint32_t> n;
  bool opened = false;
} clocks;
}  // namespace

void sample_cpu_clocks() {
#ifdef __linux__
  std::lock_guard lk(clocks.mu);
  if (!clocks.opened) {
    clocks.opened = true;
    long ncpu = sysconf(_SC_NPROCESSORS_CONF);
    for (long i = 0; i < ncpu && i < 64; i++) {
      std::string p = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_cur_freq";
      clocks.fds.push_back(open(p.c_str(), O_RDONLY | O_CLOEXEC));
    }
    clocks.sum_khz.assign(clocks.fds.size(), 0);
    clocks.min_khz.assign(clocks.fds.size(), ~0ull);
    clocks.n.assign(clocks.fds.size(), 0);
  }
  for (size_t i = 0; i < clocks.fds.size(); i++) {
    char b[32];
    ssize_t r = clocks.fds[i] >= 0 ? pread(clocks.fds[i], b, sizeof b - 1, 0) : -1;
    if (r <= 0) continue;  // offline core or no permission: skipped
    b[r] = 0;
    uint64_t khz = std::strtoull(b, nullptr, 10);
    clocks.sum_khz[i] += khz, clocks.n[i]++;
    clocks.min_khz[i] = std::min(clocks.min_khz[i], khz);
  }
#endif
}

std::string thread_perf_report(double frames) {
  std::string out;
  char b[192];
  // JIT translations since the last report, per thread name (fibers share one); cores gone since then = "ended".
  struct Jit { uint64_t blocks = 0, insts = 0; };
  std::map<std::string, Jit> by_name;
  StartStats st;
  uint64_t live_blocks = 0, live_insts = 0;
  {
    std::lock_guard lk(kernel_lock());
    for (auto& [uid, t] : threads) {
      if (!t->cpu) continue;
      uint64_t bl = t->cpu->jit_counts().blocks.load(std::memory_order_relaxed);
      uint64_t in = t->cpu->jit_counts().insts.load(std::memory_order_relaxed);
      Jit& j = by_name[t->name];
      j.blocks += bl - t->jit_seen_blocks, j.insts += in - t->jit_seen_insts;
      live_blocks += bl - t->jit_seen_blocks, live_insts += in - t->jit_seen_insts;
      t->jit_seen_blocks = bl, t->jit_seen_insts = in;
    }
    st = starts;
    starts = {};
  }
  static uint64_t seen_blocks = 0, seen_insts = 0;  // only the timing log thread calls this
  uint64_t all_blocks = guest::Cpu::jit_totals().blocks.load(std::memory_order_relaxed);
  uint64_t all_insts = guest::Cpu::jit_totals().insts.load(std::memory_order_relaxed);
  uint64_t d_blocks = all_blocks - seen_blocks, d_insts = all_insts - seen_insts;
  seen_blocks = all_blocks, seen_insts = all_insts;
  if (d_blocks) {
    std::vector<std::pair<std::string, Jit>> v(by_name.begin(), by_name.end());
    std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.second.blocks > y.second.blocks; });
    std::snprintf(b, sizeof b, "  jit (this window): %llu blocks, %.1fk guest insts translated (%.1f blocks/frame):",
                  (unsigned long long)d_blocks, d_insts / 1e3, d_blocks / frames);
    out += b;
    for (size_t i = 0, k = 0; i < v.size() && k < 5; i++) {
      if (!v[i].second.blocks) continue;
      std::snprintf(b, sizeof b, "%s %s %llu (%.1fk)", k++ ? "," : "", v[i].first.c_str(),
                    (unsigned long long)v[i].second.blocks, v[i].second.insts / 1e3);
      out += b;
    }
    if (d_blocks > live_blocks) {
      std::snprintf(b, sizeof b, ", ended threads %llu (%.1fk)", (unsigned long long)(d_blocks - live_blocks),
                    (d_insts - std::min(d_insts, live_insts)) / 1e3);
      out += b;
    }
    out += "\n";
  }
  // Thread starts: the whole sceKernelStartThread (under kernel_lock()) and its core build; all core builds by size.
  std::string builds = guest::Cpu::jit_build_report();
  if (st.n || !builds.empty()) {
    out += "  thread starts:";
    if (st.n) {
      std::snprintf(b, sizeof b, " %ux sceKernelStartThread %.2f ms avg (max %.2f), of it make_core %.2f (max %.2f), rest %.2f;",
                    st.n, st.ns / 1e6 / st.n, st.max / 1e6, st.core_ns / 1e6 / st.n, st.core_max / 1e6,
                    (st.ns - st.core_ns) / 1e6 / st.n);
      out += b;
    }
    if (!builds.empty()) out += " Dynarmic Jit constructor by code cache: " + builds;
    out += "\n";
  }
#ifdef __linux__
  {
    std::lock_guard lk(clocks.mu);
    std::string line;
    uint32_t samples = 0;
    for (size_t i = 0; i < clocks.n.size(); i++) {
      if (!clocks.n[i]) {
        line += " -";
        continue;
      }
      samples = std::max(samples, clocks.n[i]);
      bool fast = is_fast_core(int(i));
      std::snprintf(b, sizeof b, " %s%.0f", fast ? "*" : "", clocks.sum_khz[i] / 1e3 / clocks.n[i]);
      line += b;
      if (fast) {
        std::snprintf(b, sizeof b, "(min %.0f)", clocks.min_khz[i] / 1e3);
        line += b;
      }
      clocks.sum_khz[i] = 0, clocks.n[i] = 0, clocks.min_khz[i] = ~0ull;
    }
    if (samples) {
      std::snprintf(b, sizeof b, "  cpu clocks MHz (avg of %u samples, cpu0 first, * = fast core):", samples);
      out += b + line + "\n";
    }
  }
#endif
  return out;
}

guest::Cpu& adopt_host_thread(const std::string& name) {
  auto t = std::make_unique<Thread>();
  t->name = name;
  t->tls = proc.mem->alloc(TLS_SIZE, "thread TLS");
  make_core(*t);
  t->cpu->tpidruro = t->tls;
  t->state = Thread::RUNNING;
  Thread* tp = t.get();
  {
    std::lock_guard lk(kernel_lock());
    t->uid = new_uid();
    threads[t->uid] = std::move(t);
  }
  enter(tp);
  return *tp->cpu;
}

// sceKernelExitThread(status): ends the calling thread as if its entry returned `status`.
void ExitThread(Cpu& c) {
  c.exited = true;
  c.exit_code = arg(c, 0);
  c.stop();
}

void register_threads() {
  REG(sceKernelExitThread, ExitThread);
  REG(sceKernelCreateThread, CreateThread);
  REG(sceKernelStartThread, StartThread);
  REG(sceKernelWaitThreadEnd, WaitThreadEnd);
  REG(sceKernelWaitThreadEndCB, WaitThreadEnd);
  REG(sceKernelDeleteThread, DeleteThread);
  REG(sceKernelGetThreadId, GetThreadId);
  REG(sceKernelDelayThread, DelayThread);
  REG(sceKernelDelayThreadCB, DelayThread);
  REG(sceKernelChangeThreadPriority, ChangeThreadPriority);
  REG(sceKernelChangeThreadCpuAffinityMask, ChangeThreadCpuAffinityMask);
  REG(sceKernelGetThreadExitStatus, GetThreadExitStatus);
}

}  // namespace hle
