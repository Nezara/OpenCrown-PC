// libkernel memory: direct memory (offset space + host mappings), flexible memory, mmap. All host memory is plain
// committed RW pages; GPU visibility and protection bits are ignored (nothing runs on a GPU here).
#include <mutex>

#include <windows.h>

#include "../common.h"
#include "pure.h"
#include "render/texwatch.h"

namespace dcp::hle {
namespace {

// Guess: the game only uses the size to bound AllocateDirectMemory; 5 GiB-ish like a retail PS4 game budget.
constexpr uint64_t kDirectMemorySize = 0x13C000000ull;
std::mutex g_mu;
uint64_t g_dmem_cursor = 0;

DCP_SYSV uint64_t sceKernelGetDirectMemorySize() { return kDirectMemorySize; }

DCP_SYSV int32_t sceKernelAllocateDirectMemory(int64_t start, int64_t end, uint64_t len, uint64_t align, int32_t type,
                                               int64_t* out) {
    if (!out) return kErr(kEINVAL);
    std::lock_guard<std::mutex> lk(g_mu);
    int64_t off = dmem_alloc(&g_dmem_cursor, uint64_t(start), uint64_t(end), len, align);
    DCP_HLE_LOG("sceKernelAllocateDirectMemory", "len 0x%llx align 0x%llx type %d -> 0x%llx", (unsigned long long)len,
                (unsigned long long)align, type, (long long)off);
    if (off < 0) return kErr(kEAGAIN);
    *out = off;
    return kOk;
}

// The offset only names the allocation; every map gets fresh host memory (no aliasing of one offset; unverified that
// the game never maps a block twice).
DCP_SYSV int32_t sceKernelMapDirectMemory(void** addr, uint64_t len, int32_t prot, int32_t flags, int64_t off,
                                          uint64_t align) {
    if (!addr) return kErr(kEINVAL);
    void* p = alloc_low(len, align);
    DCP_HLE_LOG("sceKernelMapDirectMemory", "len 0x%llx prot 0x%x flags 0x%x off 0x%llx align 0x%llx -> %p",
                (unsigned long long)len, prot, flags, (long long)off, (unsigned long long)align, p);
    if (!p) return kErr(kENOMEM);
    *addr = p;
    return kOk;
}

DCP_SYSV int32_t sceKernelReleaseDirectMemory(int64_t, uint64_t) { return kOk; }  // offsets are never reused

DCP_SYSV int32_t sceKernelMapNamedFlexibleMemory(void** addr, uint64_t len, int32_t, int32_t, const char* name) {
    if (!addr) return kErr(kEINVAL);
    void* p = alloc_aligned(len, 0x4000);
    DCP_HLE_LOG("sceKernelMapNamedFlexibleMemory", "len 0x%llx '%s' -> %p", (unsigned long long)len, name ? name : "", p);
    if (!p) return kErr(kENOMEM);
    *addr = p;
    return kOk;
}

DCP_SYSV int32_t sceKernelMapFlexibleMemory(void** addr, uint64_t len, int32_t prot, int32_t flags) {
    return sceKernelMapNamedFlexibleMemory(addr, len, prot, flags, "");
}

// Releases whole allocations only (partial unmaps are ignored and leak; guess: libc frees what it mapped).
DCP_SYSV int32_t sceKernelMunmap(void* addr, uint64_t) {
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery(addr, &mi, sizeof mi) && mi.AllocationBase == addr) {
        uint64_t a = reinterpret_cast<uint64_t>(addr), end = a;  // the whole allocation (texwatch may have split it)
        MEMORY_BASIC_INFORMATION r;
        while (VirtualQuery(reinterpret_cast<void*>(end), &r, sizeof r) && r.AllocationBase == addr) end += r.RegionSize;
        dcp::texwatch::forget(a, end - a);  // the GL texture cache must not trust write tracking on reused memory
        VirtualFree(addr, 0, MEM_RELEASE);
    }
    return kOk;
}

// POSIX mmap (libc's allocator): anonymous only; returns MAP_FAILED (-1) + errno otherwise.
DCP_SYSV void* mmap(void* addr, uint64_t len, int32_t prot, int32_t flags, int32_t fd, int64_t off) {
    DCP_HLE_LOG("mmap", "addr %p len 0x%llx prot 0x%x flags 0x%x fd %d off 0x%llx", addr, (unsigned long long)len, prot,
                flags, fd, (long long)off);
    if (!(flags & 0x1000)) return (set_errno(kENOSYS), reinterpret_cast<void*>(-1));  // MAP_ANON
    void* p = alloc_aligned(len, 0x4000);
    if (!p) return (set_errno(kENOMEM), reinterpret_cast<void*>(-1));
    return p;
}

DCP_SYSV int32_t munmap(void* addr, uint64_t len) { return sceKernelMunmap(addr, len); }
DCP_SYSV int32_t sceKernelMlock(const void*, uint64_t) { return kOk; }
DCP_SYSV int32_t msync(void*, uint64_t, int32_t) { return 0; }
DCP_SYSV int32_t getpagesize() { return 0x4000; }
DCP_SYSV int32_t sceKernelIsAddressSanitizerEnabled() { return 0; }

// libc registers its malloc table with the runtime linker; nothing to do without other modules allocating.
DCP_SYSV int32_t _sceKernelRtldSetApplicationHeapAPI(void*) { return kOk; }

}  // namespace

void register_kernel_memory() {
    const char* k = "libkernel";
    REG(k, sceKernelGetDirectMemorySize);
    REG(k, sceKernelAllocateDirectMemory);
    REG(k, sceKernelMapDirectMemory);
    REG(k, sceKernelReleaseDirectMemory);
    REG(k, sceKernelMapNamedFlexibleMemory);
    REG(k, sceKernelMapFlexibleMemory);
    REG(k, sceKernelMunmap);
    REG(k, mmap);
    REG(k, munmap);
    REG(k, sceKernelMlock);
    REG(k, msync);
    REG(k, getpagesize);
    REG(k, sceKernelIsAddressSanitizerEnabled);
    REG(k, _sceKernelRtldSetApplicationHeapAPI);
}

}  // namespace dcp::hle
