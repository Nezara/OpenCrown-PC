// Guest threads: one host thread per guest thread, each with its own Dynarmic core and TLS block, all sharing the
// guest memory and the exclusive monitor. A fault on any guest thread ends the process with a report.
#pragma once
#include <cstdint>
#include <string>

#include "guest/cpu.h"

namespace Dynarmic {
class ExclusiveMonitor;
}

namespace hle {

struct Process {
  guest::Memory* mem = nullptr;
  Dynarmic::ExclusiveMonitor* monitor = nullptr;  // sized for MAX_CORES
  guest::Cpu::SvcHandler svc;
};
constexpr size_t MAX_CORES = 64;  // Dynarmic processor ids handed out to guest threads (and ULT workers)

void init_threads(const Process& p);

// Creates the main thread and runs `entry(r0, r1)` on the calling host thread. Returns its exit code when the entry
// returns; a fault on any thread prints a report and exits the process.
uint32_t run_main_thread(uint32_t entry, uint32_t stack_size, uint32_t r0, uint32_t r1);

// For other HLE libraries (SceUlt workers): a kernel thread object for the calling host thread, with its own core
// and TLS, registered under a new uid. Returns the core to run guest code on.
guest::Cpu& adopt_host_thread(const std::string& name);

// Frame timing: the calling guest thread waited `us` in sceDisplayWaitVblankStart*. thread_times(frames) = the
// busiest guest threads since the last call, CPU ms and vblank-wait ms per frame each (CPU time on POSIX hosts).
void note_vblank_wait(uint64_t us);
std::string thread_times(double frames);
// [timing] lines since the last call: JIT translations per thread, sceKernelStartThread split (whole call / core
// build) and core build times by code cache size, the cores' average clocks (sampled by sample_cpu_clocks(), Linux /
// Android where cpufreq is readable). Empty lines are left out.
std::string thread_perf_report(double frames);
void sample_cpu_clocks();
// Name of the guest thread the calling host thread runs ("?" for none).
const std::string& current_thread_name();

// Spike log (frame_stats.cpp). A guest thread's frame work runs between its frame waits: frame_wait_begin() when the
// calling thread starts waiting for the next frame (sceDisplayWaitVblankStart*, the AddEntry queue wait, the display
// queue's idle wait) closes its work segment (wall, CPU, imports, guest profile samples), frame_wait_end() starts the
// next. frame_presented() = a flip (sceDisplaySetFrameBuf): ends a frame interval. frame_stats_report() = the
// [timing] lines since the last call: flip interval histogram and the worst frames with each thread's segment.
void frame_wait_begin();
void frame_wait_end();
void frame_presented();
std::string frame_stats_report();

// Ends the process after a guest fault on core `c` (prints the fault and registers).
[[noreturn]] void die(guest::Cpu& c, const std::string& where);

}  // namespace hle
