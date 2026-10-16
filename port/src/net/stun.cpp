// Minimal STUN client (stun.h).
#include "net/stun.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dcp::net::stun {
namespace {

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

bool wsa_init() {
    static bool ok = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    return ok;
}

}  // namespace

TxId new_txid() {
    static std::atomic<uint64_t> ctr{0};
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    uint64_t s = uint64_t(q.QuadPart) ^ (uint64_t(GetCurrentProcessId()) << 32) ^ (++ctr * 0x9e3779b97f4a7c15ull);
    TxId t;
    for (int i = 0; i < 12; ++i) {  // splitmix64 stream
        if (i % 8 == 0) {
            s += 0x9e3779b97f4a7c15ull;
            uint64_t z = s;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
            s ^= z ^ (z >> 31);
        }
        t.b[i] = uint8_t(s >> (8 * (i % 8)));
    }
    return t;
}

std::vector<uint8_t> encode_binding_request(const TxId& tx) {
    std::vector<uint8_t> m(20, 0);
    m[0] = kBindingRequest >> 8, m[1] = kBindingRequest & 0xff;  // length 0 (m[2..3])
    m[4] = 0x21, m[5] = 0x12, m[6] = 0xA4, m[7] = 0x42;
    std::memcpy(m.data() + 8, tx.b, 12);
    return m;
}

bool looks_like_stun(const uint8_t* p, size_t n) {
    return n >= 20 && (p[0] & 0xC0) == 0 && be32(p + 4) == kMagicCookie && (be16(p + 2) & 3) == 0 &&
           size_t(be16(p + 2)) + 20 <= n;
}

Parse decode_binding_response(const uint8_t* p, size_t n, const TxId& tx, uint32_t* ip_be, uint16_t* port_be) {
    if (!looks_like_stun(p, n)) return Parse::NotStun;
    if (std::memcmp(p + 8, tx.b, 12) != 0) return Parse::OtherTx;
    if (be16(p) != kBindingSuccess) return Parse::Error;
    size_t end = 20 + be16(p + 2);
    bool have = false, have_xor = false;
    uint32_t ip = 0;
    uint16_t port = 0;
    for (size_t o = 20; o + 4 <= end;) {
        uint16_t type = be16(p + o), len = be16(p + o + 2);
        const uint8_t* v = p + o + 4;
        if (o + 4 + len > end) return Parse::Error;
        if ((type == kAttrXorMappedAddress || type == kAttrMappedAddress) && len >= 8 && v[1] == 0x01 /*IPv4*/) {
            bool x = type == kAttrXorMappedAddress;
            if (x || !have_xor) {
                uint16_t pt = be16(v + 2);
                uint32_t a = be32(v + 4);
                if (x) pt ^= uint16_t(kMagicCookie >> 16), a ^= kMagicCookie;
                ip = htonl(a), port = htons(pt), have = true, have_xor = have_xor || x;
            }
        }
        o += 4 + ((len + 3u) & ~3u);
    }
    if (!have) return Parse::Error;
    if (ip_be) *ip_be = ip;
    if (port_be) *port_be = port;
    return Parse::Ok;
}

bool resolve(const std::string& host_port, uint32_t* ip_be, uint16_t* port_be) {
    if (!wsa_init()) return false;
    size_t c = host_port.rfind(':');
    std::string host = c == std::string::npos ? host_port : host_port.substr(0, c);
    int port = c == std::string::npos ? 3478 : std::atoi(host_port.c_str() + c + 1);
    if (host.empty() || port <= 0 || port > 65535) return false;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    *ip_be = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
    *port_be = htons(uint16_t(port));
    freeaddrinfo(res);
    return true;
}

std::vector<std::string> configured_servers() {
    std::vector<std::string> v;
    const char* e = std::getenv("DCP_STUN");
    if (e && *e) {
        std::string s = e;
        if (s == "0" || s == "off" || s == "none") return v;
        size_t i = 0;
        while (i <= s.size()) {
            size_t j = s.find(',', i);
            if (j == std::string::npos) j = s.size();
            if (j > i) v.push_back(s.substr(i, j - i));
            i = j + 1;
        }
        return v;
    }
    for (const char* d : kDefaultServers) v.push_back(d);
    return v;
}

namespace {
// One server, on an existing socket. Resend schedule 0, 250, 750, 1750, 3750 ... (doubling) until timeout_ms.
bool query_one(SOCKET s, uint32_t sip, uint16_t sport, unsigned timeout_ms, uint32_t* ip_be, uint16_t* port_be) {
    TxId tx = new_txid();
    std::vector<uint8_t> req = encode_binding_request(tx);
    sockaddr_in to{};
    to.sin_family = AF_INET, to.sin_addr.s_addr = sip, to.sin_port = sport;
    uint64_t start = GetTickCount64(), next = start;
    unsigned gap = 250;
    for (;;) {
        uint64_t t = GetTickCount64();
        if (t - start >= timeout_ms) return false;
        if (t >= next) {
            sendto(s, reinterpret_cast<const char*>(req.data()), int(req.size()), 0, reinterpret_cast<sockaddr*>(&to),
                   sizeof to);
            next = t + gap, gap *= 2;
        }
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(s, &rs);
        uint64_t wait = std::min<uint64_t>(next, start + timeout_ms) - GetTickCount64();
        if (int64_t(wait) < 1) wait = 1;
        timeval tv{long(wait / 1000), long((wait % 1000) * 1000)};
        if (select(0, &rs, nullptr, nullptr, &tv) <= 0) continue;
        uint8_t buf[1500];
        sockaddr_in from{};
        int fl = sizeof from;
        int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) continue;
        Parse r = decode_binding_response(buf, size_t(n), tx, ip_be, port_be);
        if (r == Parse::Ok) return true;
        if (r == Parse::Error) return false;
    }
}
}  // namespace

bool query_blocking(const std::vector<std::string>& servers, unsigned timeout_ms, uint32_t* ip_be, uint16_t* port_be,
                    std::string* server_used, int* nat_same, uint16_t local_port) {
    if (nat_same) *nat_same = -1;
    if (!wsa_init() || servers.empty()) return false;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET, a.sin_port = htons(local_port), a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { closesocket(s); return false; }
    BOOL no_reset = FALSE;
    DWORD ret = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /*SIO_UDP_CONNRESET*/, &no_reset, sizeof no_reset, nullptr, 0, &ret, nullptr,
             nullptr);
    bool ok = false;
    size_t used = 0;
    for (size_t i = 0; i < servers.size() && !ok; ++i) {
        uint32_t sip;
        uint16_t sport;
        if (!resolve(servers[i], &sip, &sport)) continue;
        if (query_one(s, sip, sport, timeout_ms, ip_be, port_be)) ok = true, used = i;
    }
    if (ok && server_used) *server_used = servers[used];
    if (ok && nat_same) {  // mapping behaviour: same socket, a different server
        for (size_t i = 0; i < servers.size(); ++i) {
            if (i == used) continue;
            uint32_t sip, ip2;
            uint16_t sport, p2;
            if (!resolve(servers[i], &sip, &sport) || sip == 0) continue;
            if (query_one(s, sip, sport, timeout_ms, &ip2, &p2)) {
                *nat_same = ip2 == *ip_be && p2 == *port_be ? 1 : 0;
                break;
            }
        }
    }
    closesocket(s);
    return ok;
}

std::string ip_str(uint32_t ip_be) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&ip_be);
    char t[16];
    std::snprintf(t, sizeof t, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return t;
}

bool is_private_or_loopback(uint32_t ip_be) {
    uint32_t a = ntohl(ip_be);
    return (a >> 24) == 127 || (a >> 24) == 10 || (a >> 20) == 0xAC1 || (a >> 16) == 0xC0A8 || (a >> 16) == 0xA9FE ||
           (a >> 22) == (100u << 2 | 1u) || a == 0;
}

uint32_t default_route_ip() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in to{};
    to.sin_family = AF_INET, to.sin_port = htons(53);
    inet_pton(AF_INET, "1.1.1.1", &to.sin_addr);
    uint32_t ip = 0;
    if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0) {
        sockaddr_in me{};
        int len = sizeof me;
        if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) ip = me.sin_addr.s_addr;
    }
    closesocket(s);
    return ip;
}

bool is_rfc1918(uint32_t ip_be) {
    uint32_t a = ntohl(ip_be);
    return (a >> 24) == 10 || (a >> 20) == 0xAC1 || (a >> 16) == 0xC0A8;
}

}  // namespace dcp::net::stun
