#include "texwatch.h"

#include <windows.h>

#include <atomic>
#include <unordered_map>

namespace dcp::texwatch {
namespace {

constexpr uint64_t kPage = 0x1000;
SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<uint64_t, bool> g_ro;         // page index -> currently write-protected by us
std::unordered_map<uint64_t, uint64_t> g_write;  // page index -> sequence of its last recorded write
std::atomic<uint64_t> g_seq{1};
std::atomic<uint64_t> g_last_write{0};
PVOID g_veh = nullptr;

// Records a write to page pg and lifts our protection; caller holds g_lock exclusively. False if we do not track pg.
bool unprotect_locked(uint64_t pg) {
    auto it = g_ro.find(pg);
    if (it == g_ro.end()) return false;
    g_ro.erase(it);
    uint64_t s = ++g_seq;
    g_write[pg] = s;
    g_last_write = s;
    DWORD old;
    return VirtualProtect(reinterpret_cast<void*>(pg * kPage), kPage, PAGE_READWRITE, &old) != 0;
}

LONG CALLBACK on_fault(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    uint64_t pg = uint64_t(r->ExceptionInformation[1]) / kPage;
    AcquireSRWLockExclusive(&g_lock);
    bool ours = unprotect_locked(pg);
    bool tracked = !ours && !g_ro.count(pg) && g_write.count(pg);
    ReleaseSRWLockExclusive(&g_lock);
    if (ours) return EXCEPTION_CONTINUE_EXECUTION;
    // Several threads can fault on one page at once: the first handler lifts the protection, the others then find the
    // page no longer protected by us and only need to retry their store (if it really is writable again now).
    MEMORY_BASIC_INFORMATION mi;
    if (tracked && VirtualQuery(reinterpret_cast<void*>(pg * kPage), &mi, sizeof mi) &&
        (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

bool protect(uint64_t base, uint64_t size, uint64_t* seq) {
    *seq = g_seq.load();
    if (!size) return false;
    uint64_t p0 = base / kPage, p1 = (base + size + kPage - 1) / kPage;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, on_fault);
    bool ok = true;
    // Pages already ours stay read-only; the rest must be plain read-write memory.
    for (uint64_t pg = p0; pg < p1 && ok;) {
        if (g_ro.count(pg)) { ++pg; continue; }
        uint64_t run = pg;
        while (run < p1 && !g_ro.count(run)) ++run;
        uint64_t a = pg * kPage, n = (run - pg) * kPage;
        DWORD old;
        // No VirtualQuery here: its cost grows with the whole protection run (~2 us per MB of a large RW pool), not the
        // range. VirtualProtect fails on uncommitted pages / ranges spanning allocations and reports the first page.
        if (!VirtualProtect(reinterpret_cast<void*>(a), n, PAGE_READONLY, &old)) { ok = false; break; }
        if (old != PAGE_READWRITE) {
            VirtualProtect(reinterpret_cast<void*>(a), n, old, &old);
            ok = false;
            break;
        }
        for (uint64_t q = pg; q < run; ++q) g_ro[q] = true;
        pg = run;
    }
    *seq = g_seq.load();
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

bool written_since(uint64_t base, uint64_t size, uint64_t seq) {
    uint64_t p0 = base / kPage, p1 = (base + size + kPage - 1) / kPage;
    // Unmapped memory is reported through forget(), so no VirtualQuery (it cost ~30 us per call on large runs).
    if (g_last_write.load() <= seq) return false;
    // A shared page may have been written and re-protected by another texture's re-upload: check the write log.
    AcquireSRWLockShared(&g_lock);
    bool w = false;
    for (uint64_t pg = p0; pg < p1 && !w; ++pg) {
        auto it = g_write.find(pg);
        w = it != g_write.end() && it->second > seq;
    }
    ReleaseSRWLockShared(&g_lock);
    return w;
}

uint64_t seq_now() { return g_seq.load(); }

void forget(uint64_t base, uint64_t size) {
    if (!size || !g_veh) return;
    AcquireSRWLockExclusive(&g_lock);
    for (uint64_t pg = base / kPage; pg < (base + size + kPage - 1) / kPage; ++pg)
        if (g_ro.erase(pg)) g_write[pg] = g_last_write = ++g_seq;
    ReleaseSRWLockExclusive(&g_lock);
}

void host_write(const void* p, uint64_t n) {
    if (!n || !g_veh) return;
    uint64_t a = reinterpret_cast<uint64_t>(p);
    AcquireSRWLockExclusive(&g_lock);
    for (uint64_t pg = a / kPage; pg < (a + n + kPage - 1) / kPage; ++pg) unprotect_locked(pg);
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace dcp::texwatch
