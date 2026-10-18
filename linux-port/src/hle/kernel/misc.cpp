// libkernel odds and ends: errno, module TLS (__tls_get_addr), time and sleep, process info, signals (inert),
// stack-protector / debug traps. Also holds the HLE config and register_all.
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

#include <sched.h>

#include "../common.h"

namespace dcp::hle {
namespace {

Config g_cfg;
bool g_trace = false;
thread_local int t_errno = 0;

// ---- module TLS: one block per (thread, module), allocated on first access ----
struct TlsModule { const void* init; uint64_t filesz, memsz, align; };
std::mutex g_tls_mu;
std::vector<TlsModule> g_tls_mods;  // index = module id - 1
thread_local std::vector<uint8_t*> t_tls_blocks;

uint64_t align_up_tls(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }  // aligned_alloc wants a multiple

uint8_t* tls_block(uint64_t modid) {
    if (modid == 0) return nullptr;
    if (t_tls_blocks.size() < modid) t_tls_blocks.resize(modid, nullptr);
    uint8_t*& b = t_tls_blocks[modid - 1];
    if (!b) {
        TlsModule m;
        {
            std::lock_guard<std::mutex> lk(g_tls_mu);
            if (modid > g_tls_mods.size()) return nullptr;
            m = g_tls_mods[modid - 1];
        }
        uint64_t al = m.align < 16 ? 16 : m.align;
        b = static_cast<uint8_t*>(std::aligned_alloc(al, align_up_tls(m.memsz ? m.memsz : 1, al)));  // never freed (threads are few)
        std::memset(b, 0, m.memsz);
        std::memcpy(b, m.init, m.filesz);
    }
    return b;
}

struct TlsIndex { uint64_t module, offset; };
DCP_SYSV void* __tls_get_addr(const TlsIndex* ti) {
    uint8_t* b = tls_block(ti->module);
    return b ? b + ti->offset : nullptr;
}

DCP_SYSV int* __error() { return &t_errno; }

// ---- time ----
// Process time counter = CLOCK_MONOTONIC nanoseconds since register_all (frequency 1 GHz).
uint64_t g_mono0 = 0;
uint64_t mono_ns() {
    timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

DCP_SYSV uint64_t sceKernelGetProcessTime() { return (mono_ns() - g_mono0) / 1000; }  // microseconds since start
DCP_SYSV uint64_t sceKernelGetProcessTimeCounter() { return mono_ns() - g_mono0; }
DCP_SYSV uint64_t sceKernelGetProcessTimeCounterFrequency() { return 1000000000ull; }

void sleep_us(uint64_t us) {
    if (us == 0) {
        ::sched_yield();
        return;
    }
    timespec ts{time_t(us / 1000000), long(us % 1000000) * 1000};
    while (::nanosleep(&ts, &ts) != 0) {}  // EINTR: sleep the rest
}
DCP_SYSV int32_t sceKernelUsleep(uint32_t us) { return sleep_us(us), kOk; }

struct Timespec { int64_t sec, nsec; };
struct Timeval { int64_t sec, usec; };

uint64_t unix_100ns() {
    timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return uint64_t(ts.tv_sec) * 10000000ull + uint64_t(ts.tv_nsec) / 100;
}
DCP_SYSV int32_t clock_gettime(int32_t id, Timespec* ts) {
    if (id == 0) {  // CLOCK_REALTIME; everything else is treated as monotonic
        uint64_t t = unix_100ns();
        ts->sec = int64_t(t / 10000000), ts->nsec = int64_t(t % 10000000 * 100);
    } else {
        timespec m;
        ::clock_gettime(CLOCK_MONOTONIC, &m);
        ts->sec = int64_t(m.tv_sec), ts->nsec = int64_t(m.tv_nsec);
    }
    return 0;
}
DCP_SYSV int32_t gettimeofday(Timeval* tv, void*) {
    if (tv) {
        uint64_t t = unix_100ns();
        tv->sec = int64_t(t / 10000000), tv->usec = int64_t(t % 10000000 / 10);
    }
    return 0;
}
DCP_SYSV int32_t nanosleep(const Timespec* req, Timespec* rem) {
    sleep_us(uint64_t(req->sec) * 1000000 + uint64_t(req->nsec) / 1000);
    if (rem) rem->sec = rem->nsec = 0;
    return 0;
}
DCP_SYSV int32_t _nanosleep(const Timespec* req, Timespec* rem) { return nanosleep(req, rem); }
DCP_SYSV int32_t getrusage(int32_t, void* ru) { return std::memset(ru, 0, 144), 0; }  // FreeBSD struct rusage
// Local time = UTC (guess: no time-zone shim yet).
DCP_SYSV int32_t sceKernelConvertUtcToLocaltime(int64_t t, int64_t* local, void* tz, int32_t* dst) {
    if (local) *local = t;
    if (tz) std::memset(tz, 0, 8);
    if (dst) *dst = 0;
    return kOk;
}
DCP_SYSV int32_t sceKernelConvertLocaltimeToUtc(int64_t t, int64_t, int64_t* utc, void* tz, int32_t* dst) {
    if (utc) *utc = t;
    if (tz) std::memset(tz, 0, 8);
    if (dst) *dst = 0;
    return kOk;
}

// ---- process ----
DCP_SYSV uint64_t sceKernelGetProcParam() { return g_cfg.proc_param; }
DCP_SYSV int32_t sceKernelIsNeoMode() { return g_cfg.neo ? 1 : 0; }  // 1 = PS4 Pro: the game picks 3840x2160 targets
DCP_SYSV void _exit(int32_t status) { g_cfg.on_exit(status); }
DCP_SYSV void __pthread_cxa_finalize(void*) {}
DCP_SYSV int32_t sceKernelGetModuleInfoFromAddr(uint64_t addr, int32_t, void*) {
    DCP_HLE_LOG("sceKernelGetModuleInfoFromAddr", "0x%llx -> ESRCH (not implemented)", (unsigned long long)addr);
    return kErr(3);
}
DCP_SYSV int32_t sceKernelGetModuleInfoForUnwind(uint64_t addr, int32_t, void*) {
    DCP_HLE_LOG("sceKernelGetModuleInfoForUnwind", "0x%llx -> ESRCH (no C++ exceptions expected)", (unsigned long long)addr);
    return kErr(3);
}

// ---- signals: accepted and inert ----
DCP_SYSV void* signal(int32_t, void*) { return nullptr; }
DCP_SYSV int32_t sigprocmask(int32_t, const void*, void* old) { return old ? (std::memset(old, 0, 16), 0) : 0; }
DCP_SYSV int32_t _sigprocmask(int32_t h, const void* s, void* old) { return sigprocmask(h, s, old); }
DCP_SYSV int32_t sigfillset(void* s) { return std::memset(s, 0xFF, 16), 0; }

// ---- traps ----
DCP_SYSV void __stack_chk_fail() {
    std::fprintf(stderr, "HLE __stack_chk_fail: stack smashing detected\n");
    g_cfg.on_exit(134);
}
DCP_SYSV void sceKernelDebugRaiseException(uint32_t code, uint32_t arg) {
    std::fprintf(stderr, "HLE sceKernelDebugRaiseException(0x%x, 0x%x)\n", code, arg);
    g_cfg.on_exit(134);
}
DCP_SYSV void sceKernelDebugRaiseExceptionOnReleaseMode(uint32_t code, uint32_t arg) {
    sceKernelDebugRaiseException(code, arg);
}

const char* g_progname = "eboot.bin";

}  // namespace

const Config& config() { return g_cfg; }
bool trace_enabled() { return g_trace; }
int* errno_ptr() { return &t_errno; }

uint32_t add_tls_module(const void* init, uint64_t filesz, uint64_t memsz, uint64_t align) {
    std::lock_guard<std::mutex> lk(g_tls_mu);
    g_tls_mods.push_back({init, filesz, memsz, align});
    return uint32_t(g_tls_mods.size());
}

void register_kernel_misc() {
    const char* k = "libkernel";
    REG(k, __tls_get_addr); REG(k, __error);
    REG(k, sceKernelGetProcessTime); REG(k, sceKernelGetProcessTimeCounter);
    REG(k, sceKernelGetProcessTimeCounterFrequency); REG(k, sceKernelUsleep); REG(k, clock_gettime);
    REG(k, gettimeofday); REG(k, nanosleep); REG(k, _nanosleep); REG(k, getrusage);
    REG(k, sceKernelConvertUtcToLocaltime); REG(k, sceKernelConvertLocaltimeToUtc);
    REG(k, sceKernelGetProcParam); REG(k, sceKernelIsNeoMode); REG(k, _exit); REG(k, __pthread_cxa_finalize);
    REG(k, sceKernelGetModuleInfoFromAddr); REG(k, sceKernelGetModuleInfoForUnwind);
    REG(k, signal); REG(k, sigprocmask); REG(k, _sigprocmask); REG(k, sigfillset);
    REG(k, __stack_chk_fail); REG(k, sceKernelDebugRaiseException); REG(k, sceKernelDebugRaiseExceptionOnReleaseMode);
    loader::register_hle_data("__progname", &g_progname);
}

void register_all(const Config& cfg) {
    g_cfg = cfg;
    const char* t = std::getenv("DCP_TRACE");
    g_trace = t && *t == '1';
    g_mono0 = mono_ns();
    register_kernel_memory();
    register_kernel_thread();
    register_kernel_file();
    register_kernel_misc();
    register_services();
    register_savedata();
    register_dialogs();
    register_pad();
    register_audio();
    register_video();
    register_font();
    register_net();
    register_np();
}

}  // namespace dcp::hle
