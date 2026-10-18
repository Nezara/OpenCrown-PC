// HLE shims for the PS4 system libraries the game (and the user's libc.prx) import. Everything registered here is
// called from game code with the SysV ABI (native on Linux). Sources in this directory are built with -fno-exceptions
// (no unwinding through game frames), so shims may be written directly.
#pragma once
#include <cstdint>
#include <string>

namespace dcp::hle {

struct Config {
    std::string app0;          // host folder that /app0/ maps to (the user's dump folder holding eboot.bin)
    uint64_t proc_param = 0;   // absolute address of the eboot's PT_SCE_PROCPARAM (sceKernelGetProcParam)
    void (*on_exit)(int) = nullptr;  // process exit (flushes logs, terminates)
    std::string save_root;     // host folder holding SAVEDATA00/... (PLAN rule 0; DCP_SAVE_ROOT overrides for tests)
    std::string input_script;  // contents of --input (headless pad source); empty = connected pad, no buttons
    uint64_t max_frames = 0;   // stop cleanly after this many vblanks (0 = run forever)
    uint64_t eboot_base = 0;   // load address of eboot.bin (eboot-relative addresses in notes/map are added to it)
    bool neo = false;          // report a PS4 Pro (sceKernelIsNeoMode): the game renders 3840x2160 targets
};

// Default save root: ${XDG_DATA_HOME:-~/.local/share}/OpenCrown/saves, or $DCP_SAVE_ROOT when set.
std::string default_save_root();

void register_all(const Config& cfg);
uint64_t vblank_count_public();  // frame clock (vblanks waited), for host-side diagnostics

// Gives the calling thread an alternate signal stack (sigaltstack), so fault handlers (dcp_run's crash report,
// texwatch) still run when the thread's own stack is exhausted. Game threads get one at start; call it on host threads
// that run game code. Idempotent.
void install_signal_stack();

// TLS blocks for modules with PT_TLS (DTPMOD64 ids start at 1). Returns the module id.
uint32_t add_tls_module(const void* init, uint64_t filesz, uint64_t memsz, uint64_t align);

}  // namespace dcp::hle
