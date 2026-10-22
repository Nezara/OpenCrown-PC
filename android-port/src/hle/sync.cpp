// SceLibKernel / SceThreadmgr sync objects with real blocking: mutexes, light-weight mutexes and
// condition variables, event flags. DCP_LOCK_STATS=1 adds a contended-lock report to the [timing] block.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#ifdef __linux__
#include <sched.h>
#endif

#include "hle/kobj.h"
#include "hle/threads.h"

namespace hle {
namespace {

constexpr uint32_t MUTEX_ATTR_RECURSIVE = 0x02;
constexpr uint32_t EVF_ATTR_MULTI = 0x1000;  // SCE_EVENT_WAITMULTIPLE
constexpr uint32_t EVF_WAITOR = 1, EVF_WAITCLEAR = 2, EVF_WAITCLEAR_PAT = 4;

struct Mutex {
  std::string name;
  uint32_t attr = 0;
  bool lw = false;
  uint32_t owner = 0;
  int32_t count = 0;
  bool deleted = false;
  std::condition_variable cv;
  // DCP_LOCK_STATS bookkeeping (lock_stats_report)
  uint32_t work = 0;      // lw: the guest work area
  int waiters = 0;        // threads blocked in lock() or the WaitLwCond re-lock
  bool notified = false;  // an unlock woke a waiter that has not looked at the mutex yet
  uint64_t held_since = 0;
  int owner_cpu = -1;  // host core the owner was on when it took the mutex (Linux)
};
struct EventFlag {
  std::string name;
  uint32_t attr = 0, pattern = 0;
  int waiters = 0;
  bool deleted = false;
  std::condition_variable cv;
};
std::map<uint32_t, std::shared_ptr<Mutex>> mutexes;  // kernel_lock()
std::map<uint32_t, std::shared_ptr<EventFlag>> flags;

// --- DCP_LOCK_STATS=1: contention report --------------------------------------------------------------------------
// Per mutex and log window: every thread's blocking waits and the holds; for the main thread's waits also who owned
// the mutex when the wait began, what that owner was doing then (guest code / which import), how much CPU it got
// during the wait (POSIX thread clock: "ran" near 100% = it was running, low = runnable but not scheduled, or
// blocked), the host core it took the mutex on, and the guest caller. "lost" = a waiter woken by an unlock found the
// mutex taken again; "barged" = a thread took a free mutex while a woken waiter had not run yet.
struct LockStats {
  std::string name;
  uint32_t work = 0;
  uint64_t waits = 0, wait_ns = 0, main_waits = 0, main_ns = 0, main_max = 0;
  uint64_t holds = 0, hold_ns = 0, hold_max = 0;
  std::string hold_max_by;
  uint64_t lost = 0, barged = 0;
  uint64_t owner_cpu_ns = 0, owner_wall_ns = 0;  // main's waits whose owner has a CPU clock
  uint32_t owner_fast = 0, owner_slow = 0;
  std::map<std::string, uint32_t> by_waiter, owners, owner_in, bargers;
  std::map<uint32_t, uint32_t> callers;
};
std::map<uint32_t, LockStats> lock_stats;  // kernel_lock(); by mutex uid, emptied by each report
thread_local int my_held = 0;              // guest mutexes the calling thread owns
thread_local std::string my_held_name;     // the last one it took

// kernel_lock() and ngs_lock waits, any thread. wait_stats_lock is a leaf: nothing else is taken under it.
struct WaitStats {
  uint64_t n = 0, ns = 0, max = 0;
  void add(uint64_t t) { n++, ns += t, max = std::max(max, t); }
};
std::mutex wait_stats_lock;
WaitStats kl_main, kl_other, ngs_all, ngs_holding;
std::string ngs_max_by;

uint64_t now_ns() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}

int this_cpu() {
#ifdef __linux__
  return sched_getcpu();
#else
  return -1;
#endif
}

LockStats& stats_for(uint32_t uid, const Mutex& m) {  // kernel lock held
  LockStats& s = lock_stats[uid];
  if (s.name.empty()) s.name = m.name.empty() ? "?" : m.name, s.work = m.work;
  return s;
}

// The calling thread became the owner of `m` (kernel lock held); `waited` = it blocked for it first.
void took(uint32_t uid, Mutex& m, bool waited) {
  if (!lock_stats_on()) return;
  m.held_since = now_ns();
  m.owner_cpu = this_cpu();
  my_held++, my_held_name = m.name;
  if (!waited && m.notified && m.waiters > 0) {
    LockStats& s = stats_for(uid, m);
    s.barged++, s.bargers[current_thread_name()]++;
  }
}

// The calling thread let go of `m` completely (kernel lock held), right before the notify.
void released(uint32_t uid, Mutex& m) {
  if (!lock_stats_on()) return;
  if (my_held > 0) my_held--;
  if (m.waiters > 0) m.notified = true;
  if (!m.held_since) return;
  uint64_t h = now_ns() - m.held_since;
  m.held_since = 0;
  LockStats& s = stats_for(uid, m);
  s.holds++, s.hold_ns += h;
  if (h > s.hold_max) s.hold_max = h, s.hold_max_by = current_thread_name();
}

// Waits until `m` is free or deleted (kernel lock held through `lk`); returns 0 or wait_until's error. *waited = it
// had to block (DCP_LOCK_STATS only; such a wait is recorded).
uint32_t wait_free(Cpu& c, std::unique_lock<std::mutex>& lk, uint32_t uid, const std::shared_ptr<Mutex>& mp,
                   uint32_t timeout, bool* waited) {
  Mutex& m = *mp;
  auto is_free = [&] { return m.count == 0 || m.deleted; };
  *waited = false;
  if (!lock_stats_on() || is_free()) return wait_until(c, lk, m.cv, timeout, is_free);
  *waited = true;
  const bool main = is_main_thread();
  const uint32_t owner = m.owner;
  const uint64_t t0 = now_ns();
  ThreadProbe op;
  bool known = false;
  if (main) {
    known = probe_thread(owner, op);
    LockStats& s = stats_for(uid, m);
    s.owners[known ? op.name : "?"]++;
    s.owner_in[!known ? "?" : op.import.empty() ? "guest code" : op.blocking ? op.import + " (blocked)" : op.import]++;
    if (m.owner_cpu >= 0) (is_fast_core(m.owner_cpu) ? s.owner_fast : s.owner_slow)++;
    // criCs_Enter (0x8145f0b4) pushes {r4, r5, r6, lr}: its caller is the stacked LR (notes/android_perf_strategy.md).
    uint32_t caller = c.reg(14) & ~1u, sp = c.reg(13);
    if (m.name == "CriCsLwMutex" && c.mem().valid(sp + 12, 4)) caller = c.mem().read<uint32_t>(sp + 12) & ~1u;
    s.callers[caller]++;
  }
  m.waiters++;
  bool first = true;
  uint32_t r = wait_until(c, lk, m.cv, timeout, [&] {
    bool ok = is_free();
    if (!first && m.notified) {
      m.notified = false;
      if (!ok) stats_for(uid, m).lost++;
    }
    first = false;
    return ok;
  });
  m.waiters--;
  uint64_t dt = now_ns() - t0;
  LockStats& s = stats_for(uid, m);
  s.waits++, s.wait_ns += dt, s.by_waiter[current_thread_name()]++;
  if (main) {
    s.main_waits++, s.main_ns += dt, s.main_max = std::max(s.main_max, dt);
    ThreadProbe now;
    if (known && op.cpu_ns != ~0ull && probe_thread(owner, now) && now.cpu_ns != ~0ull && now.cpu_ns >= op.cpu_ns)
      s.owner_cpu_ns += now.cpu_ns - op.cpu_ns, s.owner_wall_ns += dt;
  }
  return r;
}

// "a 92%, b 8%" for the top `k` entries of a count map.
template <typename K, typename F>
std::string top_share(const std::map<K, uint32_t>& m, size_t k, F name) {
  std::vector<std::pair<K, uint32_t>> v(m.begin(), m.end());
  uint64_t total = 0;
  for (auto& e : v) total += e.second;
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  std::string out;
  char b[32];
  for (size_t i = 0; i < v.size() && i < k; i++) {
    std::snprintf(b, sizeof b, " %.0f%%", 100.0 * v[i].second / double(total ? total : 1));
    out += (i ? ", " : "") + name(v[i].first) + b;
  }
  if (v.size() > k) out += ", ...";
  return out;
}

uint32_t create_mutex(const std::string& name, uint32_t attr, int32_t init, bool lw, uint32_t work = 0) {
  auto m = std::make_shared<Mutex>();
  m->name = name;
  m->attr = attr;
  m->lw = lw;
  m->work = work;
  if (init > 0) m->owner = current_thread_uid(), m->count = init;
  std::lock_guard lk(kernel_lock());
  uint32_t uid = new_uid();
  mutexes[uid] = std::move(m);
  return uid;
}

uint32_t lock(Cpu& c, uint32_t uid, int32_t count, uint32_t timeout) {
  std::unique_lock lk(kernel_lock());
  auto it = mutexes.find(uid);
  if (it == mutexes.end()) return SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;
  std::shared_ptr<Mutex> m = it->second;
  bool lw = m->lw;
  if (count <= 0) count = 1;
  uint32_t me = current_thread_uid();
  if (m->count > 0 && m->owner == me) {
    if (!(m->attr & MUTEX_ATTR_RECURSIVE))
      return lw ? SCE_KERNEL_ERROR_LW_MUTEX_RECURSIVE : SCE_KERNEL_ERROR_MUTEX_RECURSIVE;
    m->count += count;
    return 0;
  }
  bool waited = false;
  uint32_t r = wait_free(c, lk, uid, m, timeout, &waited);
  if (r) return r;
  if (m->deleted) return SCE_KERNEL_ERROR_WAIT_DELETE;
  m->owner = me;
  m->count = count;
  took(uid, *m, waited);
  return 0;
}

uint32_t unlock(uint32_t uid, int32_t count) {
  std::lock_guard lk(kernel_lock());
  auto it = mutexes.find(uid);
  if (it == mutexes.end()) return SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;
  Mutex& m = *it->second;
  if (count <= 0) count = 1;
  if (m.owner != current_thread_uid() || m.count < count)
    return m.lw ? SCE_KERNEL_ERROR_LW_MUTEX_UNLOCK_UDF : SCE_KERNEL_ERROR_MUTEX_UNLOCK_UDF;
  if ((m.count -= count) == 0) {
    m.owner = 0;
    released(uid, m);
    m.cv.notify_one();
  }
  return 0;
}

uint32_t remove_mutex(uint32_t uid) {
  std::lock_guard lk(kernel_lock());
  auto it = mutexes.find(uid);
  if (it == mutexes.end()) return SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;
  it->second->deleted = true;
  it->second->cv.notify_all();
  mutexes.erase(it);
  return 0;
}

// sceKernelCreateMutex(name, attr, initCount, opt*) / LockMutex(uid, count, timeout*) / UnlockMutex(uid, count)
void CreateMutex(Cpu& c) { ret(c, create_mutex(str(c, arg(c, 0), 32), arg(c, 1), int32_t(arg(c, 2)), false)); }
void LockMutex(Cpu& c) { ret(c, lock(c, arg(c, 0), int32_t(arg(c, 1)), arg(c, 2))); }
void UnlockMutex(Cpu& c) { ret(c, unlock(arg(c, 0), int32_t(arg(c, 1)))); }
void DeleteMutex(Cpu& c) { ret(c, remove_mutex(arg(c, 0))); }

// Light-weight mutexes: 32-byte guest work area, our uid in its first word.
// sceKernelCreateLwMutex(work*, name, attr, initCount, opt*) / LockLwMutexCB(work*, count, timeout*) /
// UnlockLwMutex(work*, count) / DeleteLwMutex(work*)
void CreateLwMutex(Cpu& c) {
  uint32_t work = arg(c, 0);
  if (!c.mem().valid(work, 32)) return ret(c, SCE_KERNEL_ERROR_ILLEGAL_ADDR);
  std::memset(c.mem().ptr(work), 0, 32);
  c.mem().write<uint32_t>(work, create_mutex(str(c, arg(c, 1), 32), arg(c, 2), int32_t(arg(c, 3)), true, work));
  ret(c, 0);
}
void LockLwMutex(Cpu& c) { ret(c, lock(c, c.mem().read<uint32_t>(arg(c, 0)), int32_t(arg(c, 1)), arg(c, 2))); }
void UnlockLwMutex(Cpu& c) { ret(c, unlock(c.mem().read<uint32_t>(arg(c, 0)), int32_t(arg(c, 1)))); }
void DeleteLwMutex(Cpu& c) { ret(c, remove_mutex(c.mem().read<uint32_t>(arg(c, 0)))); }

// sceKernelTryLockMutex(uid, count): lock without waiting.
void TryLockMutex(Cpu& c) {
  std::lock_guard lk(kernel_lock());
  auto it = mutexes.find(arg(c, 0));
  if (it == mutexes.end()) return ret(c, SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID);
  Mutex& m = *it->second;
  int32_t count = int32_t(arg(c, 1));
  if (count <= 0) count = 1;
  uint32_t me = current_thread_uid();
  if (m.count > 0 && m.owner == me) {
    if (!(m.attr & MUTEX_ATTR_RECURSIVE)) return ret(c, SCE_KERNEL_ERROR_MUTEX_RECURSIVE);
    m.count += count;
    return ret(c, 0);
  }
  if (m.count > 0) return ret(c, SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN);
  m.owner = me;
  m.count = count;
  took(it->first, m, false);
  ret(c, 0);
}

// --- light-weight condition variables ---------------------------------------------------------------------------
// 16-byte guest work area, our uid in its first word. Bound to a light-weight mutex at creation; Wait releases that
// mutex completely (any recursion count), waits for a signal, then takes it back with the same count.
struct Cond {
  uint32_t lw_mutex_work = 0;
  int waiters = 0, pending = 0;
  bool deleted = false;
  std::condition_variable cv;
};
std::map<uint32_t, std::shared_ptr<Cond>> conds;  // kernel_lock()

// sceKernelCreateLwCond(work*, name, attr, lwMutexWork*, opt*)
void CreateLwCond(Cpu& c) {
  uint32_t work = arg(c, 0);
  if (!c.mem().valid(work, 16)) return ret(c, SCE_KERNEL_ERROR_ILLEGAL_ADDR);
  auto cd = std::make_shared<Cond>();
  cd->lw_mutex_work = arg(c, 3);
  std::lock_guard lk(kernel_lock());
  uint32_t uid = new_uid();
  conds[uid] = std::move(cd);
  std::memset(c.mem().ptr(work), 0, 16);
  c.mem().write<uint32_t>(work, uid);
  ret(c, 0);
}
void DeleteLwCond(Cpu& c) {
  std::lock_guard lk(kernel_lock());
  auto it = conds.find(c.mem().read<uint32_t>(arg(c, 0)));
  if (it == conds.end()) return ret(c, SCE_KERNEL_ERROR_UNKNOWN_LW_COND_ID);
  it->second->deleted = true;
  it->second->cv.notify_all();
  conds.erase(it);
  ret(c, 0);
}
void signal_cond(Cpu& c, bool all) {
  std::lock_guard lk(kernel_lock());
  auto it = conds.find(c.mem().read<uint32_t>(arg(c, 0)));
  if (it == conds.end()) return ret(c, SCE_KERNEL_ERROR_UNKNOWN_LW_COND_ID);
  Cond& cd = *it->second;
  if (all) cd.pending = cd.waiters;
  else if (cd.pending < cd.waiters) cd.pending++;
  cd.cv.notify_all();
  ret(c, 0);
}
void SignalLwCond(Cpu& c) { signal_cond(c, false); }
void SignalLwCondAll(Cpu& c) { signal_cond(c, true); }
// sceKernelWaitLwCond(work*, timeout*)
void WaitLwCond(Cpu& c) {
  uint32_t timeout = arg(c, 1);
  std::unique_lock lk(kernel_lock());
  auto it = conds.find(c.mem().read<uint32_t>(arg(c, 0)));
  if (it == conds.end()) return ret(c, SCE_KERNEL_ERROR_UNKNOWN_LW_COND_ID);
  std::shared_ptr<Cond> cd = it->second;
  auto mt = mutexes.find(c.mem().read<uint32_t>(cd->lw_mutex_work));
  if (mt == mutexes.end()) return ret(c, SCE_KERNEL_ERROR_UNKNOWN_LW_MUTEX_ID);
  std::shared_ptr<Mutex> m = mt->second;
  const uint32_t muid = mt->first;  // `mt` does not survive the wait (DeleteLwMutex erases)
  uint32_t me = current_thread_uid();
  if (m->owner != me || m->count <= 0) return ret(c, SCE_KERNEL_ERROR_LW_MUTEX_UNLOCK_UDF);
  int32_t held = m->count;
  m->count = 0;
  m->owner = 0;
  released(muid, *m);
  m->cv.notify_one();
  cd->waiters++;
  uint32_t r = wait_until(c, lk, cd->cv, timeout, [&] { return cd->deleted || cd->pending > 0; });
  cd->waiters--;
  if (!r && !cd->deleted) cd->pending--;
  if (cd->pending > cd->waiters) cd->pending = cd->waiters;
  bool waited = false;
  wait_free(c, lk, muid, m, 0, &waited);  // timeout 0 = forever: cannot fail
  if (!m->deleted) {
    m->owner = me;
    m->count = held;
    took(muid, *m, waited);
  }
  if (r) return ret(c, r);
  ret(c, cd->deleted ? uint32_t(SCE_KERNEL_ERROR_WAIT_DELETE) : 0u);
}

// --- event flags ------------------------------------------------------------------------------------------------
bool met(const EventFlag& f, uint32_t bits, uint32_t mode) {
  return (mode & EVF_WAITOR) ? (f.pattern & bits) != 0 : (f.pattern & bits) == bits;
}
void consume(EventFlag& f, uint32_t bits, uint32_t mode) {
  if (mode & EVF_WAITCLEAR) f.pattern = 0;
  else if (mode & EVF_WAITCLEAR_PAT) f.pattern &= ~bits;
}

// sceKernelCreateEventFlag(name, attr, initPattern, opt*)
void CreateEventFlag(Cpu& c) {
  auto f = std::make_shared<EventFlag>();
  f->name = str(c, arg(c, 0), 32);
  f->attr = arg(c, 1);
  f->pattern = arg(c, 2);
  std::lock_guard lk(kernel_lock());
  uint32_t uid = new_uid();
  flags[uid] = std::move(f);
  ret(c, uid);
}
void DeleteEventFlag(Cpu& c) {
  std::lock_guard lk(kernel_lock());
  auto it = flags.find(arg(c, 0));
  if (it == flags.end()) return ret(c, SCE_KERNEL_ERROR_UNKNOWN_EVF_ID);
  it->second->deleted = true;
  it->second->cv.notify_all();
  flags.erase(it);
  ret(c, 0);
}
// sceKernelSetEventFlag(uid, bits): pattern |= bits
void SetEventFlag(Cpu& c) {
  std::lock_guard lk(kernel_lock());
  auto it = flags.find(arg(c, 0));
  if (it == flags.end()) return ret(c, SCE_KERNEL_ERROR_UNKNOWN_EVF_ID);
  it->second->pattern |= arg(c, 1);
  it->second->cv.notify_all();
  ret(c, 0);
}
// sceKernelClearEventFlag(uid, bits): pattern &= bits (the bits to keep)
void ClearEventFlag(Cpu& c) {
  std::lock_guard lk(kernel_lock());
  auto it = flags.find(arg(c, 0));
  if (it == flags.end()) return ret(c, SCE_KERNEL_ERROR_UNKNOWN_EVF_ID);
  it->second->pattern &= arg(c, 1);
  ret(c, 0);
}
// sceKernelPollEventFlag(uid, bits, mode, outBits*)
void PollEventFlag(Cpu& c) {
  uint32_t bits = arg(c, 1), mode = arg(c, 2), out = arg(c, 3);
  std::lock_guard lk(kernel_lock());
  auto it = flags.find(arg(c, 0));
  if (it == flags.end()) return ret(c, SCE_KERNEL_ERROR_UNKNOWN_EVF_ID);
  EventFlag& f = *it->second;
  if (out) c.mem().write<uint32_t>(out, f.pattern);
  if (!met(f, bits, mode)) return ret(c, SCE_KERNEL_ERROR_EVF_COND);  // unverified which COND code the game expects
  consume(f, bits, mode);
  ret(c, 0);
}
// sceKernelWaitEventFlagCB(uid, bits, mode, outBits*, timeout*)
void WaitEventFlag(Cpu& c) {
  uint32_t bits = arg(c, 1), mode = arg(c, 2), out = arg(c, 3), timeout = arg(c, 4);
  std::unique_lock lk(kernel_lock());
  auto it = flags.find(arg(c, 0));
  if (it == flags.end()) return ret(c, SCE_KERNEL_ERROR_UNKNOWN_EVF_ID);
  std::shared_ptr<EventFlag> f = it->second;
  if (f->waiters > 0 && !(f->attr & EVF_ATTR_MULTI)) return ret(c, SCE_KERNEL_ERROR_EVF_MULTI);
  f->waiters++;
  uint32_t r = wait_until(c, lk, f->cv, timeout, [&] { return f->deleted || met(*f, bits, mode); });
  f->waiters--;
  if (out) c.mem().write<uint32_t>(out, f->pattern);
  if (r) return ret(c, r);
  if (f->deleted) return ret(c, SCE_KERNEL_ERROR_WAIT_DELETE);
  consume(*f, bits, mode);
  ret(c, 0);
}

}  // namespace

bool lock_stats_on() {
  static const bool on = [] {
    const char* e = std::getenv("DCP_LOCK_STATS");
    return e && *e == '1';
  }();
  return on;
}

void note_kernel_lock_wait(uint64_t ns, bool main_thread) {
  std::lock_guard lk(wait_stats_lock);
  (main_thread ? kl_main : kl_other).add(ns);
}

void note_ngs_lock_wait(uint64_t ns) {
  bool blocking = false;
  std::string in = guest::svc_current_import_self(&blocking);
  std::lock_guard lk(wait_stats_lock);
  if (ns > ngs_all.max) {
    ngs_max_by = current_thread_name() + (in.empty() ? "" : " in " + in);
    if (my_held > 0) ngs_max_by += ", holding " + my_held_name;
  }
  ngs_all.add(ns);
  if (my_held > 0) ngs_holding.add(ns);
}

std::string lock_stats_report(double frames) {
  if (!lock_stats_on()) return {};
  std::map<uint32_t, LockStats> all;
  {
    std::lock_guard lk(kernel_lock());
    all.swap(lock_stats);
  }
  WaitStats km, ko, ng, nh;
  std::string nby;
  {
    std::lock_guard lk(wait_stats_lock);
    km = kl_main, ko = kl_other, ng = ngs_all, nh = ngs_holding, nby = ngs_max_by;
    kl_main = kl_other = ngs_all = ngs_holding = {};
    ngs_max_by.clear();
  }
  std::string out;
  char b[320];
  std::snprintf(b, sizeof b,
                "  locks (DCP_LOCK_STATS; counts per window): kernel_lock waits main %llux %.2f ms (max %.2f), other "
                "threads %llux %.2f ms (max %.2f) | ngs_lock waits %llux %.2f ms, max %.3f ms%s%s%s, %llux %.2f ms "
                "while holding a guest mutex\n",
                (unsigned long long)km.n, km.ns / 1e6, km.max / 1e6, (unsigned long long)ko.n, ko.ns / 1e6,
                ko.max / 1e6, (unsigned long long)ng.n, ng.ns / 1e6, ng.max / 1e6, nby.empty() ? "" : " (",
                nby.c_str(), nby.empty() ? "" : ")", (unsigned long long)nh.n, nh.ns / 1e6);
  out += b;
  // The mutexes the main thread waited on longest, then the most waited-on by other threads; 4 at most.
  std::vector<const LockStats*> v;
  for (auto& [uid, s] : all)
    if (s.waits) v.push_back(&s);
  std::sort(v.begin(), v.end(), [](const LockStats* a, const LockStats* b) {
    return a->main_ns != b->main_ns ? a->main_ns > b->main_ns : a->wait_ns > b->wait_ns;
  });
  auto same = [](const std::string& s) { return s; };
  for (size_t i = 0; i < v.size() && i < 4; i++) {
    const LockStats& s = *v[i];
    std::snprintf(b, sizeof b,
                  "  lock \"%s\" %08x: main waited %llux %.2f ms/frame (max %.2f ms), all threads %llux %.2f ms/frame [",
                  s.name.c_str(), s.work, (unsigned long long)s.main_waits, s.main_ns / frames / 1e6, s.main_max / 1e6,
                  (unsigned long long)s.waits, s.wait_ns / frames / 1e6);
    out += b + top_share(s.by_waiter, 4, same);
    std::snprintf(b, sizeof b, "]; woken-lost %llux, barged %llux", (unsigned long long)s.lost,
                  (unsigned long long)s.barged);
    out += b;
    if (!s.bargers.empty()) out += " [" + top_share(s.bargers, 3, same) + "]";
    if (s.holds) {
      std::snprintf(b, sizeof b, "; holds %.1f/frame avg %.3f max %.2f ms (%s)", s.holds / frames,
                    s.hold_ns / 1e6 / s.holds, s.hold_max / 1e6, s.hold_max_by.c_str());
      out += b;
    }
    out += "\n";
    if (!s.main_waits) continue;
    out += "    main's waits: owner [" + top_share(s.owners, 3, same) + "]; owner in [" + top_share(s.owner_in, 4, same) +
           "]";
    if (s.owner_wall_ns) {
      std::snprintf(b, sizeof b, "; owner ran %.0f%% of the wait", 100.0 * s.owner_cpu_ns / s.owner_wall_ns);
      out += b;
    }
    if (s.owner_fast + s.owner_slow) {
      std::snprintf(b, sizeof b, "; owner took it on a fast core %u, slow core %u", s.owner_fast, s.owner_slow);
      out += b;
    }
    out += "; callers [" + top_share(s.callers, 4, [](uint32_t a) {
             char h[12];
             std::snprintf(h, sizeof h, "%08x", a);
             return std::string(h);
           }) +
           "]\n";
  }
  return out;
}

void register_sync() {
  REG(sceKernelCreateMutex, CreateMutex);
  REG(sceKernelLockMutex, LockMutex);
  REG(sceKernelUnlockMutex, UnlockMutex);
  REG(sceKernelDeleteMutex, DeleteMutex);
  REG(sceKernelCreateLwMutex, CreateLwMutex);
  REG(sceKernelLockLwMutexCB, LockLwMutex);
  REG(sceKernelUnlockLwMutex, UnlockLwMutex);
  REG(sceKernelLockLwMutex, LockLwMutex);
  REG(sceKernelTryLockMutex, TryLockMutex);
  REG(sceKernelCreateLwCond, CreateLwCond);
  REG(sceKernelDeleteLwCond, DeleteLwCond);
  REG(sceKernelSignalLwCond, SignalLwCond);
  REG(sceKernelSignalLwCondAll, SignalLwCondAll);
  REG(sceKernelWaitLwCond, WaitLwCond);
  REG(sceKernelDeleteLwMutex, DeleteLwMutex);
  REG(sceKernelCreateEventFlag, CreateEventFlag);
  REG(sceKernelDeleteEventFlag, DeleteEventFlag);
  REG(sceKernelSetEventFlag, SetEventFlag);
  REG(sceKernelClearEventFlag, ClearEventFlag);
  REG(sceKernelPollEventFlag, PollEventFlag);
  REG(sceKernelWaitEventFlagCB, WaitEventFlag);
}

}  // namespace hle
