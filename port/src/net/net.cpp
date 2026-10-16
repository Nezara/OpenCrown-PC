// Carrier factory, port-mapping hook and the peer address table (carrier.h).
#include <map>
#include <mutex>

#include "net/carrier.h"
#include "net/udp_carrier.h"

namespace dcp::net {
namespace {
std::mutex g_mu;
CarrierFactory g_factory;
PortMapHook g_portmap;
std::map<uint16_t, std::pair<uint32_t, uint16_t>> g_peers;
struct Alt {
    uint32_t pub_ip, alt_ip;
    uint16_t pub_port, alt_port;
};
std::map<uint16_t, Alt> g_alts;  // member id -> LAN candidate
}  // namespace

void set_carrier_factory(CarrierFactory f) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_factory = std::move(f);
}

std::unique_ptr<Carrier> make_carrier(uint16_t port, CarrierSink* sink) {
    CarrierFactory f;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        f = g_factory;
    }
    if (f) return f(port, sink);
    UdpCarrierOptions o;
    o.port = port;
    return make_udp_carrier(o, sink);
}

void set_port_map_hook(PortMapHook h) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_portmap = std::move(h);
}

void run_port_map_hook(uint16_t local_port, bool open) {
    PortMapHook h;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        h = g_portmap;
    }
    if (h && local_port) h(local_port, open);
}

void set_peer_address(uint16_t member_id, uint32_t ip_be, uint16_t port_be) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_peers[member_id] = {ip_be, port_be};
}
void clear_peer_address(uint16_t member_id) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_peers.erase(member_id);
    g_alts.erase(member_id);
}
void clear_peer_addresses() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_peers.clear();
    g_alts.clear();
}
bool get_peer_address(uint16_t member_id, uint32_t* ip_be, uint16_t* port_be) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_peers.find(member_id);
    if (it == g_peers.end()) return false;
    if (ip_be) *ip_be = it->second.first;
    if (port_be) *port_be = it->second.second;
    return true;
}

void set_peer_alt(uint16_t member_id, uint32_t pub_ip_be, uint16_t pub_port_be, uint32_t alt_ip_be, uint16_t alt_port_be) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_alts[member_id] = {pub_ip_be, alt_ip_be, pub_port_be, alt_port_be};
}
bool get_alt_address(uint32_t pub_ip_be, uint16_t pub_port_be, uint32_t* alt_ip_be, uint16_t* alt_port_be) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& [id, a] : g_alts)
        if (a.pub_ip == pub_ip_be && a.pub_port == pub_port_be) {
            *alt_ip_be = a.alt_ip, *alt_port_be = a.alt_port;
            return true;
        }
    return false;
}

}  // namespace dcp::net
