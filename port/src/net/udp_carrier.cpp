// UdpCarrier implementation; format and rules in udp_carrier.h. One I/O thread per carrier receives, retransmits and
// sends keepalives; send() transmits immediately from the caller's thread when the window allows. Sink events are
// collected under the lock and dispatched after it is released.
#include "net/udp_carrier.h"

#include "net/stun.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace dcp::net {
namespace {

constexpr uint16_t kMagic = 0x4344;
constexpr uint8_t kVersion = 1;
enum : uint8_t { kSyn = 1, kSynAck = 2, kData = 3, kAck = 4, kFin = 5, kPing = 6 };
constexpr size_t kHeader = 12;
constexpr size_t kFragment = 1200;        // payload per DATA datagram (stays under common path MTUs)
constexpr size_t kMaxMessage = 4u << 20;  // reassembly cap
constexpr uint32_t kWindow = 256;         // DATA datagrams in flight
constexpr uint32_t kRxWindow = 1024;      // out-of-order datagrams buffered
constexpr int kMaxRetries = 40;

uint64_t now_ms() { return GetTickCount64(); }
void put16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
uint16_t get16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
uint32_t get32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
bool seq_lt(uint32_t a, uint32_t b) { return int32_t(a - b) < 0; }

bool wsa_init() {
    static bool ok = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    return ok;
}

uint32_t new_session() {
    static std::atomic<uint32_t> ctr{0};
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    uint32_t s = uint32_t(q.QuadPart * 2654435761u) ^ (GetCurrentProcessId() << 16) ^ (++ctr * 0x9e3779b9u);
    return s ? s : 1;
}

struct Frag {
    uint32_t seq = 0;
    bool last = false;
    std::vector<uint8_t> data;
    uint64_t sent_ms = 0;
    int retries = 0;      // timeout retransmissions (backoff, dead peer)
    bool resent = false;  // any retransmission (Karn: no RTT sample)
};

struct Conn {
    ConnId id = -1;
    PeerAddr peer;
    PeerAddr alt;          // LAN candidate (get_alt_address) while connecting; ip_be 0 = none or settled
    uint16_t lvport = 0;
    LinkState st = LinkState::Connecting;
    uint32_t my_session = 0, peer_session = 0;
    uint64_t start_ms = 0, last_rx_ms = 0, last_tx_ms = 0, last_syn_ms = 0;
    // send side
    uint32_t next_seq = 0;
    std::deque<Frag> unsent;
    std::map<uint32_t, Frag> inflight;
    size_t queued = 0;
    double srtt = 100, rttvar = 50;
    // receive side
    uint32_t rx_next = 0;
    std::map<uint32_t, std::pair<bool, std::vector<uint8_t>>> ooo;
    std::vector<uint8_t> assembling;
    bool ack_due = false;
};

struct Event {
    enum Kind { Connected, Message, Closed } kind;
    ConnId id;
    CloseReason reason = CloseReason::Local;
    std::vector<uint8_t> data;
};

class UdpCarrier final : public Carrier {
public:
    UdpCarrier(const UdpCarrierOptions& o, CarrierSink* sink, SOCKET s) : opt_(o), sink_(sink), sock_(s) {
        sockaddr_in a{};
        int al = sizeof a;
        if (getsockname(sock_, reinterpret_cast<sockaddr*>(&a), &al) == 0) port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] { run(); });
    }
    ~UdpCarrier() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (resolver_.joinable()) resolver_.join();
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [id, c] : conns_)
            if (c.st == LinkState::Connected) send_ctl(c, kFin);
        closesocket(sock_);
    }

    ConnId connect(const PeerAddr& peer, uint16_t local_vport) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [id, c] : conns_)
            if (c.st != LinkState::Closed && same(c, peer.ip_be, peer.port_be, local_vport)) return -1;
        Conn c;
        c.id = next_id_++;
        c.peer = peer;
        c.lvport = local_vport;
        c.my_session = new_session();
        c.start_ms = c.last_rx_ms = now_ms();
        if (get_alt_address(peer.ip_be, peer.port_be, &c.alt.ip_be, &c.alt.port_be))
            std::fprintf(stderr, "UDPC conn %d: also trying the LAN candidate %s:%u\n", c.id,
                         stun::ip_str(c.alt.ip_be).c_str(), ntohs(c.alt.port_be));
        send_syn(c);
        c.last_syn_ms = c.start_ms;
        ConnId id = c.id;
        conns_.emplace(id, std::move(c));
        return id;
    }

    bool send(ConnId id, const uint8_t* data, size_t len) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = conns_.find(id);
        if (it == conns_.end() || it->second.st == LinkState::Closed || len > kMaxMessage) return false;
        Conn& c = it->second;
        size_t off = 0;
        do {
            size_t n = std::min(kFragment, len - off);
            Frag f;
            f.seq = c.next_seq++;
            f.last = off + n == len;
            f.data.assign(data + off, data + off + n);
            c.queued += n;
            c.unsent.push_back(std::move(f));
            off += n;
        } while (off < len);
        if (c.st == LinkState::Connected) pump_send(c, now_ms());
        return true;
    }

    size_t queued_bytes(ConnId id) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = conns_.find(id);
        return it == conns_.end() ? 0 : it->second.queued;
    }
    LinkState state(ConnId id) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = conns_.find(id);
        return it == conns_.end() ? LinkState::Closed : it->second.st;
    }
    void disconnect(ConnId id) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = conns_.find(id);
        if (it == conns_.end()) return;
        if (it->second.st == LinkState::Connected) send_ctl(it->second, kFin);
        conns_.erase(it);
    }
    uint16_t local_port() const override { return port_; }

    void start_stun(const std::vector<std::string>& servers) override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stun_.st == StunState::Running || resolver_.joinable()) return;
            stun_ = Stun{};
            stun_.st = servers.empty() ? StunState::Failed : StunState::Running;
            if (servers.empty()) return;
        }
        // DNS off the I/O thread; the I/O thread sends/retries once targets are known (timers()).
        resolver_ = std::thread([this, servers] {
            std::vector<std::pair<uint32_t, uint16_t>> t;
            std::vector<std::string> names;
            for (auto& s : servers) {
                uint32_t ip;
                uint16_t port;
                if (stop_) return;
                if (stun::resolve(s, &ip, &port)) t.push_back({ip, port}), names.push_back(s);
                else std::fprintf(stderr, "STUN cannot resolve %s\n", s.c_str());
            }
            std::lock_guard<std::mutex> lk(mu_);
            stun_.targets = std::move(t);
            stun_.names = std::move(names);
            stun_.resolved = true;
            if (stun_.targets.empty()) stun_.st = StunState::Failed;
        });
    }
    StunState stun_result(uint32_t* ip_be, uint16_t* port_be) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (stun_.st == StunState::Done) {
            if (ip_be) *ip_be = stun_.ip;
            if (port_be) *port_be = stun_.port;
        }
        return stun_.st;
    }
    void punch(uint32_t ip_be, uint16_t port_be) override {
        static const uint8_t kPunch[4] = {'D', 'C', 'P', 'H'};  // < kHeader: dropped by on_datagram
        sockaddr_in a{};
        a.sin_family = AF_INET, a.sin_port = port_be, a.sin_addr.s_addr = ip_be;
        std::lock_guard<std::mutex> lk(mu_);
        for (int i = 0; i < 3; ++i)
            sendto(sock_, reinterpret_cast<const char*>(kPunch), 4, 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
    }

private:
    // A connection is (peer UDP endpoint, Rudp port); both ends use the same Rudp port (the game binds every context
    // to port 20, asm bd98c). PeerAddr::vport (the peer socket's vport, 4649) is informational: one UDP port per socket.
    static bool same(const Conn& c, uint32_t ip, uint16_t port, uint16_t rport) {
        return c.peer.ip_be == ip && c.peer.port_be == port && c.lvport == rport;
    }

    void raw_send(const Conn& c, const uint8_t* buf, size_t len) {
        if (opt_.drop_every_n > 0 && ++tx_count_ % unsigned(opt_.drop_every_n) == 0) return;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = c.peer.port_be;
        a.sin_addr.s_addr = c.peer.ip_be;
        sendto(sock_, reinterpret_cast<const char*>(buf), int(len), 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
    }
    void header(uint8_t* b, const Conn& c, uint8_t type) {
        put16(b, kMagic);
        b[2] = kVersion;
        b[3] = type;
        put16(b + 4, c.lvport);
        put16(b + 6, c.lvport);
        put32(b + 8, c.my_session);
    }
    void send_ctl(Conn& c, uint8_t type) {
        uint8_t b[kHeader];
        header(b, c, type);
        raw_send(c, b, sizeof b);
        c.last_tx_ms = now_ms();
    }
    // While connecting, SYNs also go to the LAN candidate (same NAT as the peer: its public endpoint may need a
    // hairpin the router lacks). on_datagram settles on whichever address the peer answers from.
    void send_syn(Conn& c) {
        send_ctl(c, kSyn);
        if (!c.alt.ip_be) return;
        Conn to_alt = Conn{};
        to_alt.peer = c.alt, to_alt.lvport = c.lvport, to_alt.my_session = c.my_session;
        uint8_t b[kHeader];
        header(b, to_alt, kSyn);
        raw_send(to_alt, b, sizeof b);
    }
    void send_ack(Conn& c) {
        uint8_t b[kHeader + 8];
        header(b, c, kAck);
        uint32_t bits = 0;
        for (uint32_t i = 0; i < 32; ++i)
            if (c.ooo.count(c.rx_next + 1 + i)) bits |= 1u << i;
        put32(b + kHeader, c.rx_next);
        put32(b + kHeader + 4, bits);
        raw_send(c, b, sizeof b);
        c.last_tx_ms = now_ms();
        c.ack_due = false;
    }
    void send_frag(Conn& c, Frag& f, uint64_t t) {
        uint8_t b[kHeader + 5 + kFragment];
        header(b, c, kData);
        put32(b + kHeader, f.seq);
        b[kHeader + 4] = f.last ? 1 : 0;
        std::memcpy(b + kHeader + 5, f.data.data(), f.data.size());
        raw_send(c, b, kHeader + 5 + f.data.size());
        f.sent_ms = t;
        c.last_tx_ms = t;
    }
    // Move unsent fragments into flight while the window allows.
    void pump_send(Conn& c, uint64_t t) {
        while (!c.unsent.empty() && c.inflight.size() < kWindow) {
            Frag f = std::move(c.unsent.front());
            c.unsent.pop_front();
            uint32_t s = f.seq;
            send_frag(c, c.inflight.emplace(s, std::move(f)).first->second, t);
        }
    }
    uint64_t rto(const Conn& c, int retries) const {
        double base = c.srtt + 4 * c.rttvar;
        if (base < 30) base = 30;
        if (base > 1000) base = 1000;
        return uint64_t(base) << (retries < 4 ? retries : 4);
    }
    void close(Conn& c, CloseReason r, std::vector<Event>& ev) {
        if (c.st == LinkState::Closed) return;
        c.st = LinkState::Closed;
        c.unsent.clear();
        c.inflight.clear();
        c.queued = 0;
        ev.push_back({Event::Closed, c.id, r, {}});
    }
    void connected(Conn& c, uint32_t peer_session, std::vector<Event>& ev) {
        c.st = LinkState::Connected;
        c.peer_session = peer_session;
        ev.push_back({Event::Connected, c.id, CloseReason::Local, {}});
        pump_send(c, now_ms());
    }

    void on_datagram(const uint8_t* b, size_t n, const sockaddr_in& from, std::vector<Event>& ev) {
        if (n < kHeader || get16(b) != kMagic || b[2] != kVersion) return;
        uint8_t type = b[3];
        uint16_t src_vport = get16(b + 4), dst_vport = get16(b + 6);
        uint32_t session = get32(b + 8);
        Conn* c = nullptr;
        if (src_vport != dst_vport) return;
        for (auto& [id, k] : conns_)
            if (k.st != LinkState::Closed && same(k, from.sin_addr.s_addr, from.sin_port, dst_vport)) c = &k;
        if (c) c->alt = {};  // the public endpoint works: stop trying the LAN candidate
        // LAN candidate answered (SYN or SYNACK from it): the peer is behind our NAT, talk to it there from now on.
        if (!c && (type == kSyn || type == kSynAck))
            for (auto& [id, k] : conns_)
                if (k.st == LinkState::Connecting && k.alt.ip_be == from.sin_addr.s_addr &&
                    k.alt.port_be == from.sin_port && k.lvport == dst_vport) {
                    std::fprintf(stderr, "UDPC conn %d: peer reached on its LAN address %s:%u\n", k.id,
                                 stun::ip_str(k.alt.ip_be).c_str(), ntohs(k.alt.port_be));
                    k.peer.ip_be = k.alt.ip_be, k.peer.port_be = k.alt.port_be, k.alt = {};
                    c = &k;
                    break;
                }
        // Peer-reflexive port (as in ICE): a SYN from the right IP but another port, while we are still connecting to
        // that IP, means the peer's NAT mapped it elsewhere (or its STUN endpoint update reached us after we started).
        // Adopt the observed port; the SYN proves the peer is trying to reach us on this Rudp port.
        if (!c && type == kSyn) {
            Conn* cand = nullptr;
            int n_cand = 0;
            for (auto& [id, k] : conns_)
                if (k.st == LinkState::Connecting && k.peer.ip_be == from.sin_addr.s_addr && k.lvport == dst_vport)
                    cand = &k, ++n_cand;
            if (n_cand == 1) {
                std::fprintf(stderr, "UDPC conn %d: peer port %u -> %u (seen from its SYN)\n", cand->id,
                             ntohs(cand->peer.port_be), ntohs(from.sin_port));
                cand->peer.port_be = from.sin_port;
                c = cand;
            }
        }
        if (!c) return;  // unsolicited: the peer keeps retrying its SYN until we connect() too
        uint64_t t = now_ms();
        if (type == kSyn) {
            c->last_rx_ms = t;
            if (c->st == LinkState::Connecting) connected(*c, session, ev);
            else if (session != c->peer_session) { close(*c, CloseReason::Reset, ev); return; }
            send_ctl(*c, kSynAck);
            return;
        }
        if (type == kSynAck) {
            c->last_rx_ms = t;
            if (c->st == LinkState::Connecting) connected(*c, session, ev);
            return;
        }
        if (c->st != LinkState::Connected || session != c->peer_session) return;
        c->last_rx_ms = t;
        switch (type) {
        case kData: {
            if (n < kHeader + 5) return;
            uint32_t seq = get32(b + kHeader);
            bool last = b[kHeader + 4] & 1;
            c->ack_due = true;
            if (seq_lt(seq, c->rx_next) || seq - c->rx_next >= kRxWindow) return;
            c->ooo.emplace(seq, std::make_pair(last, std::vector<uint8_t>(b + kHeader + 5, b + n)));
            for (auto it = c->ooo.find(c->rx_next); it != c->ooo.end(); it = c->ooo.find(c->rx_next)) {
                auto& [l, d] = it->second;
                if (c->assembling.size() + d.size() > kMaxMessage) { close(*c, CloseReason::Error, ev); return; }
                c->assembling.insert(c->assembling.end(), d.begin(), d.end());
                if (l) {
                    ev.push_back({Event::Message, c->id, CloseReason::Local, std::move(c->assembling)});
                    c->assembling.clear();
                }
                c->ooo.erase(it);
                ++c->rx_next;
            }
            break;
        }
        case kAck: {
            if (n < kHeader + 8) return;
            uint32_t next = get32(b + kHeader), bits = get32(b + kHeader + 4);
            auto acked = [&](std::map<uint32_t, Frag>::iterator it) {
                if (!it->second.resent) {  // Karn: RTT samples only from first transmissions
                    double r = double(t - it->second.sent_ms);
                    c->rttvar = 0.75 * c->rttvar + 0.25 * (r > c->srtt ? r - c->srtt : c->srtt - r);
                    c->srtt = 0.875 * c->srtt + 0.125 * r;
                }
                c->queued -= it->second.data.size();
                return c->inflight.erase(it);
            };
            for (auto it = c->inflight.begin(); it != c->inflight.end();)
                it = seq_lt(it->first, next) ? acked(it) : std::next(it);
            for (uint32_t i = 0; i < 32; ++i)
                if (bits & (1u << i)) {
                    auto it = c->inflight.find(next + 1 + i);
                    if (it != c->inflight.end()) acked(it);
                }
            // Fast retransmit: holes below the highest selectively acked seq are resent without waiting for the RTO
            // (at most once per smoothed RTT each).
            if (bits) {
                uint32_t top = next + 1 + uint32_t(31 - __builtin_clz(bits));
                for (uint32_t s = next; seq_lt(s, top); ++s) {
                    auto it = c->inflight.find(s);
                    if (it != c->inflight.end() && double(t - it->second.sent_ms) >= std::max(c->srtt, 5.0)) {
                        it->second.resent = true;
                        send_frag(*c, it->second, t);
                    }
                }
            }
            pump_send(*c, t);
            break;
        }
        case kFin: close(*c, CloseReason::PeerClosed, ev); break;
        default: break;  // kPing: keepalive only
        }
    }

    // STUN (net/stun.h): one server at a time, resend at 0/250/750/1750 ms, kStunPerServerMs per server, then the next.
    static constexpr unsigned kStunPerServerMs = 3000;
    struct Stun {
        StunState st = StunState::Idle;
        bool resolved = false;
        std::vector<std::pair<uint32_t, uint16_t>> targets;
        std::vector<std::string> names;
        size_t idx = 0;
        stun::TxId tx;
        uint64_t server_start = 0, next = 0;
        unsigned gap = 250;
        uint32_t ip = 0;
        uint16_t port = 0;
    };
    void stun_tick(uint64_t t) {
        if (stun_.st != StunState::Running || !stun_.resolved) return;
        if (stun_.server_start && t - stun_.server_start >= kStunPerServerMs) {
            std::fprintf(stderr, "STUN no answer from server %zu\n", stun_.idx);
            ++stun_.idx, stun_.server_start = 0;
        }
        if (stun_.idx >= stun_.targets.size()) {
            stun_.st = StunState::Failed;
            std::fprintf(stderr, "STUN failed (UDP port %u): no server answered\n", port_);
            return;
        }
        if (!stun_.server_start) stun_.server_start = stun_.next = t, stun_.gap = 250, stun_.tx = stun::new_txid();
        if (t < stun_.next) return;
        auto req = stun::encode_binding_request(stun_.tx);
        sockaddr_in a{};
        a.sin_family = AF_INET, a.sin_addr.s_addr = stun_.targets[stun_.idx].first, a.sin_port = stun_.targets[stun_.idx].second;
        sendto(sock_, reinterpret_cast<const char*>(req.data()), int(req.size()), 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
        stun_.next = t + stun_.gap, stun_.gap *= 2;
    }
    void on_stun(const uint8_t* b, size_t n) {
        if (stun_.st != StunState::Running) return;
        uint32_t ip;
        uint16_t port;
        auto r = stun::decode_binding_response(b, n, stun_.tx, &ip, &port);
        if (r == stun::Parse::Ok) {
            stun_.st = StunState::Done, stun_.ip = ip, stun_.port = port;
            std::fprintf(stderr, "STUN UDP port %u is %s:%u in public (via %s)\n", port_, stun::ip_str(ip).c_str(),
                         ntohs(port), stun_.idx < stun_.names.size() ? stun_.names[stun_.idx].c_str() : "?");
        } else if (r == stun::Parse::Error) {
            ++stun_.idx, stun_.server_start = 0;  // error response: try the next server
        }
    }

    void timers(std::vector<Event>& ev) {
        uint64_t t = now_ms();
        stun_tick(t);
        for (auto& [id, c] : conns_) {
            if (c.st == LinkState::Connecting) {
                if (t - c.start_ms > opt_.connect_timeout_ms) { close(c, CloseReason::Timeout, ev); continue; }
                if (t - c.last_syn_ms >= 100) send_syn(c), c.last_syn_ms = t;
                continue;
            }
            if (c.st != LinkState::Connected) continue;
            if (t - c.last_rx_ms > opt_.idle_timeout_ms) { close(c, CloseReason::Timeout, ev); continue; }
            bool dead = false;
            for (auto& [s, f] : c.inflight)
                if (t - f.sent_ms >= rto(c, f.retries)) {
                    if (++f.retries > kMaxRetries) { dead = true; break; }
                    f.resent = true;
                    send_frag(c, f, t);
                }
            if (dead) { close(c, CloseReason::Timeout, ev); continue; }
            pump_send(c, t);
            if (c.ack_due) send_ack(c);
            if (t - c.last_tx_ms >= 1000) send_ctl(c, kPing);
        }
    }

    void run() {
        std::vector<uint8_t> buf(65536);
        while (!stop_) {
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(sock_, &rs);
            timeval tv{0, 5000};
            int r = select(0, &rs, nullptr, nullptr, &tv);
            std::vector<Event> ev;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (r > 0) {
                    for (;;) {  // drain: socket is nonblocking
                        sockaddr_in from{};
                        int fl = sizeof from;
                        int n = recvfrom(sock_, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                                         reinterpret_cast<sockaddr*>(&from), &fl);
                        if (n < 0) break;  // WSAEWOULDBLOCK, or WSAECONNRESET (ICMP port unreachable): ignore
                        if (stun::looks_like_stun(buf.data(), size_t(n))) { on_stun(buf.data(), size_t(n)); continue; }
                        on_datagram(buf.data(), size_t(n), from, ev);
                    }
                }
                for (auto& [id, c] : conns_)  // ack promptly after a receive batch
                    if (c.ack_due && c.st == LinkState::Connected) send_ack(c);
                timers(ev);
            }
            for (auto& e : ev) {
                if (e.kind == Event::Connected) sink_->on_connected(e.id);
                else if (e.kind == Event::Message) sink_->on_message(e.id, e.data.data(), e.data.size());
                else sink_->on_closed(e.id, e.reason);
            }
        }
    }

    UdpCarrierOptions opt_;
    CarrierSink* sink_;
    SOCKET sock_;
    uint16_t port_ = 0;
    std::mutex mu_;
    std::map<ConnId, Conn> conns_;
    ConnId next_id_ = 1;
    unsigned tx_count_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::thread resolver_;
    Stun stun_;
};

}  // namespace

std::unique_ptr<Carrier> make_udp_carrier(const UdpCarrierOptions& opt, CarrierSink* sink) {
    if (!wsa_init()) return nullptr;
    SOCKET s;
    if (opt.adopt_socket != ~uintptr_t(0)) {
        s = SOCKET(opt.adopt_socket);
    } else {
        s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) return nullptr;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(opt.port);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { closesocket(s); return nullptr; }
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    BOOL no_reset = FALSE;  // ICMP port-unreachable must not break recvfrom (peer not up yet)
    DWORD ret = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /*SIO_UDP_CONNRESET*/, &no_reset, sizeof no_reset, nullptr, 0, &ret, nullptr,
             nullptr);
    return std::make_unique<UdpCarrier>(opt, sink, s);
}

}  // namespace dcp::net
