// Separate cameras in online adventures.
#include "own_camera.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include <sys/mman.h>

#include "hle.h"
#include "loader/hle.h"
#include "net/rudp.h"

namespace dcp::own_camera {
namespace {

uint64_t g_base = 0;
bool g_filter = true, g_rx = true, g_local = true;
uint64_t g_log_n = 0, g_log_last = ~0ull;
long g_rx_muted[7] = {}, g_filtered = 0, g_local_applied = 0, g_self_applied = 0, g_rx_other = 0;

template <class T> T rd(uint64_t a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof v);
    return v;
}

using Fn1 = DCP_SYSV uint64_t (*)(uint64_t);

uint64_t gs() { return rd<uint64_t>(g_base + 0xc1dc40); }

bool online(uint64_t g) {
    uint64_t ps = rd<uint64_t>(g + 0x78);
    if (!ps || !(rd<uint32_t>(ps + 0x16908) & rd<uint32_t>(g_base + 0xc15b20))) return false;
    int used = 0;
    for (int i = 0; i < 4; ++i) used += rd<int32_t>(ps + 0x16918 + 0x1c * i) != 0xff;
    return used >= 2;
}

int32_t round_of(uint64_t g) {
    uint64_t act = rd<uint64_t>(g + 0x88);
    return act ? rd<int32_t>(act + 0x108) : -1;
}

uint64_t player(uint64_t g, int i) {
    uint64_t mgr = rd<uint64_t>(g + 0x90);
    return mgr && i >= 0 && i < 4 ? rd<uint64_t>(mgr + 0x120 + 0x20 * uint64_t(i)) : 0;
}

uint64_t record(uint64_t g, int i) {
    uint64_t srv = rd<uint64_t>(g + 0x80);
    return srv ? srv + 0xa8 + 0x23b0 * uint64_t(i) : 0;
}

bool local(uint64_t g, int i) {
    uint64_t rec = record(g, i);
    return rec && reinterpret_cast<Fn1>(g_base + 0x271a0)(rec) & 1;
}

bool in_stage(uint64_t g, uint64_t p) {
    uint64_t act = rd<uint64_t>(g + 0x88);
    return p && act && int8_t(rd<uint8_t>(p + 0x103)) > int8_t(0x82) && rd<int32_t>(p + 0x7e4) == rd<int32_t>(act + 0xf0);
}

bool g_room_on = false;

bool gate(uint64_t g) {
    if (!g || !g_room_on || !online(g)) return false;
    int32_t r = round_of(g);
    if (r < 0 || r == 9 || r >= 0xff) return false;
    for (int i = 0; i < 4; ++i)
        if (local(g, i) && in_stage(g, player(g, i))) return true;
    return false;
}

void log_line(uint64_t g) {
    uint64_t v = dcp::hle::vblank_count_public();
    if (!g_log_n || v == g_log_last || v % g_log_n) return;
    g_log_last = v;
    uint64_t ps = rd<uint64_t>(g + 0x78);
    char b[1024];
    int n = std::snprintf(b, sizeof b, "OWNCAM v%llu gate %d online %d round %d stage %d flags 0x%x |",
                          (unsigned long long)v, gate(g), online(g), round_of(g),
                          rd<uint64_t>(g + 0x88) ? rd<int32_t>(rd<uint64_t>(g + 0x88) + 0xf0) : -1,
                          ps ? rd<uint32_t>(ps + 0x16908) : 0);
    for (int i = 0; i < 4 && n < int(sizeof b) - 120; ++i) {
        uint64_t p = player(g, i), rec = record(g, i);
        if (!p) continue;
        n += std::snprintf(b + n, sizeof b - n, " P%d local %d valid %d x %.1f rec %d/0x%x idx %d", i + 1, local(g, i),
                           in_stage(g, p), rd<float>(p + 0x3f0), rec ? rd<int32_t>(rec + 0x21e0) : -9,
                           rec ? rd<uint32_t>(rec + 0x21e4) : 0, rec ? rd<int32_t>(rec + 0x21d8) : -9);
    }
    std::snprintf(b + n, sizeof b - n, " | filtered %ld rx-muted %ld/%ld/%ld/%ld/%ld/%ld type4 %ld sends in-send %ld local %ld\n",
                  g_filtered, g_rx_muted[1], g_rx_muted[2], g_rx_muted[3], g_rx_muted[4], g_rx_muted[5], g_rx_muted[6],
                  g_rx_other, g_self_applied, g_local_applied);
    std::fputs(b, stderr);
}

void lock_tick(uint64_t g);
void room_tick(uint64_t g);

DCP_SYSV uint64_t cam_player(uint64_t mgr, uint32_t i, uint64_t caller) {
    uint64_t p = i > 3 ? 0 : rd<uint64_t>(mgr + 0x120 + 0x20 * uint64_t(i));
    uint64_t g = gs();
    if (g && i == 0) {
        room_tick(g);
        log_line(g);
        if (gate(g)) lock_tick(g);
    }
    if (!p || !g_filter) return p;
    if (caller && (caller < g_base + 0x370f00 || caller >= g_base + 0x377000)) return p;
    if (!gate(g) || local(g, int(i))) return p;
    ++g_filtered;
    return 0;
}

thread_local int t_in_send = 0;
uint64_t g_rx_orig = 0;
int g_rxlog = 0;

DCP_SYSV uint64_t rx_mute(uint32_t type_minus_1, uint64_t cmd) {
    if (g_rxlog > 0 && gs() && online(gs())) {
        --g_rxlog;
        uint64_t g = gs(), ps = g ? rd<uint64_t>(g + 0x78) : 0;
        char b[240];
        int n = std::snprintf(b, sizeof b, "OWNCAM-RX v%llu type %u gate %d my slot %d in-send %d hdr",
                              (unsigned long long)dcp::hle::vblank_count_public(), type_minus_1 + 1, gate(g),
                              ps ? rd<uint8_t>(ps + 0x168f0) : -1, t_in_send);
        for (int i = 0; i < 0x14; ++i) n += std::snprintf(b + n, sizeof b - n, "%s%02x", i % 4 ? "" : " ", rd<uint8_t>(cmd + 8 + i));
        std::snprintf(b + n, sizeof b - n, " b8 %.1f %.1f %.1f\n", rd<float>(cmd + 0xb8), rd<float>(cmd + 0xbc), rd<float>(cmd + 0xc0));
        std::fputs(b, stderr);
    }
    if (t_in_send) { ++g_self_applied; return 0; }
    if (!g_rx || type_minus_1 == 3 || !gate(gs())) { g_rx_other += type_minus_1 == 3; return 0; }
    if (type_minus_1 < 6) ++g_rx_muted[type_minus_1 + 1];
    return 1;
}

using Fn2 = DCP_SYSV uint64_t (*)(uint64_t, uint64_t);
using Fn4 = DCP_SYSV uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t);
DCP_SYSV uint64_t own_send(uint64_t cam, uint64_t cmd, uint64_t a2, uint64_t a3, uint64_t fn) {
    if (!g_local || !gate(gs())) return reinterpret_cast<Fn4>(fn)(cam, cmd, a2, a3);
    t_in_send = 1;
    uint64_t r = reinterpret_cast<Fn4>(fn)(cam, cmd, a2, a3);
    t_in_send = 0;
    reinterpret_cast<Fn2>(g_rx_orig)(cam, cmd), ++g_local_applied;
    return r;
}

constexpr uint32_t kMagic = 0x3157434f;
constexpr int kMaxRunning = 16;
constexpr int32_t kNoLimit = 10000000;
constexpr uint64_t kStaleMs = 120000, kStaleDoneMs = 60000;

struct Rect {
    int32_t l = 0, r = 0, a = 0, b = 0;
    int32_t centre() const { return int32_t((int64_t(l) + r) / 2); }
};

struct Heading {
    int32_t prev = 0;
    uint64_t prev_ms = 0;
    int32_t move = 0;
    void update(int32_t c, uint64_t t) {
        if (!prev_ms) prev = c, prev_ms = t;
        move = c - prev;
        if (t - prev_ms >= 1000) prev = c, prev_ms = t;
    }
};
struct Running {
    int32_t id;
    int8_t dir;
};
struct Group {
    uint64_t obj;
    int32_t id;
    int8_t dir;
    int32_t lo = 0, hi = 0;
    bool done = false;
    int32_t alive = -1;
    uint64_t change_ms = 0;
};

std::mutex g_lock_mu;

bool g_want = true;
std::map<int32_t, uint64_t> g_hello_at;
uint64_t g_host_word_at = 0;
bool g_host_word_on = false;
uint64_t g_room_sent_v = ~0ull;
struct Peer {
    int32_t round, stage;
    Rect view;
    uint64_t at_ms;
    Heading heading;
};
Heading g_my_heading;
std::map<int32_t, Peer> g_peer_views;
int32_t g_host_round = -1, g_host_stage = -1;
std::vector<Running> g_host_running;
int32_t g_hold_l = -kNoLimit, g_hold_r = kNoLimit;
uint64_t g_host_at_ms = 0;

uint64_t g_sys = 0;
int32_t g_key_round = -1, g_key_stage = -1;
std::vector<Group> g_groups;
int8_t g_lease_dir = 0;
uint64_t g_lock_sent_v = ~0ull;
bool g_lock_log = true;

uint64_t now_ms() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
}
int32_t stage_of(uint64_t g) {
    uint64_t act = rd<uint64_t>(g + 0x88);
    return act ? rd<int32_t>(act + 0xf0) : -1;
}
bool is_host() { return g_sys && reinterpret_cast<Fn1>(g_base + 0x26220)(rd<uint64_t>(g_sys + 0x80)) & 1; }
Rect view_rect(uint64_t vc) {
    return {int32_t(rd<float>(vc + 0x9c)), int32_t(rd<float>(vc + 0xa4)), int32_t(rd<float>(vc + 0xb0)),
            int32_t(rd<float>(vc + 0xb8))};
}
void group_range(uint64_t grp, int32_t& lo, int32_t& hi) {
    uint64_t vt = rd<uint64_t>(grp);
    lo = rd<int32_t>(grp + 0x344) + int32_t(reinterpret_cast<Fn1>(rd<uint64_t>(vt + 0x2b0))(grp));
    hi = rd<int32_t>(grp + 0x348) + int32_t(reinterpret_cast<Fn1>(rd<uint64_t>(vt + 0x2b0))(grp));
}

bool overlaps(uint64_t grp, const Rect& v) {
    int32_t lo, hi;
    group_range(grp, lo, hi);
    if (v.a < lo && lo <= v.l) return true;
    if (!(lo > v.l && v.b <= hi) && hi >= v.r) return true;
    if (v.l <= lo && hi <= v.r) return true;
    return rd<uint8_t>(grp + 0x360) != 0;
}

int32_t g_seen_l = 0, g_seen_r = 0;
bool g_seen = false;
void see(const Rect& v) {
    if (v.l <= -99999 || v.r >= 99999 || v.l >= v.r) return;
    if (!g_seen) g_seen_l = v.l, g_seen_r = v.r, g_seen = true;
    g_seen_l = v.l < g_seen_l ? v.l : g_seen_l, g_seen_r = v.r > g_seen_r ? v.r : g_seen_r;
}

int8_t forward_dir(uint64_t grp, const Rect& v, int32_t move) {
    int32_t lo, hi;
    group_range(grp, lo, hi);
    if (g_seen) {
        int64_t behind_lo = int64_t(lo) - g_seen_l, beyond_hi = int64_t(g_seen_r) - hi;
        if (behind_lo - beyond_hi > 200) return 1;
        if (beyond_hi - behind_lo > 200) return -1;
    }
    if (move > 40) return 1;
    if (move < -40) return -1;
    int32_t c = v.centre();
    return hi - c >= c - lo ? 1 : -1;
}

void rekey(uint64_t g) {
    int32_t r = round_of(g), s = stage_of(g);
    if (r == g_key_round && s == g_key_stage) return;
    g_key_round = r, g_key_stage = s, g_groups.clear(), g_lease_dir = 0, g_seen = false;
}
const Group* find_group(int32_t id) {
    for (const Group& x : g_groups)
        if (x.id == id) return &x;
    return nullptr;
}
bool host_heard(uint64_t g) {
    std::lock_guard<std::mutex> lk(g_lock_mu);
    return g_host_at_ms && now_ms() - g_host_at_ms < 3000 && g_host_round == round_of(g) && g_host_stage == stage_of(g);
}
bool host_runs(int32_t id, int8_t* dir = nullptr) {
    std::lock_guard<std::mutex> lk(g_lock_mu);
    for (const Running& r : g_host_running)
        if (r.id == id) return dir && (*dir = r.dir), true;
    return false;
}

void on_side(int32_t ctx, const uint8_t* d, size_t n) {
    if (n < 8 || rd<uint32_t>(reinterpret_cast<uint64_t>(d)) != kMagic) return;
    uint64_t p = reinterpret_cast<uint64_t>(d);
    int32_t type = rd<int32_t>(p + 4);
    std::lock_guard<std::mutex> lk(g_lock_mu);
    if (type == 1 && n >= 8 + 6 * 4) {
        Peer& peer = g_peer_views[ctx];
        peer.round = rd<int32_t>(p + 8), peer.stage = rd<int32_t>(p + 12), peer.at_ms = now_ms();
        peer.view = {rd<int32_t>(p + 16), rd<int32_t>(p + 20), rd<int32_t>(p + 24), rd<int32_t>(p + 28)};
        peer.heading.update(peer.view.centre(), peer.at_ms);
    } else if (type == 3 && n >= 9) {
        g_hello_at[ctx] = now_ms();
        if (d[8] & 1) g_host_word_at = now_ms(), g_host_word_on = (d[8] & 2) != 0;
    } else if (type == 2 && n >= 8 + 12) {
        int32_t cnt = rd<int32_t>(p + 16);
        if (cnt < 0 || cnt > kMaxRunning || n < size_t(20 + 5 * cnt)) return;
        g_host_round = rd<int32_t>(p + 8), g_host_stage = rd<int32_t>(p + 12), g_host_at_ms = now_ms();
        g_host_running.clear();
        for (int i = 0; i < cnt; ++i) g_host_running.push_back({rd<int32_t>(p + 20 + 5 * i), int8_t(d[24 + 5 * i])});
        bool hold = n >= size_t(28 + 5 * cnt);
        g_hold_l = hold ? rd<int32_t>(p + 20 + 5 * cnt) : -kNoLimit, g_hold_r = hold ? rd<int32_t>(p + 24 + 5 * cnt) : kNoLimit;
    }
}

void apply_hold(int32_t l, int32_t r) {
    if (l <= -kNoLimit && r >= kNoLimit) return;
    uint64_t vc = rd<uint64_t>(g_sys + 0xc0);
    if (!vc) return;
    bool on = rd<int32_t>(vc + 0xd4) > 0;
    int32_t cl = on ? rd<int32_t>(vc + 0xcc) : -kNoLimit, cr = on ? rd<int32_t>(vc + 0xd0) : kNoLimit;
    l = l > cl ? l : cl, r = r < cr ? r : cr;
    if (l >= r) return;
    reinterpret_cast<Fn2>(g_base + 0x113f0)(vc, uint64_t(uint32_t(l)));
    reinterpret_cast<Fn2>(g_base + 0x11410)(vc, uint64_t(uint32_t(r)));
}

void lock_log(const char* what, int32_t id, int8_t dir) {
    if (g_lock_log)
        std::fprintf(stderr, "OWNCAM-LOCK v%llu %s %s group %d dir %+d\n",
                     (unsigned long long)dcp::hle::vblank_count_public(), is_host() ? "host" : "guest", what, id, dir);
}

void lock_tick(uint64_t g) {
    uint64_t v = dcp::hle::vblank_count_public();
    if (!g_sys || v == g_lock_sent_v || v % 6) return;
    g_lock_sent_v = v;
    rekey(g);
    uint8_t b[28 + 5 * kMaxRunning];
    auto put = [&](int at, int32_t x) { std::memcpy(b + at, &x, 4); };
    put(0, int32_t(kMagic)), put(8, round_of(g)), put(12, stage_of(g));
    if (is_host()) {
        Rect mine = view_rect(rd<uint64_t>(g_sys + 0xc0));
        g_my_heading.update(mine.centre(), now_ms());
        see(mine);
        {
            std::lock_guard<std::mutex> lk(g_lock_mu);
            for (auto& [ctx, p] : g_peer_views)
                if (now_ms() - p.at_ms < 2000 && p.round == round_of(g) && p.stage == stage_of(g)) see(p.view);
        }
        int cnt = 0;
        int32_t hold_l = -kNoLimit, hold_r = kNoLimit;
        const uint64_t t = now_ms();
        for (auto it = g_groups.begin(); it != g_groups.end();) {
            bool running = rd<uint8_t>(it->obj + 0x5b) == 1;
            int32_t alive = int32_t(reinterpret_cast<Fn1>(g_base + 0x464e30)(it->obj));
            if (alive != it->alive) it->alive = alive, it->change_ms = t;
            bool holds = alive > 0 && t - it->change_ms < (running ? kStaleMs : kStaleDoneMs);
            if (!running && !it->done) it->done = true, lock_log(holds ? "done, enemies left" : "done", it->id, it->dir);
            if (!running && !holds) {
                if (alive > 0) lock_log("stopped holding (count unchanged)", it->id, it->dir);
                else lock_log("released", it->id, it->dir);
                it = g_groups.erase(it);
                continue;
            }
            if (holds && it->dir > 0) hold_r = it->hi < hold_r ? it->hi : hold_r;
            if (holds && it->dir < 0) hold_l = it->lo > hold_l ? it->lo : hold_l;
            if (running && cnt < kMaxRunning) put(20 + 5 * cnt, it->id), b[24 + 5 * cnt] = uint8_t(it->dir), ++cnt;
            ++it;
        }
        g_hold_l = hold_l, g_hold_r = hold_r;
        put(4, 2), put(16, cnt), put(20 + 5 * cnt, hold_l), put(24 + 5 * cnt, hold_r);
        dcp::net::rudp::side_broadcast(b, size_t(28 + 5 * cnt));
        apply_hold(hold_l, hold_r);
        return;
    }
    Rect r = view_rect(rd<uint64_t>(g_sys + 0xc0));
    put(4, 1), put(16, r.l), put(20, r.r), put(24, r.a), put(28, r.b);
    dcp::net::rudp::side_broadcast(b, 32);
    if (!host_heard(g)) return;
    {
        int32_t l, r;
        {
            std::lock_guard<std::mutex> lk(g_lock_mu);
            l = g_hold_l, r = g_hold_r;
        }
        apply_hold(l, r);
    }
    for (auto it = g_groups.begin(); it != g_groups.end();) {
        if (host_runs(it->id)) { ++it; continue; }
        if (rd<uint8_t>(it->obj + 0x5b) == 1) {
            uint64_t vt = rd<uint64_t>(it->obj);
            reinterpret_cast<Fn2>(rd<uint64_t>(vt + 0xa8))(it->obj, 2);
        }
        lock_log("ended by host", it->id, it->dir);
        it = g_groups.erase(it);
    }
}

void room_tick(uint64_t g) {
    uint64_t v = dcp::hle::vblank_count_public();
    if (v == g_room_sent_v || v % 30) return;
    g_room_sent_v = v;
    bool was = g_room_on;
    if (!online(g)) {
        g_room_on = false;
    } else {
        uint64_t srv = rd<uint64_t>(g + 0x80);
        bool host = srv && reinterpret_cast<Fn1>(g_base + 0x26220)(srv) & 1;
        uint8_t b[12] = {};
        uint32_t magic = kMagic, type = 3;
        std::memcpy(b, &magic, 4), std::memcpy(b + 4, &type, 4);
        const uint64_t t = now_ms();
        if (host) {
            int linked = dcp::net::rudp::side_peers(), heard = 0;
            {
                std::lock_guard<std::mutex> lk(g_lock_mu);
                for (auto& [ctx, at] : g_hello_at) heard += t - at < 3000;
            }
            g_room_on = g_want && linked > 0 && heard >= linked;
            b[8] = uint8_t(1 | (g_room_on ? 2 : 0));
        } else {
            std::lock_guard<std::mutex> lk(g_lock_mu);
            g_room_on = g_host_word_at && t - g_host_word_at < 3000 && g_host_word_on;
        }
        dcp::net::rudp::side_broadcast(b, sizeof b);
    }
    if (g_room_on != was)
        std::fprintf(stderr, "own_camera: separate cameras %s\n", g_room_on ? "on for this room" : "off (shared camera)");
}

DCP_SYSV uint64_t group_in_view(uint64_t grp) {
    uint64_t r = reinterpret_cast<Fn1>(g_base + 0x464b90)(grp) & 1;
    uint64_t g = gs();
    if (!g || !gate(g)) return r;
    g_sys = reinterpret_cast<Fn1>(rd<uint64_t>(rd<uint64_t>(grp) + 0xd8))(grp);
    rekey(g);
    int32_t id = rd<int32_t>(grp + 0x1f8);
    if (find_group(id)) return 1;
    if (!is_host()) {
        if (!host_heard(g)) return r;
        int8_t dir = 0;
        if (!host_runs(id, &dir)) return 0;
        g_groups.push_back({grp, id, dir});
        lock_log("start (host)", id, dir);
        return 1;
    }
    Rect by = view_rect(rd<uint64_t>(g_sys + 0xc0));
    int32_t move = g_my_heading.move;
    bool remote = false;
    if (!r) {
        std::lock_guard<std::mutex> lk(g_lock_mu);
        for (auto& [ctx, p] : g_peer_views)
            if (now_ms() - p.at_ms < 2000 && p.round == round_of(g) && p.stage == stage_of(g) && overlaps(grp, p.view)) {
                r = 1, by = p.view, move = p.heading.move, remote = true;
                break;
            }
    }
    if (!r) return 0;
    int32_t lo, hi;
    group_range(grp, lo, hi);
    if (hi > g_hold_r + 50 || lo < g_hold_l - 50) return 0;
    see(by);
    int8_t dir = forward_dir(grp, by, move);
    Group x{grp, id, dir};
    x.lo = lo, x.hi = hi;
    g_groups.push_back(x);
    lock_log(remote ? "start (guest view)" : "start (host view)", id, dir);
    if (g_lock_log)
        std::fprintf(stderr, "OWNCAM-LOCK   range %d..%d explored %d..%d view %d..%d move %d hold %d..%d\n", lo, hi,
                     g_seen ? g_seen_l : 0, g_seen ? g_seen_r : 0, by.l, by.r, move, g_hold_l, g_hold_r);
    return 1;
}

DCP_SYSV uint64_t group_count(uint64_t grp) {
    uint64_t g = gs();
    if (g && g_sys && gate(g) && !is_host() && host_runs(rd<int32_t>(grp + 0x1f8))) return 0x7fffffff;
    return reinterpret_cast<Fn1>(g_base + 0x464e30)(grp);
}

DCP_SYSV void lease_left(uint64_t vc, int32_t v, uint64_t grp) {
    g_lease_dir = 0;
    uint64_t g = gs();
    if (g && gate(g))
        if (const Group* x = find_group(rd<int32_t>(grp + 0x1f8))) g_lease_dir = x->dir;
    reinterpret_cast<Fn2>(g_base + 0x113f0)(vc, uint64_t(uint32_t(g_lease_dir > 0 ? -kNoLimit : v)));
}
DCP_SYSV void lease_right(uint64_t vc, int32_t v) {
    reinterpret_cast<Fn2>(g_base + 0x11410)(vc, uint64_t(uint32_t(g_lease_dir < 0 ? kNoLimit : v)));
    g_lease_dir = 0;
}

bool off(const char* env) {
    const char* v = std::getenv(env);
    return v && std::strcmp(v, "0") == 0;
}

void write_code(uint8_t* at, const uint8_t* bytes, size_t n) {
    uint64_t lo = reinterpret_cast<uint64_t>(at) & ~0xfffull, hi = (reinterpret_cast<uint64_t>(at) + n + 0xfff) & ~0xfffull;
    mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC);
    std::memcpy(at, bytes, n);
    mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_EXEC);
}

uint8_t* alloc_near(uint64_t from, size_t size) {
    for (uint64_t a = (from + 0xffff) & ~0xffffull; a < from + (1ull << 30); a += 0x10000) {
        void* p = mmap(reinterpret_cast<void*>(a), size, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) continue;
        if (p == reinterpret_cast<void*>(a)) return static_cast<uint8_t*>(p);
        munmap(p, size);
    }
    return nullptr;
}

struct Emit {
    uint8_t* p;
    void b(std::initializer_list<uint8_t> v) { for (uint8_t x : v) *p++ = x; }
    void q(uint64_t v) { std::memcpy(p, &v, 8), p += 8; }
    void jmp_abs(uint64_t to) { b({0xff, 0x25, 0, 0, 0, 0}), q(to); }
    void call_abs(uint64_t fn) { b({0x48, 0xb8}), q(fn), b({0xff, 0xd0}); }
    void save_args() { b({0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xec, 0x08}); }
    void restore_args() { b({0x48, 0x83, 0xc4, 0x08, 0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f}); }
};

bool fits_rel32(const uint8_t* to, const uint8_t* next) {
    int64_t r = int64_t(to - next);
    return r == int32_t(r);
}

void put_rel32(uint8_t* bytes, const uint8_t* to, const uint8_t* next) {
    int32_t r = int32_t(to - next);
    std::memcpy(bytes, &r, 4);
}

struct CallSite {
    uint32_t site;
    uint8_t expect[5];
    bool by_caller;
};

constexpr CallSite kCalls[] = {
    {0x374c7a, {0xe8, 0x81, 0xfd, 0xf4, 0xff}, true},
    {0x374d7a, {0xe8, 0x81, 0xfc, 0xf4, 0xff}, true},
    {0x372845, {0xe8, 0xb6, 0x21, 0xf5, 0xff}, false},
    {0x372b25, {0xe8, 0xd6, 0x1e, 0xf5, 0xff}, false},
};
constexpr uint32_t kRx = 0x371a30;
constexpr uint8_t kRxBytes[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x85, 0xf6};
constexpr uint32_t kSends[] = {0x371d98, 0x371e3c, 0x371eeb, 0x371f51, 0x371fb8};
constexpr uint8_t kSendBytes[] = {0xff, 0x90, 0x68, 0x01, 0x00, 0x00};

struct LockSite {
    uint32_t site;
    uint8_t op;
    uint32_t target;
};
constexpr LockSite kInView{0x4648ea, 0xe8, 0x464b90}, kCount{0x464ae2, 0xe8, 0x464e30},
    kLeaseL{0x464b53, 0xe8, 0x113f0}, kLeaseR{0x464b65, 0xe9, 0x11410};

}

void install(uint8_t* base) {
    g_want = !off("DCP_OWN_CAMERA");
    g_base = reinterpret_cast<uint64_t>(base);
    g_filter = !off("DCP_OWN_CAMERA_FILTER"), g_rx = !off("DCP_OWN_CAMERA_RX"), g_local = !off("DCP_OWN_CAMERA_LOCAL");
    if (const char* n = std::getenv("DCP_OWN_CAMERA_LOG")) g_log_n = std::strtoull(n, nullptr, 10);
    if (const char* n = std::getenv("DCP_OWN_CAMERA_RXLOG")) g_rxlog = std::atoi(n);
    for (const CallSite& c : kCalls)
        if (std::memcmp(base + c.site, c.expect, 5)) {
            std::fprintf(stderr, "own_camera: unexpected bytes at eboot+0x%x; not installed\n", c.site);
            return;
        }
    bool bad = std::memcmp(base + kRx, kRxBytes, sizeof kRxBytes) != 0;
    for (uint32_t a : kSends) bad |= std::memcmp(base + a, kSendBytes, sizeof kSendBytes) != 0;
    if (bad) {
        std::fprintf(stderr, "own_camera: unexpected bytes at the camera receive / send sites; not installed\n");
        return;
    }
    for (const LockSite& s : {kInView, kCount, kLeaseL, kLeaseR}) {
        uint8_t want[5] = {s.op};
        put_rel32(want + 1, base + s.target, base + s.site + 5);
        if (std::memcmp(base + s.site, want, 5)) {
            std::fprintf(stderr, "own_camera: unexpected bytes at eboot+0x%x (fight lock); not installed\n", s.site);
            return;
        }
    }
    uint8_t* page = alloc_near(g_base + 0x1300000, 0x1000);
    if (!page) { std::fprintf(stderr, "own_camera: no relay memory near the image; not installed\n"); return; }
    uint8_t *by_caller = page, *plain = page + 0x40, *rx = page + 0x80, *tx = page + 0x100;
    Emit m{by_caller};
    m.b({0x48, 0x8b, 0x55, 0x08}), m.jmp_abs(reinterpret_cast<uint64_t>(&cam_player));
    m.p = plain;
    m.b({0x31, 0xd2}), m.jmp_abs(reinterpret_cast<uint64_t>(&cam_player));

    m.p = rx;
    m.b({0x48, 0x85, 0xf6, 0x74, 0x00});
    uint8_t* je1 = m.p - 1;

    m.b({0x0f, 0xbf, 0x46, 0x10, 0xff, 0xc8, 0x83, 0xf8, 0x05, 0x77, 0x00});
    uint8_t* ja1 = m.p - 1;
    m.save_args();
    m.b({0x89, 0xc7});
    m.call_abs(reinterpret_cast<uint64_t>(&rx_mute));
    m.restore_args();
    m.b({0x84, 0xc0, 0x74, 0x06, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3});
    uint8_t* orig = m.p;
    *je1 = uint8_t(orig - (je1 + 1)), *ja1 = uint8_t(orig - (ja1 + 1));
    m.b({0x55, 0x48, 0x89, 0xe5, 0x48, 0x85, 0xf6}), m.jmp_abs(g_base + kRx + 7);
    g_rx_orig = reinterpret_cast<uint64_t>(orig);

    m.p = tx;
    m.b({0x4c, 0x8b, 0x80, 0x68, 0x01, 0x00, 0x00, 0x48, 0xb8}), m.q(reinterpret_cast<uint64_t>(&own_send)), m.b({0xff, 0xe0});

    uint8_t *inview = page + 0x180, *count = page + 0x1a0, *lease_l = page + 0x1c0, *lease_r = page + 0x1e0;
    m.p = inview, m.jmp_abs(reinterpret_cast<uint64_t>(&group_in_view));
    m.p = count, m.jmp_abs(reinterpret_cast<uint64_t>(&group_count));
    m.p = lease_l, m.b({0x4c, 0x89, 0xfa}), m.jmp_abs(reinterpret_cast<uint64_t>(&lease_left));
    m.p = lease_r, m.jmp_abs(reinterpret_cast<uint64_t>(&lease_right));

    for (const CallSite& c : kCalls)
        if (!fits_rel32(c.by_caller ? by_caller : plain, base + c.site + 5)) {
            std::fprintf(stderr, "own_camera: relay out of rel32 range; not installed\n");
            return;
        }
    bool out_of_range = !fits_rel32(rx, base + kRx + 5);
    for (uint32_t a : kSends) out_of_range |= !fits_rel32(tx, base + a + 5);
    const std::pair<LockSite, uint8_t*> lock_hooks[] = {{kInView, inview}, {kCount, count}, {kLeaseL, lease_l}, {kLeaseR, lease_r}};
    for (auto& [s, to] : lock_hooks) out_of_range |= !fits_rel32(to, base + s.site + 5);
    if (out_of_range) {
        std::fprintf(stderr, "own_camera: relay out of rel32 range; not installed\n");
        return;
    }
    if (const char* d = std::getenv("DCP_OWN_CAMERA_DUMP"); d && *d && *d != '0') {
        std::fprintf(stderr, "own_camera: relays ");
        for (int i = 0; i < 0x160; ++i) std::fprintf(stderr, "%02x%s", page[i], (i & 0x3f) == 0x3f ? " | " : "");
        std::fprintf(stderr, "\n");
    }
    for (const CallSite& c : kCalls) {
        uint8_t call[5] = {0xe8};
        put_rel32(call + 1, c.by_caller ? by_caller : plain, base + c.site + 5);
        write_code(base + c.site, call, 5);
    }
    uint8_t jr[7] = {0xe9, 0, 0, 0, 0, 0x66, 0x90};
    put_rel32(jr + 1, rx, base + kRx + 5);
    write_code(base + kRx, jr, sizeof jr);
    for (uint32_t a : kSends) {
        uint8_t call[6] = {0xe8, 0, 0, 0, 0, 0x90};
        put_rel32(call + 1, tx, base + a + 5);
        write_code(base + a, call, sizeof call);
    }
    for (auto& [s, to] : lock_hooks) {
        uint8_t op[5] = {s.op};
        put_rel32(op + 1, to, base + s.site + 5);
        write_code(base + s.site, op, 5);
    }
    g_lock_log = !off("DCP_OWN_CAMERA_LOCKLOG");
    dcp::net::rudp::set_side_handler(&on_side);
    std::fprintf(stderr, "own_camera: installed, %s when hosting (filter %d, net camera commands dropped %d, own commands "
                 "applied locally %d, shared fight lock; online adventures only)\n", g_want ? "on" : "off", g_filter, g_rx,
                 g_local);
}

}
