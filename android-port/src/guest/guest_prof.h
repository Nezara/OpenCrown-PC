// Guest-level sampling profiler (opt-in: env DCP_GUEST_PROF=1; on Android also through the debug.opencrown.env
// system property, see tools/mobile_run). Shows which guest functions (eboot addresses) burn each thread's time.
//
// How: every JIT block, right after its first instruction, stores its own guest start address (bit 0 = Thumb) to a
// per-thread slot in guest memory (one fastmem store per block entry; emitted by Cpu's translation hook, so Dynarmic
// itself is untouched). HLE imports mark the thread as "in import n" while they run. A sampler thread reads every
// registered thread's slot + import state at DCP_GUEST_PROF_HZ (default 1000), and whether the thread is on the
// CPU (busy: used CPU this tick and /proc run state R on Linux / Android; on Windows any CPU in guest code, >= half a
// tick in an import) or not (idle: blocked). Thread CPU totals from the thread CPU clocks. Aggregated per thread
// name and rewritten every 5 s to DCP_GUEST_PROF_OUT (default guest_prof.txt in the working folder; Android: the
// app's internal files folder). tools/guest_prof.py rolls the block addresses up to guest functions.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace guest {
class Cpu;
class Memory;

namespace prof {

bool enabled();
// Output file when DCP_GUEST_PROF_OUT is not set. Call before the first register_thread.
void set_default_output(const std::string& path);
// Import index -> readable name, for the output file.
void set_import_names(std::function<std::string(uint32_t)> fn);
// The calling host thread runs `cpu` as guest thread `name` from now on (gives the core its slot; starts the sampler
// on first use). No-op when the profiler is off.
void register_thread(const std::string& name, Cpu& cpu, Memory& mem);

// Spike log: the calling thread's most sampled guest block addresses (and imports) since steady-clock time `since_ns`,
// e.g. "81234567 x9, sceFoo x3" (busy samples only; the sampler keeps the last ~32k samples of all threads). Empty
// when the profiler is off or nothing was sampled.
std::string recent_hot(uint64_t since_ns, size_t top);

// RAII: the calling thread is inside import `imm` (or back in guest code with imm = -1, for guest calls made from an
// HLE function) until the scope ends.
class Scope {
 public:
  explicit Scope(int32_t imm);
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  void* rec_;
  int32_t prev_;
};

}  // namespace prof
}  // namespace guest
