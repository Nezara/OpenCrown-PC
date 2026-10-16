// Rudp contexts and poll sets over Carriers; see rudp.h. One global mutex guards all Rudp state; carrier sinks run on
// carrier I/O threads and take it too (carriers never hold their own lock while calling a sink, so the order
// rudp lock -> carrier lock is the only one). No guest code is ever called from here.
#include "net/rudp.h"

#include "net/stun.h"

#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace dcp::net::rudp {
namespace {

struct Ctx {
    int32_t sock = -1;
    uint16_t vport = 0;
    ConnId conn = -1;
    CtxState st = CtxState::Created;
    std::deque<std::vector<uint8_t>> rx;
    uint32_t sndbuf = 0x40000, rcvbuf = 0x40000;
    std::map<int32_t, int32_t> opts;
    uint64_t rx_msgs = 0, rx_bytes = 0, tx_msgs = 0, tx_bytes = 0;  // for the connection log lines
};

struct Sock;
struct SockSink final : CarrierSink {
    int32_t sock;
    explicit SockSink(int32_t s) : sock(s) {}
    void on_connected(ConnId) override;
    void on_message(ConnId, const uint8_t*, size_t) override;
    void on_closed(ConnId, CloseReason) override;
};

struct Sock {
    std::unique_ptr<SockSink> sink;
    std::unique_ptr<Carrier> carrier;  // destroyed before sink
    uint16_t vport = 0;
    std::map<ConnId, int32_t> conn_ctx;
};

std::mutex g_mu;
std::condition_variable g_cv;
bool g_inited = false;
std::map<int32_t, Ctx> g_ctx;
std::map<int32_t, std::map<int32_t, uint16_t>> g_polls;  // poll -> ctx -> requested events
std::map<int32_t, Sock> g_socks;
int32_t g_next_ctx = 1, g_next_poll = 1, g_next_sock = 1;

Ctx* ctx_for(int32_t sock, ConnId c) {
    auto s = g_socks.find(sock);
    if (s == g_socks.end()) return nullptr;
    auto m = s->second.conn_ctx.find(c);
    if (m == s->second.conn_ctx.end()) return nullptr;
    auto it = g_ctx.find(m->second);
    return it == g_ctx.end() ? nullptr : &it->second;
}

void SockSink::on_connected(ConnId c) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (Ctx* x = ctx_for(sock, c); x && x->st == CtxState::Connecting) {
        x->st = CtxState::Connected;
        std::fprintf(stderr, "RUDP socket %d conn %d connected\n", sock, c);
    }
    g_cv.notify_all();
}
void SockSink::on_message(ConnId c, const uint8_t* d, size_t n) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (Ctx* x = ctx_for(sock, c); x && x->st != CtxState::Closed) {
        x->rx.emplace_back(d, d + n);
        if (x->rx_msgs++ == 0) std::fprintf(stderr, "RUDP socket %d conn %d first message in (%zu bytes)\n", sock, c, n);
        x->rx_bytes += n;
    }
    g_cv.notify_all();
}
void SockSink::on_closed(ConnId c, CloseReason why) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (Ctx* x = ctx_for(sock, c)) {
        x->st = CtxState::Closed;
        std::fprintf(stderr, "RUDP socket %d conn %d closed (reason %d): in %llu msgs / %llu B, out %llu msgs / %llu B\n",
                     sock, c, int(why), (unsigned long long)x->rx_msgs, (unsigned long long)x->rx_bytes,
                     (unsigned long long)x->tx_msgs, (unsigned long long)x->tx_bytes);
    }
    g_cv.notify_all();
}

Carrier* carrier_of(const Ctx& x) {
    auto s = g_socks.find(x.sock);
    return s == g_socks.end() ? nullptr : s->second.carrier.get();
}

uint16_t events_of(const Ctx& x) {
    uint16_t e = 0;
    if (!x.rx.empty()) e |= kEvRead;
    if (x.st == CtxState::Connected) {
        Carrier* c = carrier_of(x);
        if (c && c->queued_bytes(x.conn) < x.sndbuf) e |= kEvWrite;
    }
    if (x.st == CtxState::Closed) e |= kEvError;
    return e;
}

void drop_ctx_locked(int32_t id) {
    auto it = g_ctx.find(id);
    if (it == g_ctx.end()) return;
    Ctx& x = it->second;
    if (x.conn >= 0) {
        auto s = g_socks.find(x.sock);
        if (s != g_socks.end()) {
            if (s->second.carrier) s->second.carrier->disconnect(x.conn);
            s->second.conn_ctx.erase(x.conn);
        }
    }
    for (auto& [p, set] : g_polls) set.erase(id);
    g_ctx.erase(it);
}

}  // namespace

int32_t init() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_inited = true;
    return 0;
}

int32_t end() {
    std::lock_guard<std::mutex> lk(g_mu);
    while (!g_ctx.empty()) drop_ctx_locked(g_ctx.begin()->first);
    g_polls.clear();
    g_inited = false;
    return 0;
}

int32_t create_context(int32_t* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_inited) return kErrNotInit;
    if (!out) return kErrInvalid;
    int32_t id = g_next_ctx++;
    g_ctx[id] = Ctx{};
    *out = id;
    return 0;
}

int32_t bind(int32_t ctx, int32_t socket, uint16_t vport, int32_t) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_ctx.find(ctx);
    auto s = g_socks.find(socket);
    if (it == g_ctx.end() || s == g_socks.end() || !s->second.carrier || it->second.st != CtxState::Created)
        return kErrInvalid;
    it->second.sock = socket;
    it->second.vport = vport;
    it->second.st = CtxState::Bound;
    return 0;
}

int32_t set_option(int32_t ctx, int32_t opt, const void* val, uint32_t len) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_ctx.find(ctx);
    if (it == g_ctx.end() || !val || len < 4) return kErrInvalid;
    int32_t v;
    std::memcpy(&v, val, 4);
    it->second.opts[opt] = v;
    if (opt == 2 && v > 0) it->second.sndbuf = uint32_t(v);  // 2 = send buffer (asm bd9b8, 0x40000)
    if (opt == 3 && v > 0) it->second.rcvbuf = uint32_t(v);  // 3 = receive buffer (asm bd9da)
    return 0;
}

int32_t get_option(int32_t ctx, int32_t opt, void* val, uint32_t len) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_ctx.find(ctx);
    if (it == g_ctx.end() || !val || len < 4) return kErrInvalid;
    // Option 0xe is read on connect / abort and must give >= 0 with a successful call, also for closed contexts
    // (asm bdf47, bdd0c, bdfae; meaning unverified). Unknown options read back what was set, else 0.
    int32_t v = 0;
    if (opt != 0xe) {
        auto o = it->second.opts.find(opt);
        if (o != it->second.opts.end()) v = o->second;
    }
    std::memcpy(val, &v, 4);
    return 0;
}

int32_t initiate(int32_t ctx, const PeerAddr& peer) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_ctx.find(ctx);
    if (it == g_ctx.end() || it->second.st != CtxState::Bound) return kErrInvalid;
    Ctx& x = it->second;
    auto& s = g_socks[x.sock];
    ConnId c = s.carrier->connect(peer, x.vport);
    if (c < 0) return kErrInvalid;
    x.conn = c;
    x.st = CtxState::Connecting;
    s.conn_ctx[c] = ctx;
    return 0;
}

int32_t read(int32_t ctx, void* buf, size_t len) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_ctx.find(ctx);
    if (it == g_ctx.end()) return kErrInvalid;
    Ctx& x = it->second;
    if (x.rx.empty()) return x.st == CtxState::Closed ? kErrClosed : kErrWouldBlock;
    auto& m = x.rx.front();
    size_t n = m.size() < len ? m.size() : len;  // a message longer than buf is truncated (game buf = 0x40000)
    if (n) std::memcpy(buf, m.data(), n);
    x.rx.pop_front();
    return int32_t(n);
}

int32_t write(int32_t ctx, const void* buf, size_t len) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_ctx.find(ctx);
    if (it == g_ctx.end()) return kErrInvalid;
    Ctx& x = it->second;
    if (x.st == CtxState::Closed) return kErrClosed;
    if (x.st != CtxState::Connected) return kErrWouldBlock;
    if (len == 0 || !buf) return 0;
    Carrier* c = carrier_of(x);
    if (!c) return kErrClosed;
    size_t q = c->queued_bytes(x.conn);
    if (q > 0 && q + len > x.sndbuf) return kErrWouldBlock;  // an empty queue always takes one whole message
    if (!c->send(x.conn, static_cast<const uint8_t*>(buf), len)) return kErrClosed;
    if (x.tx_msgs++ == 0) std::fprintf(stderr, "RUDP ctx %d first message out (%zu bytes)\n", ctx, len);
    x.tx_bytes += len;
    return int32_t(len);
}

int32_t terminate(int32_t ctx) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_ctx.find(ctx);
    if (it == g_ctx.end()) return kErrInvalid;
    if (it->second.tx_msgs || it->second.rx_msgs)
        std::fprintf(stderr, "RUDP ctx %d terminated: in %llu msgs / %llu B, out %llu msgs / %llu B\n", ctx,
                     (unsigned long long)it->second.rx_msgs, (unsigned long long)it->second.rx_bytes,
                     (unsigned long long)it->second.tx_msgs, (unsigned long long)it->second.tx_bytes);
    drop_ctx_locked(ctx);
    return 0;
}

int32_t poll_create(int32_t) {
    std::lock_guard<std::mutex> lk(g_mu);
    int32_t id = g_next_poll++;
    g_polls[id];
    return id;
}

int32_t poll_destroy(int32_t poll) {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_polls.erase(poll) ? 0 : kErrInvalid;
}

int32_t poll_control(int32_t poll, int32_t op, int32_t ctx, uint16_t events) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto p = g_polls.find(poll);
    if (p == g_polls.end()) return kErrInvalid;
    if (op == kPollRemove) return p->second.erase(ctx) ? 0 : kErrInvalid;
    if (!g_ctx.count(ctx)) return kErrInvalid;
    if (op == kPollAdd || op == kPollModify) { p->second[ctx] = events; return 0; }
    return kErrInvalid;
}

int32_t poll_wait(int32_t poll, PollEvent* out, int32_t max, uint64_t timeout_us) {
    std::unique_lock<std::mutex> lk(g_mu);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(timeout_us);
    for (;;) {
        auto p = g_polls.find(poll);
        if (p == g_polls.end() || !out || max <= 0) return kErrInvalid;
        int32_t n = 0;
        for (auto& [id, req] : p->second) {
            if (n >= max) break;
            auto it = g_ctx.find(id);
            if (it == g_ctx.end()) continue;
            uint16_t e = events_of(it->second) & (req | kEvError);
            if (e) out[n++] = PollEvent{id, req, e};
        }
        if (n > 0 || timeout_us == 0) return n;
        // Writability changes (acks) do not notify; re-check periodically.
        if (g_cv.wait_until(lk, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(5))) ==
                std::cv_status::timeout &&
            std::chrono::steady_clock::now() >= deadline)
            return 0;
    }
}

CtxState context_state(int32_t ctx) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_ctx.find(ctx);
    return it == g_ctx.end() ? CtxState::Closed : it->second.st;
}

int32_t socket_create() {
    std::lock_guard<std::mutex> lk(g_mu);
    int32_t id = g_next_sock++;
    g_socks[id];
    return id;
}

int32_t socket_bind(int32_t socket, uint16_t port_host, uint16_t vport) {
    SockSink* sink;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto s = g_socks.find(socket);
        if (s == g_socks.end() || s->second.carrier) return kErrInvalid;
        s->second.sink = std::make_unique<SockSink>(socket);
        s->second.vport = vport;
        sink = s->second.sink.get();
    }
    auto c = make_carrier(port_host, sink);  // outside the lock: starts the I/O thread
    uint16_t port = c ? c->local_port() : 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto s = g_socks.find(socket);
        if (!c || s == g_socks.end()) return kErrInvalid;
        s->second.carrier = std::move(c);
    }
    run_port_map_hook(port, true);  // UPnP mapping (net/upnp.h), before the address is advertised (carrier.h)
    {
        std::lock_guard<std::mutex> lk(g_mu);  // public endpoint of this socket (net.md 11); $DCP_STUN=0 disables
        auto s = g_socks.find(socket);
        if (s != g_socks.end() && s->second.carrier) s->second.carrier->start_stun(stun::configured_servers());
    }
    return 0;
}

int32_t socket_close(int32_t socket) {
    Sock dead;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto s = g_socks.find(socket);
        if (s == g_socks.end()) return kErrInvalid;
        for (auto& [id, x] : g_ctx)
            if (x.sock == socket) {
                if (x.st != CtxState::Created && x.st != CtxState::Bound) x.st = CtxState::Closed;
                x.conn = -1;
            }
        dead = std::move(s->second);
        g_socks.erase(s);
        g_cv.notify_all();
    }
    if (dead.carrier) run_port_map_hook(dead.carrier->local_port(), false);
    dead.carrier.reset();  // joins its I/O thread; must not hold g_mu (the thread may be waiting for it)
    return 0;
}

uint16_t socket_port(int32_t socket) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto s = g_socks.find(socket);
    return s == g_socks.end() || !s->second.carrier ? 0 : s->second.carrier->local_port();
}

int open_socket_count() {
    std::lock_guard<std::mutex> lk(g_mu);
    int n = 0;
    for (auto& [id, s] : g_socks) n += s.carrier != nullptr;
    return n;
}

Carrier::StunState public_endpoint(uint32_t* ip_be, uint16_t* port_be) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& [id, s] : g_socks)
        if (s.carrier) return s.carrier->stun_result(ip_be, port_be);
    return Carrier::StunState::Idle;
}

void punch(uint32_t ip_be, uint16_t port_be) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& [id, s] : g_socks)
        if (s.carrier) return s.carrier->punch(ip_be, port_be);
}

uint16_t bound_port() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& [id, s] : g_socks)
        if (s.carrier) return s.carrier->local_port();
    return 0;
}

void log_stats(const char* tag) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& [id, x] : g_ctx)
        std::fprintf(stderr, "RUDP %s ctx %d state %d: in %llu msgs / %llu B, out %llu msgs / %llu B\n", tag, id, int(x.st),
                     (unsigned long long)x.rx_msgs, (unsigned long long)x.rx_bytes, (unsigned long long)x.tx_msgs,
                     (unsigned long long)x.tx_bytes);
}

}  // namespace dcp::net::rudp
