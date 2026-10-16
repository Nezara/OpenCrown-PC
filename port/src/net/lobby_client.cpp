// Lobby client (lobby_client.h).
#include "net/lobby_client.h"

#include <winsock2.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

#include "net/ws.h"

namespace dcp::net {

struct LobbyClient::Impl {
    std::string url;
    std::mutex mu;
    std::deque<std::string> out;           // encoded JSON texts waiting for the socket
    std::deque<uint32_t> out_rids;         // rid of each queued text (0 = none), to fail them if the link drops
    std::deque<uint32_t> inflight;         // sent, awaiting a response
    std::deque<LobbyMessage> in;
    std::atomic<bool> up{false}, dead{false}, stop{false};
    std::thread th;
    uint32_t next_rid = 1;
    bool started = false;

    void fail_pending_locked() {
        auto fail = [&](uint32_t rid) {
            if (!rid) return;
            LobbyMessage m;
            m.kind = LobbyMessage::Response, m.rid = rid, m.err = kErrLobbyDown;
            in.push_back(std::move(m));
        };
        for (uint32_t r : out_rids) fail(r);
        for (uint32_t r : inflight) fail(r);
        out.clear(), out_rids.clear(), inflight.clear();
    }

    void run() {
        std::string err;
        ws::Conn s;
        if (!s.open(url, 5000, &err)) {
            std::fprintf(stderr, "LOBBY connect %s failed: %s\n", url.c_str(), err.c_str());
            std::lock_guard<std::mutex> lk(mu);
            dead = true;
            fail_pending_locked();
            LobbyMessage m;
            m.kind = LobbyMessage::Down;
            in.push_back(std::move(m));
            return;
        }
        std::fprintf(stderr, "LOBBY connected to %s%s\n", url.c_str(), s.secure() ? " (TLS)" : "");
        up = true;
        ws::Reader rd;
        auto last_ping = std::chrono::steady_clock::now();
        bool closed = false;
        while (!stop && !closed) {
            {  // flush queued requests
                std::deque<std::string> o;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    o.swap(out);
                    for (uint32_t r : out_rids)
                        if (r) inflight.push_back(r);
                    out_rids.clear();
                }
                for (auto& t : o) {
                    std::string f = ws::encode_frame(ws::kText, t, true);
                    if (!s.send_all(f)) closed = true;
                }
            }
            auto now = std::chrono::steady_clock::now();
            if (now - last_ping > std::chrono::seconds(20)) {
                std::string f = ws::encode_frame(ws::kPing, "", true);
                s.send_all(f);
                last_ping = now;
            }
            if (!s.readable(5)) continue;
            char buf[16384];
            int n = s.recv(buf, sizeof buf);
            if (n <= 0) break;
            rd.feed(buf, size_t(n));
            ws::Opcode op;
            std::string payload;
            while (rd.next(&op, &payload)) {
                if (op == ws::kPing) {
                    std::string f = ws::encode_frame(ws::kPong, payload, true);
                    s.send_all(f);
                } else if (op == ws::kClose) {
                    closed = true;
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
            if (rd.error()) break;
        }
        s.close();
        std::fprintf(stderr, "LOBBY disconnected from %s\n", url.c_str());
        std::lock_guard<std::mutex> lk(mu);
        up = false;
        dead = true;
        fail_pending_locked();
        LobbyMessage m;
        m.kind = LobbyMessage::Down;
        in.push_back(std::move(m));
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

uint32_t LobbyClient::request(const std::string& op, Json body) {
    std::lock_guard<std::mutex> lk(d_->mu);
    uint32_t rid = d_->next_rid++;
    body["op"] = op;
    body["rid"] = int64_t(rid);
    if (d_->dead) {  // no reconnect within a session: report the failure as the response
        LobbyMessage m;
        m.kind = LobbyMessage::Response, m.rid = rid, m.err = kErrLobbyDown;
        d_->in.push_back(std::move(m));
        return rid;
    }
    d_->out.push_back(body.dump());
    d_->out_rids.push_back(rid);
    if (!d_->started) {
        d_->started = true;
        d_->th = std::thread([this] { d_->run(); });
    }
    return rid;
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
