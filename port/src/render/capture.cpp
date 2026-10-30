// Hooks on the game's render wrappers (addresses: notes/map/render_cmds.md s3, s8). Each hook is a 5-byte jmp to a
// relay that calls an observer with the original argument registers, then runs the displaced prologue and jumps
// back: the game's own code still runs unchanged (it keeps writing its PM4, which the GNM stub ignores).
#include "capture.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <windows.h>

#include "hle/hle.h"
#include "loader/hle.h"

namespace dcp::render {
namespace {

uint8_t* g_base = nullptr;
bool g_enabled = false;

// ---- per-DCB GPU state (the GNM context state the wrappers set) ----
struct State {
    int vs = -1, ps = -1;
    uint32_t blend = 0, prim = 4, index_size = 0;
    bool scissor_on = false;
    int32_t scissor[4]{};
    uint64_t rt = 0;
    uint32_t rt_w = 1920, rt_h = 1080;
    VSharp vb[16];
    int nvb = 0;
    VSharp cb[2][8];  // [0] pixel stage, [1] vertex stage
    uint32_t tex[2][8]{};
    uint32_t samp[2][4]{};
};
SRWLOCK g_state_lock = SRWLOCK_INIT;
std::unordered_map<uint64_t, State>* g_states;
thread_local uint64_t t_dcb = 0;
thread_local Cls t_cls = Cls::None;

constexpr uint64_t kCueOffset = 0x2e8;  // constant-update object embedded in the DCB object (X1 Execute: lea r12+0x2e8)

State& state_of(uint64_t dcb) {  // caller holds the lock
    t_dcb = dcb;
    return (*g_states)[dcb];
}

// Committed-and-readable check with a small per-thread region cache (VirtualQuery is too slow per draw).
std::atomic<uint64_t> g_vq_calls{0};
bool readable(uint64_t a, uint64_t n) {
    if (!a || n > (64ull << 20)) return false;
    struct Region { uint64_t lo = 0, hi = 0; };
    thread_local Region cache[16];
    thread_local unsigned next = 0;
    for (const Region& r : cache)
        if (a >= r.lo && a + n <= r.hi) return true;
    // Walk every region: texwatch splits guest memory into read-only / read-write runs.
    for (uint64_t x = a; x < a + n;) {
        ++g_vq_calls;
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(reinterpret_cast<void*>(x), &mi, sizeof mi) || mi.State != MEM_COMMIT ||
            (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        uint64_t b = reinterpret_cast<uint64_t>(mi.BaseAddress), e = b + mi.RegionSize;
        cache[next++ % 16] = {b, e};
        x = e;
    }
    return true;
}

template <size_t N>
void copy_dw(uint32_t (&dst)[N], uint64_t src) {
    if (src && readable(src, N * 4)) std::memcpy(dst, reinterpret_cast<const void*>(src), N * 4);
    else std::memset(dst, 0, N * 4);
}

// ---- program pointer -> pool index ----
SRWLOCK g_prog_lock = SRWLOCK_INIT;
std::unordered_map<uint64_t, int>* g_prog_index;
int pool_index(uint64_t prog, uint64_t pool, int n, int stride, int tag) {
    if (!prog) return -1;
    uint64_t key = prog ^ (uint64_t(tag) << 60);
    AcquireSRWLockShared(&g_prog_lock);
    auto it = g_prog_index->find(key);
    int r = it != g_prog_index->end() ? it->second : -2;
    ReleaseSRWLockShared(&g_prog_lock);
    if (r != -2) return r;
    r = -1;
    for (int i = 0; i < n && r < 0; ++i)
        for (int o = 0; o < stride; o += 8)
            if (*reinterpret_cast<uint64_t*>(g_base + pool + uint64_t(i) * stride + o) == prog) { r = i; break; }
    AcquireSRWLockExclusive(&g_prog_lock);
    (*g_prog_index)[key] = r;
    ReleaseSRWLockExclusive(&g_prog_lock);
    if (r < 0) std::fprintf(stderr, "render: program %llx not in the %s pool\n", (unsigned long long)prog, tag ? "VS" : "PS");
    return r;
}

// ---- observers (SysV, original argument registers) ----
#define OBS(name) DCP_SYSV void name(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
#define UNUSED_ARGS (void)a0, (void)a1, (void)a2, (void)a3, (void)a4, (void)a5
struct Locked {
    Locked() { AcquireSRWLockExclusive(&g_state_lock); }
    ~Locked() { ReleaseSRWLockExclusive(&g_state_lock); }
};

OBS(obs_set_ps) { UNUSED_ARGS; int i = pool_index(a1, 0xe97350, 60, 0x20, 0); Locked l; state_of(a0 - kCueOffset).ps = i; }
OBS(obs_set_vs) { UNUSED_ARGS; int i = pool_index(a1, 0xe97ad0, 25, 0x30, 1); Locked l; state_of(a0 - kCueOffset).vs = i; }
int stage_ix(uint64_t stage) { return stage == 1 ? 0 : stage == 2 ? 1 : -1; }
OBS(obs_set_cb) {  // (cue, stage, slot, V#*)
    UNUSED_ARGS;
    int s = stage_ix(a1);
    if (s < 0 || a2 >= 8) return;
    Locked l;
    copy_dw(state_of(a0 - kCueOffset).cb[s][a2].w, a3);
}
OBS(obs_set_tex) {  // (cue, stage, slot, T#*)
    UNUSED_ARGS;
    if (a1 != 1 || a2 >= 2) return;
    Locked l;
    copy_dw(state_of(a0 - kCueOffset).tex[a2], a3);
}
OBS(obs_set_samp) {
    UNUSED_ARGS;
    if (a1 != 1 || a2 >= 2) return;
    Locked l;
    copy_dw(state_of(a0 - kCueOffset).samp[a2], a3);
}
OBS(obs_set_vb) {  // (cue, stage, first, count, V#[])
    UNUSED_ARGS;
    if (a1 != 2 || a2 + a3 > 16) return;
    Locked l;
    State& s = state_of(a0 - kCueOffset);
    for (uint64_t i = 0; i < a3; ++i) copy_dw(s.vb[a2 + i].w, a4 ? a4 + i * 16 : 0);
    s.nvb = int(a2 + a3);
}
OBS(obs_set_blend) { UNUSED_ARGS; if (a1 == 0) { Locked l; state_of(a0).blend = uint32_t(a2); } }
// Dcb_InitDefaultHardwareState(dcb): on the GPU this resets the context registers, CB_BLEND0_CONTROL to 0 (blend off).
// The layer clear (VS24/PS51, FUN_0059a930) never sets blend and relies on it: without this reset it inherited the
// last blend this DCB used, so after leaving a game for the title the intro logos piled up (bug reports 20 / 30).
OBS(obs_init_default_state) { UNUSED_ARGS; Locked l; state_of(a0).blend = 0; }
OBS(obs_set_prim) { UNUSED_ARGS; Locked l; state_of(a0).prim = uint32_t(a1) & 0x3f; }
OBS(obs_set_index_size) { UNUSED_ARGS; Locked l; state_of(a0).index_size = uint32_t(a1) & 1; }
OBS(obs_set_scissor) {  // (dcb, x0, y0, x1, y1): PA_SC_SCREEN_SCISSOR
    UNUSED_ARGS;
    Locked l;
    State& s = state_of(a0);
    s.scissor_on = true;
    s.scissor[0] = int32_t(a1 & 0xffff), s.scissor[1] = int32_t(a2 & 0xffff);
    s.scissor[2] = int32_t(a3 & 0xffff), s.scissor[3] = int32_t(a4 & 0xffff);
}
OBS(obs_set_rt) {  // (dcb, slot, Gnm::RenderTarget*): CB_COLOR_BASE = dword0 << 8; packed size 0x48 bytes on (GUESS)
    UNUSED_ARGS;
    if (a1 != 0) return;
    uint32_t d[20];
    copy_dw(d, a2);
    Locked l;
    State& s = state_of(a0);
    s.rt = uint64_t(d[0]) << 8;
    uint32_t packed = d[18], w = (packed & 0x3fff) + 1, h = (packed >> 14 & 0x3fff) + 1;
    if (a2 && w >= 16 && h >= 16 && w <= 4096 && h <= 4096) s.rt_w = w, s.rt_h = h;
    else s.rt_w = 1920, s.rt_h = 1080;
}
#define OBS_CLS(name, c) OBS(name) { UNUSED_ARGS; t_cls = c; }
OBS_CLS(obs_exec_sprite, Cls::Sprite)
OBS_CLS(obs_exec_x1, Cls::X1)
OBS_CLS(obs_exec_x2, Cls::X2)
OBS_CLS(obs_exec_x3, Cls::X3)
OBS_CLS(obs_exec_x4, Cls::X4)
OBS_CLS(obs_exec_a, Cls::A)
OBS_CLS(obs_exec_b, Cls::B)
OBS_CLS(obs_layer_replay, Cls::Layer)

// ---- hook installation ----
struct HookSpec {
    uint32_t addr;
    void* observer;
    uint8_t ins[4];  // lengths of the displaced instructions (0-terminated)
    bool jcc_last;   // last displaced instruction is a jcc rel32 (re-targeted in the relay)
};
// Standard prologue: push rbp (55) / mov rbp,rsp (48 89 e5) / push r1x (41 5x), or push rbx..rdi (5x) when ins[2] == 1.
const HookSpec kHooks[] = {
    {0x5650d0, (void*)&obs_set_vs, {1, 3, 2}, false},       // Cue_SetVsShader(cue, VS entry+0x18, 0, fetch, ...)
    {0x565280, (void*)&obs_set_ps, {1, 3, 2}, false},       // Cue_SetPsShader(cue, PS entry+0x18, ...)
    {0x5646d0, (void*)&obs_set_cb, {1, 3, 2}, false},       // Cue_SetConstantBuffer(cue, stage, slot, V#*)
    {0x564030, (void*)&obs_set_tex, {1, 3, 2}, false},      // Cue_SetTexture(cue, stage, slot, T#*)
    {0x5644b0, (void*)&obs_set_samp, {1, 3, 2}, false},     // Cue_SetSampler(cue, stage, slot, S#*)
    {0x564b30, (void*)&obs_set_vb, {2, 2, 6}, true},        // Cue_SetVertexBuffers: mov eax,ecx; test; je
    {0x55ffb0, (void*)&obs_set_blend, {1, 3, 2}, false},    // Dcb_SetBlendControl(dcb, rt, word)
    {0x55fef0, (void*)&obs_init_default_state, {1, 3, 1}, false},  // Dcb_InitDefaultHardwareState(dcb): push rbx
    {0x5606a0, (void*)&obs_set_prim, {1, 3, 2}, false},     // Dcb_SetPrimitiveType(dcb, prim)
    {0x560970, (void*)&obs_set_index_size, {1, 3, 2}, false},
    {0x5612d0, (void*)&obs_set_scissor, {1, 3, 2}, false},  // Dcb_SetScreenScissor(dcb, x0, y0, x1, y1)
    {0x638870, (void*)&obs_set_rt, {1, 3, 2}, false},       // Dcb_SetRenderTarget(dcb, slot, rt*)
    {0x5a7260, (void*)&obs_exec_sprite, {1, 3, 2}, false},
    {0x59f7b0, (void*)&obs_exec_x1, {1, 3, 2}, false},
    {0x5a0330, (void*)&obs_exec_x2, {1, 3, 2}, false},
    {0x5a12e0, (void*)&obs_exec_x3, {1, 3, 2}, false},
    {0x5a15e0, (void*)&obs_exec_x4, {1, 3, 2}, false},
    {0x07ddb0, (void*)&obs_exec_a, {1, 3, 2}, false},
    {0x07eb40, (void*)&obs_exec_b, {1, 3, 2}, false},
    {0x5a3a00, (void*)&obs_layer_replay, {1, 3, 2}, false},
};

bool prologue_ok(const uint8_t* p, const HookSpec& h) {
    if (h.jcc_last) return p[0] == 0x89 && p[1] == 0xc8 && p[2] == 0x85 && p[3] == 0xc0 && p[4] == 0x0f && p[5] == 0x84;
    bool head = p[0] == 0x55 && p[1] == 0x48 && p[2] == 0x89 && p[3] == 0xe5;
    if (h.ins[2] == 1) return head && (p[4] & 0xf8) == 0x50;
    return head && p[4] == 0x41 && (p[5] & 0xf0) == 0x50;
}

uint8_t* alloc_near(uint64_t from, size_t size) {
    for (uint64_t a = (from + 0xffff) & ~0xffffull; a < from + (1ull << 30); a += 0x10000)
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
            return static_cast<uint8_t*>(p);
    return nullptr;
}

struct Emitter {
    uint8_t* p;
    void b(std::initializer_list<uint8_t> v) { for (uint8_t x : v) *p++ = x; }
    void q(uint64_t v) { std::memcpy(p, &v, 8); p += 8; }
    void d(int32_t v) { std::memcpy(p, &v, 4); p += 4; }
    void jmp_abs(uint64_t t) { b({0xff, 0x25, 0, 0, 0, 0}); q(t); }
};

bool install_one(const HookSpec& h, Emitter& e) {
    uint8_t* target = g_base + h.addr;
    if (!prologue_ok(target, h)) return false;
    int len = 0;
    for (uint8_t n : h.ins) len += n;
    uint8_t* relay = e.p;
    e.b({0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51});  // push rdi rsi rdx rcx r8 r9
    e.b({0x48, 0x83, 0xec, 0x08});                          // sub rsp, 8 (16-byte alignment at the call)
    e.b({0x48, 0xb8});                                      // mov rax, observer
    e.q(reinterpret_cast<uint64_t>(h.observer));
    e.b({0xff, 0xd0});                                      // call rax
    e.b({0x48, 0x83, 0xc4, 0x08});
    e.b({0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f});  // pop r9 r8 rcx rdx rsi rdi
    int off = 0;
    for (int k = 0; k < 4 && h.ins[k]; ++k) {
        int n = h.ins[k];
        bool last = k == 3 || !h.ins[k + 1];
        if (last && h.jcc_last) {  // 0f 8x rel32 -> same jcc to an absolute jump stub
            int32_t rel;
            std::memcpy(&rel, target + off + 2, 4);
            uint64_t dest = reinterpret_cast<uint64_t>(target + off + n) + int64_t(rel);
            e.b({0x0f, target[off + 1]});
            e.d(14);                                        // skip the fall-through jmp_abs below
            e.jmp_abs(reinterpret_cast<uint64_t>(target + len));
            e.jmp_abs(dest);
            off += n;
            goto patch;
        }
        std::memcpy(e.p, target + off, n);
        e.p += n;
        off += n;
    }
    e.jmp_abs(reinterpret_cast<uint64_t>(target + len));
patch:
    int64_t rel = int64_t(relay - (target + 5));
    if (rel != int32_t(rel)) return false;
    DWORD old;
    VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xe9;
    int32_t r32 = int32_t(rel);
    std::memcpy(target + 1, &r32, 4);
    for (int i = 5; i < len; ++i) target[i] = 0x90;
    VirtualProtect(target, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, len);
    return true;
}

// ---- draw records and frames ----
std::mutex g_rec_mutex;
std::unordered_set<DrawRec*>* g_pending;     // recorded, marker not yet submitted
std::vector<std::unique_ptr<DrawRec>>* g_cur; // submitted since the last flip
std::mutex g_q_mutex;
std::condition_variable g_q_cv;
std::deque<std::unique_ptr<Frame>>* g_queue;
uint64_t g_display[16];
int g_ndisplay = 0;
std::atomic<uint64_t> g_flip{0};

uint64_t vertices_needed(const DrawRec& r) {
    if (!r.indexed) return r.count;
    uint32_t mx = 0;
    for (uint32_t i : r.indices) mx = i > mx ? i : mx;
    return r.indices.empty() ? 0 : uint64_t(mx) + 1;
}

void copy_bytes(std::vector<uint8_t>& out, uint64_t a, uint64_t n) {
    if (n && readable(a, n)) out.assign(reinterpret_cast<const uint8_t*>(a), reinterpret_cast<const uint8_t*>(a) + n);
}

}  // namespace

const char* cls_name(Cls c) {
    static const char* n[] = {"none", "layer", "sprite", "X1", "X2", "X3", "X4", "A", "B"};
    return unsigned(c) < unsigned(Cls::Count) ? n[unsigned(c)] : "?";
}

bool capture_enabled() { return g_enabled; }

uint32_t sprite_ps_table(uint32_t perm) {
    if (!g_base || perm >= 48) return 0xffffffffu;
    return reinterpret_cast<const uint32_t*>(g_base + 0x79f890)[perm];
}

bool install_hooks(uint8_t* base) {
    g_base = base;
    g_states = new std::unordered_map<uint64_t, State>();
    g_prog_index = new std::unordered_map<uint64_t, int>();
    g_pending = new std::unordered_set<DrawRec*>();
    g_cur = new std::vector<std::unique_ptr<DrawRec>>();
    g_queue = new std::deque<std::unique_ptr<Frame>>();
    for (const HookSpec& h : kHooks)
        if (!prologue_ok(base + h.addr, h)) {
            std::fprintf(stderr, "render: unexpected prologue at eboot+0x%x; hooks not installed\n", h.addr);
            return false;
        }
    uint8_t* mem = alloc_near(reinterpret_cast<uint64_t>(base) + 0x1000000, 0x10000);
    if (!mem) { std::fprintf(stderr, "render: no relay memory near the image\n"); return false; }
    Emitter e{mem};
    for (const HookSpec& h : kHooks)
        if (!install_one(h, e)) {
            std::fprintf(stderr, "render: hook at eboot+0x%x failed\n", h.addr);
            return false;
        }
    FlushInstructionCache(GetCurrentProcess(), mem, 0x10000);
    g_enabled = true;
    std::fprintf(stderr, "render: %zu hooks installed\n", sizeof kHooks / sizeof kHooks[0]);
    return true;
}

std::atomic<uint64_t> g_draw_ticks{0}, g_draw_calls{0};
void on_draw_impl(uint32_t* cmd, uint32_t size, uint32_t count, uint64_t index_addr, bool indexed);

void on_draw(uint32_t* cmd, uint32_t size, uint32_t count, uint64_t index_addr, bool indexed) {
    if (!g_enabled || !cmd || size < 4) return;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    on_draw_impl(cmd, size, count, index_addr, indexed);
    QueryPerformanceCounter(&t1);
    g_draw_ticks += uint64_t(t1.QuadPart - t0.QuadPart);
    ++g_draw_calls;
}

void on_draw_impl(uint32_t* cmd, uint32_t size, uint32_t count, uint64_t index_addr, bool indexed) {
    auto r = std::make_unique<DrawRec>();
    State s;
    {
        Locked l;
        auto it = g_states->find(t_dcb);
        if (it == g_states->end()) return;
        s = it->second;
    }
    r->cls = t_cls;
    r->vs = s.vs, r->ps = s.ps, r->blend = s.blend, r->prim = s.prim, r->count = count, r->indexed = indexed;
    r->scissor_on = s.scissor_on;
    std::memcpy(r->scissor, s.scissor, sizeof s.scissor);
    r->rt = s.rt, r->rt_w = s.rt_w, r->rt_h = s.rt_h;
    std::memcpy(r->tex, s.tex, sizeof s.tex);
    std::memcpy(r->samp, s.samp, sizeof s.samp);
    if (indexed) {
        uint32_t isz = s.index_size ? 4 : 2;
        if (count > (1u << 20) || !readable(index_addr, uint64_t(count) * isz)) return;
        r->indices.resize(count);
        for (uint32_t i = 0; i < count; ++i)
            r->indices[i] = isz == 4 ? reinterpret_cast<const uint32_t*>(index_addr)[i]
                                     : reinterpret_cast<const uint16_t*>(index_addr)[i];
    }
    uint64_t nv = vertices_needed(*r);
    r->vb.resize(size_t(s.nvb));
    for (int i = 0; i < s.nvb; ++i) {
        r->vb[i].v = s.vb[i];
        uint64_t stride = s.vb[i].stride() ? s.vb[i].stride() : 16;
        copy_bytes(r->vb[i].bytes, s.vb[i].base(), nv ? (nv - 1) * stride + 16 : 0);
        if (r->vb[i].bytes.empty() && nv) copy_bytes(r->vb[i].bytes, s.vb[i].base(), nv * stride);
    }
    for (int st = 0; st < 2; ++st)
        for (int k = 0; k < 8; ++k) {
            const VSharp& v = s.cb[st][k];
            uint64_t n = uint64_t(v.records()) * (v.stride() ? v.stride() : 1);
            if (n > 4096) n = 4096;
            copy_bytes(st ? r->vcb[k] : r->pcb[k], v.base(), n);
        }
    DrawRec* raw = r.release();
    {
        std::lock_guard<std::mutex> g(g_rec_mutex);
        g_pending->insert(raw);
    }
    cmd[0] = 0xC0001000u | ((size - 2) << 16);  // NOP covering the packet, payload = marker
    cmd[1] = kMarkerMagic;
    uint64_t p = reinterpret_cast<uint64_t>(raw);
    cmd[2] = uint32_t(p), cmd[3] = uint32_t(p >> 32);
}

void on_marker(const uint32_t* payload, uint32_t ndw) {
    if (!g_enabled || ndw < 3 || payload[0] != kMarkerMagic) return;
    DrawRec* r = reinterpret_cast<DrawRec*>(uint64_t(payload[1]) | uint64_t(payload[2]) << 32);
    std::lock_guard<std::mutex> g(g_rec_mutex);
    if (g_pending->erase(r)) g_cur->emplace_back(r);
}

void set_display_buffers(void* const* addrs, int start, int n) {
    for (int i = 0; i < n && start + i < 16; ++i) g_display[start + i] = reinterpret_cast<uint64_t>(addrs[i]);
    if (start + n > g_ndisplay) g_ndisplay = start + n < 16 ? start + n : 16;
}

void on_flip(uint32_t display_index) {
    if (!g_enabled) return;
    auto f = std::make_unique<Frame>();
    f->flip = ++g_flip;
    if (f->flip % 60 == 0) {
        LARGE_INTEGER fq;
        QueryPerformanceFrequency(&fq);
        std::fprintf(stderr, "render: capture %llu draws, %.1f ms total, %llu VirtualQuery (last 60 flips)\n",
                     (unsigned long long)g_draw_calls.exchange(0), g_draw_ticks.exchange(0) * 1000.0 / fq.QuadPart,
                     (unsigned long long)g_vq_calls.exchange(0));
    }
    f->vblank = dcp::hle::vblank_count_public();
    f->display = display_index < 16 ? g_display[display_index] : 0;
    {
        std::lock_guard<std::mutex> g(g_rec_mutex);
        f->draws.swap(*g_cur);
        if (g_pending->size() > 200000) {  // markers never submitted (should not happen): drop them
            std::fprintf(stderr, "render: dropping %zu unsubmitted draws\n", g_pending->size());
            for (DrawRec* r : *g_pending) delete r;
            g_pending->clear();
        }
    }
    {
        std::lock_guard<std::mutex> g(g_q_mutex);
        while (g_queue->size() >= 3) g_queue->pop_front();  // consumer is behind: keep the newest frames
        g_queue->push_back(std::move(f));
    }
    g_q_cv.notify_one();
}

std::unique_ptr<Frame> take_frame(uint32_t timeout_ms) {
    if (!g_queue) return nullptr;
    std::unique_lock<std::mutex> g(g_q_mutex);
    if (g_queue->empty() && timeout_ms)
        g_q_cv.wait_for(g, std::chrono::milliseconds(timeout_ms), [] { return !g_queue->empty(); });
    if (g_queue->empty()) return nullptr;
    auto f = std::move(g_queue->front());
    g_queue->pop_front();
    return f;
}

}  // namespace dcp::render
