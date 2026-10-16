// UPnP IGD port mapping of the game's P2P UDP port (miniupnpc, BSD-3, fetched by CMake). Best effort: a router
// without UPnP, a refusal or a timeout is logged and ignored (STUN + hole punching still apply). See net.md 11.2.
//   install_hook(): registers net::set_port_map_hook (carrier.h) unless $DCP_UPNP=0. On open the mapping runs on a
//   helper thread (SSDP discovery takes ~2 s; sceNetBind must not wait); lease kLeaseSeconds, renewed at half-life;
//   removed on socket close and at process exit.
#pragma once
#include <cstdint>
#include <string>

namespace dcp::net::upnp {

constexpr int kLeaseSeconds = 3600;
constexpr int kDiscoverMs = 2000;

enum class State { Off, Pending, Mapped, Failed };
struct Status {
    State state = State::Off;
    uint16_t local_port = 0;
    std::string ext_ip;    // the router's WAN address ("" if unknown)
    uint16_t ext_port = 0;
    std::string message;   // human-readable outcome
};

bool enabled();  // false when $DCP_UPNP is 0/off/false
void install_hook();
Status status();

// Synchronous one-shot (launcher "Test connection"): map `port` UDP for `lease_s` seconds, report, and remove it
// again if `remove_after`. Blocks for discovery (~2 s).
Status probe(uint16_t port, int lease_s, bool remove_after);

}  // namespace dcp::net::upnp
