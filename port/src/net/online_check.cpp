// Launcher connection test (online_check.h).
#include "net/online_check.h"

#include <winsock2.h>

#include <chrono>
#include <cstdio>

#include "net/json.h"
#include "net/lobby_client.h"
#include "net/stun.h"
#include "net/upnp.h"
#include "net/ws.h"

namespace dcp::net {

OnlineCheck run_online_check(const std::string& lobby_url, const std::string& name, bool try_upnp, uint16_t udp_port) {
    OnlineCheck r;
    auto t0 = std::chrono::steady_clock::now();
    {
        ws::Conn c;
        std::string err;
        if (!c.open(lobby_url, 5000, &err)) {
            r.lobby_msg = "cannot connect: " + err;
        } else {
            Json h;
            h["op"] = "hello", h["rid"] = int64_t(1), h["proto"] = kLobbyProtocol, h["version"] = kGameNetVersion;
            h["title"] = "CUSA10454", h["name"] = name.empty() ? std::string("Player") : name.substr(0, 16);
            h["account_id"] = "0", h["udp_port"] = int(udp_port);
            if (!c.send_all(ws::encode_frame(ws::kText, h.dump(), true))) {
                r.lobby_msg = "send failed";
            } else {
                ws::Reader rd;
                bool done = false;
                auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!done && std::chrono::steady_clock::now() < end) {
                    if (!c.readable(50)) continue;
                    char buf[8192];
                    int n = c.recv(buf, sizeof buf);
                    if (n <= 0) break;
                    rd.feed(buf, size_t(n));
                    ws::Opcode op;
                    std::string pl;
                    while (!done && rd.next(&op, &pl)) {
                        Json j;
                        if (op != ws::kText || !Json::parse(pl, &j) || j.get("rid").as_int() != 1) continue;
                        done = true;
                        int64_t e = j.get("err").as_int();
                        if (e) {
                            char t[64];
                            std::snprintf(t, sizeof t, "lobby refused hello (err 0x%08x)", uint32_t(e));
                            r.lobby_msg = t;
                        } else {
                            r.lobby_ok = true;
                            r.your_ip = j.get("your_ip").as_string();
                            r.lobby_msg = c.secure() ? "connected (TLS)" : "connected";
                        }
                    }
                }
                if (!done && r.lobby_msg.empty()) r.lobby_msg = "no answer to hello within 5 s";
            }
            c.send_all(ws::encode_frame(ws::kClose, "", true));
        }
    }
    r.lobby_ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());

    auto servers = stun::configured_servers();
    if (servers.empty()) {
        r.stun_msg = "STUN disabled ($DCP_STUN)";
    } else {
        uint32_t ip = 0;
        uint16_t port = 0;
        std::string used;
        r.stun_ok = stun::query_blocking(servers, 3000, &ip, &port, &used, &r.nat_same);
        if (r.stun_ok) {
            r.public_ip = stun::ip_str(ip), r.public_port = ntohs(port);
            r.stun_msg = "via " + used;
        } else {
            r.stun_msg = "no STUN server answered (UDP blocked?)";
        }
    }

    if (try_upnp) {
        r.upnp_tried = true;
        upnp::Status st = upnp::probe(udp_port, 60, true);
        r.upnp_ok = st.state == upnp::State::Mapped;
        r.upnp_msg = st.message;
    }

    if (!r.lobby_ok) r.verdict = "Lobby unreachable: online play will not work.";
    else if (!r.stun_ok) r.verdict = "Lobby ok, but UDP to the Internet seems blocked: only LAN play is likely to work.";
    else if (r.nat_same == 0 && !r.upnp_ok)
        r.verdict = "Symmetric NAT: direct connections will probably fail (no relay yet). Try enabling UPnP on the router.";
    else if (r.nat_same == 0)
        r.verdict = "Symmetric NAT, but UPnP mapped the port: connections should work if the router has a public address.";
    else if (r.nat_same == -1) r.verdict = "Lobby and STUN ok (only one STUN server answered: NAT type not checked).";
    else r.verdict = "Ready: the NAT keeps one public port per socket, so direct (hole-punched) connections should work.";
    return r;
}

}  // namespace dcp::net
