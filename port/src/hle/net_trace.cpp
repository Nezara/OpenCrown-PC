// Online session trace, see net_trace.h. Addresses are eboot-relative (q.py); field offsets from
// notes/map/town_session.md 2-3 and the asm quoted next to each site. Diagnostics only: nothing here changes what the
// game decides (every hook runs the displaced instruction unchanged and only reads memory).
#include "net_trace.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <windows.h>

#include "hle.h"

namespace dcp::net_trace {
namespace {

uint64_t g_base = 0;
constexpr uint64_t kImageSpan = 0x2000000;  // eboot image is ~0xc3xxxx; for "eboot+" return-address printing only

// ---- safe reads (game objects come and go; the watcher thread reads concurrently) ----
bool readable(uint64_t a, size_t n) {
    thread_local uint64_t lo = 0, hi = 0;
    if (a >= lo && a + n <= hi) return true;
    if (a < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof mi) || mi.State != MEM_COMMIT ||
        (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    lo = reinterpret_cast<uint64_t>(mi.BaseAddress), hi = lo + mi.RegionSize;
    return a + n <= hi;
}
template <class T> T rd(uint64_t a, T def = T(-1)) {
    if (!readable(a, sizeof(T))) return def;
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof v);
    return v;
}

// Singletons: game sys = FUN_0007cf50(0) = [0xc1dc40]; Act = gs+0x88; platform sys ("ps": DcNetIf at +0x16910, error
// code +0x16a38, task mode +0x5b / step +0x5c) = gs+0x78; platform singleton FUN_00016b10 = [0xc17758]: DcNet +0xb0,
// DcNetMatching +0xc8.
uint64_t gs() { return rd<uint64_t>(g_base + 0xc1dc40, 0); }
uint64_t act() { uint64_t g = gs(); return g ? rd<uint64_t>(g + 0x88, 0) : 0; }
uint64_t psys() { uint64_t g = gs(); return g ? rd<uint64_t>(g + 0x78, 0) : 0; }
uint64_t plat() { return rd<uint64_t>(g_base + 0xc17758, 0); }

unsigned long long vb() { return (unsigned long long)dcp::hle::vblank_count_public(); }

struct Line {  // one log line, written with a single fprintf
    char b[1024];
    int n = 0;
    void f(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        int k = std::vsnprintf(b + n, sizeof b - n, fmt, ap);
        va_end(ap);
        if (k > 0) n = std::min<int>(int(sizeof b) - 1, n + k);
    }
    void addr(const char* what, uint64_t a) {
        if (a >= g_base && a < g_base + kImageSpan) f(" %s eboot+0x%llx", what, (unsigned long long)(a - g_base));
        else f(" %s 0x%llx", what, (unsigned long long)a);
    }
    void hex(const char* what, uint64_t a, size_t len) {
        f(" %s ", what);
        if (!readable(a, len)) { f("?"); return; }
        for (size_t i = 0; i < len; ++i) f("%02x", reinterpret_cast<const uint8_t*>(a)[i]);
    }
    void str(uint64_t a) {
        char s[100];
        size_t k = 0;
        if (readable(a, 1))
            for (; k < sizeof s - 1 && readable(a + k, 1); ++k) {
                char c = *reinterpret_cast<const char*>(a + k);
                if (!c) break;
                s[k] = (c >= 0x20 && c < 0x7f) ? c : '.';
            }
        s[k] = 0;
        f(" '%s'", s);
    }
    void state() {  // Act and platform-sys state at the moment of the call
        uint64_t a = act(), p = psys();
        if (a)
            f(" | act m%d.%d.%d kind %d round %d sub %d", rd<uint8_t>(a + 0x5b), rd<uint8_t>(a + 0x5c),
              rd<uint8_t>(a + 0x5d), rd<int32_t>(a + 0xe8), rd<uint8_t>(a + 0x108), rd<uint8_t>(a + 0xf0));
        if (p)
            f(" | ps m%d.%d err %d flags 0x%x slot %d gen %d", rd<uint8_t>(p + 0x5b), rd<uint8_t>(p + 0x5c),
              rd<int32_t>(p + 0x16a38), rd<uint32_t>(p + 0x16908), rd<uint8_t>(p + 0x168f0), rd<uint8_t>(p + 0x168f1));
    }
    void head(const char* what) { f("NETTRACE v%llu t%lu %s", vb(), GetCurrentThreadId(), what); }
    void out() {
        if (n < int(sizeof b) - 1) b[n++] = '\n';
        b[n] = 0;
        std::fputs(b, stderr);
    }
};

// ---- per-site callbacks ----
using Fn = void (*)(CONTEXT*, const char* name);

void on_call(CONTEXT* c, const char* name) {  // function entry: [rsp] = return address
    Line l;
    l.head(name);
    l.addr("ret", rd<uint64_t>(c->Rsp, 0));
    l.f(" rdi=%llx rsi=%llx", (unsigned long long)c->Rdi, (unsigned long long)c->Rsi);
    l.state();
    l.out();
}

bool changed(uint64_t obj, uint64_t site, uint64_t key);
void on_slots(CONTEXT* c, const char* name) {  // a06c0(sys, localSlot, bytes[4]); per-frame caller a0455 deduped
    uint64_t ret = rd<uint64_t>(c->Rsp, 0), p = c->Rdi;
    uint64_t key = (c->Rsi & 0xff) | uint64_t(rd<uint32_t>(c->Rdx, 0)) << 8;
    for (int i = 0; i < 4; ++i) key ^= uint64_t(rd<uint8_t>(p + 0x16918 + 0x1c * i)) << (40 + 6 * i);
    if (!changed(ret, 0xa06c0, key)) return;
    Line l;
    l.head(name);
    l.addr("ret", rd<uint64_t>(c->Rsp, 0));
    l.f(" local %d", int(c->Rsi));
    l.hex("connected", c->Rdx, 4);
    l.f(" table");
    for (int i = 0; i < 4; ++i) l.f(" %d", rd<int32_t>(p + 0x16918 + 0x1c * i));
    l.state();
    l.out();
}

void on_change_mode(CONTEXT* c, const char* name) {  // 43c6b0(act, mode): leaves (host-off + 9d9e0) on a real change
    Line l;
    l.head(name);
    l.addr("ret", rd<uint64_t>(c->Rsp, 0));
    l.f(" new mode %d", int(c->Rsi & 0xff));
    l.state();
    l.out();
}

void on_net_error(CONTEXT* c, const char* name) {
    Line l;
    l.head(name);
    l.addr("ret", rd<uint64_t>(c->Rsp, 0));
    uint64_t p = psys();
    l.f(" code %d", p ? rd<int32_t>(p + 0x16a38) : -1);
    l.state();
    l.out();
}

// Debug text helpers: GameServer 28770(task, fmt, ...) ("Connect Start", "Connect Timeout %d") and Act 4364d0(act,
// fmt, ...) (AC_STEP_* / STEP_SETUP_* incl. "... Time out"). Consecutive repeats are counted, not printed.
void on_text(CONTEXT* c, const char* name) {
    static uint64_t last_fmt = 0, last_a = 0;
    static int repeats = 0;
    if (c->Rsi == last_fmt && c->Rdx == last_a) { ++repeats; return; }
    Line l;
    l.head(name);
    if (repeats) l.f(" (previous x%d)", repeats + 1);
    last_fmt = c->Rsi, last_a = c->Rdx, repeats = 0;
    l.str(c->Rsi);
    l.f(" args %lld %lld %lld", (long long)c->Rdx, (long long)c->Rcx, (long long)c->R8);
    l.addr("ret", rd<uint64_t>(c->Rsp, 0));
    l.state();
    l.out();
}

// Task bodies called every frame: print only when the task's state tuple changes.
struct Seen { uint64_t obj = 0; uint64_t key = ~0ull; };
Seen g_seen[32];
SRWLOCK g_seen_lock = SRWLOCK_INIT;
bool changed(uint64_t obj, uint64_t site, uint64_t key) {
    AcquireSRWLockExclusive(&g_seen_lock);
    Seen* s = nullptr;
    for (Seen& x : g_seen)
        if (x.obj == (obj ^ site)) { s = &x; break; }
    if (!s)
        for (Seen& x : g_seen)
            if (!x.obj) { s = &x, s->obj = obj ^ site; break; }
    bool ch = s && s->key != key;
    if (s) s->key = key;
    ReleaseSRWLockExclusive(&g_seen_lock);
    return ch;
}

// 9f550 (joiner connect check, ps mode 3), 9f1e0 (peer side), 9d8a0 (peer-lost watch): rdi = platform sys.
void on_ps_task(CONTEXT* c, const char* name) {
    uint64_t p = c->Rdi;
    uint64_t key = uint64_t(rd<uint8_t>(p + 0x5b)) | uint64_t(rd<uint8_t>(p + 0x5c)) << 8 |
                   uint64_t(rd<uint8_t>(p + 0x5d)) << 16 | uint64_t(rd<uint8_t>(p + 0x16a38)) << 24 |
                   uint64_t(rd<uint8_t>(p + 0x168f0)) << 32 | uint64_t(rd<uint8_t>(p + 0x168f1)) << 40 |
                   uint64_t(rd<uint8_t>(p + 0x16a40)) << 48 | uint64_t(rd<uint8_t>(p + 0x16908)) << 56;
    if (!changed(p, uint64_t(uintptr_t(name)), key)) return;
    Line l;
    l.head(name);
    l.f(" ps=%llx step %d timer %d f4 %d f8 %d a40 %d a44/48 %d/%d retry %d", (unsigned long long)p, rd<uint8_t>(p + 0x5c),
        rd<int32_t>(p + 0x64), rd<int32_t>(p + 0x168f4), rd<int32_t>(p + 0x168f8), rd<int32_t>(p + 0x16a40),
        rd<uint8_t>(p + 0x16a44), rd<uint8_t>(p + 0x16a48), rd<int32_t>(p + 0x169ac));
    l.f(" table");
    for (int i = 0; i < 4; ++i) l.f(" %d/%d", rd<int32_t>(p + 0x16918 + 0x1c * i), rd<uint8_t>(p + 0x16920 + 0x1c * i));
    l.state();
    l.out();
}

void on_gs_connect(CONTEXT* c, const char* name) {  // 28330(gameserver task): per-peer queue at +0x8f78..+0x8f80
    uint64_t t = c->Rdi;
    uint64_t q0 = rd<uint64_t>(t + 0x8f78, 0), q1 = rd<uint64_t>(t + 0x8f80, 0);
    int head = q0 && q1 > q0 ? rd<int8_t>(q0) : -1;
    uint64_t key = uint64_t(rd<uint8_t>(t + 0x5b)) | uint64_t(rd<uint8_t>(t + 0x5c)) << 8 | uint64_t(uint8_t(head)) << 16 |
                   uint64_t((q1 - q0) & 0xff) << 24;
    if (!changed(t, 0x28330, key)) return;
    Line l;
    l.head(name);
    l.f(" task=%llx mode %d step %d queue %lld head %d timer %d", (unsigned long long)t, rd<uint8_t>(t + 0x5b),
        rd<uint8_t>(t + 0x5c), (long long)(q1 - q0), head, rd<int32_t>(t + 0x64));
    l.state();
    l.out();
}

// 9e980 NetCtrl::ReceiveCommand, at 0x9eaf6 (mov eax,[rbp-0x58]): header = [rbp-0x48] (0x14 B: +8 s16 id, +0xa u16
// object, +0xc dest mask, +0xe round, +0xf sub-id, +0x10 sender player, +0x11 generation (vs ps+0x168f1), +0x12
// target slot, +0x13 sender slot); [rbp-0x58] bit 0 = drop (generation / not-online-and-not-0x3e9..0x3ed / player
// mismatch); dl = object 0 (NetCtrl command -> a1710); r12 = sys. NetCtrl and Act-level commands are always logged,
// object commands only when dropped.
int g_rx_lines = 0, g_rx_obj = 0;
void on_rx(CONTEXT* c, const char* name) {
    uint64_t h = c->Rbp - 0x48, p = c->R12;
    int drop = rd<int32_t>(c->Rbp - 0x58) & 1;
    uint8_t slot = rd<uint8_t>(p + 0x168f0), dest = rd<uint8_t>(h + 0xc);
    bool for_me = slot < 8 && (dest >> slot) & 1;
    bool netctrl = c->Rdx & 0xff;
    uint16_t obj = rd<uint16_t>(h + 0xa);
    const char* kind = netctrl ? "netctrl" : obj == 0xffff ? "scene" : "object";
    if (!netctrl && obj != 0xffff && !drop && for_me) return;
    if (!netctrl && obj != 0xffff && ++g_rx_obj > 300) return;
    if (++g_rx_lines > 4000) return;
    Line l;
    l.head(name);
    l.f(" %s id 0x%x obj 0x%x %s", kind, rd<uint16_t>(h + 8), obj, drop ? "DROP(flag)" : for_me ? "accept" : "DROP(dest)");
    l.f(" tag %d/%d mine %d/%d gen %d mine %d dest 0x%x slot %d->%d player %d", rd<uint8_t>(h + 0xe), rd<uint8_t>(h + 0xf),
        rd<uint8_t>(act() + 0x108), rd<uint8_t>(act() + 0xf0), rd<uint8_t>(h + 0x11), rd<uint8_t>(p + 0x168f1), dest,
        rd<uint8_t>(h + 0x13), rd<uint8_t>(h + 0x12), rd<uint8_t>(h + 0x10));
    l.hex("hdr", h, 0x14);
    l.state();
    l.out();
}

// 9e980 at 0x9ec87: object-command tag decision done, r12b = execute (tag round/sub-id == Act+0x108/+0xf0, or the
// object's state outside 2..5; then CtrlId check). Logged only when refused.
void on_rx_obj(CONTEXT* c, const char* name) {
    if (c->R12 & 1) return;
    if (++g_rx_obj > 300) return;
    uint64_t h = c->Rbp - 0x48, o = rd<uint64_t>(c->Rbp - 0x58, 0);
    Line l;
    l.head(name);
    l.f(" object cmd REFUSED id 0x%x obj 0x%x tag %d/%d mine %d/%d ctrl 0x%x obj-state? ", rd<uint16_t>(h + 8),
        rd<uint16_t>(h + 0xa), rd<uint8_t>(h + 0xe), rd<uint8_t>(h + 0xf), rd<uint8_t>(act() + 0x108),
        rd<uint8_t>(act() + 0xf0), rd<uint32_t>(h + 4));
    l.f("obj=%llx", (unsigned long long)o);
    l.state();
    l.out();
}

// 9d5e0(cmd, stream, sys): writes the 0x14-B header (cmd+8 16 B + cmd+0x18 u32) then cmd->Serialize. Object sends
// (obj id != 0 / 0xffff) are counted, not printed.
int g_tx_lines = 0, g_tx_obj = 0;
void on_tx(CONTEXT* c, const char* name) {
    uint64_t cmd = c->Rdi;
    uint8_t h[0x14];
    for (int i = 0; i < 16; ++i) h[i] = rd<uint8_t>(cmd + 8 + i, 0);
    uint32_t t = rd<uint32_t>(cmd + 0x18, 0);
    std::memcpy(h + 16, &t, 4);
    uint16_t id, obj;
    std::memcpy(&id, h + 8, 2), std::memcpy(&obj, h + 0xa, 2);
    if (obj != 0 && obj != 0xffff) { ++g_tx_obj; return; }
    if (++g_tx_lines > 3000) return;
    Line l;
    l.head(name);
    l.f(" %s id 0x%x dest 0x%x tag %d/%d player %d gen %d target %d slot %d (object sends so far %d) hdr ",
        obj ? "scene" : "netctrl", id, h[0xc], h[0xe], h[0xf], h[0x10], h[0x11], h[0x12], h[0x13], g_tx_obj);
    for (uint8_t x : h) l.f("%02x", x);
    l.addr("ret", rd<uint64_t>(c->Rsp, 0));
    l.state();
    l.out();
}

struct Site {
    uint32_t rva;
    uint8_t len;          // displaced instruction length
    uint8_t expect[8];
    Fn fn;
    const char* name;
    bool lea_rax = false;  // displaced instruction is `lea rax, [rip+d32]`: emulated, no relay
    uint8_t* relay = nullptr;
};

#define PUSH_RBP 1, {0x55}
Site g_sites[] = {
    {0x9d9e0, PUSH_RBP, on_call, "Dissolve(9d9e0)"},
    {0x9bb70, PUSH_RBP, on_call, "LeaveIfSignedIn(9bb70)"},
    {0xbaba0, PUSH_RBP, on_call, "QueueLeaveRoom(baba0)"},
    {0x9c6c0, PUSH_RBP, on_call, "GoOffline(9c6c0)"},
    {0x9c5b0, 7, {0x48, 0x8d, 0x05, 0x69, 0x95, 0xb7, 0x00}, on_call, "HostBitOn(9c5b0)", true},
    {0x9c5d0, 7, {0x48, 0x8d, 0x05, 0x49, 0x95, 0xb7, 0x00}, on_call, "HostBitOff(9c5d0)", true},
    {0xbad20, PUSH_RBP, on_call, "QueueCloseRoom(bad20)"},
    {0x9b8f0, PUSH_RBP, on_call, "DcNetDropPeer(9b8f0)"},
    {0xa2ce0, PUSH_RBP, on_call, "KickSlot(a2ce0)"},
    {0xa06c0, PUSH_RBP, on_slots, "ApplyConnectedSlots(a06c0)"},
    {0x43c6b0, PUSH_RBP, on_change_mode, "Act::ChangeMode(43c6b0)"},
    {0x43f5b0, PUSH_RBP, on_net_error, "Act::OnNetError(43f5b0)"},
    {0x43f570, PUSH_RBP, on_call, "Act::EnterAcceptConnect(43f570)"},
    {0xa0200, PUSH_RBP, on_call, "EnterAcceptConnectIfGuest(a0200)"},
    {0x9f550, PUSH_RBP, on_ps_task, "ConnectCheck(9f550)"},
    {0x9f1e0, PUSH_RBP, on_ps_task, "ConnectCheck2(9f1e0)"},
    {0x9d8a0, PUSH_RBP, on_ps_task, "PeerWatch(9d8a0)"},
    {0x28330, PUSH_RBP, on_gs_connect, "GameServer::ConnectTask(28330)"},
    {0x28770, PUSH_RBP, on_text, "GSLOG"},
    {0x4364d0, PUSH_RBP, on_text, "ACTLOG"},
    {0x9eaf6, 3, {0x8b, 0x45, 0xa8}, on_rx, "RX"},
    {0x9ec87, 3, {0x8b, 0x5d, 0xb4}, on_rx_obj, "RX"},
    {0x9d5e0, PUSH_RBP, on_tx, "TX"},
};
#undef PUSH_RBP

LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    for (Site& s : g_sites) {
        uint64_t at = g_base + s.rva;
        if (!s.relay && !s.lea_rax) continue;
        if (c->Rip != at && c->Rip != at + 1) continue;
        DWORD err = GetLastError();
        s.fn(c, s.name);
        SetLastError(err);
        if (s.lea_rax) {
            int32_t d;
            std::memcpy(&d, s.expect + 3, 4);
            c->Rax = at + 7 + int64_t(d);
            c->Rip = at + 7;
        } else {
            c->Rip = reinterpret_cast<uint64_t>(s.relay);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void write_byte(uint8_t* at, uint8_t v) {
    DWORD old;
    VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old);
    *at = v;
    VirtualProtect(at, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 1);
}

// Watcher: state fields polled every 2 ms, printed on change (catches mode changes made without any hooked call).
DWORD WINAPI watcher(void*) {
    uint64_t last[4] = {~0ull, ~0ull, ~0ull, ~0ull};
    for (;;) {
        Sleep(2);
        uint64_t a = act(), p = psys(), pl = plat();
        uint64_t dn = pl ? rd<uint64_t>(pl + 0xb0, 0) : 0, mt = pl ? rd<uint64_t>(pl + 0xc8, 0) : 0;
        uint64_t k[4] = {
            a ? (uint64_t(rd<uint8_t>(a + 0x5b)) | uint64_t(rd<uint8_t>(a + 0x5c)) << 8 | uint64_t(rd<uint8_t>(a + 0x5d)) << 16 |
                 uint64_t(rd<uint8_t>(a + 0xe8)) << 24 | uint64_t(rd<uint8_t>(a + 0xf0)) << 32 |
                 uint64_t(rd<uint8_t>(a + 0x108)) << 40 | uint64_t(rd<uint8_t>(a + 0x110)) << 48)
              : 0,
            p ? (uint64_t(rd<uint8_t>(p + 0x5b)) | uint64_t(rd<uint8_t>(p + 0x5c)) << 8 | uint64_t(rd<uint32_t>(p + 0x16a38)) << 16 |
                 uint64_t(rd<uint8_t>(p + 0x168f0)) << 48 | uint64_t(rd<uint8_t>(p + 0x168f1)) << 56)
              : 0,
            p ? (uint64_t(rd<uint32_t>(p + 0x16908)) | uint64_t(rd<uint8_t>(p + 0x16918)) << 32 |
                 uint64_t(rd<uint8_t>(p + 0x16918 + 0x1c)) << 40 | uint64_t(rd<uint8_t>(p + 0x16918 + 0x38)) << 48 |
                 uint64_t(rd<uint8_t>(p + 0x16918 + 0x54)) << 56)
              : 0,
            (dn ? (uint64_t(rd<uint32_t>(dn + 0x210)) | uint64_t(rd<uint8_t>(dn + 0xedc)) << 32) : 0) |
                (mt ? uint64_t(rd<uint8_t>(mt + 0x5b)) << 40 : 0),
        };
        if (k[0] == last[0] && k[1] == last[1] && k[2] == last[2] && k[3] == last[3]) continue;
        Line l;
        l.head("STATE");
        if (a)
            l.f(" act m%d.%d.%d kind %d sub %d round %d town-picked %d ec 0x%x", rd<uint8_t>(a + 0x5b), rd<uint8_t>(a + 0x5c),
                rd<uint8_t>(a + 0x5d), rd<int32_t>(a + 0xe8), rd<uint8_t>(a + 0xf0), rd<uint8_t>(a + 0x108),
                rd<uint8_t>(a + 0x110), rd<uint32_t>(a + 0xec));
        if (p) {
            l.f(" | ps m%d.%d err %d slot %d gen %d flags 0x%x table", rd<uint8_t>(p + 0x5b), rd<uint8_t>(p + 0x5c),
                rd<int32_t>(p + 0x16a38), rd<uint8_t>(p + 0x168f0), rd<uint8_t>(p + 0x168f1), rd<uint32_t>(p + 0x16908));
            for (int i = 0; i < 4; ++i) l.f(" %d", rd<int32_t>(p + 0x16918 + 0x1c * i));
        }
        if (dn) l.f(" | dcnet 0x%x edc %d", rd<uint32_t>(dn + 0x210), rd<uint8_t>(dn + 0xedc));
        if (mt) l.f(" | matching m%d", rd<uint8_t>(mt + 0x5b));
        l.out();
        std::memcpy(last, k, sizeof last);
    }
    return 0;
}

}  // namespace

void install(uint8_t* base) {
    const char* e = std::getenv("DCP_NET_TRACE");
    if (!e || !*e || !std::strcmp(e, "0")) return;
    g_base = reinterpret_cast<uint64_t>(base);
    uint8_t* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (!page) { std::fprintf(stderr, "NETTRACE: no relay memory\n"); return; }
    uint8_t* r = page;
    int n = 0;
    AddVectoredExceptionHandler(1, veh);
    for (Site& s : g_sites) {
        uint8_t* at = base + s.rva;
        if (std::memcmp(at, s.expect, s.len)) {
            std::fprintf(stderr, "NETTRACE: unexpected bytes at eboot+0x%x (%s); not hooked\n", s.rva, s.name);
            continue;
        }
        if (!s.lea_rax) {  // relay: displaced instruction, then jmp [rip+0] -> next instruction
            s.relay = r;
            std::memcpy(r, s.expect, s.len), r += s.len;
            const uint8_t j[6] = {0xff, 0x25, 0, 0, 0, 0};
            std::memcpy(r, j, 6), r += 6;
            uint64_t back = g_base + s.rva + s.len;
            std::memcpy(r, &back, 8), r += 8;
            r = reinterpret_cast<uint8_t*>((reinterpret_cast<uint64_t>(r) + 15) & ~15ull);
        }
        FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
        write_byte(at, 0xcc);
        ++n;
    }
    CreateThread(nullptr, 0, watcher, nullptr, 0, nullptr);
    std::fprintf(stderr, "NETTRACE: %d sites hooked, state watcher on\n", n);
}

}  // namespace dcp::net_trace
