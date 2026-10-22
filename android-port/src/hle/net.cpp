// SceNet (init, the one P2P socket, byte order, InetNtop) and SceLibRudp, on our own transport (net/rudp.h over the
// UDP carrier, src/net). The game opens exactly one socket, "RUDP_NetId" (AF_INET, type 6 = P2P datagram), when it
// goes online (RudpMgr_OpenSocket 0x813a2746, before create / join) and runs one Rudp context per peer slot over it
// (RudpMgr 0x813a29de..0x813a3344; notes/map/vita_net_109.md 4.6). Offline play never reaches sceNetSocket.
// Layouts and values (asm, 1.09):
//   SceNetSockaddrIn: +0 u8 len 0x10, +1 u8 family 2, +2 u16 port (network order), +4 u32 addr (network order),
//     +8 u16 vport (network order). Bind: port 3658, vport 4649; Initiate: peer address + port from signaling.
//   Setsockopt(s, 0xffff, 0x1000 / 0x2000 / 0x1100 (non-blocking), &1, 4): accepted, the carrier is non-blocking.
//   SceRudpPollEvent: 8 B {+0 ctx, +4 u16 requested, +6 u16 returned} = net::rudp::PollEvent.
//   Rudp event handler 0x813a2997 (eventId, soc, data, len, addr, addrlen, arg): event 2 closes all slots and reopens
//     the socket. We never raise events (a dead carrier shows as kEvError on its contexts instead).
// Both instances on one PC: the port is $DCP_NET_PORT if set, else the game's 3658; when that is taken the socket
// binds any free port, and Matching2 tells the lobby the port actually bound (hle/matching2.cpp).
#include <cstdlib>
#include <cstring>

#include "hle/common.h"
#include "hle/online.h"
#include "net/carrier.h"
#include "net/rudp.h"
#include "net/upnp.h"

namespace hle {
namespace {

namespace rudp = dcp::net::rudp;
constexpr uint32_t net_error(int e) { return 0x80410100u | unsigned(e); }  // SCE_NET_ERROR_E*
constexpr int NET_EBADF = 9, NET_EINVAL = 22, NET_EAFNOSUPPORT = 47, NET_EPROTONOSUPPORT = 43, NET_EADDRINUSE = 48;
constexpr int SOCK_DGRAM_P2P = 6;

uint16_t bswap16(uint16_t v) { return uint16_t((v >> 8) | (v << 8)); }

void ok(Cpu& c) { ret(c, 0); }

// --- SceNet --------------------------------------------------------------------------------------------------------
// sceNetInit({memory, size 0x20000, flags}) (init step 0): the UPnP hook maps the P2P port at bind time (online only).
void NetInit(Cpu& c) {
  if (online_enabled()) dcp::net::upnp::install_hook();
  ret(c, 0);
}
void NetHtons(Cpu& c) { ret(c, bswap16(uint16_t(arg(c, 0)))); }

// sceNetInetNtop(af, const void* src, char* dst, size): dst on success, NULL on failure (debug strings only).
void NetInetNtop(Cpu& c) {
  uint32_t af = arg(c, 0), src = arg(c, 1), dst = arg(c, 2), size = arg(c, 3);
  Memory& m = c.mem();
  if (af != 2 || !src || !dst || !m.valid(src, 4) || !size || !m.valid(dst, size)) return ret(c, 0);
  const uint8_t* a = m.ptr(src);
  char b[16];
  int n = std::snprintf(b, sizeof b, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
  if (n < 0 || uint32_t(n) >= size) return ret(c, 0);
  std::memcpy(m.ptr(dst), b, size_t(n) + 1);
  ret(c, dst);
}

// sceNetSocket(name, domain, type, protocol): only the Rudp P2P socket exists.
void NetSocket(Cpu& c) {
  std::string name = arg(c, 0) ? str(c, arg(c, 0), 32) : "";
  int32_t domain = int32_t(arg(c, 1)), type = int32_t(arg(c, 2));
  HLE_NOTE("sceNetSocket(\"%s\", %d, %d, %d)\n", name.c_str(), domain, type, int32_t(arg(c, 3)));
  if (domain != 2) return ret(c, net_error(NET_EAFNOSUPPORT));
  if (type != SOCK_DGRAM_P2P) return ret(c, net_error(NET_EPROTONOSUPPORT));
  ret(c, uint32_t(rudp::socket_create()));
}
void NetSetsockopt(Cpu& c) { ret(c, 0); }

// sceNetBind(s, const SceNetSockaddrIn*, len)
void NetBind(Cpu& c) {
  int32_t s = int32_t(arg(c, 0));
  uint32_t addr = arg(c, 1), len = arg(c, 2);
  if (!addr || len < 16 || !c.mem().valid(addr, 16)) return ret(c, net_error(NET_EINVAL));
  uint16_t port = bswap16(c.mem().read<uint16_t>(addr + 2)), vport = bswap16(c.mem().read<uint16_t>(addr + 8));
  if (const char* e = std::getenv("DCP_NET_PORT"); e && *e) port = uint16_t(std::atoi(e));
  int32_t r = rudp::socket_bind(s, port, vport);
  if (r != 0 && port) {  // taken (a second instance on this PC): any free port; the lobby gets the real one
    HLE_NOTE("sceNetBind: UDP port %u is taken, binding any free port\n", port);
    r = rudp::socket_bind(s, 0, vport);
  }
  HLE_NOTE("sceNetBind(%d, port %u, vport %u) -> %s, UDP port %u\n", s, port, vport, r == 0 ? "ok" : "FAILED",
           rudp::socket_port(s));
  ret(c, r == 0 ? 0u : net_error(NET_EADDRINUSE));
}
void NetSocketClose(Cpu& c) {
  int32_t s = int32_t(arg(c, 0));
  HLE_NOTE("sceNetSocketClose(%d)\n", s);
  ret(c, rudp::socket_close(s) == 0 ? 0u : net_error(NET_EBADF));
}

// --- SceLibRudp ----------------------------------------------------------------------------------------------------
// RudpMgr_Init (0x813a2afa): Init(pool, 0x100000), GetStatus(buf, 0xf8), EnableInternalIOThread(0x2000, 0xbf),
// SetEventHandler(0x813a2997, this), PollCreate(0x80); results unchecked. The carrier owns its I/O thread.
void RudpInit(Cpu& c) { ret(c, uint32_t(rudp::init())); }
void RudpEnd(Cpu& c) { ret(c, uint32_t(rudp::end())); }
void RudpGetStatus(Cpu& c) {
  uint32_t p = arg(c, 0), n = arg(c, 1);
  if (p && n && n <= 0x1000 && c.mem().valid(p, n)) std::memset(c.mem().ptr(p), 0, n);
  ret(c, 0);
}
// sceRudpCreateContext(handler, arg, int* ctx): RudpMgr_Connect passes (NULL, NULL, &ctx).
void RudpCreateContext(Cpu& c) {
  uint32_t out = arg(c, 2);
  if (!out || !c.mem().valid(out, 4)) return ret(c, uint32_t(rudp::kErrInvalid));
  int32_t id = 0;
  int32_t r = rudp::create_context(&id);
  if (r == 0) c.mem().write<int32_t>(out, id);
  ret(c, uint32_t(r));
}
// sceRudpBind(ctx, soc, vport 20, muxMode 1)
void RudpBind(Cpu& c) {
  ret(c, uint32_t(rudp::bind(int32_t(arg(c, 0)), int32_t(arg(c, 1)), uint16_t(arg(c, 2)), int32_t(arg(c, 3) & 0xff))));
}
// sceRudpSetOption / GetOption(ctx, option, value*, len): 2 / 3 = buffer sizes 0x40000; 14 is read on connect and on
// error and must succeed with a value >= 0 (rudp.cpp).
void RudpSetOption(Cpu& c) {
  uint32_t v = arg(c, 2), len = arg(c, 3);
  if (!v || !c.mem().valid(v, len)) return ret(c, uint32_t(rudp::kErrInvalid));
  ret(c, uint32_t(rudp::set_option(int32_t(arg(c, 0)), int32_t(arg(c, 1)), c.mem().ptr(v), len)));
}
void RudpGetOption(Cpu& c) {
  uint32_t v = arg(c, 2), len = arg(c, 3);
  if (!v || !c.mem().valid(v, len)) return ret(c, uint32_t(rudp::kErrInvalid));
  ret(c, uint32_t(rudp::get_option(int32_t(arg(c, 0)), int32_t(arg(c, 1)), c.mem().ptr(v), len)));
}
// sceRudpInitiate(ctx, const SceNetSockaddrIn* {peer address, peer port from signaling, vport 4649}, 0x10, 0)
void RudpInitiate(Cpu& c) {
  int32_t ctx = int32_t(arg(c, 0));
  uint32_t addr = arg(c, 1), len = arg(c, 2);
  if (!addr || len < 16 || !c.mem().valid(addr, 16)) return ret(c, uint32_t(rudp::kErrInvalid));
  dcp::net::PeerAddr p;
  p.port_be = c.mem().read<uint16_t>(addr + 2);
  p.ip_be = c.mem().read<uint32_t>(addr + 4);
  p.vport = bswap16(c.mem().read<uint16_t>(addr + 8));
  const uint8_t* ip = c.mem().ptr(addr + 4);
  HLE_NOTE("sceRudpInitiate(ctx %d -> %u.%u.%u.%u:%u vport %u)\n", ctx, ip[0], ip[1], ip[2], ip[3], bswap16(p.port_be),
           p.vport);
  ret(c, uint32_t(rudp::initiate(ctx, p)));
}
// sceRudpRead(ctx, buf, len 0x40000, flags, info* NULL): one whole message per call (each is one DcNet packet).
void RudpRead(Cpu& c) {
  uint32_t buf = arg(c, 1), len = arg(c, 2);
  if (!buf || !c.mem().valid(buf, len)) return ret(c, uint32_t(rudp::kErrInvalid));
  ret(c, uint32_t(rudp::read(int32_t(arg(c, 0)), c.mem().ptr(buf), len)));
}
// sceRudpWrite(ctx, buf, len, flags): the whole message or kErrWouldBlock (the game retries).
void RudpWrite(Cpu& c) {
  uint32_t buf = arg(c, 1), len = arg(c, 2);
  if (len && (!buf || !c.mem().valid(buf, len))) return ret(c, uint32_t(rudp::kErrInvalid));
  ret(c, uint32_t(rudp::write(int32_t(arg(c, 0)), len ? c.mem().ptr(buf) : nullptr, len)));
}
void RudpTerminate(Cpu& c) { ret(c, uint32_t(rudp::terminate(int32_t(arg(c, 0))))); }
void RudpPollCreate(Cpu& c) { ret(c, uint32_t(rudp::poll_create(int32_t(arg(c, 0))))); }
void RudpPollDestroy(Cpu& c) { ret(c, uint32_t(rudp::poll_destroy(int32_t(arg(c, 0))))); }
// sceRudpPollControl(poll, op 1 add / 3 delete, ctx, events 0xb)
void RudpPollControl(Cpu& c) {
  ret(c, uint32_t(rudp::poll_control(int32_t(arg(c, 0)), int32_t(arg(c, 1)), int32_t(arg(c, 2)), uint16_t(arg(c, 3)))));
}
// sceRudpPollWait(poll, SceRudpPollEvent* ev, size 0x80, u64 timeout 0): the u64 is on the stack (AAPCS, r3 unused).
void RudpPollWait(Cpu& c) {
  uint32_t ev = arg(c, 1), n = arg(c, 2);
  uint64_t timeout = c.mem().read<uint64_t>(c.reg(13));
  if (!ev || !n || n > 0x1000 || !c.mem().valid(ev, n * 8)) return ret(c, uint32_t(rudp::kErrInvalid));
  if (timeout > 100000) timeout = 100000;  // never block a game thread long (the game passes 0)
  ret(c, uint32_t(rudp::poll_wait(int32_t(arg(c, 0)), reinterpret_cast<rudp::PollEvent*>(c.mem().ptr(ev)), int32_t(n),
                                  timeout)));
}

}  // namespace

void register_net() {
  REG(sceNetInit, NetInit);
  REG(sceNetTerm, ok);
  REG(sceNetHtons, NetHtons);
  REG(sceNetNtohs, NetHtons);
  REG(sceNetInetNtop, NetInetNtop);
  REG(sceNetSocket, NetSocket);
  REG(sceNetSetsockopt, NetSetsockopt);
  REG(sceNetBind, NetBind);
  REG(sceNetSocketClose, NetSocketClose);

  REG(sceRudpInit, RudpInit);
  REG(sceRudpEnd, RudpEnd);
  REG(sceRudpGetStatus, RudpGetStatus);
  REG(sceRudpEnableInternalIOThread, ok);
  REG(sceRudpSetEventHandler, ok);
  REG(sceRudpCreateContext, RudpCreateContext);
  REG(sceRudpBind, RudpBind);
  REG(sceRudpSetOption, RudpSetOption);
  REG(sceRudpGetOption, RudpGetOption);
  REG(sceRudpInitiate, RudpInitiate);
  REG(sceRudpRead, RudpRead);
  REG(sceRudpWrite, RudpWrite);
  REG(sceRudpTerminate, RudpTerminate);
  REG(sceRudpPollCreate, RudpPollCreate);
  REG(sceRudpPollDestroy, RudpPollDestroy);
  REG(sceRudpPollControl, RudpPollControl);
  REG(sceRudpPollWait, RudpPollWait);
}

}  // namespace hle
