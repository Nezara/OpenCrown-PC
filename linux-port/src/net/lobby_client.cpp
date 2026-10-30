// Lobby client (lobby_client.h).
#include "net/lobby_client.h"

#include <arpa/inet.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

#include "net/ws.h"

namespace dcp::net {

namespace {
using Clock = std::chrono::steady_clock;
// No frame at all this long (pings every 20 s) = link dead. Under the lobby's 30 s seat grace, so a reconnect can still
// retake the seat.
constexpr auto kSilence = std::chrono::seconds(30);
// A request unanswered this long = link dead, even if pings still come back (the lobby answers within a second).
// The game waits on some answers before it goes on (e.g. the room data sent when a dungeon starts).
constexpr auto kAnswerTimeout = std::chrono::seconds(10);
constexpr auto kProbeTimeout = std::chrono::seconds(5);  // check_link(): the ping it sends must be answered by then

// Requests a reconnect may send again: a repeat does no harm and the game waits on their answers. The others
// (create / join / leave / chat, hello / resume / endpoint, which the owner re-sends itself) still fail on a drop.
bool resendable(const std::string& op) {
    return op == "set_room_internal" || op == "set_room_external" || op == "search" || op == "set_user_info" ||
           op == "worlds" || op == "friend_list" || op == "set_presence";
}
}  // namespace

struct LobbyClient::Impl {
    std::string url;
    std::mutex mu;
    struct Item {
        std::string text;      // encoded JSON
        uint32_t rid = 0;      // 0 = no response expected
        bool resend = false;   // sent again after a reconnect instead of failing (resendable())
        Clock::time_point at;  // when it went out (inflight only)
    };
    std::deque<Item> out;                  // waiting for the socket
    std::deque<Item> urgent;               // same, sent even while paused (hello / resume after a reconnect)
    std::deque<Item> inflight;             // sent, awaiting a response (oldest first)
    std::deque<LobbyMessage> in;
    std::atomic<bool> up{false}, dead{false}, stop{false}, probe{false};
    bool paused = false;                   // after a reconnect, until resume_traffic()
    std::thread th;
    uint32_t next_rid = 1;
    bool started = false;

    void fail_one_locked(uint32_t rid) {
        if (!rid) return;
        LobbyMessage m;
        m.kind = LobbyMessage::Response, m.rid = rid, m.err = kErrLobbyDown;
        in.push_back(std::move(m));
    }
    void fail_locked(std::deque<Item>& q) {
        for (const Item& i : q) fail_one_locked(i.rid);
        q.clear();
    }
    void fail_pending_locked() { fail_locked(urgent), fail_locked(out), fail_locked(inflight); }
    void push_locked(LobbyMessage::Kind k) {
        LobbyMessage m;
        m.kind = k;
        in.push_back(std::move(m));
    }
    void give_up() {
        std::lock_guard<std::mutex> lk(mu);
        dead = true;
        fail_pending_locked();
        push_locked(LobbyMessage::Down);
    }
    bool sleep_ms(int ms) {  // false = stopping
        for (int t = 0; t < ms && !stop; t += 50) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return !stop;
    }

    // One connected socket until it drops; returns why.
    std::string session(ws::Conn& s) {
        ws::Reader rd;
        auto last_ping = Clock::now(), last_rx = last_ping;
        Clock::time_point probe_from{};  // check_link(): when its ping went out (epoch = none pending)
        probe = false;                   // a fresh socket needs no check
        for (;;) {
            if (stop) return "stopping";
            bool stuck = false;
            {  // flush queued requests (urgent first; the rest only when not paused)
                std::deque<Item> o;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    o.swap(urgent);
                    if (!paused) {
                        for (auto& i : out) o.push_back(std::move(i));
                        out.clear();
                    }
                    const auto t = Clock::now();
                    for (const Item& i : o)
                        if (i.rid) inflight.push_back(i), inflight.back().at = t;
                    stuck = !inflight.empty() && t - inflight.front().at > kAnswerTimeout;
                }
                for (auto& i : o) {
                    std::string f = ws::encode_frame(ws::kText, i.text, true);
                    if (!s.send_all(f)) return "send failed";
                }
            }
            if (stuck) return "a request got no answer for 10 s";
            auto now = Clock::now();
            if (probe.exchange(false) && probe_from == Clock::time_point{}) {
                std::string f = ws::encode_frame(ws::kPing, "", true);
                s.send_all(f);
                probe_from = last_ping = now;
            }
            if (probe_from != Clock::time_point{}) {
                if (last_rx >= probe_from) probe_from = {};
                else if (now - probe_from > kProbeTimeout) return "no answer after the app came back";
            }
            if (now - last_ping > std::chrono::seconds(20)) {
                std::string f = ws::encode_frame(ws::kPing, "", true);
                s.send_all(f);
                last_ping = now;
            }
            if (now - last_rx > kSilence) return "no answer for 30 s";
            if (!s.readable(5)) continue;
            char buf[16384];
            int n = s.recv(buf, sizeof buf);
            if (n <= 0) return n == 0 ? "closed by the server" : "connection lost";
            last_rx = Clock::now();
            rd.feed(buf, size_t(n));
            ws::Opcode op;
            std::string payload;
            while (rd.next(&op, &payload)) {
                if (op == ws::kPing) {
                    std::string f = ws::encode_frame(ws::kPong, payload, true);
                    s.send_all(f);
                } else if (op == ws::kClose) {
                    char why[96];
                    if (payload.size() >= 2)
                        std::snprintf(why, sizeof why, "close %u %.60s",
                                      (unsigned(uint8_t(payload[0])) << 8) | uint8_t(payload[1]), payload.c_str() + 2);
                    else
                        std::snprintf(why, sizeof why, "close");
                    return why;
                } else if (op == ws::kText) {
                    Json j;
                    if (!Json::parse(payload, &j) || !j.is_object()) {
                        std::fprintf(stderr, "LOBBY bad message: %.200s\n", payload.c_str());
                        continue;
                    }
                    LobbyMessage m;
                    if (j.has("rid")) {
                        m.kind = LobbyMessage::Response;
                        m.rid = uint32_t(j.get("rid").as_int());
                        m.err = int32_t(j.get("err").as_int(0));
                    } else {
                        m.kind = LobbyMessage::Event;
                        m.ev = j.get("ev").as_string();
                    }
                    m.body = std::move(j);
                    std::lock_guard<std::mutex> lk(mu);
                    if (m.kind == LobbyMessage::Response)
                        for (auto it = inflight.begin(); it != inflight.end(); ++it)
                            if (it->rid == m.rid) {
                                inflight.erase(it);
                                break;
                            }
                    in.push_back(std::move(m));
                }
            }
            if (rd.error()) return "bad frame";
        }
    }

    void run() {
        const char* e = std::getenv("DCP_LOBBY_RECONNECT");
        const bool reconnect = !(e && *e == '0');
        bool ever_up = false;
        int tries = 0;
        auto deadline = Clock::now();
        for (;;) {
            std::string err;
            ws::Conn s;
            if (!s.open(url, 5000, &err)) {
                if (!ever_up) {  // unreachable at launch: no retry (the game stays offline)
                    std::fprintf(stderr, "LOBBY connect %s failed: %s\n", url.c_str(), err.c_str());
                    return give_up();
                }
                ++tries;
                std::fprintf(stderr, "LOBBY reconnect try %d/%d failed: %s\n", tries, kReconnectTries, err.c_str());
                if (tries >= kReconnectTries || Clock::now() >= deadline) {
                    std::fprintf(stderr, "LOBBY gave up reconnecting to %s\n", url.c_str());
                    return give_up();
                }
                if (!sleep_ms(std::min(500 << (tries - 1), 8000))) return;
                continue;
            }
            std::fprintf(stderr, "LOBBY %s %s%s\n", ever_up ? "reconnected to" : "connected to", url.c_str(),
                         s.secure() ? " (TLS)" : "");
            if (ever_up) {
                std::lock_guard<std::mutex> lk(mu);
                push_locked(LobbyMessage::Back);
            }
            ever_up = true;
            up = true;
            std::string why = session(s);
            s.close();
            up = false;
            std::fprintf(stderr, "LOBBY disconnected from %s (%s)\n", url.c_str(), why.c_str());
            if (stop) return;
            if (!reconnect) return give_up();
            {  // unanswered requests: the resendable ones go out again first on the new socket (after hello /
               // resume), the rest fail; queued ones wait for it
                std::lock_guard<std::mutex> lk(mu);
                paused = true;
                size_t again = 0;
                for (auto it = inflight.rbegin(); it != inflight.rend(); ++it)
                    if (it->resend) out.push_front(std::move(*it)), ++again;
                    else fail_one_locked(it->rid);
                inflight.clear();
                if (again) std::fprintf(stderr, "LOBBY %zu unanswered request(s) will be sent again\n", again);
                fail_locked(urgent);  // a hello / resume for the socket that just went
                push_locked(LobbyMessage::Lost);
            }
            tries = 0;
            deadline = Clock::now() + std::chrono::milliseconds(kReconnectMs);
            if (!sleep_ms(250)) return;
        }
    }
};

LobbyClient& LobbyClient::instance() {
    static LobbyClient c;
    return c;
}

LobbyClient::LobbyClient() : d_(new Impl) {
    const char* e = std::getenv("DCP_LOBBY");
    d_->url = e && *e ? e : kDefaultLobbyUrl;
}

LobbyClient::~LobbyClient() {
    d_->stop = true;
    if (d_->th.joinable()) d_->th.detach();  // process exit; never block on the socket
}

uint32_t LobbyClient::request(const std::string& op, Json body, bool urgent) {
    std::lock_guard<std::mutex> lk(d_->mu);
    uint32_t rid = d_->next_rid++;
    body["op"] = op;
    body["rid"] = int64_t(rid);
    if (d_->dead) {  // gave up (or never reached the lobby): report the failure as the response
        LobbyMessage m;
        m.kind = LobbyMessage::Response, m.rid = rid, m.err = kErrLobbyDown;
        d_->in.push_back(std::move(m));
        return rid;
    }
    Impl::Item it;
    it.text = body.dump(), it.rid = rid, it.resend = !urgent && resendable(op);
    (urgent ? d_->urgent : d_->out).push_back(std::move(it));
    if (!d_->started) {
        d_->started = true;
        d_->th = std::thread([this] { d_->run(); });
    }
    return rid;
}

void LobbyClient::resume_traffic() {
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->paused = false;
}

bool LobbyClient::poll(LobbyMessage* out) {
    std::lock_guard<std::mutex> lk(d_->mu);
    if (d_->in.empty()) return false;
    *out = std::move(d_->in.front());
    d_->in.pop_front();
    return true;
}

void LobbyClient::check_link() {
    if (d_->up) d_->probe = true;
}

bool LobbyClient::connected() const { return d_->up; }
const std::string& LobbyClient::url() const { return d_->url; }

}  // namespace dcp::net
