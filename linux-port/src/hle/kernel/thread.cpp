// libkernel threads and synchronisation on pthreads: scePthread* (threads, attrs, mutexes, conds, keys, once), event
// flags, event queues (registration only). All PS4 handle types are pointer-sized values owned by the game that we
// fill with pointers to our own objects. Priorities, affinity and scheduling policy are recorded and ignored.
#include <atomic>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <mutex>

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>

#include "../common.h"

namespace dcp::hle {
namespace {

using SysvEntry = DCP_SYSV void* (*)(void*);
using SysvDtor = DCP_SYSV void (*)(void*);

struct Attr {
    uint64_t stacksize = 0x100000;  // guess: SCE default 1 MiB
    int32_t detach = 0, policy = 1, prio = 700, inherit = 4;
    uint64_t affinity = 0x7f;
};

struct Thread {
    pthread_t h{};
    bool started = false;
    SysvEntry entry = nullptr;
    void* arg = nullptr;
    void* ret = nullptr;
    char name[32] = {};
    Attr attr;
};

thread_local Thread* t_self = nullptr;
thread_local jmp_buf* t_exit_jmp = nullptr;  // scePthreadExit jumps back to thread_main (game frames have no host unwind info)

constexpr int kMaxKeys = 256;
std::atomic<bool> g_key_used[kMaxKeys];
SysvDtor g_key_dtor[kMaxKeys];
thread_local void* t_specific[kMaxKeys];

Thread* self() {
    if (!t_self) {  // a thread we did not create (the main game thread): adopt it
        t_self = new Thread;
        t_self->h = pthread_self();
        t_self->started = true;
        std::strcpy(t_self->name, "main");
    }
    return t_self;
}

void* thread_main(void* p) {
    Thread* t = static_cast<Thread*>(p);
    t_self = t;
    install_signal_stack();
    jmp_buf exit_jmp;
    t_exit_jmp = &exit_jmp;
    if (setjmp(exit_jmp) == 0) t->ret = t->entry(t->arg);
    t_exit_jmp = nullptr;
    for (int k = 0; k < kMaxKeys; ++k)  // POSIX key destructors (one pass)
        if (t_specific[k] && g_key_dtor[k] && g_key_used[k]) {
            void* v = t_specific[k];
            t_specific[k] = nullptr;
            g_key_dtor[k](v);
        }
    return nullptr;
}

// Absolute CLOCK_MONOTONIC deadline `usec` from now (the clock every condvar below is created with).
timespec deadline_after_us(uint64_t usec) {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += time_t(usec / 1000000);
    ts.tv_nsec += long(usec % 1000000) * 1000;
    if (ts.tv_nsec >= 1000000000) ts.tv_sec += 1, ts.tv_nsec -= 1000000000;
    return ts;
}
void init_mono_cond(pthread_cond_t* cv) {
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(cv, &ca);
    pthread_condattr_destroy(&ca);
}

// ---- attributes ----
DCP_SYSV int32_t scePthreadAttrInit(Attr** a) { return a ? (*a = new Attr, kOk) : kErr(kEINVAL); }
DCP_SYSV int32_t scePthreadAttrDestroy(Attr** a) {
    if (a && *a) delete *a, *a = nullptr;
    return kOk;
}
DCP_SYSV int32_t scePthreadAttrSetstacksize(Attr** a, uint64_t s) { return (*a)->stacksize = s, kOk; }
DCP_SYSV int32_t scePthreadAttrSetdetachstate(Attr** a, int32_t s) { return (*a)->detach = s, kOk; }
DCP_SYSV int32_t scePthreadAttrGetdetachstate(Attr** a, int32_t* s) { return *s = (*a)->detach, kOk; }
DCP_SYSV int32_t scePthreadAttrSetschedpolicy(Attr** a, int32_t p) { return (*a)->policy = p, kOk; }
DCP_SYSV int32_t scePthreadAttrSetschedparam(Attr** a, const int32_t* prio) { return (*a)->prio = *prio, kOk; }
DCP_SYSV int32_t scePthreadAttrSetinheritsched(Attr** a, int32_t i) { return (*a)->inherit = i, kOk; }
DCP_SYSV int32_t scePthreadAttrSetaffinity(Attr** a, uint64_t m) { return (*a)->affinity = m, kOk; }
DCP_SYSV int32_t scePthreadAttrGetaffinity(Attr** a, uint64_t* m) { return *m = (*a)->affinity, kOk; }
DCP_SYSV int32_t scePthreadAttrGet(Thread* t, Attr** a) {
    if (!a || !t) return kErr(kEINVAL);
    if (!*a) *a = new Attr;
    **a = t->attr;
    return kOk;
}

// ---- threads ----
DCP_SYSV int32_t scePthreadCreate(Thread** out, Attr** attr, SysvEntry entry, void* arg, const char* name) {
    if (!out || !entry) return kErr(kEINVAL);
    Thread* t = new Thread;
    if (attr && *attr) t->attr = **attr;
    t->entry = entry, t->arg = arg;
    if (name) std::strncpy(t->name, name, sizeof t->name - 1);
    // Host stacks get 1 MiB more than asked (our shims and libc run on them too). A pthread stack is one mapping with
    // the guard page at the bottom only, so PS4 frames > 4 KiB (no stack probes) need nothing extra.
    uint64_t stack = t->attr.stacksize + (1u << 20);
    *out = t;  // published before the thread runs: entries may read their own handle
    pthread_attr_t pa;
    pthread_attr_init(&pa);
    pthread_attr_setstacksize(&pa, size_t(stack));
    t->started = pthread_create(&t->h, &pa, thread_main, t) == 0;
    pthread_attr_destroy(&pa);
    std::fprintf(stderr, "HLE scePthreadCreate: '%s' entry %p arg %p stack 0x%llx prio %d affinity 0x%llx\n", t->name,
                 reinterpret_cast<void*>(entry), arg, (unsigned long long)t->attr.stacksize, t->attr.prio,
                 (unsigned long long)t->attr.affinity);
    return t->started ? kOk : kErr(kEAGAIN);
}

DCP_SYSV Thread* scePthreadSelf() { return self(); }
DCP_SYSV int32_t scePthreadEqual(Thread* a, Thread* b) { return a == b; }
DCP_SYSV int32_t scePthreadJoin(Thread* t, void** ret) {
    if (!t) return kErr(kEINVAL);
    if (t->started && !pthread_equal(t->h, pthread_self())) pthread_join(t->h, nullptr);
    if (ret) *ret = t->ret;
    return kOk;
}
DCP_SYSV int32_t scePthreadDetach(Thread*) { return kOk; }
DCP_SYSV void scePthreadExit(void* ret) {
    self()->ret = ret;
    if (t_exit_jmp) longjmp(*t_exit_jmp, 1);
    pthread_exit(nullptr);  // a thread we did not create
}
DCP_SYSV int32_t scePthreadCancel(Thread* t) {
    DCP_HLE_LOG("scePthreadCancel", "'%s' ignored (no async cancel on the host)", t ? t->name : "?");
    return kOk;
}
DCP_SYSV int32_t scePthreadYield() { return ::sched_yield(), kOk; }
DCP_SYSV int32_t sched_yield() { return ::sched_yield(), 0; }
DCP_SYSV int32_t scePthreadSetprio(Thread* t, int32_t p) { return t->attr.prio = p, kOk; }
DCP_SYSV int32_t scePthreadGetprio(Thread* t, int32_t* p) { return *p = t->attr.prio, kOk; }
DCP_SYSV int32_t scePthreadSetaffinity(Thread* t, uint64_t m) { return t->attr.affinity = m, kOk; }
DCP_SYSV int32_t scePthreadGetaffinity(Thread* t, uint64_t* m) { return *m = t->attr.affinity, kOk; }
DCP_SYSV int32_t scePthreadRename(Thread* t, const char* n) {
    if (t && n) std::strncpy(t->name, n, sizeof t->name - 1);
    return kOk;
}
DCP_SYSV int32_t scePthreadGetname(Thread* t, char* n) {
    if (t && n) std::strcpy(n, t->name);
    return kOk;
}

// ---- keys / once ----
DCP_SYSV int32_t scePthreadKeyCreate(int32_t* key, SysvDtor dtor) {
    for (int k = 0; k < kMaxKeys; ++k) {
        bool f = false;
        if (g_key_used[k].compare_exchange_strong(f, true)) {
            g_key_dtor[k] = dtor;
            *key = k;
            return kOk;
        }
    }
    return kErr(kEAGAIN);
}
DCP_SYSV int32_t scePthreadKeyDelete(int32_t key) {
    if (key >= 0 && key < kMaxKeys) g_key_used[key] = false;
    return kOk;
}
DCP_SYSV void* scePthreadGetspecific(int32_t key) { return key >= 0 && key < kMaxKeys ? t_specific[key] : nullptr; }
DCP_SYSV int32_t scePthreadSetspecific(int32_t key, const void* v) {
    if (key < 0 || key >= kMaxKeys) return kErr(kEINVAL);
    t_specific[key] = const_cast<void*>(v);
    return kOk;
}

std::recursive_mutex g_once_mu;  // one lock for all once-blocks: init routines are rare and short
DCP_SYSV int32_t scePthreadOnce(int32_t* once, DCP_SYSV void (*fn)()) {
    std::lock_guard<std::recursive_mutex> lk(g_once_mu);
    if (*once == 0) {  // ScePthreadOnce = {int32 state; mutex}; state 0 = not run
        *once = 1;
        fn();
    }
    return kOk;
}

// ---- mutexes ----
// Every type is recursive (PTHREAD_MUTEX_RECURSIVE): fine for NORMAL/ADAPTIVE (re-locking is UB there) and RECURSIVE;
// ERRORCHECK loses its EDEADLK diagnostics.
struct Mutex { pthread_mutex_t cs; char name[32]; };
struct MutexAttr { int32_t type = 1, protocol = 0; };

Mutex* new_mutex(const char* name) {
    Mutex* m = new Mutex;
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->cs, &ma);
    pthread_mutexattr_destroy(&ma);
    std::strncpy(m->name, name ? name : "", sizeof m->name - 1);
    m->name[sizeof m->name - 1] = 0;
    return m;
}
// PS4 static initializers are NULL (and 1 for the adaptive one): create on first use.
Mutex* get_mutex(Mutex** p) {
    Mutex* m = __atomic_load_n(p, __ATOMIC_ACQUIRE);
    if (reinterpret_cast<uintptr_t>(m) > 1) return m;
    Mutex* n = new_mutex("static");
    if (__atomic_compare_exchange_n(p, &m, n, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return n;
    pthread_mutex_destroy(&n->cs);
    delete n;
    return m;
}

DCP_SYSV int32_t scePthreadMutexattrInit(MutexAttr** a) { return *a = new MutexAttr, kOk; }
DCP_SYSV int32_t scePthreadMutexattrDestroy(MutexAttr** a) {
    if (a && *a) delete *a, *a = nullptr;
    return kOk;
}
DCP_SYSV int32_t scePthreadMutexattrSettype(MutexAttr** a, int32_t t) { return (*a)->type = t, kOk; }
DCP_SYSV int32_t scePthreadMutexattrSetprotocol(MutexAttr** a, int32_t p) { return (*a)->protocol = p, kOk; }
DCP_SYSV int32_t scePthreadMutexInit(Mutex** m, MutexAttr**, const char* name) {
    if (!m) return kErr(kEINVAL);
    *m = new_mutex(name);
    return kOk;
}
DCP_SYSV int32_t scePthreadMutexDestroy(Mutex** m) {
    if (m && reinterpret_cast<uintptr_t>(*m) > 1) pthread_mutex_destroy(&(*m)->cs), delete *m;
    if (m) *m = nullptr;
    return kOk;
}
DCP_SYSV int32_t scePthreadMutexLock(Mutex** m) { return pthread_mutex_lock(&get_mutex(m)->cs), kOk; }
DCP_SYSV int32_t scePthreadMutexTrylock(Mutex** m) {
    return pthread_mutex_trylock(&get_mutex(m)->cs) == 0 ? kOk : kErr(kEBUSY);
}
DCP_SYSV int32_t scePthreadMutexUnlock(Mutex** m) { return pthread_mutex_unlock(&get_mutex(m)->cs), kOk; }

// ---- condition variables ----
struct Cond { pthread_cond_t cv; };
Cond* get_cond(Cond** p) {
    Cond* c = __atomic_load_n(p, __ATOMIC_ACQUIRE);
    if (c) return c;
    Cond* n = new Cond;
    init_mono_cond(&n->cv);
    if (__atomic_compare_exchange_n(p, &c, n, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return n;
    pthread_cond_destroy(&n->cv);
    delete n;
    return c;
}
DCP_SYSV int32_t scePthreadCondInit(Cond** c, void*, const char*) {
    *c = nullptr;
    get_cond(c);
    return kOk;
}
DCP_SYSV int32_t scePthreadCondDestroy(Cond** c) {
    if (c && *c) pthread_cond_destroy(&(*c)->cv), delete *c, *c = nullptr;
    return kOk;
}
DCP_SYSV int32_t scePthreadCondWait(Cond** c, Mutex** m) {
    pthread_cond_wait(&get_cond(c)->cv, &get_mutex(m)->cs);
    return kOk;
}
DCP_SYSV int32_t scePthreadCondTimedwait(Cond** c, Mutex** m, uint32_t usec) {
    timespec ts = deadline_after_us(usec);
    if (pthread_cond_timedwait(&get_cond(c)->cv, &get_mutex(m)->cs, &ts) == 0) return kOk;
    return kErr(kETIMEDOUT);
}
DCP_SYSV int32_t scePthreadCondSignal(Cond** c) { return pthread_cond_signal(&get_cond(c)->cv), kOk; }
DCP_SYSV int32_t scePthreadCondBroadcast(Cond** c) { return pthread_cond_broadcast(&get_cond(c)->cv), kOk; }

// ---- event flags ----
struct EventFlag {
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv;
    uint64_t bits = 0;
    char name[32] = {};
    EventFlag() { init_mono_cond(&cv); }
};
constexpr uint32_t kEvfAnd = 1, kEvfOr = 2, kEvfClearAll = 0x10, kEvfClearPat = 0x20;

bool evf_match(uint64_t bits, uint64_t pat, uint32_t mode) {
    return (mode & kEvfAnd) ? (bits & pat) == pat : (bits & pat) != 0;
}
void evf_consume(EventFlag* e, uint64_t pat, uint32_t mode) {
    if (mode & kEvfClearAll) e->bits = 0;
    else if (mode & kEvfClearPat) e->bits &= ~pat;
}

DCP_SYSV int32_t sceKernelCreateEventFlag(EventFlag** out, const char* name, uint32_t attr, uint64_t init, void*) {
    if (!out) return kErr(kEINVAL);
    EventFlag* e = new EventFlag;
    e->bits = init;
    if (name) std::strncpy(e->name, name, sizeof e->name - 1);
    DCP_HLE_LOG("sceKernelCreateEventFlag", "'%s' attr 0x%x init 0x%llx", e->name, attr, (unsigned long long)init);
    *out = e;
    return kOk;
}
DCP_SYSV int32_t sceKernelDeleteEventFlag(EventFlag* e) { return pthread_cond_destroy(&e->cv), delete e, kOk; }
DCP_SYSV int32_t sceKernelSetEventFlag(EventFlag* e, uint64_t bits) {
    pthread_mutex_lock(&e->lock);
    e->bits |= bits;
    pthread_mutex_unlock(&e->lock);
    pthread_cond_broadcast(&e->cv);
    return kOk;
}
DCP_SYSV int32_t sceKernelClearEventFlag(EventFlag* e, uint64_t keep) {  // PS4 semantics: bits &= arg
    pthread_mutex_lock(&e->lock);
    e->bits &= keep;
    pthread_mutex_unlock(&e->lock);
    return kOk;
}
DCP_SYSV int32_t sceKernelPollEventFlag(EventFlag* e, uint64_t pat, uint32_t mode, uint64_t* res) {
    pthread_mutex_lock(&e->lock);
    bool ok = evf_match(e->bits, pat, mode);
    if (res) *res = e->bits;
    if (ok) evf_consume(e, pat, mode);
    pthread_mutex_unlock(&e->lock);
    return ok ? kOk : kErr(kEBUSY);
}
DCP_SYSV int32_t sceKernelWaitEventFlag(EventFlag* e, uint64_t pat, uint32_t mode, uint64_t* res, uint32_t* timeout) {
    timespec deadline = deadline_after_us(timeout ? *timeout : 0);
    pthread_mutex_lock(&e->lock);
    while (!evf_match(e->bits, pat, mode)) {
        if (!timeout) {
            pthread_cond_wait(&e->cv, &e->lock);
        } else if (pthread_cond_timedwait(&e->cv, &e->lock, &deadline) != 0 && !evf_match(e->bits, pat, mode)) {
            if (res) *res = e->bits;
            *timeout = 0;
            pthread_mutex_unlock(&e->lock);
            return kErr(kETIMEDOUT);
        }
    }
    if (res) *res = e->bits;
    evf_consume(e, pat, mode);
    pthread_mutex_unlock(&e->lock);
    return kOk;
}

// ---- event queues: created and registered only (no waiter seen in the eboot, engine.md section 5) ----
DCP_SYSV int32_t sceKernelCreateEqueue(void** eq, const char* name) {
    static std::atomic<uintptr_t> next{0x1000};
    *eq = reinterpret_cast<void*>(next.fetch_add(0x10));
    DCP_HLE_LOG("sceKernelCreateEqueue", "'%s' -> %p", name ? name : "", *eq);
    return kOk;
}
DCP_SYSV int32_t sceKernelDeleteEqueue(void*) { return kOk; }

}  // namespace

void install_signal_stack() {
    static thread_local bool done = false;
    if (done) return;
    constexpr size_t kSize = 256 << 10;  // the crash report formats text (fprintf) on it
    void* p = mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return;
    stack_t ss = {};
    ss.ss_sp = p;
    ss.ss_size = kSize;
    done = sigaltstack(&ss, nullptr) == 0;  // never freed: one per game thread, threads are few
}

void register_kernel_thread() {
    const char* k = "libkernel";
    REG(k, scePthreadAttrInit); REG(k, scePthreadAttrDestroy); REG(k, scePthreadAttrSetstacksize);
    REG(k, scePthreadAttrSetdetachstate); REG(k, scePthreadAttrGetdetachstate); REG(k, scePthreadAttrSetschedpolicy);
    REG(k, scePthreadAttrSetschedparam); REG(k, scePthreadAttrSetinheritsched); REG(k, scePthreadAttrSetaffinity);
    REG(k, scePthreadAttrGetaffinity); REG(k, scePthreadAttrGet);
    REG(k, scePthreadCreate); REG(k, scePthreadSelf); REG(k, scePthreadEqual); REG(k, scePthreadJoin);
    REG(k, scePthreadDetach); REG(k, scePthreadExit); REG(k, scePthreadCancel); REG(k, scePthreadYield);
    REG(k, sched_yield); REG(k, scePthreadSetprio); REG(k, scePthreadGetprio); REG(k, scePthreadSetaffinity);
    REG(k, scePthreadGetaffinity); REG(k, scePthreadRename); REG(k, scePthreadGetname);
    REG(k, scePthreadKeyCreate); REG(k, scePthreadKeyDelete); REG(k, scePthreadGetspecific);
    REG(k, scePthreadSetspecific); REG(k, scePthreadOnce);
    REG(k, scePthreadMutexattrInit); REG(k, scePthreadMutexattrDestroy); REG(k, scePthreadMutexattrSettype);
    REG(k, scePthreadMutexattrSetprotocol); REG(k, scePthreadMutexInit); REG(k, scePthreadMutexDestroy);
    REG(k, scePthreadMutexLock); REG(k, scePthreadMutexTrylock); REG(k, scePthreadMutexUnlock);
    REG(k, scePthreadCondInit); REG(k, scePthreadCondDestroy); REG(k, scePthreadCondWait);
    REG(k, scePthreadCondTimedwait); REG(k, scePthreadCondSignal); REG(k, scePthreadCondBroadcast);
    REG(k, sceKernelCreateEventFlag); REG(k, sceKernelDeleteEventFlag); REG(k, sceKernelSetEventFlag);
    REG(k, sceKernelClearEventFlag); REG(k, sceKernelPollEventFlag); REG(k, sceKernelWaitEventFlag);
    REG(k, sceKernelCreateEqueue); REG(k, sceKernelDeleteEqueue);
}

}  // namespace dcp::hle
