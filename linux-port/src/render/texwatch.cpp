#include "texwatch.h"

#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>

#include <atomic>
#include <shared_mutex>
#include <unordered_map>

namespace dcp::texwatch {
namespace {

constexpr uint64_t kPage = 0x1000;
std::shared_mutex g_lock;
std::unordered_map<uint64_t, bool> g_ro;         // page index -> currently write-protected by us
std::unordered_map<uint64_t, uint64_t> g_write;  // page index -> sequence of its last recorded write
std::atomic<uint64_t> g_seq{1};
std::atomic<uint64_t> g_last_write{0};
bool g_veh = false;           // our SIGSEGV handler is installed
struct sigaction g_prev_segv;  // the handler before ours (dcp_run's crash report): faults that are not ours go there

// Records a write to page pg and lifts our protection; caller holds g_lock exclusively. False if we do not track pg.
bool unprotect_locked(uint64_t pg) {
    auto it = g_ro.find(pg);
    if (it == g_ro.end()) return false;
    g_ro.erase(it);
    uint64_t s = ++g_seq;
    g_write[pg] = s;
    g_last_write = s;
    return mprotect(reinterpret_cast<void*>(pg * kPage), kPage, PROT_READ | PROT_WRITE) == 0;  // fine in a handler
}

void on_fault(int sig, siginfo_t* si, void* uc) {
    // x86-64 page-fault error code: bit 1 = write access.
    const auto* ctx = static_cast<const ucontext_t*>(uc);
    bool write = (ctx->uc_mcontext.gregs[REG_ERR] & 2) != 0;
    if (sig == SIGSEGV && si->si_code == SEGV_ACCERR && write) {
        uint64_t pg = reinterpret_cast<uint64_t>(si->si_addr) / kPage;
        // Several threads can fault on one page at once: the first handler lifts the protection, the others then find
        // the page no longer protected by us but in the write log, and only need to retry their store. A page that
        // something else made read-only would fault forever, so retries without progress (no write recorded anywhere
        // since this thread's last retry) are limited and then handed on to the crash report.
        static thread_local uint64_t retry_seq = 0;
        static thread_local int retries = 0;
        g_lock.lock();
        bool ours = unprotect_locked(pg);
        bool lifted = !ours && !g_ro.count(pg) && g_write.count(pg);
        g_lock.unlock();
        if (ours) return;  // retry the store
        if (lifted) {
            uint64_t s = g_seq.load();
            retries = s == retry_seq ? retries + 1 : 1;
            retry_seq = s;
            if (retries <= 16) return;
        }
    }
    if (g_prev_segv.sa_flags & SA_SIGINFO) {
        if (g_prev_segv.sa_sigaction) return g_prev_segv.sa_sigaction(sig, si, uc);
    } else if (g_prev_segv.sa_handler != SIG_DFL && g_prev_segv.sa_handler != SIG_IGN) {
        return g_prev_segv.sa_handler(sig);
    }
    signal(sig, SIG_DFL);  // no one else: let the fault re-raise and kill the process
}

void install_handler() {
    struct sigaction sa = {};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_prev_segv);
    g_veh = true;
}

}  // namespace

bool protect(uint64_t base, uint64_t size, uint64_t* seq) {
    *seq = g_seq.load();
    if (!size) return false;
    uint64_t p0 = base / kPage, p1 = (base + size + kPage - 1) / kPage;
    g_lock.lock();
    if (!g_veh) install_handler();
    bool ok = true;
    // Pages already ours stay read-only; the rest must be plain read-write memory.
    for (uint64_t pg = p0; pg < p1 && ok;) {
        if (g_ro.count(pg)) { ++pg; continue; }
        uint64_t run = pg;
        while (run < p1 && !g_ro.count(run)) ++run;
        uint64_t a = pg * kPage, n = (run - pg) * kPage;
        // mprotect fails (ENOMEM) on unmapped pages. It does not report the old protection, so the caller must only
        // pass guest read-write memory (the GL texture cache does: textures live in direct memory).
        if (mprotect(reinterpret_cast<void*>(a), n, PROT_READ) != 0) { ok = false; break; }
        for (uint64_t q = pg; q < run; ++q) g_ro[q] = true;
        pg = run;
    }
    *seq = g_seq.load();
    g_lock.unlock();
    return ok;
}

bool written_since(uint64_t base, uint64_t size, uint64_t seq) {
    uint64_t p0 = base / kPage, p1 = (base + size + kPage - 1) / kPage;
    // Unmapped memory is reported through forget(), so no mapping query here.
    if (g_last_write.load() <= seq) return false;
    // A shared page may have been written and re-protected by another texture's re-upload: check the write log.
    g_lock.lock_shared();
    bool w = false;
    for (uint64_t pg = p0; pg < p1 && !w; ++pg) {
        auto it = g_write.find(pg);
        w = it != g_write.end() && it->second > seq;
    }
    g_lock.unlock_shared();
    return w;
}

uint64_t seq_now() { return g_seq.load(); }

void forget(uint64_t base, uint64_t size) {
    if (!size || !g_veh) return;
    g_lock.lock();
    for (uint64_t pg = base / kPage; pg < (base + size + kPage - 1) / kPage; ++pg)
        if (g_ro.erase(pg)) g_write[pg] = g_last_write = ++g_seq;
    g_lock.unlock();
}

void host_write(const void* p, uint64_t n) {
    if (!n || !g_veh) return;
    uint64_t a = reinterpret_cast<uint64_t>(p);
    g_lock.lock();
    for (uint64_t pg = a / kPage; pg < (a + n + kPage - 1) / kPage; ++pg) unprotect_locked(pg);
    g_lock.unlock();
}

}  // namespace dcp::texwatch
