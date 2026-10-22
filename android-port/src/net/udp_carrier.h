// UdpCarrier: reliable ordered messages over one UDP socket (own thin framing, no third-party lib).
// Wire format (all little endian), every datagram:
//   u16 magic 0x4344 'DC' | u8 version 1 | u8 type | u16 src Rudp port | u16 dst Rudp port | u32 sender session
//   SYN 1 / SYNACK 2: no payload (simultaneous open: each side sends SYN until it hears SYN or SYNACK)
//   DATA 3: u32 seq | u8 flags (bit0 = last fragment of a message) | payload (<= kFragment bytes)
//   ACK 4: u32 next expected seq | u32 bitmap (bit i = seq next+1+i received)
//   FIN 5, PING 6 (keepalive): no payload
// Connections are keyed by (peer ip, peer UDP port, Rudp port; both ends use the same Rudp port). A session nonce per
// connect() detects a restarted peer (SYN with a new session while connected = reset).
#pragma once
#include <memory>

#include "net/carrier.h"

namespace dcp::net {

struct UdpCarrierOptions {
    uint16_t port = 0;                 // host order; 0 = any free port
    uintptr_t adopt_socket = ~uintptr_t(0);  // an already bound UDP sock::Socket to use instead (e.g. from an ICE layer)
    int drop_every_n = 0;              // test only: drop every n-th outgoing datagram (deterministic loss)
    unsigned connect_timeout_ms = 15000;
    unsigned idle_timeout_ms = 10000;  // no datagram from a connected peer for this long = Timeout
};

// Returns nullptr if the socket cannot be created/bound.
std::unique_ptr<Carrier> make_udp_carrier(const UdpCarrierOptions& opt, CarrierSink* sink);

}  // namespace dcp::net
