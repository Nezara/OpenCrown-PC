// Minimal STUN client (RFC 5389 Binding Request / Response, IPv4 only), own code, no dependency. Used to learn the
// public (NAT-mapped) UDP endpoint of the game's P2P socket: the UdpCarrier sends the request from its own socket and
// recognises the response in its receive loop (STUN messages start with two zero bits and carry the magic cookie, the
// carrier's own datagrams start with 'DC'), so the mapping we learn is the one peers will reach. See notes/map/net.md 11.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dcp::net::stun {

constexpr uint32_t kMagicCookie = 0x2112A442;
constexpr uint16_t kBindingRequest = 0x0001;
constexpr uint16_t kBindingSuccess = 0x0101;
constexpr uint16_t kBindingError = 0x0111;
constexpr uint16_t kAttrMappedAddress = 0x0001;
constexpr uint16_t kAttrXorMappedAddress = 0x0020;

// Default servers, tried in order ("host:port").
inline const char* const kDefaultServers[] = {"stun.cloudflare.com:3478", "stun.l.google.com:19302"};

struct TxId {
    uint8_t b[12] = {};
};
TxId new_txid();

// 20-byte Binding Request with no attributes.
std::vector<uint8_t> encode_binding_request(const TxId& tx);

// True if the datagram looks like a STUN message (top two bits 0, length fits, magic cookie).
bool looks_like_stun(const uint8_t* p, size_t n);

enum class Parse { NotStun, OtherTx, Error, Ok };
// Parses a Binding Success Response for `tx`; XOR-MAPPED-ADDRESS preferred, MAPPED-ADDRESS accepted. IPv4 only.
// ip_be / port_be in network byte order.
Parse decode_binding_response(const uint8_t* p, size_t n, const TxId& tx, uint32_t* ip_be, uint16_t* port_be);

// Resolves "host:port" (IPv4). Blocking (DNS). False on failure.
bool resolve(const std::string& host_port, uint32_t* ip_be, uint16_t* port_be);

// Servers to use: $DCP_STUN (comma separated "host:port" list; "0"/"off" = disabled -> empty) or the defaults.
std::vector<std::string> configured_servers();

// Blocking query on a fresh ephemeral socket (launcher "Test connection", tools). Retries per RFC 5389 7.2.1-ish
// (resend at 0, 250, 750, 1750 ms ... up to `timeout_ms` per server), then the next server. Returns false if no server
// answered. `server_used` gets the server that answered. Also reports whether a second query from the same socket to
// the fallback server mapped to the same endpoint (endpoint-independent mapping = hole punching friendly) when
// `nat_same` is given: 1 same, 0 different (symmetric NAT: P2P will likely fail), -1 unknown.
bool query_blocking(const std::vector<std::string>& servers, unsigned timeout_ms, uint32_t* ip_be, uint16_t* port_be,
                    std::string* server_used, int* nat_same = nullptr, uint16_t local_port = 0);

std::string ip_str(uint32_t ip_be);
bool is_private_or_loopback(uint32_t ip_be);  // 127/8, 10/8, 172.16/12, 192.168/16, 169.254/16, 100.64/10 (CGNAT)
bool is_rfc1918(uint32_t ip_be);               // 10/8, 172.16/12, 192.168/16 only (home / office LAN addresses)
// IPv4 address of the interface that carries the default route (a connected UDP socket's local address; nothing is
// sent). 0 if none.
uint32_t default_route_ip();

}  // namespace dcp::net::stun
