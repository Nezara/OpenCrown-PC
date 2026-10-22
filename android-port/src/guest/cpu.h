// One guest CPU core on Dynarmic's A32 JIT. Guest code is ARMv7-A / Thumb-2 with VFPv3 + NEON (Cortex-A9).
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "guest/memory.h"

namespace Dynarmic {
class ExclusiveMonitor;
namespace A32 {
class Jit;
}
}  // namespace Dynarmic

namespace guest {

// SVC immediates of the return trampolines (ARM `svc #n ; b .` in guest memory, set up by the runner).
constexpr uint32_t SVC_THREAD_RETURN = 0xffffff;  // LR of a thread entry: the thread's entry function returned
constexpr uint32_t SVC_CALL_RETURN = 0xfffffe;    // LR of invoke() / call(): the called function returned

class Cpu {
 public:
  // svc(cpu, imm24) runs for every SVC; the import stubs carry their import index there.
  using SvcHandler = std::function<void(Cpu&, uint32_t)>;

  Cpu(Memory& mem, Dynarmic::ExclusiveMonitor& monitor, size_t core, SvcHandler svc,
      size_t code_cache = 128u << 20);
  ~Cpu();

  // Runs until stop() or a fault. Returns false on a fault (reason in fault()).
  bool run();
  void stop();

  // Calls guest function `fn` (bit 0 = Thumb) with word arguments (r0-r3, then the stack) from inside an HLE function and returns
  // its r0. A JIT can't be re-entered from its own callback, so the call runs on a child core (created on first use,
  // one per nesting level) on this host thread and this core's guest stack, below the current sp. A fault in the
  // callee becomes this core's fault. `call_return` = guest address of an ARM `svc #SVC_CALL_RETURN`.
  uint32_t call(uint32_t fn, std::initializer_list<uint32_t> args);
  // Runs `fn` on this core (which must not be running) with stack pointer `sp` until it returns; result in reg(0).
  // Arguments past the fourth go on the stack below `sp` (AAPCS).
  bool invoke(uint32_t fn, std::initializer_list<uint32_t> args, uint32_t sp);
  bool invoke(uint32_t fn, const uint32_t* args, size_t n, uint32_t sp);
  static inline uint32_t call_return = 0;

  // Set when the thread entry returned through the SVC_THREAD_RETURN trampoline.
  bool exited = false;
  uint32_t exit_code = 0;

  uint32_t reg(int i) const;
  void set_reg(int i, uint32_t v);
  uint32_t pc() const { return reg(15); }
  void set_pc(uint32_t addr);  // bit 0 selects Thumb
  uint32_t cpsr() const;
  // VFP registers (the game uses the hard-float ABI: float args in s0.., results in s0 / d0).
  float sreg(int i) const;
  void set_sreg(int i, float v);
  double dreg(int i) const;
  void set_dreg(int i, double v);
  uint32_t tpidruro = 0;       // CP15 c13/c0/3: the thread's TLS pointer (read-only for user code)
  uint32_t tpidrurw = 0;       // CP15 c13/c0/2
  // Guest profiler (guest/guest_prof.h): guest address this core's blocks store their start PC to, 0 = off. Read when
  // a block is translated; set before the core runs.
  uint32_t prof_slot = 0;

  // JIT translations ([timing] "jit" line): blocks and guest instructions translated by this core and its call()
  // child cores (written by the owning host thread only), and by every core of the process.
  struct JitCounts {
    std::atomic<uint64_t> blocks{0}, insts{0};
  };
  const JitCounts& jit_counts() const { return *counts_; }
  static const JitCounts& jit_totals();
  // Core construction time ([timing] "thread starts" line): Dynarmic's Jit constructor (code cache mapping, prelude
  // emit and, on arm64, its invalidate of the whole cache) per code cache size since the last call, as
  // "64 MiB 3x 15.12 ms (max 15.20)" text; empty when no core was built.
  static std::string jit_build_report();

  Memory& mem() { return mem_; }
  const std::string& fault() const { return fault_; }
  void set_fault(std::string why);
  // A guest load/store fault: also stops the JIT at the access itself (PC synced when check_halt_on_memory_access).
  void set_memory_fault(std::string why);

 private:
  struct Env;
  struct Cp15;
  Memory& mem_;
  Dynarmic::ExclusiveMonitor& monitor_;
  size_t core_;
  size_t code_cache_;
  SvcHandler svc_;
  std::unique_ptr<Cpu> child_;  // for call()
  std::string fault_;
  std::unique_ptr<Env> env_;
  std::unique_ptr<Dynarmic::A32::Jit> jit_;
  JitCounts own_counts_;
  JitCounts* counts_ = &own_counts_;  // a call() child counts into its parent's
};

}  // namespace guest
