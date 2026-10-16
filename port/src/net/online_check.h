// "Test connection" for the launcher's Online tab (net.md 11.3): lobby reachability + hello, the public UDP endpoint
// from STUN (with the mapping-behaviour check), and optionally a UPnP probe. Blocking (a few seconds); run it on a
// worker thread. Opens its own sockets: it does not touch the game's LobbyClient singleton.
#pragma once
#include <cstdint>
#include <string>

namespace dcp::net {

struct OnlineCheck {
    bool lobby_ok = false;
    std::string lobby_msg;   // error text or "connected (TLS)"
    std::string your_ip;     // address the lobby sees
    int lobby_ms = 0;        // connect + hello round trip
    bool stun_ok = false;
    std::string stun_msg;
    std::string public_ip;   // STUN-mapped endpoint of an ephemeral UDP socket
    uint16_t public_port = 0;
    int nat_same = -1;       // 1 endpoint-independent mapping, 0 symmetric, -1 not compared
    bool upnp_tried = false, upnp_ok = false;
    std::string upnp_msg;
    std::string verdict;     // one-line summary for the UI
};

// `udp_port` is the port to probe UPnP with (the game's, 3658 by default; mapped then removed).
OnlineCheck run_online_check(const std::string& lobby_url, const std::string& name, bool try_upnp, uint16_t udp_port);

}  // namespace dcp::net
