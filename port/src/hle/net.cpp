// libSceNet (the P2P socket path only), libSceRudp and the Matching2 signaling address lookup. The game opens exactly
// one socket, "RUDP_NetId" (AF_INET, type 6 = SOCK_DGRAM_P2P), at room create/join (RudpMgr_OpenSocket FUN_000bd4c0)
// and runs Rudp contexts over it (notes/map/net.md 2.1, 2.4, 4.1); offline play never reaches sceNetSocket.
// The work is done by net/rudp (host C++); these are the SysV entry points. Struct layouts confirmed in asm:
//   SceNetSockaddrIn (bd584..bd5bf, bd9e7..bda1c): +0 u8 len 0x10, +1 u8 family 2, +2 u16 port (net order),
//     +4 u32 addr (net order), +8 u16 vport (net order), rest zero.
//   SceRudpPollEvent (bdd40): 8 B, +0 int ctx, +6 u16 returned events.
//   Rudp event handler (bd240): handler(eventId, ..., arg as 7th arg on the stack). Event 2 makes the game close and
//     reopen its socket (bd260). We never raise events (no guest calls from host I/O threads).
#include <cstdlib>
#include <cstring>

#include "common.h"
#include "net/carrier.h"
#include "net/rudp.h"
#include "net/upnp.h"

namespace dcp::hle {
namespace {

namespace rudp = net::rudp;
constexpr int32_t kNetErr(int e) { return int32_t(0x80410100u | unsigned(e)); }  // SCE_NET_ERROR_E*
constexpr int kNetEBADF = 9, kNetEINVAL = 22, kNetEPROTONOSUPPORT = 43, kNetEADDRINUSE = 48;
constexpr int kSockDgramP2P = 6;

uint16_t bswap16(uint16_t v) { return uint16_t((v >> 8) | (v << 8)); }

// ---- libSceNet ----
DCP_SYSV int32_t sceNetInit() {
    net::upnp::install_hook();  // UPnP mapping of the P2P port at bind (net.md 11.2); no network until then
    return kOk;
}
DCP_SYSV int32_t sceNetTerm() { return kOk; }
DCP_SYSV int32_t sceNetPoolCreate(const char* name, int32_t size, int32_t) {
    DCP_HLE_LOG("sceNetPoolCreate", "%s size 0x%x", name ? name : "?", size);
    return 1;  // pool id (FUN_00599d00 stores it at platform+0x123d4; only passed to the sceHttpInit stub)
}
DCP_SYSV int32_t sceNetPoolDestroy(int32_t) { return kOk; }
DCP_SYSV uint16_t sceNetHtons(uint16_t v) { return bswap16(v); }
DCP_SYSV uint16_t sceNetNtohs(uint16_t v) { return bswap16(v); }
DCP_SYSV uint32_t sceNetHtonl(uint32_t v) { return __builtin_bswap32(v); }
DCP_SYSV uint32_t sceNetNtohl(uint32_t v) { return __builtin_bswap32(v); }

DCP_SYSV int32_t sceNetSocket(const char* name, int32_t family, int32_t type, int32_t proto) {
    std::fprintf(stderr, "HLE sceNetSocket(\"%s\", %d, %d, %d)\n", name ? name : "", family, type, proto);
    if (family != 2 || type != kSockDgramP2P) return kNetErr(kNetEPROTONOSUPPORT);  // only the Rudp P2P socket
    return rudp::socket_create();
}

// Called with SOL_SOCKET 0xffff, opts 0x20000 / 0x40000 / 0x1200 (nonblocking), value 1 (asm bd517..bd577). The
// carrier socket is always nonblocking; nothing to apply.
DCP_SYSV int32_t sceNetSetsockopt(int32_t s, int32_t level, int32_t opt, const void*, uint32_t) {
    DCP_HLE_LOG("sceNetSetsockopt", "s %d level 0x%x opt 0x%x", s, level, opt);
    return kOk;
}

// Real port 3658 / vport 4649 (asm bd588..bd5bf). DCP_NET_PORT overrides the host UDP port (two instances on one PC).
DCP_SYSV int32_t sceNetBind(int32_t s, const uint8_t* addr, uint32_t len) {
    if (!addr || len < 16) return kNetErr(kNetEINVAL);
    uint16_t port, vport;
    std::memcpy(&port, addr + 2, 2);
    std::memcpy(&vport, addr + 8, 2);
    port = bswap16(port), vport = bswap16(vport);
    if (const char* e = std::getenv("DCP_NET_PORT"); e && *e) port = uint16_t(std::atoi(e));
    int32_t r = rudp::socket_bind(s, port, vport);
    std::fprintf(stderr, "HLE sceNetBind(%d, port %u, vport %u) -> %s, UDP port %u\n", s, port, vport,
                 r == 0 ? "ok" : "FAILED", rudp::socket_port(s));
    return r == 0 ? kOk : kNetErr(kNetEADDRINUSE);
}

DCP_SYSV int32_t sceNetSocketClose(int32_t s) {
    std::fprintf(stderr, "HLE sceNetSocketClose(%d)\n", s);
    return rudp::socket_close(s) == 0 ? kOk : kNetErr(kNetEBADF);
}

// ---- libSceRudp ----
void* g_handler = nullptr;  // SetEventHandler (FUN_000bd240); stored, never called (see header comment)
void* g_handler_arg = nullptr;

DCP_SYSV int32_t sceRudpInit(void* pool, int32_t size) {
    DCP_HLE_LOG("sceRudpInit", "pool %p size 0x%x", pool, size);
    return rudp::init();
}
DCP_SYSV int32_t sceRudpEnd() { return rudp::end(); }
DCP_SYSV int32_t sceRudpGetStatus(void* buf, uint64_t size) {  // (buf, 0xf8) at bd6dd; result unused
    if (buf) std::memset(buf, 0, size);
    return kOk;
}
DCP_SYSV int32_t sceRudpEnableInternalIOThread(uint32_t, uint32_t) { return kOk; }  // carriers own their I/O
DCP_SYSV int32_t sceRudpSetEventHandler(void* fn, void* arg) {
    g_handler = fn, g_handler_arg = arg;
    return kOk;
}
// (0, 0, &id) at bd973.
DCP_SYSV int32_t sceRudpCreateContext(void*, void*, int32_t* ctx) { return rudp::create_context(ctx); }
// (ctx, sock, vport 20, mux 1) at bd996.
DCP_SYSV int32_t sceRudpBind(int32_t ctx, int32_t soc, uint16_t vport, uint8_t mux) {
    return rudp::bind(ctx, soc, vport, mux);
}
DCP_SYSV int32_t sceRudpSetOption(int32_t ctx, int32_t opt, const void* v, uint32_t len) {
    return rudp::set_option(ctx, opt, v, len);
}
DCP_SYSV int32_t sceRudpGetOption(int32_t ctx, int32_t opt, void* v, uint32_t len) {
    return rudp::get_option(ctx, opt, v, len);
}
// (ctx, sockaddr{peer ip, peer port from signaling, vport htons(4649)}, 0x10, 0) at bda20.
DCP_SYSV int32_t sceRudpInitiate(int32_t ctx, const uint8_t* addr, uint32_t len, int32_t) {
    if (!addr || len < 16) return rudp::kErrInvalid;
    net::PeerAddr p;
    std::memcpy(&p.port_be, addr + 2, 2);
    std::memcpy(&p.ip_be, addr + 4, 4);
    std::memcpy(&p.vport, addr + 8, 2);
    p.vport = bswap16(p.vport);
    const uint8_t* ip = addr + 4;
    std::fprintf(stderr, "HLE sceRudpInitiate(ctx %d -> %u.%u.%u.%u:%u vport %u)\n", ctx, ip[0], ip[1], ip[2], ip[3],
                 bswap16(p.port_be), p.vport);
    return rudp::initiate(ctx, p);
}
DCP_SYSV int32_t sceRudpRead(int32_t ctx, void* buf, uint64_t len, uint8_t, void*) {
    return rudp::read(ctx, buf, len);
}
DCP_SYSV int32_t sceRudpWrite(int32_t ctx, const void* buf, uint64_t len, uint8_t) {
    return rudp::write(ctx, buf, len);
}
DCP_SYSV int32_t sceRudpTerminate(int32_t ctx) { return rudp::terminate(ctx); }
DCP_SYSV int32_t sceRudpPollCreate(uint64_t size) { return rudp::poll_create(int32_t(size)); }
DCP_SYSV int32_t sceRudpPollDestroy(int32_t poll) { return rudp::poll_destroy(poll); }
DCP_SYSV int32_t sceRudpPollControl(int32_t poll, int32_t op, int32_t ctx, uint16_t ev) {
    return rudp::poll_control(poll, op, ctx, ev);
}
// (poll, buf, 0x80, timeout 0) at bdbe0.
DCP_SYSV int32_t sceRudpPollWait(int32_t poll, rudp::PollEvent* ev, uint64_t n, uint64_t timeout_us) {
    return rudp::poll_wait(poll, ev, int32_t(n), timeout_us);
}

// ---- libSceNpMatching2 (address lookup only; the rest of Matching2 is a later step) ----
// (ctxId, roomId, memberId, &status, &peerAddr, &peerPort) from the Established handler at b6476; addr and port are
// network order (the port goes unchanged into the Initiate sockaddr, bd8df/bda05). Status 2 = active (SDK value).
DCP_SYSV int32_t sceNpMatching2SignalingGetConnectionStatus(uint16_t, uint64_t, uint16_t member, int32_t* status,
                                                            uint32_t* addr, uint16_t* port) {
    uint32_t ip = 0;
    uint16_t pt = 0;
    bool ok = net::get_peer_address(member, &ip, &pt);
    if (status) *status = ok ? 2 : 0;
    if (addr) *addr = ip;
    if (port) *port = pt;
    DCP_HLE_LOG("sceNpMatching2SignalingGetConnectionStatus", "member 0x%x -> %s", member, ok ? "known" : "unknown");
    return ok ? kOk : int32_t(0x80022b05);  // error value unverified; the game ignores the result (b647b)
}

}  // namespace

void register_net() {
    const char* n = "libSceNet";
    REG(n, sceNetInit); REG(n, sceNetTerm); REG(n, sceNetPoolCreate); REG(n, sceNetPoolDestroy);
    REG(n, sceNetHtons); REG(n, sceNetNtohs); REG(n, sceNetHtonl); REG(n, sceNetNtohl);
    REG(n, sceNetSocket); REG(n, sceNetSetsockopt); REG(n, sceNetBind); REG(n, sceNetSocketClose);
    const char* r = "libSceRudp";
    REG(r, sceRudpInit); REG(r, sceRudpEnd); REG(r, sceRudpGetStatus); REG(r, sceRudpEnableInternalIOThread);
    REG(r, sceRudpSetEventHandler); REG(r, sceRudpCreateContext); REG(r, sceRudpBind); REG(r, sceRudpSetOption);
    REG(r, sceRudpGetOption); REG(r, sceRudpInitiate); REG(r, sceRudpRead); REG(r, sceRudpWrite);
    REG(r, sceRudpTerminate); REG(r, sceRudpPollCreate); REG(r, sceRudpPollDestroy); REG(r, sceRudpPollControl);
    REG(r, sceRudpPollWait);
    REG("libSceNpMatching2", sceNpMatching2SignalingGetConnectionStatus);
}

}  // namespace dcp::hle
