// Carrier: the transport under the libSceRudp shim (net/rudp.h). A carrier moves whole messages reliably and in order
// between this process and a peer; Rudp keeps the game-facing semantics (contexts, slots, poll events, status) on top.
// Implementations: UdpCarrier (net/udp_carrier.h, own thin reliable framing over one UDP socket: the direct P2P path and
// the test carrier). Future: an ICE/TURN carrier (hole punching via STUN, TURN fallback) can plug in here, or hand an
// already-punched socket to UdpCarrier. See notes/map/net.md "Transport shim".
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dcp::net {

// Peer endpoint as the game gives it to sceRudpInitiate: IPv4 address and UDP port in network byte order (copied
// unchanged from SceNetSockaddrIn / Matching2 signaling), Rudp virtual port in host order.
struct PeerAddr {
    uint32_t ip_be = 0;
    uint16_t port_be = 0;
    uint16_t vport = 0;
};

using ConnId = int;
enum class LinkState { Connecting, Connected, Closed };
enum class CloseReason { Local, PeerClosed, Timeout, Reset, Error };

// Events from a carrier. Called on the carrier's I/O thread with no carrier lock held (a sink may call back into the
// carrier). Order per connection: on_connected, then on_message*, then on_closed once.
struct CarrierSink {
    virtual ~CarrierSink() = default;
    virtual void on_connected(ConnId) = 0;
    virtual void on_message(ConnId, const uint8_t* data, size_t len) = 0;
    virtual void on_closed(ConnId, CloseReason) = 0;
};

class Carrier {
public:
    virtual ~Carrier() = default;
    // Start a connection to `peer` from local virtual port `local_vport`. Both sides call this (simultaneous open, as
    // the game does with sceRudpInitiate); the link comes up once both have. Returns -1 on error.
    virtual ConnId connect(const PeerAddr& peer, uint16_t local_vport) = 0;
    // Queue one message (reliable, ordered, boundaries preserved). Allowed while Connecting (sent once connected).
    // False if the connection is closed or unknown.
    virtual bool send(ConnId, const uint8_t* data, size_t len) = 0;
    // Bytes accepted by send() and not yet acknowledged by the peer.
    virtual size_t queued_bytes(ConnId) = 0;
    virtual LinkState state(ConnId) = 0;
    // Close (graceful notice to the peer, best effort) and forget the connection. No on_closed for it afterwards.
    virtual void disconnect(ConnId) = 0;
    // Local UDP port in host order (0 if not applicable).
    virtual uint16_t local_port() const = 0;

    // Public endpoint discovery (STUN from this carrier's own socket, net/stun.h). start_stun returns at once (DNS and
    // retries run on the carrier's threads); stun_result polls. Ip/port in network order. Default: unsupported.
    enum class StunState { Idle, Running, Done, Failed };
    virtual void start_stun(const std::vector<std::string>& servers) { (void)servers; }
    virtual StunState stun_result(uint32_t* ip_be, uint16_t* port_be) {
        (void)ip_be, (void)port_be;
        return StunState::Failed;
    }
    // Hole punching: send a few throwaway datagrams to `peer` from this socket so our NAT opens a mapping towards it
    // before the real SYNs arrive (receivers drop them). Best effort.
    virtual void punch(uint32_t ip_be, uint16_t port_be) { (void)ip_be, (void)port_be; }
};

// Creates the carrier for a bound P2P socket (sceNetBind). Default: UdpCarrier on `port` (host order, 0 = any).
// A future ICE carrier replaces this. Returns nullptr on failure.
using CarrierFactory = std::function<std::unique_ptr<Carrier>(uint16_t port, CarrierSink* sink)>;
void set_carrier_factory(CarrierFactory f);  // empty = default
std::unique_ptr<Carrier> make_carrier(uint16_t port, CarrierSink* sink);

// Port-mapping hook (future UPnP IGD / NAT-PMP step): called with the carrier's actual local UDP port right after a
// P2P socket is bound (open = true) and before that port is advertised, and again when it closes (open = false).
// Runs on the caller's thread (the game thread inside sceNetBind / sceNetSocketClose): keep it quick or make it async;
// it must never fail the bind. Empty = no mapping.
using PortMapHook = std::function<void(uint16_t local_port, bool open)>;
void set_port_map_hook(PortMapHook h);
void run_port_map_hook(uint16_t local_port, bool open);

// Peer address source for the Matching2 signaling shim (filled by the future lobby/signaling code; read by
// sceNpMatching2SignalingGetConnectionStatus). memberId as the game sees it (0x10 * (slot + 1), net.md 5.3).
void set_peer_address(uint16_t member_id, uint32_t ip_be, uint16_t port_be);
void clear_peer_address(uint16_t member_id);
void clear_peer_addresses();
bool get_peer_address(uint16_t member_id, uint32_t* ip_be, uint16_t* port_be);
// LAN candidate of a member behind the same NAT as us (lobby signal local_ip/local_port, net.md 11.1): a carrier
// connecting to that member's public endpoint also sends its SYNs to the candidate and keeps whichever answers. Cleared
// with the member's peer address.
void set_peer_alt(uint16_t member_id, uint32_t pub_ip_be, uint16_t pub_port_be, uint32_t alt_ip_be, uint16_t alt_port_be);
bool get_alt_address(uint32_t pub_ip_be, uint16_t pub_port_be, uint32_t* alt_ip_be, uint16_t* alt_port_be);

}  // namespace dcp::net
