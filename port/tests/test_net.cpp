// P2P transport tests on loopback (no game data): UdpCarrier through the Carrier interface with deterministic packet
// loss, then the Rudp layer the way the game's RudpMgr drives it (two sockets, simultaneous Initiate, poll events,
// whole-message Write/Read, close).
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/carrier.h"
#include "net/online_check.h"
#include "net/rudp.h"
#include "net/stun.h"
#include "net/udp_carrier.h"
#include "net/upnp.h"

using namespace dcp::net;
using Clock = std::chrono::steady_clock;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static std::vector<uint8_t> pattern(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed * 2654435761u + 1;
    for (auto& b : v) x = x * 1664525u + 1013904223u, b = uint8_t(x >> 24);
    return v;
}
// Message i: size varies (1 B .. 3 KB, plus a few large ones), content derived from (dir, i).
static size_t msg_size(int i) { return i % 50 == 7 ? 0x3ff00 : size_t(1 + (i * 37) % 3000); }

static bool wait_until(int ms, auto pred) {
    auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (Clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

static PeerAddr loopback(uint16_t port, uint16_t vport) {
    PeerAddr p;
    p.ip_be = htonl(INADDR_LOOPBACK);
    p.port_be = htons(port);
    p.vport = vport;
    return p;
}

struct Sink final : CarrierSink {
    std::mutex mu;
    int connected = 0, closed = 0;
    CloseReason reason = CloseReason::Local;
    std::vector<std::vector<uint8_t>> msgs;
    void on_connected(ConnId) override { std::lock_guard<std::mutex> l(mu); ++connected; }
    void on_message(ConnId, const uint8_t* d, size_t n) override {
        std::lock_guard<std::mutex> l(mu);
        msgs.emplace_back(d, d + n);
    }
    void on_closed(ConnId, CloseReason r) override { std::lock_guard<std::mutex> l(mu); ++closed, reason = r; }
    size_t count() { std::lock_guard<std::mutex> l(mu); return msgs.size(); }
};

static void test_carrier_lossy() {
    Sink sa, sb;
    UdpCarrierOptions o;
    o.drop_every_n = 7;  // every 7th datagram lost on both sides: exercises SYN retry, retransmit, SACK
    o.idle_timeout_ms = 400;
    auto a = make_udp_carrier(o, &sa), b = make_udp_carrier(o, &sb);
    CHECK(a && b);
    if (!a || !b) return;
    CHECK(a->local_port() != 0 && a->local_port() != b->local_port());
    ConnId ca = a->connect(loopback(b->local_port(), 4649), 20);
    CHECK(a->state(ca) == LinkState::Connecting);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // b initiates later: the late side must still connect
    ConnId cb = b->connect(loopback(a->local_port(), 4649), 20);
    CHECK(ca >= 0 && cb >= 0);
    CHECK(wait_until(1000, [&] { return a->state(ca) == LinkState::Connected && b->state(cb) == LinkState::Connected; }));
    constexpr int kN = 120;
    auto sent = Clock::now();
    for (int i = 0; i < kN; ++i) {
        auto m = pattern(msg_size(i), 1000 + i);
        CHECK(a->send(ca, m.data(), m.size()));
        auto r = pattern(msg_size(i) / 3 + 1, 5000 + i);
        CHECK(b->send(cb, r.data(), r.size()));
    }
    CHECK(wait_until(1500, [&] { return sb.count() == kN && sa.count() == kN; }));
    std::printf("lossy: %lld ms for %d messages each way\n",
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sent).count(), kN);
    CHECK(wait_until(500, [&] { return a->queued_bytes(ca) == 0 && b->queued_bytes(cb) == 0; }));
    for (int i = 0; i < kN && i < int(sb.msgs.size()); ++i) CHECK(sb.msgs[i] == pattern(msg_size(i), 1000 + i));
    for (int i = 0; i < kN && i < int(sa.msgs.size()); ++i) CHECK(sa.msgs[i] == pattern(msg_size(i) / 3 + 1, 5000 + i));
    CHECK(sa.connected == 1 && sb.connected == 1);
    a->disconnect(ca);  // FIN may be dropped; the idle timeout closes b's side then
    CHECK(wait_until(1000, [&] { return b->state(cb) == LinkState::Closed; }));
    CHECK(sb.closed == 1 && (sb.reason == CloseReason::PeerClosed || sb.reason == CloseReason::Timeout));
    CHECK(!b->send(cb, reinterpret_cast<const uint8_t*>("x"), 1));
}

// An outage shorter than the idle timeout while a backlog larger than the send window waits (bug report 26: a guest
// frozen for 30-45 s, no disconnect). Every datagram is lost both ways for 2.5 s while b keeps sending small messages,
// as the game does. The link must survive, the backlog must arrive soon after the outage (not at the next backed-off
// retransmit), and the sender must not resend its whole window on every timeout while the link is dark.
static void test_carrier_blackout() {
    Sink sa, sb;
    std::atomic<bool> dark{false};
    std::atomic<int> dark_data{0};  // DATA-sized datagrams a sent while dark
    UdpCarrierOptions oa, ob;
    oa.idle_timeout_ms = ob.idle_timeout_ms = 5000;
    oa.drop = [&](size_t len) {
        if (dark && len > 100) ++dark_data;
        return dark.load();
    };
    ob.drop = [&](size_t) { return dark.load(); };
    auto a = make_udp_carrier(oa, &sa), b = make_udp_carrier(ob, &sb);
    CHECK(a && b);
    if (!a || !b) return;
    ConnId ca = a->connect(loopback(b->local_port(), 4649), 20), cb = b->connect(loopback(a->local_port(), 4649), 20);
    CHECK(wait_until(1000, [&] { return a->state(ca) == LinkState::Connected && b->state(cb) == LinkState::Connected; }));
    constexpr int kWarm = 50, kBacklog = 400;  // backlog: 2 fragments each, 800 > the 256 window
    for (int i = 0; i < kWarm; ++i) {  // RTT samples, so the RTO is the loopback one
        auto m = pattern(64, 9000 + i);
        a->send(ca, m.data(), m.size());
    }
    CHECK(wait_until(1000, [&] { return sb.count() == kWarm; }));
    std::atomic<bool> stop{false};
    std::thread chatter([&] {
        for (int i = 0; !stop; ++i) {
            auto m = pattern(20, 20000 + i);
            b->send(cb, m.data(), m.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    dark = true;
    for (int i = 0; i < kBacklog; ++i) {
        auto m = pattern(1500, 10000 + i);
        CHECK(a->send(ca, m.data(), m.size()));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    int sent_dark = dark_data;
    dark = false;
    auto lit = Clock::now();
    CHECK(wait_until(3000, [&] { return sb.count() == kWarm + kBacklog; }));
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - lit).count();
    std::printf("blackout: backlog in %lld ms after the outage, %d data datagrams sent during it\n", ms, sent_dark);
    CHECK(ms < 150);
    CHECK(sent_dark < 600);
    stop = true;
    chatter.join();
    CHECK(a->state(ca) == LinkState::Connected && b->state(cb) == LinkState::Connected);
    CHECK(sa.closed == 0 && sb.closed == 0);
    for (int i = 0; i < kBacklog && kWarm + i < int(sb.msgs.size()); ++i)
        CHECK(sb.msgs[kWarm + i] == pattern(1500, 10000 + i));
}

static int32_t g_ca, g_cb;
static std::vector<std::vector<uint8_t>> g_rxa, g_rxb;
static std::vector<std::vector<uint8_t>>* rx_for(int32_t ctx) { return ctx == g_ca ? &g_rxa : &g_rxb; }

static void test_rudp() {
    CHECK(rudp::init() == 0);
    CHECK(rudp::open_socket_count() == 0);
    int32_t poll = rudp::poll_create(0x80);
    int32_t sa = rudp::socket_create(), sb = rudp::socket_create();
    CHECK(rudp::socket_bind(sa, 0, 4649) == 0 && rudp::socket_bind(sb, 0, 4649) == 0);
    uint16_t pa = rudp::socket_port(sa), pb = rudp::socket_port(sb);
    CHECK(pa && pb && pa != pb && rudp::open_socket_count() == 2);
    // RudpMgr_Connect order (FUN_000bd8b0): CreateContext, Bind(vport 20, mux 1), SetOption 2/3, Initiate, PollControl.
    CHECK(rudp::create_context(&g_ca) == 0 && rudp::create_context(&g_cb) == 0);
    CHECK(rudp::context_state(g_ca) == rudp::CtxState::Created);
    CHECK(rudp::bind(g_ca, sa, 20, 1) == 0 && rudp::bind(g_cb, sb, 20, 1) == 0);
    CHECK(rudp::context_state(g_ca) == rudp::CtxState::Bound);
    int32_t buf_sz = 0x40000;
    CHECK(rudp::set_option(g_ca, 2, &buf_sz, 4) == 0 && rudp::set_option(g_ca, 3, &buf_sz, 4) == 0);
    CHECK(rudp::set_option(g_cb, 2, &buf_sz, 4) == 0 && rudp::set_option(g_cb, 3, &buf_sz, 4) == 0);
    std::vector<uint8_t> buf(0x40000);
    CHECK(rudp::read(g_ca, buf.data(), buf.size()) == rudp::kErrWouldBlock);
    CHECK(rudp::initiate(g_ca, loopback(pb, 4649)) == 0);
    CHECK(rudp::initiate(g_cb, loopback(pa, 4649)) == 0);
    CHECK(rudp::context_state(g_ca) == rudp::CtxState::Connecting);
    CHECK(rudp::write(g_ca, "x", 1) == rudp::kErrWouldBlock);  // not connected yet
    CHECK(rudp::poll_control(poll, rudp::kPollAdd, g_ca, 0xb) == 0 && rudp::poll_control(poll, rudp::kPollAdd, g_cb, 0xb) == 0);
    // Connected = WRITE event (the game's state 1 -> 2 transition, then GetOption(0xe) must give >= 0).
    int writable = 0;
    CHECK(wait_until(1000, [&] {
        rudp::PollEvent ev[8];
        int n = rudp::poll_wait(poll, ev, 8, 1000);
        writable = 0;
        for (int i = 0; i < n; ++i) writable += (ev[i].rtn_events & rudp::kEvWrite) != 0;
        return writable == 2;
    }));
    CHECK(rudp::context_state(g_ca) == rudp::CtxState::Connected && rudp::context_state(g_cb) == rudp::CtxState::Connected);
    int32_t opt = -1;
    CHECK(rudp::get_option(g_ca, 0xe, &opt, 4) == 0 && opt == 0);
    // Both directions: many small messages and a large one, one outstanding Write retried on WouldBlock.
    constexpr int kN = 400;
    int ia = 0, ib = 0;
    auto deadline = Clock::now() + std::chrono::milliseconds(1500);
    while ((ia < kN || ib < kN || g_rxa.size() < kN || g_rxb.size() < kN) && Clock::now() < deadline) {
        if (ia < kN) {
            auto m = pattern(msg_size(ia), ia);
            int r = rudp::write(g_ca, m.data(), m.size());
            CHECK(r == int(m.size()) || r == rudp::kErrWouldBlock);
            if (r == int(m.size())) ++ia;
        }
        if (ib < kN) {
            auto m = pattern(msg_size(ib + 3), 777 + ib);
            int r = rudp::write(g_cb, m.data(), m.size());
            CHECK(r == int(m.size()) || r == rudp::kErrWouldBlock);
            if (r == int(m.size())) ++ib;
        }
        rudp::PollEvent ev[8];
        int n = rudp::poll_wait(poll, ev, 8, 0);
        for (int i = 0; i < n; ++i)
            if (ev[i].rtn_events & rudp::kEvRead)
                for (int r; (r = rudp::read(ev[i].ctx, buf.data(), buf.size())) >= 0;)
                    rx_for(ev[i].ctx)->emplace_back(buf.begin(), buf.begin() + r);
        if (n == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    CHECK(g_rxa.size() == size_t(kN) && g_rxb.size() == size_t(kN));
    for (int i = 0; i < kN && i < int(g_rxb.size()); ++i) CHECK(g_rxb[i] == pattern(msg_size(i), i));
    for (int i = 0; i < kN && i < int(g_rxa.size()); ++i) CHECK(g_rxa[i] == pattern(msg_size(i + 3), 777 + i));
    // Close: a terminates; b sees ERROR (8), Read gives kErrClosed, and Terminate cleans up (as RudpMgr_Pump).
    CHECK(rudp::poll_control(poll, rudp::kPollRemove, g_ca, 0) == 0);
    CHECK(rudp::terminate(g_ca) == 0);
    CHECK(rudp::context_state(g_ca) == rudp::CtxState::Closed);  // unknown id
    bool err = wait_until(1000, [&] {
        rudp::PollEvent ev[8];
        int n = rudp::poll_wait(poll, ev, 8, 1000);
        return n == 1 && ev[0].ctx == g_cb && (ev[0].rtn_events & rudp::kEvError);
    });
    CHECK(err);
    CHECK(rudp::read(g_cb, buf.data(), buf.size()) == rudp::kErrClosed);
    CHECK(rudp::write(g_cb, "x", 1) == rudp::kErrClosed);
    CHECK(rudp::get_option(g_cb, 0xe, &opt, 4) == 0);
    CHECK(rudp::poll_control(poll, rudp::kPollRemove, g_cb, 0) == 0 && rudp::terminate(g_cb) == 0);
    CHECK(rudp::socket_close(sa) == 0 && rudp::socket_close(sb) == 0);
    CHECK(rudp::open_socket_count() == 0);
    CHECK(rudp::poll_destroy(poll) == 0);
    CHECK(rudp::end() == 0);
    int32_t c;
    CHECK(rudp::create_context(&c) == rudp::kErrNotInit);
}

static void test_peer_table() {
    set_peer_address(0x20, htonl(0x7f000001), htons(3658));
    uint32_t ip = 0;
    uint16_t port = 0;
    CHECK(get_peer_address(0x20, &ip, &port) && ip == htonl(0x7f000001) && port == htons(3658));
    CHECK(!get_peer_address(0x30, &ip, &port));
    clear_peer_address(0x20);
    CHECK(!get_peer_address(0x20, &ip, &port));
}

// STUN (net/stun.h): message encode/decode on synthetic bytes, then a carrier discovering its endpoint through a fake
// STUN server on loopback (the response must be read off the carrier's own socket).
static void test_stun_codec() {
    stun::TxId tx;
    const uint8_t id[12] = {0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae};
    std::memcpy(tx.b, id, 12);
    auto req = stun::encode_binding_request(tx);
    const uint8_t want_req[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7,
                                  0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae};
    CHECK(req.size() == 20 && std::memcmp(req.data(), want_req, 20) == 0);
    CHECK(stun::looks_like_stun(req.data(), req.size()));
    // Success response (RFC 5769 2.2 values: 192.0.2.1:32853), a SOFTWARE attr with padding first, then
    // XOR-MAPPED-ADDRESS, then a plain MAPPED-ADDRESS with a different value that must lose to the XOR one.
    std::vector<uint8_t> r = {0x01, 0x01, 0x00, 0x00, 0x21, 0x12, 0xa4, 0x42};
    r.insert(r.end(), id, id + 12);
    const uint8_t attrs[] = {0x80, 0x22, 0x00, 0x05, 't', 'e', 's', 't', '!', 0, 0, 0,
                             0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43,
                             0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0x11, 0x11, 10, 0, 0, 1};
    r.insert(r.end(), attrs, attrs + sizeof attrs);
    r[3] = uint8_t(sizeof attrs);
    uint32_t ip = 0;
    uint16_t port = 0;
    CHECK(stun::decode_binding_response(r.data(), r.size(), tx, &ip, &port) == stun::Parse::Ok);
    CHECK(ip == htonl(0xc0000201) && port == htons(32853));
    CHECK(stun::ip_str(ip) == "192.0.2.1");
    stun::TxId other = tx;
    other.b[0] ^= 1;
    CHECK(stun::decode_binding_response(r.data(), r.size(), other, &ip, &port) == stun::Parse::OtherTx);
    auto e = r;
    e[1] = 0x11;  // Binding Error Response
    CHECK(stun::decode_binding_response(e.data(), e.size(), tx, &ip, &port) == stun::Parse::Error);
    auto t = r;
    t.resize(t.size() - 4);  // truncated datagram (length field says more)
    CHECK(stun::decode_binding_response(t.data(), t.size(), tx, &ip, &port) == stun::Parse::NotStun);
    const uint8_t dc[12] = {0x44, 0x43, 1, 1};  // UdpCarrier datagram: never STUN
    CHECK(!stun::looks_like_stun(dc, sizeof dc));
    CHECK(stun::is_private_or_loopback(htonl(0x7f000001)) && stun::is_private_or_loopback(htonl(0xc0a80105)) &&
          stun::is_private_or_loopback(htonl(0xac1f0001)) && stun::is_private_or_loopback(htonl(0x64400001)) &&
          !stun::is_private_or_loopback(htonl(0xac200001)) && !stun::is_private_or_loopback(htonl(0x08080808)));
}

// Peer-reflexive port: A was told a stale port for B (e.g. B's endpoint update arrived late); B's SYNs from its real
// port must re-target A's connecting link instead of being dropped.
static void test_reflexive_port() {
    Sink sa, sb;
    UdpCarrierOptions o;
    auto a = make_udp_carrier(o, &sa), b = make_udp_carrier(o, &sb);
    CHECK(a && b);
    if (!a || !b) return;
    uint16_t stale = uint16_t(b->local_port() == 65535 ? 65534 : b->local_port() + 1);
    ConnId ca = a->connect(loopback(stale, 4649), 20);
    ConnId cb = b->connect(loopback(a->local_port(), 4649), 20);
    CHECK(wait_until(2000, [&] { return a->state(ca) == LinkState::Connected && b->state(cb) == LinkState::Connected; }));
    const uint8_t msg[3] = {1, 2, 3};
    CHECK(a->send(ca, msg, 3));
    CHECK(wait_until(2000, [&] { std::lock_guard<std::mutex> l(sb.mu); return sb.msgs.size() == 1; }));
}

// Same router (net.md 11.1): each side only knows the other's public endpoint (unreachable here: TEST-NET-1, as
// without NAT hairpinning) plus its LAN candidate; the SYNs to the candidate must bring the link up there.
static void test_lan_candidate() {
    Sink sa, sb;
    UdpCarrierOptions o;
    auto a = make_udp_carrier(o, &sa), b = make_udp_carrier(o, &sb);
    CHECK(a && b);
    if (!a || !b) return;
    PeerAddr pub_b, pub_a;
    inet_pton(AF_INET, "192.0.2.1", &pub_b.ip_be), pub_b.port_be = htons(41002), pub_b.vport = 4649;
    inet_pton(AF_INET, "192.0.2.1", &pub_a.ip_be), pub_a.port_be = htons(41001), pub_a.vport = 4649;
    set_peer_alt(0x20, pub_b.ip_be, pub_b.port_be, htonl(INADDR_LOOPBACK), htons(b->local_port()));
    set_peer_alt(0x10, pub_a.ip_be, pub_a.port_be, htonl(INADDR_LOOPBACK), htons(a->local_port()));
    uint32_t ip = 0;
    uint16_t port = 0;
    CHECK(get_alt_address(pub_b.ip_be, pub_b.port_be, &ip, &port) && port == htons(b->local_port()));
    ConnId ca = a->connect(pub_b, 20);
    ConnId cb = b->connect(pub_a, 20);
    CHECK(wait_until(3000, [&] { return a->state(ca) == LinkState::Connected && b->state(cb) == LinkState::Connected; }));
    const uint8_t msg[3] = {4, 5, 6};
    CHECK(a->send(ca, msg, 3) && b->send(cb, msg, 3));
    CHECK(wait_until(2000, [&] {
        std::lock_guard<std::mutex> la(sa.mu), lb(sb.mu);
        return sa.msgs.size() == 1 && sb.msgs.size() == 1;
    }));
    clear_peer_addresses();
    CHECK(!get_alt_address(pub_b.ip_be, pub_b.port_be, &ip, &port));
}

static void test_stun_carrier() {
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET srv = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET, a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    int al = sizeof a;
    getsockname(srv, reinterpret_cast<sockaddr*>(&a), &al);
    std::atomic<bool> stop{false};
    std::atomic<int> requests{0};
    std::thread th([&] {  // fake STUN server: drops the first request (retry path), answers the rest
        while (!stop) {
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(srv, &rs);
            timeval tv{0, 10000};
            if (select(0, &rs, nullptr, nullptr, &tv) <= 0) continue;
            uint8_t b[1500];
            sockaddr_in from{};
            int fl = sizeof from;
            int n = recvfrom(srv, reinterpret_cast<char*>(b), sizeof b, 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (n != 20 || !stun::looks_like_stun(b, 20) || ++requests == 1) continue;
            uint8_t r[32];
            std::memcpy(r, b, 20);
            r[0] = 0x01, r[1] = 0x01, r[2] = 0, r[3] = 12;
            uint16_t xp = uint16_t(ntohs(from.sin_port) ^ 0x2112);
            uint32_t xa = ntohl(from.sin_addr.s_addr) ^ stun::kMagicCookie;
            const uint8_t at[12] = {0x00, 0x20, 0x00, 0x08, 0x00, 0x01, uint8_t(xp >> 8), uint8_t(xp),
                                    uint8_t(xa >> 24), uint8_t(xa >> 16), uint8_t(xa >> 8), uint8_t(xa)};
            std::memcpy(r + 20, at, 12);
            sendto(srv, reinterpret_cast<const char*>(r), 32, 0, reinterpret_cast<sockaddr*>(&from), fl);
        }
    });
    Sink s;
    UdpCarrierOptions o;
    auto c = make_udp_carrier(o, &s);
    CHECK(c != nullptr);
    if (c) {
        uint32_t ip = 0;
        uint16_t port = 0;
        CHECK(c->stun_result(&ip, &port) == Carrier::StunState::Idle);
        // First server does not exist (unresolvable): falls through to the fake one.
        c->start_stun({"no-such-host.invalid:3478", "127.0.0.1:" + std::to_string(ntohs(a.sin_port))});
        CHECK(wait_until(5000, [&] { return c->stun_result(&ip, &port) != Carrier::StunState::Running; }));
        CHECK(c->stun_result(&ip, &port) == Carrier::StunState::Done);
        CHECK(ip == htonl(INADDR_LOOPBACK) && ntohs(port) == c->local_port());
        CHECK(requests >= 2);
        c->punch(htonl(INADDR_LOOPBACK), a.sin_port);  // must not disturb anything
        // A failing discovery (server never answers) ends as Failed.
        Sink s2;
        auto c2 = make_udp_carrier(o, &s2);
        c2->start_stun({"127.0.0.1:9"});
        CHECK(wait_until(6000, [&] { return c2->stun_result(nullptr, nullptr) != Carrier::StunState::Running; }));
        CHECK(c2->stun_result(nullptr, nullptr) == Carrier::StunState::Failed);
    }
    stop = true;
    th.join();
    closesocket(srv);
}

// Live query (only with --live-stun): the real servers, from an ephemeral socket.
static void live_stun() {
    uint32_t ip = 0;
    uint16_t port = 0;
    std::string used;
    int same = -1;
    bool ok = stun::query_blocking(stun::configured_servers(), 3000, &ip, &port, &used, &same);
    std::printf("live STUN: %s", ok ? "ok " : "FAILED\n");
    if (ok)
        std::printf("%s:%u via %s, mapping %s\n", stun::ip_str(ip).c_str(), ntohs(port), used.c_str(),
                    same == 1 ? "endpoint-independent (hole punching friendly)" : same == 0 ? "differs per server (symmetric NAT)" : "not compared");
    CHECK(ok);
}

// UPnP toggle ($DCP_UPNP) and, with --live-upnp, a real mapping of an ephemeral port (removed right after).
static void test_upnp_toggle() {
    _putenv("DCP_UPNP=");
    CHECK(upnp::enabled());
    _putenv("DCP_UPNP=0");
    CHECK(!upnp::enabled());
    _putenv("DCP_UPNP=off");
    CHECK(!upnp::enabled());
    _putenv("DCP_UPNP=1");
    CHECK(upnp::enabled());
    CHECK(upnp::status().state == upnp::State::Off);
}
static void live_upnp() {
    upnp::Status st = upnp::probe(47999, 120, true);
    std::printf("live UPnP: %s: %s\n", st.state == upnp::State::Mapped ? "ok" : "not mapped", st.message.c_str());
}

int main(int argc, char** argv) {
    auto t0 = Clock::now();
    if (argc > 1 && std::strcmp(argv[1], "--live-stun") == 0) {
        live_stun();
        return g_fail ? 1 : 0;
    }
    if (argc > 2 && std::strcmp(argv[1], "--live-check") == 0) {  // launcher "Test connection" against a lobby URL
        OnlineCheck r = run_online_check(argv[2], "LiveCheck", true, 47998);
        std::printf("lobby: %s %s (%d ms)\nstun: %s %s:%u nat_same %d\nupnp: %s\nverdict: %s\n", r.lobby_msg.c_str(),
                    r.your_ip.c_str(), r.lobby_ms, r.stun_msg.c_str(), r.public_ip.c_str(), r.public_port, r.nat_same,
                    r.upnp_msg.c_str(), r.verdict.c_str());
        return r.lobby_ok && r.stun_ok ? 0 : 1;
    }
    if (argc > 1 && std::strcmp(argv[1], "--live-upnp") == 0) {
        live_upnp();
        return 0;
    }
    _putenv("DCP_STUN=0");  // the Rudp sockets below must not query real servers
    test_stun_codec();
    test_upnp_toggle();
    test_stun_carrier();
    test_reflexive_port();
    test_lan_candidate();
    test_carrier_lossy();
    test_carrier_blackout();
    test_rudp();
    test_peer_table();
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    std::printf("%s (%.0f ms)\n", g_fail ? "FAILED" : "all net tests passed", ms);
    return g_fail ? 1 : 0;
}
