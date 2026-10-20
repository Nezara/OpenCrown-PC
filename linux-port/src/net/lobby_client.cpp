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
constexpr auto kSilence = std::chrono::seconds(45);  // no frame at all this long (pings every 20 s) = link dead
}

struct LobbyClient::Impl {
    std::string url;
    std::mutex mu;
    std::deque<std::string> out;           // encoded JSON texts waiting for the socket
    std::deque<uint32_t> out_rids;         // rid of each queued text (0 = none), to fail them if the link drops
    std::deque<std::string> urgent;        // same, sent even while paused (hello / resume after a reconnect)
    std::deque<uint32_t> urgent_rids;
    std::deque<uint32_t> inflight;         // sent, awaiting a response
    std::deque<LobbyMessage> in;
    std::atomic<bool> up{false}, dead{false}, stop{false};
    bool paused = false;                   // after a reconnect, until resume_traffic()
    std::thread th;
    uint32_t next_rid = 1;
    bool started = false;

    void fail_locked(std::deque<uint32_t>& rids) {
        for (uint32_t rid : rids) {
            if (!rid) continue;
            LobbyMessage m;
            m.kind = LobbyMessage::Response, m.rid = rid, m.err = kErrLobbyDown;
            in.push_back(std::move(m));
        }
        rids.clear();
    }
    void fail_pending_locked() {
        fail_locked(urgent_rids), fail_locked(out_rids), fail_locked(inflight);
        out.clear(), urgent.clear();
    }
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
        auto last_ping = std::chrono::steady_clock::now(), last_rx = last_ping;
        for (;;) {
            if (stop) return "stopping";
            {  // flush queued requests (urgent first; the rest only when not paused)
                std::deque<std::string> o;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    o.swap(urgent);
                    for (uint32_t r : urgent_rids)
                        if (r) inflight.push_back(r);
                    urgent_rids.clear();
                    if (!paused) {
                        for (auto& t : out) o.push_back(std::move(t));
                        for (uint32_t r : out_rids)
                            if (r) inflight.push_back(r);
                        out.clear(), out_rids.clear();
                    }
                }
                for (auto& t : o) {
                    std::string f = ws::encode_frame(ws::kText, t, true);
                    if (!s.send_all(f)) return "send failed";
                }
            }
            auto now = std::chrono::steady_clock::now();
            if (now - last_ping > std::chrono::seconds(20)) {
                std::string f = ws::encode_frame(ws::kPing, "", true);
                s.send_all(f);
                last_ping = now;
            }
            if (now - last_rx > kSilence) return "no answer for 45 s";
            if (!s.readable(5)) continue;
            char buf[16384];
            int n = s.recv(buf, sizeof buf);
            if (n <= 0) return n == 0 ? "closed by the server" : "connection lost";
            last_rx = std::chrono::steady_clock::now();
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
                            if (*it == m.rid) {
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
        auto deadline = std::chrono::steady_clock::now();
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
                if (tries >= kReconnectTries || std::chrono::steady_clock::now() >= deadline) {
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
            {  // requests already sent got no answer; the rest wait for the new socket
                std::lock_guard<std::mutex> lk(mu);
                paused = true;
                fail_locked(inflight);
                fail_locked(urgent_rids), urgent.clear();  // a hello / resume for the socket that just went
                push_locked(LobbyMessage::Lost);
            }
            tries = 0;
            deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kReconnectMs);
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
    (urgent ? d_->urgent : d_->out).push_back(body.dump());
    (urgent ? d_->urgent_rids : d_->out_rids).push_back(rid);
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

bool LobbyClient::connected() const { return d_->up; }
const std::string& LobbyClient::url() const { return d_->url; }

}  // namespace dcp::net
