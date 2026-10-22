#include "guest/cpu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include <dynarmic/interface/A32/a32.h>
#include <cstdlib>

#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/frontend/A32/a32_location_descriptor.h>

#include "guest/guest_prof.h"

namespace guest {

using Dynarmic::A32::Coprocessor;
using Dynarmic::A32::CoprocReg;

namespace {
Cpu::JitCounts& totals() {
  static Cpu::JitCounts t;
  return t;
}

// Jit constructor times by code cache size (MiB), since the last jit_build_report().
struct BuildStat { uint32_t n = 0; uint64_t ns = 0, max = 0; };
std::mutex builds_lock;
std::map<size_t, BuildStat> builds;

const char* exception_name(Dynarmic::A32::Exception e) {
  using E = Dynarmic::A32::Exception;
  switch (e) {
    case E::UndefinedInstruction: return "undefined instruction";
    case E::UnpredictableInstruction: return "unpredictable instruction";
    case E::DecodeError: return "decode error";
    case E::Breakpoint: return "breakpoint (bkpt)";
    case E::NoExecuteFault: return "no-execute fault";
    default: return "exception";
  }
}
}  // namespace

// Memory callbacks: fastmem (or the page table) serves every mapped access inside the JIT, so these run only for
// code fetches, exclusive stores and unmapped addresses (= a guest fault).
struct Cpu::Env final : Dynarmic::A32::UserCallbacks {
  Cpu& cpu;
  Dynarmic::ExclusiveMonitor& monitor;
  explicit Env(Cpu& c, Dynarmic::ExclusiveMonitor& m) : cpu(c), monitor(m) {}

  template <typename T>
  T load(uint32_t va) {
    if (!cpu.mem_.mapped(va) || !cpu.mem_.mapped(va + sizeof(T) - 1)) {
      char b[96];
      std::snprintf(b, sizeof b, "read%zu from unmapped 0x%08x", sizeof(T) * 8, va);
      cpu.set_memory_fault(b);
      return 0;
    }
    return cpu.mem_.read<T>(va);
  }
  template <typename T>
  void store(uint32_t va, T v) {
    if (!cpu.mem_.mapped(va) || !cpu.mem_.mapped(va + sizeof(T) - 1)) {
      char b[96];
      std::snprintf(b, sizeof b, "write%zu to unmapped 0x%08x", sizeof(T) * 8, va);
      cpu.set_memory_fault(b);
      return;
    }
    cpu.mem_.write<T>(va, v);
  }
  template <typename T>
  bool store_exclusive(uint32_t va, T v, T expected) {
    if (!cpu.mem_.mapped(va)) return store(va, v), false;
    // GCC / Clang builtin (std::atomic_ref is missing from the NDK's libc++).
    return __atomic_compare_exchange_n(reinterpret_cast<T*>(cpu.mem_.ptr(va)), &expected, v, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
  }

  std::optional<uint32_t> MemoryReadCode(uint32_t va) override {
    if (!cpu.mem_.mapped(va)) return std::nullopt;  // Dynarmic raises a fetch fault -> ExceptionRaised
    return cpu.mem_.read<uint32_t>(va);
  }
  uint8_t MemoryRead8(uint32_t va) override { return load<uint8_t>(va); }
  uint16_t MemoryRead16(uint32_t va) override { return load<uint16_t>(va); }
  uint32_t MemoryRead32(uint32_t va) override { return load<uint32_t>(va); }
  uint64_t MemoryRead64(uint32_t va) override { return load<uint64_t>(va); }
  void MemoryWrite8(uint32_t va, uint8_t v) override { store(va, v); }
  void MemoryWrite16(uint32_t va, uint16_t v) override { store(va, v); }
  void MemoryWrite32(uint32_t va, uint32_t v) override { store(va, v); }
  void MemoryWrite64(uint32_t va, uint64_t v) override { store(va, v); }
  bool MemoryWriteExclusive8(uint32_t va, uint8_t v, uint8_t e) override { return store_exclusive(va, v, e); }
  bool MemoryWriteExclusive16(uint32_t va, uint16_t v, uint16_t e) override { return store_exclusive(va, v, e); }
  bool MemoryWriteExclusive32(uint32_t va, uint32_t v, uint32_t e) override { return store_exclusive(va, v, e); }
  bool MemoryWriteExclusive64(uint32_t va, uint64_t v, uint64_t e) override { return store_exclusive(va, v, e); }

  void InterpreterFallback(uint32_t pc, size_t n) override {
    char b[96];
    std::snprintf(b, sizeof b, "no JIT translation for %zu instruction(s) at 0x%08x", n, pc);
    cpu.set_fault(b);
  }
  void CallSVC(uint32_t swi) override {
    if (swi == SVC_CALL_RETURN) return cpu.stop();
    if (swi == SVC_THREAD_RETURN) {
      cpu.exited = true;
      cpu.exit_code = cpu.reg(0);
      return cpu.stop();
    }
    cpu.svc_(cpu, swi);
  }
  void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception e) override {
    using E = Dynarmic::A32::Exception;
    switch (e) {  // hints are no-ops for a user-mode guest
      case E::SendEvent: case E::SendEventLocal: case E::WaitForInterrupt: case E::WaitForEvent: case E::Yield:
      case E::PreloadData: case E::PreloadDataWithIntentToWrite: case E::PreloadInstruction:
        return;
      default: break;
    }
    char b[96];
    std::snprintf(b, sizeof b, "%s at 0x%08x", exception_name(e), pc);
    cpu.set_fault(b);
  }
  // Guest profiler: each block stores its start address (bit 0 = Thumb) to the core's slot. Emitted once the block's
  // first instruction is in the IR, never into an empty block: Dynarmic makes a block conditional only when its
  // first instruction is conditional and nothing was emitted yet, so a store before it would change translation.
  bool prof_armed = false;
  uint32_t prof_start = 0;
  void PreCodeTranslationHook(bool thumb, uint32_t pc, Dynarmic::A32::IREmitter& ir) override {
    // Translation counters: one call per guest instruction; a block's first one sees an empty block at its start PC.
    bool block_start = ir.block.empty() && Dynarmic::A32::LocationDescriptor{ir.block.Location()}.PC() == pc;
    Cpu::JitCounts& n = *cpu.counts_;
    n.insts.store(n.insts.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    totals().insts.fetch_add(1, std::memory_order_relaxed);
    if (block_start) {
      n.blocks.store(n.blocks.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
      totals().blocks.fetch_add(1, std::memory_order_relaxed);
    }
    if (!cpu.prof_slot) return;
    if (ir.block.empty()) {
      if (Dynarmic::A32::LocationDescriptor{ir.block.Location()}.PC() == pc) prof_armed = true, prof_start = pc | thumb;
      return;
    }
    if (!prof_armed) return;
    prof_armed = false;
    ir.WriteMemory32(ir.Imm32(cpu.prof_slot), ir.Imm32(prof_start), Dynarmic::IR::AccType::NORMAL);
  }
  void AddTicks(uint64_t) override {}
  uint64_t GetTicksRemaining() override { return ~0ull; }
};

// CP15: only the thread ID registers exist for user code. Everything else is undefined.
struct Cpu::Cp15 final : Coprocessor {
  Cpu& cpu;
  explicit Cp15(Cpu& c) : cpu(c) {}
  static bool tid(bool two, unsigned opc1, CoprocReg crn, CoprocReg crm) {
    return !two && opc1 == 0 && crn == CoprocReg::C13 && crm == CoprocReg::C0;
  }
  std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) override {
    return std::nullopt;
  }
  CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, CoprocReg crn, CoprocReg crm, unsigned opc2) override {
    if (tid(two, opc1, crn, crm) && opc2 == 2) return &cpu.tpidrurw;
    return {};
  }
  CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override { return {}; }
  CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg crn, CoprocReg crm, unsigned opc2) override {
    if (tid(two, opc1, crn, crm) && opc2 == 3) return &cpu.tpidruro;
    if (tid(two, opc1, crn, crm) && opc2 == 2) return &cpu.tpidrurw;
    return {};
  }
  CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, CoprocReg) override { return {}; }
  std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg, std::optional<uint8_t>) override { return std::nullopt; }
  std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg, std::optional<uint8_t>) override { return std::nullopt; }
};

Cpu::Cpu(Memory& mem, Dynarmic::ExclusiveMonitor& monitor, size_t core, SvcHandler svc, size_t code_cache)
    : mem_(mem), monitor_(monitor), core_(core), code_cache_(code_cache), svc_(std::move(svc)),
      env_(std::make_unique<Env>(*this, monitor)) {
  Dynarmic::A32::UserConfig cfg{};
  cfg.callbacks = env_.get();
  cfg.processor_id = core;
  cfg.global_monitor = &monitor;
  cfg.arch_version = Dynarmic::A32::ArchVersion::v7;
  cfg.page_table = mem.table();
  // Fastmem: guest loads / stores become one host access at base + address (no page table walk). An access to an
  // unmapped page faults in the host; Dynarmic's own handler (SEH through the code block's unwind table on Windows
  // x64, SIGSEGV on POSIX, chained behind ART's on Android) sends it to the callbacks above and recompiles that block
  // on the page table. Exclusives stay on the callbacks. DCP_NO_FASTMEM=1 = page table only (A/B, debugging).
  static const bool no_fastmem = [] {
    const char* e = std::getenv("DCP_NO_FASTMEM");
    return e && *e == '1';
  }();
  if (!no_fastmem) cfg.fastmem_pointer = reinterpret_cast<uintptr_t>(mem.base());
  cfg.coprocessors[15] = std::make_shared<Cp15>(*this);
  cfg.enable_cycle_counting = false;  // run until stop() / a fault, no tick slices
  cfg.code_cache_size = code_cache;
  // Debug: DCP_PRECISE_FAULTS=1 makes the JIT stop right at a faulting access with the PC synced (slower).
  static const bool precise = [] {
    const char* e = std::getenv("DCP_PRECISE_FAULTS");
    return e && *e == '1';
  }();
  cfg.check_halt_on_memory_access = precise;
  auto t0 = std::chrono::steady_clock::now();
  jit_ = std::make_unique<Dynarmic::A32::Jit>(cfg);
  uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  {
    std::lock_guard lk(builds_lock);
    BuildStat& b = builds[code_cache >> 20];
    b.n++, b.ns += ns, b.max = std::max(b.max, ns);
  }
  jit_->SetCpsr(0x10);           // user mode, ARM, little-endian
  jit_->SetFpscr(0x03000000);    // flush-to-zero + default NaN (unverified Vita default)
}

Cpu::~Cpu() = default;

const Cpu::JitCounts& Cpu::jit_totals() { return totals(); }

std::string Cpu::jit_build_report() {
  std::map<size_t, BuildStat> b;
  {
    std::lock_guard lk(builds_lock);
    b.swap(builds);
  }
  std::string out;
  char buf[96];
  for (auto& [mib, s] : b) {
    std::snprintf(buf, sizeof buf, "%s%zu MiB %ux %.2f ms (max %.2f)", out.empty() ? "" : ", ", mib, s.n,
                  s.ns / 1e6 / s.n, s.max / 1e6);
    out += buf;
  }
  return out;
}

bool Cpu::run() {
  jit_->Run();
  return fault_.empty();
}

void Cpu::stop() { jit_->HaltExecution(); }

bool Cpu::invoke(uint32_t fn, std::initializer_list<uint32_t> args, uint32_t sp) {
  return invoke(fn, args.begin(), args.size(), sp);
}

bool Cpu::invoke(uint32_t fn, const uint32_t* args, size_t n, uint32_t sp) {
  if (!call_return) {
    set_fault("guest call without a return trampoline");
    return false;
  }
  fault_.clear();
  // AAPCS: r0-r3, the rest on the stack from sp upwards (the callee's [sp], [sp+4], ...).
  uint32_t stack_args = n > 4 ? uint32_t(n - 4) : 0;
  sp = (sp - 4 * stack_args) & ~7u;
  for (size_t i = 0; i < n; i++) {
    if (i < 4) set_reg(int(i), args[i]);
    else mem_.write<uint32_t>(sp + 4u * uint32_t(i - 4), args[i]);
  }
  set_reg(13, sp);
  set_reg(14, call_return);
  set_pc(fn);
  return run();
}

uint32_t Cpu::call(uint32_t fn, std::initializer_list<uint32_t> args) {
  if (!child_) {  // 16 MiB, or less when this core's own cache is smaller (DCP_CODE_CACHE_MB, hle/thread.cpp)
    child_ = std::make_unique<Cpu>(mem_, monitor_, core_, svc_, std::min(size_t(16) << 20, code_cache_));
    child_->counts_ = counts_;
  }
  Cpu& k = *child_;
  k.prof_slot = prof_slot;  // same host thread: same profiler slot
  prof::Scope in_guest(-1);
  k.tpidruro = tpidruro;
  k.tpidrurw = tpidrurw;
  k.jit_->SetFpscr(jit_->Fpscr());
  if (!k.invoke(fn, args, reg(13) - 0x100)) {  // below the caller's frame
    char b[32];
    std::snprintf(b, sizeof b, "in guest call to 0x%08x: ", fn);
    set_fault(b + k.fault_);
    return 0;
  }
  return k.reg(0);
}

void Cpu::set_memory_fault(std::string why) {
  if (fault_.empty()) fault_ = std::move(why);
  jit_->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
}

void Cpu::set_fault(std::string why) {
  if (fault_.empty()) fault_ = std::move(why);
  jit_->HaltExecution();
}

uint32_t Cpu::reg(int i) const { return jit_->Regs()[i]; }
void Cpu::set_reg(int i, uint32_t v) { jit_->Regs()[i] = v; }
uint32_t Cpu::cpsr() const { return jit_->Cpsr(); }

// ExtRegs: S0..S63 words; D<n> = S<2n> (low) : S<2n+1> (high).
float Cpu::sreg(int i) const {
  float f;
  std::memcpy(&f, &jit_->ExtRegs()[i], 4);
  return f;
}
void Cpu::set_sreg(int i, float v) { std::memcpy(&jit_->ExtRegs()[i], &v, 4); }
double Cpu::dreg(int i) const {
  double d;
  std::memcpy(&d, &jit_->ExtRegs()[2 * i], 8);
  return d;
}
void Cpu::set_dreg(int i, double v) { std::memcpy(&jit_->ExtRegs()[2 * i], &v, 8); }

void Cpu::set_pc(uint32_t addr) {
  uint32_t c = jit_->Cpsr();
  jit_->SetCpsr((addr & 1) ? (c | 0x20) : (c & ~0x20u));
  jit_->Regs()[15] = addr & ~1u;
}

}  // namespace guest
