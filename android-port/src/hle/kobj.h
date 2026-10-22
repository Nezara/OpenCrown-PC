// Shared kernel state for the HLE libraries: error codes, uids, the global kernel lock, the current guest thread,
// and timed waits. Every kernel object table is guarded by kernel_lock(); a blocked guest thread waits on a
// condition variable with that lock released, so other guest threads keep running.
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "hle/common.h"

namespace hle {

// Values from the vitasdk headers (psp2/kernel/error.h).
enum : uint32_t {
  SCE_KERNEL_ERROR_ERROR = 0x80020001,
  SCE_KERNEL_ERROR_INVALID_ARGUMENT = 0x80020005,
  SCE_KERNEL_ERROR_ILLEGAL_ADDR = 0x80020006,
  SCE_KERNEL_ERROR_ILLEGAL_MODE = 0x80020010,
  SCE_KERNEL_ERROR_NO_MEMORY = 0x8002710b,
  SCE_KERNEL_ERROR_UNKNOWN_UID = 0x80028001,
  SCE_KERNEL_ERROR_WAIT_TIMEOUT = 0x80028005,
  SCE_KERNEL_ERROR_WAIT_DELETE = 0x80028006,
  SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID = 0x80028021,
  SCE_KERNEL_ERROR_DORMANT = 0x80028027,
  SCE_KERNEL_ERROR_NOT_DORMANT = 0x80028028,
  SCE_KERNEL_ERROR_UNKNOWN_EVF_ID = 0x800280e1,
  SCE_KERNEL_ERROR_EVF_MULTI = 0x800280e2,
  SCE_KERNEL_ERROR_EVF_COND = 0x800280e3,
  SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID = 0x80028141,
  SCE_KERNEL_ERROR_MUTEX_RECURSIVE = 0x80028142,
  SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN = 0x80028145,
  SCE_KERNEL_ERROR_MUTEX_UNLOCK_UDF = 0x80028144,
  SCE_KERNEL_ERROR_UNKNOWN_LW_MUTEX_ID = 0x80028181,
  SCE_KERNEL_ERROR_LW_MUTEX_RECURSIVE = 0x80028182,
  SCE_KERNEL_ERROR_LW_MUTEX_UNLOCK_UDF = 0x80028184,
  SCE_KERNEL_ERROR_UNKNOWN_LW_COND_ID = 0x800281c1,
};

std::mutex& kernel_lock();
uint32_t new_uid();

// DCP_LOCK_STATS=1: contended-lock report in the [timing] block (sync.cpp), off by default. Waits on guest
// (lw) mutexes with the owner's state, kernel_lock() waits, ngs_lock waits.
bool lock_stats_on();
void note_kernel_lock_wait(uint64_t ns, bool main_thread);  // thread.cpp: kernel_lock() was contended
void note_ngs_lock_wait(uint64_t ns);                       // ngs.cpp: the calling thread waited for ngs_lock
std::string lock_stats_report(double frames);               // "" when off
// thread.cpp, kernel lock held: thread `uid` now. cpu_ns = its CPU time so far (~0 = unknown: Windows); import = the
// import it is in ("" = guest code; DCP_LOCK_STATS only), blocking = a waiting / sleeping one.
struct ThreadProbe {
  std::string name, import;
  uint64_t cpu_ns = ~0ull;
  bool blocking = false;
};
bool probe_thread(uint32_t uid, ThreadProbe& out);
bool is_main_thread();       // the calling host thread runs the guest main thread
bool is_fast_core(int cpu);  // big.LITTLE host: `cpu` is one of the fast cores (false when unknown / not split)  // odd, positive as an int: the game treats < 0 as an error

// The guest thread running on this host thread (set by the thread runner).
uint32_t current_thread_uid();

// Waits on `cv` until `done()` (checked with the kernel lock held). `timeout` is the guest address of a SceUInt32
// microsecond budget, 0 = forever; it is updated with the time left. Returns 0 or SCE_KERNEL_ERROR_WAIT_TIMEOUT.
template <typename Pred>
uint32_t wait_until(Cpu& c, std::unique_lock<std::mutex>& lk, std::condition_variable& cv, uint32_t timeout, Pred done) {
  if (!timeout) {
    cv.wait(lk, done);
    return 0;
  }
  auto budget = std::chrono::microseconds(c.mem().read<uint32_t>(timeout));
  auto end = std::chrono::steady_clock::now() + budget;
  bool ok = cv.wait_until(lk, end, done);
  auto left = std::chrono::duration_cast<std::chrono::microseconds>(end - std::chrono::steady_clock::now()).count();
  c.mem().write<uint32_t>(timeout, ok && left > 0 ? uint32_t(left) : 0u);
  return ok ? 0u : uint32_t(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
}

}  // namespace hle
