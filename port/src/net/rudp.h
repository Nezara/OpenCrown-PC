// libSceRudp + P2P socket model in host C++ (no SysV): the HLE wrappers in hle/net.cpp forward here, and tests drive
// it directly. Semantics follow what the game's RudpMgr (FUN_000bd100..be1d0, notes/map/net.md 4.1) relies on:
//  - P2P socket = one Carrier, created at socket_bind (sceNetBind) on the requested UDP port.
//  - contexts bound to a socket + vport; initiate() on both peers; one context per peer slot (game uses 4).
//  - write(): whole message accepted (returns len) or kErrWouldBlock; read(): one whole message per call.
//  - poll events: kEvRead (message queued), kEvWrite (connected and send buffer has room), kEvError (closed).
// Error codes are the values the game compares against (asm bdc4c, bde35, bdc9a); names are ours.
#pragma once
#include <cstddef>
#include <cstdint>

#include "net/carrier.h"

namespace dcp::net::rudp {

constexpr int32_t kErrNotInit = int32_t(0x80770001);     // (value unverified)
constexpr int32_t kErrInvalid = int32_t(0x80770004);     // (value unverified)
constexpr int32_t kErrBusy = int32_t(0x80770007);        // game retries Write on this (asm bdc9a)
constexpr int32_t kErrWouldBlock = int32_t(0x80770016);  // no data / cannot send now (asm bdc4c, bdcaa)
constexpr int32_t kErrClosed = int32_t(0x80770024);      // connection closed, nothing more to read (asm bdc53)

enum : uint16_t { kEvRead = 1, kEvWrite = 2, kEvError = 8 };
enum : int { kPollAdd = 1, kPollModify = 2, kPollRemove = 3 };

// SceRudpPollEvent: 8 bytes, ctx id at +0, returned events at +6 (asm bdd40: word [ev + 6], dword [ev + 0]).
struct PollEvent {
    int32_t ctx;
    uint16_t req_events;
    uint16_t rtn_events;
};
static_assert(sizeof(PollEvent) == 8);

enum class CtxState { Created, Bound, Connecting, Connected, Closed };

int32_t init();
int32_t end();
int32_t create_context(int32_t* out_ctx);
int32_t bind(int32_t ctx, int32_t socket, uint16_t vport, int32_t mux_mode);
int32_t set_option(int32_t ctx, int32_t opt, const void* val, uint32_t len);
int32_t get_option(int32_t ctx, int32_t opt, void* val, uint32_t len);
int32_t initiate(int32_t ctx, const PeerAddr& peer);
int32_t read(int32_t ctx, void* buf, size_t len);
int32_t write(int32_t ctx, const void* buf, size_t len);
int32_t terminate(int32_t ctx);
int32_t poll_create(int32_t size);
int32_t poll_destroy(int32_t poll);
int32_t poll_control(int32_t poll, int32_t op, int32_t ctx, uint16_t events);
// Returns the number of events written (0 on timeout). timeout_us 0 = do not wait (what the game passes).
int32_t poll_wait(int32_t poll, PollEvent* out, int32_t max, uint64_t timeout_us);
CtxState context_state(int32_t ctx);  // Closed for unknown ids

// P2P sockets (libSceNet type 6). Ids are >= 1.
int32_t socket_create();
int32_t socket_bind(int32_t socket, uint16_t port_host, uint16_t vport);  // creates the carrier
int32_t socket_close(int32_t socket);
uint16_t socket_port(int32_t socket);  // actual local UDP port (host order), 0 if unbound
int open_socket_count();               // sockets with a live carrier (diagnostics: offline play opens none)
uint16_t bound_port();                 // local UDP port of the first live P2P socket (host order), 0 if none
void log_stats(const char* tag);       // one stderr line per context (message / byte counters)
// STUN result of the first live P2P socket (started at bind unless $DCP_STUN=0; net.md 11). Idle if no socket.
Carrier::StunState public_endpoint(uint32_t* ip_be, uint16_t* port_be);
// Hole punch from the first live P2P socket towards a peer endpoint (network order). No-op without a socket.
void punch(uint32_t ip_be, uint16_t port_be);

}  // namespace dcp::net::rudp
