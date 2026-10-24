// SceNpMatching2 (all 20 imports of 1.09) on our lobby client (net/lobby_client.h, protocol in
// notes/map/lobby_protocol.md). Model: port/src/hle/matching2.cpp (the Pro build's shim on the same lobby); the Vita
// layouts below are the 1.09 ones of notes/map/vita_net_109.md section 4, checked in the asm at the addresses given.
//
// Calls (request structs built by the game, DcNetMatching = `this`):
//   Init(pool 0x20000, prio, affinity, stack 0x4000) / Term; CreateContext(SceNpId*, commId*, passphrase*, u16* ctxId);
//   RegisterContextCallback(cb, arg) (no ctx id, 0x8139cb74); ContextStart(ctx, u64 timeout) / Stop / DestroyContext;
//   GetServerLocal(ctx, out {u16 serverId, u8 status = 1});
//   requests (ctx, req*, opt {+0 cb, +4 arg, +8 timeout, +0xc appReqId}*, u32* reqId -> this+0x54)
//   GetWorldInfoList {u16 serverId}; SetUserInfo {u16 serverId, BinAttr* @4, n @8};
//   CreateJoinRoom {worldId @0, maxSlot @0x10, flagAttr @0x14, roomBinAttrInternal* @0x18 n @0x1c (0x57),
//     searchableIntAttrExternal* @0x20 n @0x24 (0x4c..0x53), roomBinAttrExternal* @0x30 n @0x34 (0x55), ...};
//   JoinRoom {u64 roomId @0, ...}; LeaveRoom {u64 roomId}; SearchRoom {option @0, worldId @4, startIndex @0x10,
//     max @0x14, flagFilter @0x18, flagAttr @0x1c, IntSearchFilter* @0x20 n @0x24 (12 B: u8 op, u16 id @4, u32 @8),
//     u16* attrIds @0x30 n @0x34}; SetRoomDataExternal {roomId, IntAttr* @8 n @0xc, BinAttr* @0x18 n @0x1c};
//   SetRoomDataInternal {roomId, flagFilter @8, flagAttr @0xc, BinAttr* @0x10 n @0x14}.
//   BinAttr = 12 B {u16 id, ptr @4, size @8}; IntAttr = 8 B {u16 id, u32 value @4}.
// Callbacks (thunks, asm):
//   context 0x8139bcf4 (u16 ctx, u16 event, u8 cause, s32 err, arg): any event ends the ContextStart wait (error >= 0
//     = started), 0x6f01 marks the context dead. We send 0x6f02 (started) / 0x6f03 (stopped).
//   request (u16 ctx, u32 reqId, u16 event, s32 err, const void* data, arg): done when reqId == this+0x54.
//   room event 0x8139c9d2 (u16 ctx, u64 roomId r2:r3, u16 event, const void* data, arg).
//   signaling 0x8139cad2 (u16 ctx, u64 roomId r2:r3, u16 memberId, u16 event, s32 err, arg): 0x5102 Established ->
//     SignalingGetConnectionStatus + RudpMgr_Connect; 0x5101 Dead -> close the slot.
// Responses (what the game reads, everything else zero):
//   create / join: {RoomDataInternal* @0} (f8139c3d8); RoomDataInternal: u16 serverId @0, u32 worldId @4, u64 roomId
//     @0x10, member list {head @0x30, count @0x34, me @0x38, owner @0x3c}, flagAttr @0x48, RoomBinAttrInternal* @0x50
//     n @0x54. RoomBinAttrInternal 0x18 B: {u64 date, u16 member @8, BinAttr @0xc} (data @0x10, size @0x14: the room
//     info must be 0x78 B). Member: next @0, SceNpId @4, u64 joinDate tick @0x28, u16 memberId @0x30 (slot =
//     (memberId >> 4) - 1, so 0x10..0x40).
//   worlds: {World* @0, n @4}, World 0x40 B, worldId @4 (f8139bd46).
//   search: {startIndex @0, total @4, n @8, RoomDataExternal* @0xc} (f8139bf08); RoomDataExternal: next @0, u16 @6
//     (cur members, inferred), u64 roomId @0x18, SceNpId* owner @0x38, IntAttr* @0x48 (8 values read blind = attrs
//     0x4c..0x53 in order), BinAttr* @0x58 (0x55, <= 0x100 B).
//   room events: 0x1101 joined {Member* @0, u8 @8}, 0x1102 left {Member*}, 0x1103 kicked, 0x1104 destroyed, 0x1105
//     owner changed {u16 prev, u16 new @2}, 0x1106 internal updated {u32* newFlagAttr @4, RoomBinAttrInternal** @0x1c,
//     n @0x20} (f8139c80c).
//
// Identity: as the Pro port, the lobby knows members by account id + name only; each SceNpId we hand the game (members,
// room owners, our own) is built from those two (hle/online.h make_np_id), so every peer builds the same bytes.
//
// Threading: every game callback locks the matching mutex (this+0x8bc), which the game holds while it issues these
// calls (DcNetMatching update), so nothing is called back from inside a call. On the Vita, Matching2 runs callbacks on
// its own thread (Init gets its priority and stack); ours is the "SceNpMatching2 callback" guest thread (started at
// ContextStart, online only): it drains the lobby link every ~10 ms and runs the callbacks with our lock released.
// Response data lives in the game's heap until the callback returns.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hle/common.h"
#include "hle/online.h"
#include "hle/threads.h"
#include "net/carrier.h"
#include "net/lobby_client.h"
#include "net/rudp.h"
#include "net/stun.h"
#include "net/upnp.h"

namespace hle {
namespace {

using dcp::net::Json;
using dcp::net::LobbyClient;
using dcp::net::LobbyMessage;
namespace rudp = dcp::net::rudp;
namespace stun = dcp::net::stun;

constexpr uint16_t CTX_ID = 1, SERVER_ID = 1;
constexpr uint32_t ERR_NOT_INIT = 0x80550c80;        // ours (Matching2 "not initialized"; value unverified)
constexpr uint32_t ERR_INVALID = 0x80550c81;         // ours (invalid argument; value unverified)
constexpr uint32_t ERR_NOT_CONNECTED = 0x80550c82;   // ours: offline (no lobby URL)

// Request event ids (SDK values; the game ignores them).
enum : uint16_t { EV_GET_WORLD_INFO_LIST = 0x0002, EV_SET_ROOM_DATA_EXTERNAL = 0x0004, EV_SET_USER_INFO = 0x0007,
                  EV_CREATE_JOIN_ROOM = 0x0101, EV_JOIN_ROOM = 0x0102, EV_LEAVE_ROOM = 0x0103, EV_SEARCH_ROOM = 0x0106,
                  EV_SET_ROOM_DATA_INTERNAL = 0x0109 };
enum : uint16_t { CTX_EV_START_OVER = 0x6f01, CTX_EV_START = 0x6f02, CTX_EV_STOP = 0x6f03 };
enum : uint16_t { ROOM_EV_MEMBER_JOINED = 0x1101, ROOM_EV_MEMBER_LEFT = 0x1102, ROOM_EV_KICKED = 0x1103,
                  ROOM_EV_DESTROYED = 0x1104, ROOM_EV_OWNER_CHANGED = 0x1105, ROOM_EV_INTERNAL_UPDATED = 0x1106 };
enum : uint16_t { SIG_EV_DEAD = 0x5101, SIG_EV_ESTABLISHED = 0x5102 };

uint64_t now_ms() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}
// Microsecond tick since 0001-01-01 (SceRtcTick) from Unix ms.
uint64_t rtc_tick(int64_t unix_ms) { return uint64_t(unix_ms) * 1000ull + 62135596800000000ull; }
uint16_t bswap16(uint16_t v) { return uint16_t((v >> 8) | (v << 8)); }
// Dotted IPv4 -> network order, 0 if not one.
uint32_t ip_from(const std::string& s) {
  unsigned a[4];
  char tail;
  if (std::sscanf(s.c_str(), "%u.%u.%u.%u%c", &a[0], &a[1], &a[2], &a[3], &tail) != 4) return 0;
  for (unsigned v : a)
    if (v > 255) return 0;
  return a[0] | a[1] << 8 | a[2] << 16 | a[3] << 24;
}

Memory* g_mem = nullptr;  // set at Init

// Guest-visible blocks (zeroed, from the game's heap) that live until the callback using them has returned.
struct Arena {
  std::vector<uint32_t> blocks;
  ~Arena() {
    for (uint32_t b : blocks) guest_free(b);
  }
  uint32_t alloc(uint32_t n) {
    uint32_t a = guest_malloc(*g_mem, (n + 15) & ~15u);
    if (a) std::memset(g_mem->ptr(a), 0, n);
    blocks.push_back(a);
    return a;
  }
  uint32_t bytes(const std::vector<uint8_t>& v) {
    uint32_t a = alloc(uint32_t(v.size()) + 1);
    if (a && !v.empty()) std::memcpy(g_mem->ptr(a), v.data(), v.size());
    return a;
  }
};
template <class T> void wr(uint32_t a, uint32_t off, T v) {
  if (a) g_mem->write<T>(a + off, v);
}
template <class T> T rd(uint32_t a, uint32_t off) { return g_mem->read<T>(a + off); }

// One guest callback: function + word arguments (r0-r3, then the stack), with the data it points at.
struct Call {
  uint32_t fn = 0;
  std::vector<uint32_t> args;
  std::shared_ptr<Arena> arena;
  const char* what = "";
};

struct MemberInfo {
  uint16_t id = 0;
  uint64_t account = 0;
  std::string name;
  int64_t join_ms = 0;
};

enum class Kind { Hello, Worlds, SetUserInfo, CreateJoin, Join, Leave, Search, SetInternal, SetExternal, Endpoint,
                  Rehello, Resume };
struct Pending {
  Kind kind;
  uint32_t req_id = 0;  // the id the game got back (0 for our own requests)
  uint32_t cb = 0, arg = 0;
  uint16_t ev = 0;
};

struct SignalState {
  uint32_t ip_be = 0;
  uint16_t port_be = 0;
  bool delivered = false;
  uint64_t changed_ms = 0;   // last time the endpoint changed (Established waits SIGNAL_SETTLE_MS after it)
  uint32_t local_ip_be = 0;  // the peer's LAN candidate (signal local_ip / local_port), 0 = none
  uint16_t local_port_be = 0;
};

std::mutex g_mu;
std::condition_variable g_wake;
bool g_init = false, g_ctx_created = false, g_thread_started = false;
uint32_t g_ctx_cb = 0, g_ctx_arg = 0, g_room_cb = 0, g_room_arg = 0, g_sig_cb = 0, g_sig_arg = 0;
uint32_t g_next_req = 1;
std::map<uint32_t, Pending> g_pending;  // lobby rid -> request
bool g_hello_sent = false, g_hello_ok = false, g_start_wanted = false, g_lobby_down = false;
// Lobby link dropped and the client is reconnecting (lobby_protocol.md 1.1): the game is told nothing; its requests
// wait in the lobby client until hello + resume went through on the new socket.
bool g_link_lost = false;
std::vector<std::pair<uint16_t, uint32_t>> g_ctx_events;  // context events raised by Start / Stop, sent by the thread
std::vector<Pending> g_local_done;  // requests answered without the lobby (unchanged room data), sent by the thread
std::string g_last_external;        // last SetRoomDataExternal body sent to the lobby
// Our UDP endpoint (net.md 11): what we last told the lobby, and whether that is final for this socket.
uint16_t g_reported_port = 0;
std::string g_reported_ip;  // "" = the lobby uses the connection's address
std::string g_your_ip;      // our address as the lobby sees it (hello your_ip)
bool g_lobby_local = false;  // the lobby saw us on a loopback / private address: LAN play, keep local endpoints
bool g_endpoint_inflight = false;
uint64_t g_bound_ms = 0;  // when we first saw the P2P socket bound
constexpr uint64_t SIGNAL_SETTLE_MS = 1000;  // a peer's endpoint must be stable this long before Established
constexpr uint64_t ENDPOINT_WAIT_MS = 8000;  // max wait for our own STUN / UPnP before Established anyway
// current room
uint64_t g_room = 0;
uint16_t g_me = 0, g_owner = 0;
std::map<uint16_t, MemberInfo> g_members;
std::map<uint16_t, SignalState> g_signals;

LobbyClient& lobby() { return LobbyClient::instance(); }

uint16_t configured_udp_port() {  // what sceNetBind tries first (hle/net.cpp): $DCP_NET_PORT or the game's 3658
  if (const char* e = std::getenv("DCP_NET_PORT"); e && *e) return uint16_t(std::atoi(e));
  return 3658;
}

void forget_room_locked() {
  g_room = 0, g_members.clear(), g_signals.clear();
  dcp::net::clear_peer_addresses();
}

// --- guest structs -> JSON (request side) ----------------------------------------------------------------------
Json bin_attrs_json(uint32_t arr, uint32_t n) {  // BinAttr 12 B
  Json a = Json::array();
  for (uint32_t i = 0; arr && i < n && i < 16; ++i) {
    uint32_t e = arr + i * 12, p = rd<uint32_t>(e, 4), sz = std::min(rd<uint32_t>(e, 8), 0x1000u);
    Json o;
    o["id"] = int(rd<uint16_t>(e, 0));
    o["data"] = p && sz && g_mem->valid(p, sz) ? dcp::net::hex_encode(g_mem->ptr(p), sz) : std::string();
    a.push(o);
  }
  return a;
}
Json int_attrs_json(uint32_t arr, uint32_t n) {  // IntAttr 8 B
  Json a = Json::array();
  for (uint32_t i = 0; arr && i < n && i < 32; ++i) {
    Json o;
    o["id"] = int(rd<uint16_t>(arr, i * 8));
    o["value"] = int64_t(rd<uint32_t>(arr, i * 8 + 4));
    a.push(o);
  }
  return a;
}

// --- JSON -> guest structs (response side) ---------------------------------------------------------------------
std::vector<uint8_t> unhex(const Json& j) {
  std::vector<uint8_t> v;
  dcp::net::hex_decode(j.as_string(), &v);
  return v;
}

// BinAttr array (12 B each) from JSON attrs, `first` (if present) moved to the front.
uint32_t make_bin_attrs(Arena& a, const Json& attrs, int first, uint32_t* count) {
  std::vector<const Json*> v;
  for (const auto& e : attrs.elems())
    if (e.get("id").as_int() == first) v.insert(v.begin(), &e);
    else v.push_back(&e);
  *count = uint32_t(v.size());
  if (v.empty()) return 0;
  uint32_t arr = a.alloc(12 * uint32_t(v.size()));
  for (size_t i = 0; i < v.size(); ++i) {
    std::vector<uint8_t> d = unhex(v[i]->get("data"));
    uint32_t e = arr + uint32_t(i) * 12;
    wr<uint16_t>(e, 0, uint16_t(v[i]->get("id").as_int()));
    wr<uint32_t>(e, 4, a.bytes(d));
    wr<uint32_t>(e, 8, uint32_t(d.size()));
  }
  return arr;
}

// RoomBinAttrInternal array (0x18 B each).
uint32_t make_bin_attrs_internal(Arena& a, const Json& attrs, uint32_t* count) {
  *count = uint32_t(attrs.size());
  if (!*count) return 0;
  uint32_t arr = a.alloc(0x18 * *count);
  for (uint32_t i = 0; i < *count; ++i) {
    const Json& e = attrs.at(i);
    std::vector<uint8_t> d = unhex(e.get("data"));
    uint32_t p = arr + i * 0x18;
    wr<uint16_t>(p, 0xc, uint16_t(e.get("id").as_int()));
    wr<uint32_t>(p, 0x10, a.bytes(d));
    wr<uint32_t>(p, 0x14, uint32_t(d.size()));
  }
  return arr;
}

uint32_t make_np_id_block(Arena& a, const std::string& name, uint64_t account) {
  uint32_t p = a.alloc(NP_ID_SIZE);
  if (p) make_np_id(g_mem->ptr(p), name, account);
  return p;
}

uint32_t make_member(Arena& a, const MemberInfo& m) {  // RoomMemberDataInternal
  uint32_t p = a.alloc(0x40);
  if (p) make_np_id(g_mem->ptr(p + 4), m.name, m.account);
  wr<uint64_t>(p, 0x28, rtc_tick(m.join_ms));
  wr<uint16_t>(p, 0x30, m.id);
  return p;
}

MemberInfo member_from(const Json& j) {
  MemberInfo m;
  m.id = uint16_t(j.get("member_id").as_int());
  m.account = j.get("account_id").as_u64();
  m.name = j.get("name").as_string();
  m.join_ms = j.get("join_ms").as_int();
  return m;
}

// Create / join response; also records the room. Caller holds g_mu.
uint32_t make_join_response(Arena& a, const Json& body) {
  const Json& room = body.get("room");
  uint32_t resp = a.alloc(0x10), r = a.alloc(0x60);
  wr<uint32_t>(resp, 0, r);
  wr<uint16_t>(r, 0, SERVER_ID);
  wr<uint32_t>(r, 4, uint32_t(room.get("world_id").as_int(1)));
  wr<uint64_t>(r, 0x10, room.get("room_id").as_u64());
  wr<uint32_t>(r, 0x20, uint32_t(room.get("max_slot").as_int(4)));  // (field meaning unverified; not read)
  wr<uint32_t>(r, 0x48, uint32_t(room.get("flag_attr").as_int()));
  uint32_t nb = 0;
  wr<uint32_t>(r, 0x50, make_bin_attrs_internal(a, room.get("bin_internal"), &nb));
  wr<uint32_t>(r, 0x54, nb);
  g_room = room.get("room_id").as_u64();
  g_me = uint16_t(body.get("member_id").as_int());
  g_owner = uint16_t(room.get("owner").as_int());
  g_members.clear();
  g_signals.clear();
  uint32_t head = 0, prev = 0, me = 0, owner = 0, count = 0;
  for (const auto& mj : room.get("members").elems()) {
    MemberInfo m = member_from(mj);
    g_members[m.id] = m;
    uint32_t p = make_member(a, m);
    if (prev) wr<uint32_t>(prev, 0, p);
    else head = p;
    prev = p;
    if (m.id == g_me) me = p;
    if (m.id == g_owner) owner = p;
    ++count;
  }
  wr<uint32_t>(r, 0x30, head);
  wr<uint32_t>(r, 0x34, count);
  wr<uint32_t>(r, 0x38, me);
  wr<uint32_t>(r, 0x3c, owner);
  return resp;
}

uint32_t make_search_response(Arena& a, const Json& body) {
  uint32_t resp = a.alloc(0x10);
  uint32_t head = 0, prev = 0, n = 0;
  for (const auto& rj : body.get("rooms").elems()) {
    uint32_t r = a.alloc(0x68);
    wr<uint16_t>(r, 6, uint16_t(rj.get("cur_members").as_int()));  // (field meaning inferred)
    wr<uint64_t>(r, 0x18, rj.get("room_id").as_u64());
    wr<uint32_t>(r, 0x38, make_np_id_block(a, rj.get("owner_name").as_string(), rj.get("owner_account_id").as_u64()));
    uint32_t ia = a.alloc(8 * 8);  // the game reads 8 values blind: attrs 0x4c..0x53 in order
    for (int k = 0; k < 8; ++k) wr<uint16_t>(ia, uint32_t(k) * 8, uint16_t(0x4c + k));
    for (const auto& e : rj.get("int_attrs").elems()) {
      int id = int(e.get("id").as_int());
      if (id >= 0x4c && id <= 0x53) wr<uint32_t>(ia, uint32_t(id - 0x4c) * 8 + 4, uint32_t(e.get("value").as_int()));
    }
    wr<uint32_t>(r, 0x48, ia);
    wr<uint32_t>(r, 0x4c, 8);
    uint32_t nb = 0;
    wr<uint32_t>(r, 0x58, make_bin_attrs(a, rj.get("bin_external"), 0x55, &nb));
    wr<uint32_t>(r, 0x5c, nb);
    if (prev) wr<uint32_t>(prev, 0, r);
    else head = r;
    prev = r;
    ++n;
  }
  wr<uint32_t>(resp, 0, uint32_t(body.get("start").as_int(1)));
  wr<uint32_t>(resp, 4, uint32_t(body.get("total").as_int(n)));
  wr<uint32_t>(resp, 8, n);
  wr<uint32_t>(resp, 0xc, head);
  return resp;
}

// Queues a lobby request for a game call (opt = its SceNpMatching2RequestOptParam, 0 = our own request). Caller holds
// g_mu. urgent: goes out while a reconnect pauses traffic.
uint32_t issue(Kind k, const std::string& op, Json body, uint32_t opt, uint16_t ev, bool urgent = false) {
  Pending p;
  p.kind = k, p.ev = ev;
  if (opt) {
    p.cb = rd<uint32_t>(opt, 0);
    p.arg = rd<uint32_t>(opt, 4);
    p.req_id = g_next_req++;
  }
  uint32_t rid = lobby().request(op, std::move(body), urgent);
  g_pending[rid] = p;
  g_wake.notify_all();
  return p.req_id;
}

Json hello_body() {
  const NpIdentity& id = np_identity();
  Json h;
  h["proto"] = dcp::net::kLobbyProtocol;
  h["version"] = dcp::net::kGameNetVersion;  // 34 = 0x22, the 1.09 room protocol (= Pro's)
  h["title"] = "PCSE00019";
  h["name"] = id.online_id;
  h["account_id"] = dcp::net::u64_str(id.account_id);
  if (!id.secret.empty()) h["secret"] = id.secret;
  h["udp_port"] = int(configured_udp_port());
  return h;
}

void send_hello_locked() {
  if (g_hello_sent) return;
  g_hello_sent = true;
  g_reported_port = configured_udp_port();
  issue(Kind::Hello, "hello", hello_body(), 0, 0);
  HLE_NOTE("MATCHING2 hello -> %s as %s (account %llu)\n", lobby().url().c_str(), np_identity().online_id.c_str(),
           (unsigned long long)np_identity().account_id);
}

// Reconnect done: let the waiting requests out and re-send what the lobby keeps per connection (the endpoint).
void finish_reconnect_locked() {
  g_link_lost = false;
  lobby().resume_traffic();
  g_reported_port = 0, g_reported_ip.clear(), g_endpoint_inflight = false;
  g_last_external.clear();
}

Call room_call(uint64_t room, uint16_t ev, uint32_t data, std::shared_ptr<Arena> a, const char* what) {
  return Call{g_room_cb, {CTX_ID, 0, uint32_t(room), uint32_t(room >> 32), ev, data, g_room_arg}, std::move(a), what};
}

// Drains the lobby link and collects the guest callbacks to run (caller then runs them without g_mu).
void collect_locked(std::vector<Call>& calls) {
  if (g_ctx_cb)
    for (auto [ev, err] : g_ctx_events) calls.push_back({g_ctx_cb, {CTX_ID, ev, 0, err, g_ctx_arg}, nullptr, "context"});
  g_ctx_events.clear();
  for (const Pending& p : g_local_done)
    if (p.cb) calls.push_back({p.cb, {CTX_ID, p.req_id, p.ev, 0, 0, p.arg}, nullptr, "request"});
  g_local_done.clear();
  if (!g_hello_sent) return;
  LobbyMessage m;
  while (lobby().poll(&m)) {
    if (m.kind == LobbyMessage::Lost) {  // reconnecting (lobby_protocol.md 1.1): the game is not told
      if (g_lobby_down) continue;
      g_link_lost = true;
      g_endpoint_inflight = false;
      HLE_NOTE("MATCHING2 lobby link lost, reconnecting (room %llu, member 0x%x)\n", (unsigned long long)g_room, g_me);
      continue;
    }
    if (m.kind == LobbyMessage::Back) {  // new socket: hello again, then retake the seat
      if (!g_lobby_down) issue(Kind::Rehello, "hello", hello_body(), 0, 0, true);
      continue;
    }
    if (m.kind == LobbyMessage::Down) {
      if (g_lobby_down) continue;
      g_lobby_down = true;
      g_link_lost = false;
      g_last_external.clear();
      HLE_NOTE("MATCHING2 lobby link down\n");
      if (g_room && g_room_cb) {
        auto a = std::make_shared<Arena>();
        calls.push_back(room_call(g_room, ROOM_EV_DESTROYED, a->alloc(0x10), a, "room destroyed"));
      }
      if (g_ctx_cb && g_hello_ok)
        calls.push_back({g_ctx_cb, {CTX_ID, CTX_EV_START_OVER, 0, uint32_t(LobbyClient::kErrLobbyDown), g_ctx_arg},
                         nullptr, "context"});
      continue;
    }
    if (m.kind == LobbyMessage::Response) {
      auto it = g_pending.find(m.rid);
      if (it == g_pending.end()) continue;
      Pending p = it->second;
      g_pending.erase(it);
      if (p.kind == Kind::Rehello) {
        HLE_NOTE("MATCHING2 re-hello %s (err 0x%x)\n", m.err ? "FAILED" : "ok", uint32_t(m.err));
        if (m.err == 0) {
          g_your_ip = m.body.get("your_ip").as_string();
          g_lobby_local = stun::is_private_or_loopback(ip_from(g_your_ip));
        }
        if (m.err == 0 && g_room) {
          Json b;
          b["room_id"] = dcp::net::u64_str(g_room);
          b["member_id"] = int(g_me);
          issue(Kind::Resume, "resume", b, 0, 0, true);
          continue;
        }
        if (m.err && g_room && g_room_cb) {  // refused on the new socket: the seat is gone
          auto a = std::make_shared<Arena>();
          calls.push_back(room_call(g_room, ROOM_EV_DESTROYED, a->alloc(0x10), a, "room destroyed"));
          forget_room_locked();
        }
        finish_reconnect_locked();
        continue;
      }
      if (p.kind == Kind::Resume) {
        if (m.err == 0) {
          HLE_NOTE("MATCHING2 seat resumed: room %llu member 0x%x\n", (unsigned long long)g_room, g_me);
        } else if (g_room) {  // grace over: the others saw us leave; the game can go online again later
          HLE_NOTE("MATCHING2 seat lost (err 0x%x): room %llu\n", uint32_t(m.err), (unsigned long long)g_room);
          if (g_room_cb) {
            auto a = std::make_shared<Arena>();
            calls.push_back(room_call(g_room, ROOM_EV_DESTROYED, a->alloc(0x10), a, "room destroyed"));
          }
          forget_room_locked();
        }
        finish_reconnect_locked();
        continue;
      }
      if (p.kind == Kind::Hello) {
        g_hello_ok = m.err == 0;
        if (g_hello_ok) {
          g_your_ip = m.body.get("your_ip").as_string();
          g_lobby_local = stun::is_private_or_loopback(ip_from(g_your_ip));
        }
        HLE_NOTE("MATCHING2 hello %s (err 0x%x)%s%s\n", g_hello_ok ? "ok" : "FAILED", uint32_t(m.err),
                 g_hello_ok ? " your_ip " : "", g_hello_ok ? g_your_ip.c_str() : "");
        if (g_start_wanted && g_ctx_cb)
          calls.push_back({g_ctx_cb, {CTX_ID, CTX_EV_START, 0, uint32_t(m.err), g_ctx_arg}, nullptr, "context"});
        continue;
      }
      if (p.kind == Kind::Endpoint) {
        g_endpoint_inflight = false;
        HLE_NOTE("MATCHING2 endpoint %s (lobby has %s:%lld)\n", m.err ? "FAILED" : "ok",
                 m.body.get("ip").as_string().c_str(), (long long)m.body.get("port").as_int());
      }
      if (!p.cb) continue;  // our own request (endpoint, implicit leave)
      auto a = std::make_shared<Arena>();
      uint32_t data = 0;
      if (m.err == 0) {
        switch (p.kind) {
          case Kind::Worlds: {
            const Json& w = m.body.get("worlds");
            uint32_t n = uint32_t(std::max<size_t>(w.size(), 1));
            data = a->alloc(0x10);
            uint32_t arr = a->alloc(0x40 * n);
            for (uint32_t i = 0; i < n; ++i) {
              wr<uint16_t>(arr + i * 0x40, 0, SERVER_ID);  // (+0 unread by the game)
              wr<uint32_t>(arr + i * 0x40, 4, uint32_t(w.size() ? w.at(i).get("world_id").as_int(1) : 1));
            }
            wr<uint32_t>(data, 0, arr);
            wr<uint32_t>(data, 4, n);
            break;
          }
          case Kind::CreateJoin:
          case Kind::Join:
            data = make_join_response(*a, m.body);
            HLE_NOTE("MATCHING2 %s room %llu as member 0x%x (owner 0x%x, %zu members, code %s)\n",
                     p.kind == Kind::Join ? "joined" : "created", (unsigned long long)g_room, g_me, g_owner,
                     g_members.size(), m.body.get("room").get("join_code").as_string().c_str());
            break;
          case Kind::Search:
            data = make_search_response(*a, m.body);
            HLE_NOTE("MATCHING2 search -> %zu room(s)\n", m.body.get("rooms").size());
            break;
          default: break;  // SetUserInfo / Leave / SetInternal / SetExternal: no response data
        }
      } else {
        HLE_NOTE("MATCHING2 request %u (event 0x%x) failed: 0x%08x\n", p.req_id, p.ev, uint32_t(m.err));
        if (p.kind == Kind::SetExternal) g_last_external.clear();  // resend it next time
      }
      calls.push_back({p.cb, {CTX_ID, p.req_id, p.ev, uint32_t(m.err), data, p.arg}, a, "request"});
      continue;
    }
    // Events
    const Json& b = m.body;
    uint64_t room = b.get("room_id").as_u64();
    if (room != g_room || !g_room) continue;  // stale (left already)
    auto a = std::make_shared<Arena>();
    if (m.ev == "member_joined") {
      MemberInfo mi = member_from(b.get("member"));
      g_members[mi.id] = mi;
      HLE_NOTE("MATCHING2 member 0x%x (%s) joined\n", mi.id, mi.name.c_str());
      uint32_t d = a->alloc(0x10);
      wr<uint32_t>(d, 0, make_member(*a, mi));
      if (g_room_cb) calls.push_back(room_call(room, ROOM_EV_MEMBER_JOINED, d, a, "member joined"));
    } else if (m.ev == "member_left") {
      uint16_t id = uint16_t(b.get("member_id").as_int());
      MemberInfo mi;
      mi.id = id;
      if (g_members.count(id)) mi = g_members[id];
      g_members.erase(id);
      g_signals.erase(id);
      dcp::net::clear_peer_address(id);
      HLE_NOTE("MATCHING2 member 0x%x left\n", id);
      uint32_t d = a->alloc(0x10);
      wr<uint32_t>(d, 0, make_member(*a, mi));
      if (g_room_cb) calls.push_back(room_call(room, ROOM_EV_MEMBER_LEFT, d, a, "member left"));
    } else if (m.ev == "owner_changed") {
      uint16_t prev = uint16_t(b.get("prev").as_int()), now = uint16_t(b.get("new").as_int());
      g_owner = now;
      HLE_NOTE("MATCHING2 owner 0x%x -> 0x%x\n", prev, now);
      uint32_t d = a->alloc(0x10);
      wr<uint16_t>(d, 0, prev);
      wr<uint16_t>(d, 2, now);
      if (g_room_cb) calls.push_back(room_call(room, ROOM_EV_OWNER_CHANGED, d, a, "owner changed"));
    } else if (m.ev == "room_destroyed" || m.ev == "kicked") {
      bool kicked = m.ev == "kicked";
      HLE_NOTE("MATCHING2 %s\n", kicked ? "kicked out" : "room destroyed");
      forget_room_locked();
      if (g_room_cb)
        calls.push_back(room_call(room, kicked ? ROOM_EV_KICKED : ROOM_EV_DESTROYED, a->alloc(0x10), a, "room gone"));
    } else if (m.ev == "room_internal_updated") {
      uint32_t d = a->alloc(0x30), flags = a->alloc(8);
      wr<uint32_t>(flags, 0, uint32_t(b.get("flag_attr").as_int()));
      wr<uint32_t>(d, 4, flags);
      uint32_t n = 0, arr = make_bin_attrs_internal(*a, b.get("bin_internal"), &n);
      if (n) {
        uint32_t ptrs = a->alloc(4 * n);
        for (uint32_t i = 0; i < n; ++i) wr<uint32_t>(ptrs, i * 4, arr + i * 0x18);
        wr<uint32_t>(d, 0x1c, ptrs);
        wr<uint32_t>(d, 0x20, n);
      }
      if (g_room_cb) calls.push_back(room_call(room, ROOM_EV_INTERNAL_UPDATED, d, a, "internal updated"));
    } else if (m.ev == "signal") {
      uint16_t id = uint16_t(b.get("member_id").as_int());
      SignalState& s = g_signals[id];
      uint32_t ip = ip_from(b.get("ip").as_string());
      uint16_t port = bswap16(uint16_t(b.get("port").as_int()));
      if (s.delivered && (s.ip_be != ip || s.port_be != port))
        HLE_NOTE("MATCHING2 member 0x%x endpoint changed after Established (kept the first)\n", id);
      if (!s.delivered && (s.ip_be != ip || s.port_be != port)) s.ip_be = ip, s.port_be = port, s.changed_ms = now_ms();
      if (!s.delivered) {
        uint32_t lip = ip_from(b.get("local_ip").as_string());
        uint16_t lport = uint16_t(b.get("local_port").as_int());
        bool lan = lip && lport && stun::is_rfc1918(lip);
        s.local_ip_be = lan ? lip : 0, s.local_port_be = lan ? bswap16(lport) : 0;
      }
      if (id != g_me && port) rudp::punch(ip, port);  // open our NAT towards it now (hole punching)
      HLE_NOTE("MATCHING2 signal: member 0x%x at %s:%lld\n", id, b.get("ip").as_string().c_str(),
               (long long)b.get("port").as_int());
      for (const auto& [oid, o] : g_signals)  // two devices behind one router claiming one port: only one is reachable
        if (oid != id && oid != g_me && id != g_me && port && o.ip_be == ip && o.port_be == port)
          HLE_NOTE("MATCHING2 members 0x%x and 0x%x share one public endpoint %s:%lld\n", oid, id,
                   b.get("ip").as_string().c_str(), (long long)b.get("port").as_int());
    } else if (m.ev == "signal_dead") {
      uint16_t id = uint16_t(b.get("member_id").as_int());
      auto it = g_signals.find(id);
      bool was = it != g_signals.end() && it->second.delivered;
      if (it != g_signals.end()) g_signals.erase(it);
      dcp::net::clear_peer_address(id);
      if (was && g_sig_cb)
        calls.push_back({g_sig_cb, {CTX_ID, 0, uint32_t(room), uint32_t(room >> 32), id, SIG_EV_DEAD, 0, g_sig_arg},
                         nullptr, "signaling dead"});
    }
  }
  // Our UDP endpoint once the game's P2P socket is bound (the port may differ from the hello guess: a second instance
  // binds any free port). Behind a NAT (the lobby saw a public address) it is the STUN / UPnP mapping of the P2P
  // socket; with a LAN / local lobby, or when STUN fails, the local port with the address the lobby sees. net.md 11.
  bool endpoint_final = false;
  if (g_hello_ok && !g_lobby_down && !g_link_lost) {
    uint16_t port = rudp::bound_port();
    if (port && !g_bound_ms) g_bound_ms = now_ms();
    uint32_t pip = 0;
    uint16_t pport = 0;
    auto st = port ? rudp::public_endpoint(&pip, &pport) : dcp::net::Carrier::StunState::Idle;
    dcp::net::upnp::Status up = dcp::net::upnp::status();
    // Idle = bound but STUN not started yet: reporting then would send the local port to a peer that keeps it.
    bool stun_pending = st == dcp::net::Carrier::StunState::Running ||
                        (st == dcp::net::Carrier::StunState::Idle && !g_lobby_local &&
                         !stun::configured_servers().empty());
    bool waiting = (stun_pending || up.state == dcp::net::upnp::State::Pending) && now_ms() - g_bound_ms < ENDPOINT_WAIT_MS;
    std::string want_ip;
    uint16_t want_port = port;
    const char* how = g_lobby_local ? "LAN lobby" : "no STUN/UPnP";
    // A UPnP mapping with a public WAN address takes unsolicited packets, so it wins over the STUN mapping.
    uint32_t up_ip = up.state == dcp::net::upnp::State::Mapped ? ip_from(up.ext_ip) : 0;
    if (port && !g_lobby_local && up_ip && !stun::is_private_or_loopback(up_ip))
      want_ip = up.ext_ip, want_port = up.ext_port, how = "UPnP";
    else if (port && !g_lobby_local && st == dcp::net::Carrier::StunState::Done)
      want_ip = stun::ip_str(pip), want_port = bswap16(pport), how = "STUN";
    if (port && !waiting && (want_port != g_reported_port || want_ip != g_reported_ip)) {
      g_reported_port = want_port, g_reported_ip = want_ip;
      Json e;
      e["port"] = int(want_port);
      if (!want_ip.empty()) e["ip"] = want_ip;
      uint32_t lan = stun::default_route_ip();  // LAN candidate for a peer behind the same router (net.md 11.1)
      if (!g_lobby_local && lan && stun::is_rfc1918(lan)) e["local_ip"] = stun::ip_str(lan), e["local_port"] = int(port);
      g_endpoint_inflight = true;
      HLE_NOTE("MATCHING2 endpoint -> %s:%u (local UDP %u, %s)\n",
               want_ip.empty() ? "<address the lobby sees>" : want_ip.c_str(), want_port, port, how);
      issue(Kind::Endpoint, "endpoint", e, 0, 0);
    }
    endpoint_final = port && !waiting && !g_endpoint_inflight;
  }
  // Signaling Established per peer, once the game can act on it: callback registered, P2P socket bound, member known
  // from the room data, our endpoint at the lobby and the peer's endpoint settled.
  if (g_room && g_sig_cb && rudp::open_socket_count() > 0) {
    uint64_t t = now_ms();
    for (auto& [id, s] : g_signals) {
      if (s.delivered || !s.port_be || id == g_me || !g_members.count(id)) continue;
      if (!endpoint_final || t - s.changed_ms < SIGNAL_SETTLE_MS) continue;
      s.delivered = true;
      dcp::net::set_peer_address(id, s.ip_be, s.port_be);
      // Same public address as ours = probably the same router: also try its LAN address.
      const std::string& mine = g_reported_ip.empty() ? g_your_ip : g_reported_ip;
      if (s.local_ip_be && !mine.empty() && ip_from(mine) == s.ip_be) {
        dcp::net::set_peer_alt(id, s.ip_be, s.port_be, s.local_ip_be, s.local_port_be);
        HLE_NOTE("MATCHING2 member 0x%x shares our public address; LAN candidate %s:%u\n", id,
                 stun::ip_str(s.local_ip_be).c_str(), bswap16(s.local_port_be));
      }
      HLE_NOTE("MATCHING2 signaling Established: member 0x%x at %s:%u\n", id, stun::ip_str(s.ip_be).c_str(),
               bswap16(s.port_be));
      calls.push_back({g_sig_cb, {CTX_ID, 0, uint32_t(g_room), uint32_t(g_room >> 32), id, SIG_EV_ESTABLISHED, 0,
                                  g_sig_arg},
                       nullptr, "signaling established"});
    }
  }
}

// The callback thread (see the header comment): a guest thread of its own, as Matching2's on the Vita.
void callback_thread() {
  guest::Cpu& k = adopt_host_thread("SceNpMatching2 callback");
  constexpr uint32_t STACK = 0x10000;
  uint32_t stack = g_mem->alloc(STACK, "SceNpMatching2 callback stack");
  for (;;) {
    std::vector<Call> calls;
    {
      std::unique_lock lk(g_mu);
      g_wake.wait_for(lk, std::chrono::milliseconds(10));
      collect_locked(calls);
    }
    for (Call& c : calls) {
      if (!c.fn) continue;
      if (!k.invoke(c.fn, c.args.data(), c.args.size(), stack + STACK))
        die(k, std::string("SceNpMatching2 ") + c.what + " callback");
    }
  }
}

// ---- entry points ----
// sceNpMatching2Init(poolSize, threadPriority, cpuAffinityMask, threadStackSize) (init step 5, both modes).
void Init(Cpu& c) {
  std::lock_guard lk(g_mu);
  g_mem = &c.mem();
  g_init = true;
  ret(c, 0);
}
void Term(Cpu& c) {
  std::lock_guard lk(g_mu);
  g_init = false;
  ret(c, 0);
}
// sceNpMatching2CreateContext(const SceNpId*, commId*, passphrase*, u16* ctxId)
void CreateContext(Cpu& c) {
  uint32_t out = arg(c, 3);
  std::lock_guard lk(g_mu);
  if (!g_init) return ret(c, ERR_NOT_INIT);
  if (!out) return ret(c, ERR_INVALID);
  c.mem().write<uint16_t>(out, CTX_ID);
  g_ctx_created = true;
  HLE_NOTE("sceNpMatching2CreateContext(%s) -> ctx %u\n", arg(c, 0) ? str(c, arg(c, 0), 16).c_str() : "?", CTX_ID);
  ret(c, 0);
}
// sceNpMatching2RegisterContextCallback(cb, arg)
void RegisterContextCallback(Cpu& c) {
  std::lock_guard lk(g_mu);
  g_ctx_cb = arg(c, 0), g_ctx_arg = arg(c, 1);
  ret(c, 0);
}
// sceNpMatching2ContextStart(ctx, u64 timeout): the started event goes out once the lobby answered hello.
void ContextStart(Cpu& c) {
  std::lock_guard lk(g_mu);
  HLE_NOTE("sceNpMatching2ContextStart(%u)\n", arg(c, 0));
  if (!g_ctx_created) return ret(c, ERR_NOT_INIT);
  if (!online_enabled()) return ret(c, ERR_NOT_CONNECTED);
  if (!g_thread_started) std::thread(callback_thread).detach();
  g_thread_started = true;
  g_start_wanted = true;
  if (g_hello_ok && !g_lobby_down) g_ctx_events.push_back({CTX_EV_START, 0});  // restart: link already up
  // Restart after the link was given up for good: hello went out already and nothing else would answer. Fail the
  // start like an unreachable lobby (the game stays offline).
  else if (g_hello_sent && g_lobby_down) g_ctx_events.push_back({CTX_EV_START, uint32_t(LobbyClient::kErrLobbyDown)});
  send_hello_locked();
  g_wake.notify_all();
  ret(c, 0);
}
void ContextStop(Cpu& c) {
  std::lock_guard lk(g_mu);
  HLE_NOTE("sceNpMatching2ContextStop(%u)\n", arg(c, 0));
  if (g_room) {
    Json b;
    b["room_id"] = dcp::net::u64_str(g_room);
    issue(Kind::Leave, "leave", b, 0, 0);
    forget_room_locked();
  }
  g_start_wanted = false;
  g_ctx_events.push_back({CTX_EV_STOP, 0});  // the stop step waits for a context event before DestroyContext
  g_wake.notify_all();
  ret(c, 0);
}
void DestroyContext(Cpu& c) {
  std::lock_guard lk(g_mu);
  HLE_NOTE("sceNpMatching2DestroyContext(%u)\n", arg(c, 0));
  g_ctx_created = false;
  // A later context re-reports its endpoint (net.md 12.4): the next socket may get another port.
  g_reported_port = 0, g_reported_ip.clear(), g_endpoint_inflight = false, g_bound_ms = 0;
  g_last_external.clear();
  ret(c, 0);
}
// sceNpMatching2GetServerLocal(ctx, out {u16 serverId, u8 status}): status 1 = available (f8139ce2a).
void GetServerLocal(Cpu& c) {
  uint32_t out = arg(c, 1);
  if (!out || !c.mem().valid(out, 4)) return ret(c, ERR_INVALID);
  c.mem().write<uint16_t>(out, SERVER_ID);
  c.mem().write<uint8_t>(out + 2, 1);
  ret(c, 0);
}

// The game's block list (SceNpId[], 0x24 stride; CreateJoinRoom +0x50, JoinRoom +0x30) as lobby account ids
// (np_id_account). The lobby protocol has no block list yet, so it is only logged.
std::string blocked_note(uint32_t arr, uint32_t n) {
  if (!arr || !n) return "";
  std::string s = ", blocked accounts:";
  for (uint32_t i = 0; i < n && i < 100; i++)
    if (g_mem->valid(arr + i * NP_ID_SIZE, NP_ID_SIZE))
      s += " " + dcp::net::u64_str(np_id_account(g_mem->ptr(arr + i * NP_ID_SIZE)));
  return s + " (not sent)";
}

// Common checks of the request calls (ctx, req*, opt*, u32* reqId); g_mu held by the caller.
bool request_ok(Cpu& c) {
  if (!g_ctx_created || !arg(c, 1) || !arg(c, 2)) {
    ret(c, g_ctx_created ? ERR_INVALID : ERR_NOT_INIT);
    return false;
  }
  return true;
}
void request_id(Cpu& c, uint32_t r) {
  if (arg(c, 3)) c.mem().write<uint32_t>(arg(c, 3), r);
  ret(c, 0);
}

void GetWorldInfoList(Cpu& c) {
  std::lock_guard lk(g_mu);
  if (!request_ok(c)) return;
  request_id(c, issue(Kind::Worlds, "worlds", Json::object(), arg(c, 2), EV_GET_WORLD_INFO_LIST));
}
void SetUserInfo(Cpu& c) {
  std::lock_guard lk(g_mu);
  if (!request_ok(c)) return;
  uint32_t req = arg(c, 1);
  Json b;
  b["bin_attrs"] = bin_attrs_json(rd<uint32_t>(req, 4), rd<uint32_t>(req, 8));
  request_id(c, issue(Kind::SetUserInfo, "set_user_info", b, arg(c, 2), EV_SET_USER_INFO));
}
void CreateJoinRoom(Cpu& c) {
  std::lock_guard lk(g_mu);
  if (!request_ok(c)) return;
  uint32_t req = arg(c, 1);
  Json b;
  b["world_id"] = int64_t(rd<uint32_t>(req, 0));
  b["max_slot"] = int64_t(rd<uint32_t>(req, 0x10));
  b["flag_attr"] = int64_t(rd<uint32_t>(req, 0x14));
  b["bin_internal"] = bin_attrs_json(rd<uint32_t>(req, 0x18), rd<uint32_t>(req, 0x1c));
  b["int_attrs"] = int_attrs_json(rd<uint32_t>(req, 0x20), rd<uint32_t>(req, 0x24));
  b["bin_external"] = bin_attrs_json(rd<uint32_t>(req, 0x30), rd<uint32_t>(req, 0x34));
  b["member_bin_internal"] = Json::array();
  HLE_NOTE("sceNpMatching2CreateJoinRoom %s%s\n", b.dump().c_str(),
           blocked_note(rd<uint32_t>(req, 0x50), rd<uint32_t>(req, 0x54)).c_str());
  request_id(c, issue(Kind::CreateJoin, "create_room", b, arg(c, 2), EV_CREATE_JOIN_ROOM));
}
void JoinRoom(Cpu& c) {
  std::lock_guard lk(g_mu);
  if (!request_ok(c)) return;
  uint32_t req = arg(c, 1);
  Json b;
  b["room_id"] = dcp::net::u64_str(rd<uint64_t>(req, 0));
  HLE_NOTE("sceNpMatching2JoinRoom %s%s\n", b.dump().c_str(),
           blocked_note(rd<uint32_t>(req, 0x30), rd<uint32_t>(req, 0x34)).c_str());
  request_id(c, issue(Kind::Join, "join", b, arg(c, 2), EV_JOIN_ROOM));
}
void LeaveRoom(Cpu& c) {
  std::lock_guard lk(g_mu);
  if (!request_ok(c)) return;
  Json b;
  b["room_id"] = dcp::net::u64_str(rd<uint64_t>(arg(c, 1), 0));
  HLE_NOTE("sceNpMatching2LeaveRoom %s\n", b.dump().c_str());
  uint32_t r = issue(Kind::Leave, "leave", b, arg(c, 2), EV_LEAVE_ROOM);
  forget_room_locked();
  request_id(c, r);
}
void SearchRoom(Cpu& c) {
  std::lock_guard lk(g_mu);
  if (!request_ok(c)) return;
  uint32_t req = arg(c, 1);
  Json b;
  b["world_id"] = int64_t(rd<uint32_t>(req, 4));
  b["start"] = int64_t(rd<uint32_t>(req, 0x10));
  b["max"] = int64_t(rd<uint32_t>(req, 0x14));
  b["flag_filter"] = int64_t(rd<uint32_t>(req, 0x18));
  b["flag_attr"] = int64_t(rd<uint32_t>(req, 0x1c));
  Json f = Json::array();
  uint32_t flt = rd<uint32_t>(req, 0x20);
  for (uint32_t i = 0, n = rd<uint32_t>(req, 0x24); flt && i < n && i < 16; ++i) {
    Json o;
    o["op"] = int(rd<uint8_t>(flt, i * 12));
    o["id"] = int(rd<uint16_t>(flt, i * 12 + 4));
    o["value"] = int64_t(rd<uint32_t>(flt, i * 12 + 8));
    f.push(o);
  }
  b["filters"] = f;
  Json ids = Json::array();
  uint32_t ai = rd<uint32_t>(req, 0x30);
  for (uint32_t i = 0, n = rd<uint32_t>(req, 0x34); ai && i < n && i < 16; ++i) ids.push(int(rd<uint16_t>(ai, i * 2)));
  b["attr_ids"] = ids;
  if (const char* code = std::getenv("DCP_JOIN_CODE"); code && *code) {
    // Join by code (launcher / tests): the code names the room, so the attribute conditions (which follow the host's
    // round and options, notes/map/online_room_attrs.md) do not apply.
    b["join_code"] = code;
    b["filters"] = Json::array();
    b["flag_filter"] = int64_t(0);
  }
  HLE_NOTE("sceNpMatching2SearchRoom %s\n", b.dump().c_str());
  request_id(c, issue(Kind::Search, "search", b, arg(c, 2), EV_SEARCH_ROOM));
}
void SetRoomDataInternal(Cpu& c) {
  std::lock_guard lk(g_mu);
  if (!request_ok(c)) return;
  uint32_t req = arg(c, 1);
  Json b;
  b["room_id"] = dcp::net::u64_str(rd<uint64_t>(req, 0));
  b["flag_filter"] = int64_t(rd<uint32_t>(req, 8));
  b["flag_attr"] = int64_t(rd<uint32_t>(req, 0xc));
  b["bin_internal"] = bin_attrs_json(rd<uint32_t>(req, 0x10), rd<uint32_t>(req, 0x14));
  HLE_NOTE("sceNpMatching2SetRoomDataInternal %s\n", b.dump().c_str());
  request_id(c, issue(Kind::SetInternal, "set_room_internal", b, arg(c, 2), EV_SET_ROOM_DATA_INTERNAL));
}
void SetRoomDataExternal(Cpu& c) {
  std::lock_guard lk(g_mu);
  if (!request_ok(c)) return;
  uint32_t req = arg(c, 1), opt = arg(c, 2);
  Json b;
  b["room_id"] = dcp::net::u64_str(rd<uint64_t>(req, 0));
  b["int_attrs"] = int_attrs_json(rd<uint32_t>(req, 8), rd<uint32_t>(req, 0xc));
  b["bin_external"] = bin_attrs_json(rd<uint32_t>(req, 0x18), rd<uint32_t>(req, 0x1c));
  // The host re-sends its external room data often, almost always unchanged: repeats are answered here, so the lobby
  // only sees real changes (each message is a billed Worker request).
  std::string s = b.dump();
  if (s == g_last_external) {
    Pending p;
    p.kind = Kind::SetExternal, p.ev = EV_SET_ROOM_DATA_EXTERNAL;
    p.cb = rd<uint32_t>(opt, 0), p.arg = rd<uint32_t>(opt, 4), p.req_id = g_next_req++;
    g_local_done.push_back(p);
    g_wake.notify_all();
    return request_id(c, p.req_id);
  }
  g_last_external = s;
  HLE_NOTE("sceNpMatching2SetRoomDataExternal %s\n", s.c_str());
  request_id(c, issue(Kind::SetExternal, "set_room_external", b, opt, EV_SET_ROOM_DATA_EXTERNAL));
}
// sceNpMatching2RegisterRoomEventCallback / RegisterSignalingCallback(ctx, cb, arg); cb 0 unregisters.
void RegisterRoomEventCallback(Cpu& c) {
  std::lock_guard lk(g_mu);
  g_room_cb = arg(c, 1), g_room_arg = arg(c, 2);
  ret(c, 0);
}
void RegisterSignalingCallback(Cpu& c) {
  std::lock_guard lk(g_mu);
  g_sig_cb = arg(c, 1), g_sig_arg = arg(c, 2);
  ret(c, 0);
}
// sceNpMatching2SignalingGetLocalNetInfo({+0 size 0x10 (in), +4 local addr, +8 mapped addr, +0xc natStatus}): stored
// by the game (this+0x79c / 0x798 / 0x7a0), no reader found.
void SignalingGetLocalNetInfo(Cpu& c) {
  uint32_t p = arg(c, 0);
  if (!p || !c.mem().valid(p, 0x10)) return ret(c, ERR_INVALID);
  uint32_t lan = stun::default_route_ip();
  c.mem().write<uint32_t>(p + 4, lan ? lan : 0x0100007fu);
  c.mem().write<uint32_t>(p + 8, lan ? lan : 0x0100007fu);
  c.mem().write<uint32_t>(p + 0xc, 2);
  ret(c, 0);
}
// sceNpMatching2SignalingGetConnectionStatus(ctx, u64 roomId (r2:r3), u16 memberId, int* status, u32* addr, u16* port)
// from the Established handler (0x8139ca9e): address and port in network order, reused as-is for sceRudpInitiate.
void SignalingGetConnectionStatus(Cpu& c) {
  uint16_t member = uint16_t(arg(c, 4));
  uint32_t status = arg(c, 5), addr = arg(c, 6), port = arg(c, 7);
  uint32_t ip = 0;
  uint16_t pt = 0;
  bool known = dcp::net::get_peer_address(member, &ip, &pt);
  if (status) c.mem().write<int32_t>(status, known ? 2 : 0);  // 2 = active (SDK value)
  if (addr) c.mem().write<uint32_t>(addr, ip);
  if (port) c.mem().write<uint16_t>(port, pt);
  if (!known) HLE_NOTE("sceNpMatching2SignalingGetConnectionStatus(member 0x%x): unknown member\n", member);
  ret(c, known ? 0u : 0x80022b05u);  // (error value unverified; the game ignores the result)
}

}  // namespace

void register_matching2() {
  REG(sceNpMatching2Init, Init);
  REG(sceNpMatching2Term, Term);
  REG(sceNpMatching2CreateContext, CreateContext);
  REG(sceNpMatching2RegisterContextCallback, RegisterContextCallback);
  REG(sceNpMatching2ContextStart, ContextStart);
  REG(sceNpMatching2ContextStop, ContextStop);
  REG(sceNpMatching2DestroyContext, DestroyContext);
  REG(sceNpMatching2GetServerLocal, GetServerLocal);
  REG(sceNpMatching2GetWorldInfoList, GetWorldInfoList);
  REG(sceNpMatching2SetUserInfo, SetUserInfo);
  REG(sceNpMatching2CreateJoinRoom, CreateJoinRoom);
  REG(sceNpMatching2JoinRoom, JoinRoom);
  REG(sceNpMatching2LeaveRoom, LeaveRoom);
  REG(sceNpMatching2SearchRoom, SearchRoom);
  REG(sceNpMatching2SetRoomDataInternal, SetRoomDataInternal);
  REG(sceNpMatching2SetRoomDataExternal, SetRoomDataExternal);
  REG(sceNpMatching2RegisterRoomEventCallback, RegisterRoomEventCallback);
  REG(sceNpMatching2RegisterSignalingCallback, RegisterSignalingCallback);
  REG(sceNpMatching2SignalingGetLocalNetInfo, SignalingGetLocalNetInfo);
  REG(sceNpMatching2SignalingGetConnectionStatus, SignalingGetConnectionStatus);
}

}  // namespace hle
