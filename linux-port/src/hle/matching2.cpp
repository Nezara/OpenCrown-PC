// libSceNpMatching2 on top of the lobby client (net/lobby_client.h, protocol in notes/map/lobby_protocol.md). Covers
// the 20 imports of notes/map/net.md 2.3 except SignalingGetConnectionStatus (hle/net.cpp, reads net::get_peer_address
// which this file fills). All layouts below were read from the game's call sites / handlers (DcNetMatching):
//
// Calls (request structs built by the game):
//   CreateContextA(param{i32 userId, i32 0, u64 size 0x10}, u16* ctxId)                         bb318..bb345
//   ContextStart(ctx, timeout 0); context callback (ctx, u16 event, u8 cause, i32 err, arg)        bb437, b5540
//     event 0x6f01 marks the context unusable (b5580); we send 0x6f02 (started) / 0x6f03 (stopped). Any context
//     event sets the done byte +0x84 that ContextStop's wrapper bb940 (bb9b2) waits for: without 0x6f03 matching
//     mode 7 (leave, b7480 -> b8280 step 0) never finishes and DcNetMatching stays stuck (net.md 10).
//   GetServerId(ctx, u16*)                                                                          b8884
//   requests (ctx, req*, opt*, u32* reqId); opt = {cb @0, arg @8}; reqId kept at matching+0x74     bb585.., b8d03..
//   request callback (ctx, u32 reqId, u16 event, i32 err, void* data, arg) - handlers check reqId == +0x74,
//     store err at +0x80 and set the done byte +0x85 (b55b0, b5660, b5a90, b5d50, b56d0)
//   GetWorldInfoList req {u16 serverId}; data {World* @0, i32 n @8}, World 0x28 B, worldId u32 @+8 (b55fd, bb640)
//   SetUserInfo req {u16 serverId @0, BinAttr* @8, n @0x10}; BinAttr = {u16 id @0, ptr @8, u64 size @0x10} (bb811)
//   CreateJoinRoomA req (b8c01..b8d14): u16 maxSlot @0 (4), u32 flagAttr @8 (0x86000000), u32 worldId @0xc,
//     u64* blocked accounts @0x50 n @0x58, BinAttr* roomBinInternal @0x60 n @0x68 (id 0x57, the 0x78-B room info),
//     IntAttr* searchable @0x70 n @0x78 (8 x {u16 id, u32 value @4}, ids 0x4c..0x53), BinAttr* roomBinExternal @0x90
//     n @0x98 (id 0x55), signaling param @0xb0 (byte 1)
//   JoinRoomA req {u64 roomId @0, u8 flag @0x28, u64* blocked @0x48 n @0x50} (b9a08..b9a12)
//   create/join response data (b5b10): {RoomDataInternal* @0, Member* list @8, Member* me @0x18, Member* owner @0x20}
//     RoomDataInternal: u16 @8, u16 @0xa, u32 @0xc, u64 roomId @0x18, u32 flagAttr @0x40, BinAttrInternal* @0x48,
//       u32 @0x50 (we put the attr count); BinAttrInternal = {u64 date, u16 member @8, BinAttr @0x10} (data @0x18,
//       size @0x20; the room info must be 0x78 B, b5c31)
//     Member: next @0, u64 joinDate tick @8 (FUN_00572370 -> sceRtcSetTick), u64 accountId @0x10, onlineId 20 B
//       @0x20, u16 memberId @0x38 (slot = (memberId >> 4) - 1, net.md 5.3)
//   SearchRoom req (b906b..b92cb): u32 option @0, u32 worldId @4, u32 start @0x10, u32 max @0x14, u32 flagFilter
//     @0x18, u32 flagAttr @0x1c, IntSearchFilter* @0x20 n @0x28 (16 B: u8 op @0, u16 id @8, u32 value @0xc),
//     u16* attrIds @0x40 n @0x48 ({0x4c, 0x54, 0x55})
//   search response data (b56d0): {i32 n @8, RoomDataExternal* list @0x10}; RoomDataExternal: next @0, u16 @0xa,
//     u64 roomId @0x20, u64 owner accountId @0x40, owner onlineId 20 B @0x50, IntAttr* @0x78 (read as 8 consecutive
//     values = attrs 0x4c..0x53), BinAttr* @0x98 (first = 0x55, copied up to its size)
//   LeaveRoom req {u64 roomId}; SetRoomDataInternal req {u64 roomId, u32 flagFilter @8, u32 flagAttr @0xc,
//     BinAttr* @0x10, n @0x18} (bbdf7..bbf0d); SetRoomDataExternal req {u64 roomId, IntAttr* @8 n @0x10,
//     searchable BinAttr* @0x18 n @0x20, BinAttr* external @0x28 n @0x30} (bc10c..bc13d)
//   RegisterRoomEventCallback(ctx, cb, arg): cb(ctx, u64 roomId, u16 event, void* data, arg) (thunk b5ec0):
//     0x1103 kicked, 0x1104 room destroyed (flag bits only), 0x1105 owner changed {u16 prev, u16 new},
//     0x1106 internal data {.., u32* newFlagAttr @8, BinAttrInternal** @0x38, u64 n @0x40} (b603f..b60d8),
//     0x8101 member joined {Member* @0, u8 cause @8, u8 @0x10 = "is owner" flag}, 0x8102 member left {Member* @0}
//   RegisterSignalingCallback(ctx, cb, arg): cb(ctx, u64 roomId, u16 peerMemberId, u16 event, i32 err, arg) (thunk
//     b6330): 0x5102 Established -> GetConnectionStatus + RudpMgr_Connect; 0x5101 Dead -> close the slot
//   SignalingGetLocalNetInfo({u64 size 0x18, u32 local @8, u32 mapped @0xc, i32 natStatus @0x10}) (b8dc2..b8dfc)
//
// Threading (net.md open question 8, resolved): every callback above takes the matching mutex (+0x6b0) and is written
// to run on another thread than the requester (DcNetMatching ticks under that same mutex, b64ef, and issues the
// requests from there). So nothing is called back from inside a Matching2 call (that would self-deadlock on a
// non-recursive mutex). All callbacks are delivered from sceNpCheckCallback (hle/np.cpp -> matching2_pump), which the
// game calls once per frame from the render thread tail (FUN_0059a1e0, net.md 2.2) - a guest thread, so calling guest
// code is safe. Our own lock is never held while guest code runs.
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common.h"
#include "net/carrier.h"
#include "net/lobby_client.h"
#include "net/rudp.h"
#include "net/stun.h"
#include "net/upnp.h"

#include <arpa/inet.h>

#include <chrono>

namespace dcp::hle {

void matching2_pump();  // called from sceNpCheckCallback (hle/np.cpp)

namespace {

uint64_t steady_ms() {  // monotonic milliseconds
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
}

using net::Json;
using net::LobbyClient;
using net::LobbyMessage;

constexpr uint16_t kCtxId = 1, kServerId = 1;
constexpr int32_t kErrNotInit = int32_t(0x80550c80);  // ours (Matching2 "not initialized"; value unverified)
constexpr int32_t kErrInvalid = int32_t(0x80550c81);  // ours (invalid argument; value unverified)

// Request event ids (SDK values, the game ignores them).
enum : uint16_t { kEvGetWorldInfoList = 0x0002, kEvSetRoomDataExternal = 0x0004, kEvSetUserInfo = 0x0007,
                  kEvCreateJoinRoom = 0x0101, kEvJoinRoom = 0x0102, kEvLeaveRoom = 0x0103, kEvSearchRoom = 0x0106,
                  kEvSetRoomDataInternal = 0x0109 };
enum : uint16_t { kCtxEvStartOver = 0x6f01, kCtxEvStart = 0x6f02, kCtxEvStop = 0x6f03 };
enum : uint16_t { kRoomEvKicked = 0x1103, kRoomEvDestroyed = 0x1104, kRoomEvOwnerChanged = 0x1105,
                  kRoomEvInternalUpdated = 0x1106, kRoomEvMemberJoined = 0x8101, kRoomEvMemberLeft = 0x8102 };
enum : uint16_t { kSigEvDead = 0x5101, kSigEvEstablished = 0x5102 };

using ReqCb = DCP_SYSV void (*)(uint16_t ctx, uint32_t req, uint16_t ev, int32_t err, const void* data, void* arg);
using CtxCb = DCP_SYSV void (*)(uint16_t ctx, uint16_t ev, uint8_t cause, int32_t err, void* arg);
using RoomCb = DCP_SYSV void (*)(uint16_t ctx, uint64_t room, uint16_t ev, const void* data, void* arg);
using SigCb = DCP_SYSV void (*)(uint16_t ctx, uint64_t room, uint16_t member, uint16_t ev, int32_t err, void* arg);

template <class T> T rd(const void* p, size_t off) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(p) + off, sizeof v);
    return v;
}
template <class T> void wr(void* p, size_t off, T v) { std::memcpy(static_cast<uint8_t*>(p) + off, &v, sizeof v); }

// Zeroed guest-visible blocks that live until the callback using them returns.
struct Arena {
    std::vector<std::unique_ptr<uint8_t[]>> blocks;
    uint8_t* alloc(size_t n) {
        blocks.emplace_back(new uint8_t[n + 16]());
        return blocks.back().get();
    }
    uint8_t* bytes(const std::vector<uint8_t>& v) {
        uint8_t* p = alloc(v.size());
        if (!v.empty()) std::memcpy(p, v.data(), v.size());
        return p;
    }
};

struct MemberInfo {
    uint16_t id = 0;
    uint64_t account = 0;
    std::string name;
    int64_t join_ms = 0;
};

enum class Kind { Hello, Worlds, SetUserInfo, CreateJoin, Join, Leave, Search, SetInternal, SetExternal, Endpoint,
                  SetPresence, FriendList };
struct Pending {
    Kind kind;
    uint32_t req_id = 0;  // the id the game got back (0 for internal requests)
    ReqCb cb = nullptr;
    void* arg = nullptr;
    uint16_t ev = 0;
};

struct SignalState {
    uint32_t ip_be = 0;
    uint16_t port_be = 0;
    bool delivered = false;
    uint64_t changed_ms = 0;  // last time the endpoint changed (Established waits kSignalSettleMs after it)
    uint32_t local_ip_be = 0;  // the peer's LAN candidate (signal local_ip / local_port), 0 = none
    uint16_t local_port_be = 0;
};

std::mutex g_mu;
bool g_init = false;
bool g_ctx_created = false;
CtxCb g_ctx_cb = nullptr;
void* g_ctx_arg = nullptr;
RoomCb g_room_cb = nullptr;
void* g_room_arg = nullptr;
SigCb g_sig_cb = nullptr;
void* g_sig_arg = nullptr;
uint32_t g_next_req = 1;
std::map<uint32_t, Pending> g_pending;  // lobby rid -> request
bool g_hello_sent = false, g_hello_ok = false, g_start_wanted = false;
int32_t g_hello_err = 0;
bool g_lobby_down = false;
std::vector<std::pair<uint16_t, int32_t>> g_ctx_events;  // context events raised by Start/Stop, sent from the pump
std::vector<Pending> g_local_done;  // requests answered without the lobby (unchanged room data), sent from the pump
std::string g_last_external;        // last SetRoomDataExternal body sent to the lobby
uint16_t g_reported_port = 0;
// Public endpoint (net.md 11): what we last told the lobby, and whether that is final for this socket.
std::string g_reported_ip;           // "" = lobby uses the connection's address
std::string g_your_ip;               // our address as the lobby sees it (hello your_ip)
bool g_lobby_local = false;          // lobby saw us on a loopback/private address: LAN play, keep local endpoints
bool g_endpoint_inflight = false;
uint64_t g_bound_ms = 0;             // when we first saw the P2P socket bound
constexpr uint64_t kSignalSettleMs = 1000;    // a peer's endpoint must be stable this long before Established
constexpr uint64_t kEndpointWaitMs = 8000;    // max wait for our own STUN before Established anyway
// current room
uint64_t g_room = 0;
uint16_t g_me = 0, g_owner = 0;
std::map<uint16_t, MemberInfo> g_members;
std::map<uint16_t, SignalState> g_signals;
// Toolkit2 presence (friends.md 5, lobby_protocol.md 4.3): our blob as last given by the game, what the lobby has, and
// the GetPresence fetch (one friend_list).
std::string g_pres_data, g_pres_status;  // hex
std::string g_pres_sent = "/";           // data + "/" + status last sent ("/" = nothing yet)
bool g_fl_busy = false;
uint64_t g_fl_gen = 0;
std::vector<PresenceFriend> g_fl_result;

LobbyClient& lobby() { return LobbyClient::instance(); }

uint16_t configured_udp_port() {  // what sceNetBind will use (hle/net.cpp): DCP_NET_PORT or the game's 3658
    if (const char* e = std::getenv("DCP_NET_PORT"); e && *e) return uint16_t(std::atoi(e));
    return 3658;
}

// Microsecond tick since 0001-01-01 (SceRtcTick) from Unix ms.
uint64_t rtc_tick(int64_t unix_ms) { return uint64_t(unix_ms) * 1000ull + 62135596800000000ull; }

Json bin_attrs_json(const uint8_t* arr, uint64_t n) {
    Json a = Json::array();
    for (uint64_t i = 0; arr && i < n && i < 16; ++i) {
        const uint8_t* e = arr + i * 0x18;
        uint16_t id = rd<uint16_t>(e, 0);
        const uint8_t* p = rd<const uint8_t*>(e, 8);
        uint64_t sz = rd<uint64_t>(e, 0x10);
        if (sz > 0x1000) sz = 0x1000;
        Json o;
        o["id"] = int(id);
        o["data"] = p ? net::hex_encode(p, size_t(sz)) : std::string();
        a.push(o);
    }
    return a;
}
Json int_attrs_json(const uint8_t* arr, uint64_t n) {
    Json a = Json::array();
    for (uint64_t i = 0; arr && i < n && i < 16; ++i) {
        Json o;
        o["id"] = int(rd<uint16_t>(arr, i * 8));
        o["value"] = int64_t(rd<uint32_t>(arr, i * 8 + 4));
        a.push(o);
    }
    return a;
}

std::vector<uint8_t> unhex(const Json& j) {
    std::vector<uint8_t> v;
    net::hex_decode(j.as_string(), &v);
    return v;
}

// BinAttr array (0x18 B each) from JSON attrs, with `first` (if present) moved to the front.
uint8_t* make_bin_attrs(Arena& a, const Json& attrs, int first, uint32_t* count) {
    std::vector<const Json*> v;
    for (const auto& e : attrs.elems())
        if (e.get("id").as_int() == first) v.insert(v.begin(), &e);
        else v.push_back(&e);
    *count = uint32_t(v.size());
    if (v.empty()) return nullptr;
    uint8_t* arr = a.alloc(0x18 * v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        std::vector<uint8_t> d = unhex(v[i]->get("data"));
        wr<uint16_t>(arr + i * 0x18, 0, uint16_t(v[i]->get("id").as_int()));
        wr<uint8_t*>(arr + i * 0x18, 8, a.bytes(d));
        wr<uint64_t>(arr + i * 0x18, 0x10, d.size());
    }
    return arr;
}

// BinAttrInternal array (0x28 B each).
uint8_t* make_bin_attrs_internal(Arena& a, const Json& attrs, uint32_t* count) {
    *count = uint32_t(attrs.size());
    if (!*count) return nullptr;
    uint8_t* arr = a.alloc(0x28 * attrs.size());
    for (size_t i = 0; i < attrs.size(); ++i) {
        const Json& e = attrs.at(i);
        std::vector<uint8_t> d = unhex(e.get("data"));
        uint8_t* p = arr + i * 0x28;
        wr<uint16_t>(p, 0x10, uint16_t(e.get("id").as_int()));
        wr<uint8_t*>(p, 0x18, a.bytes(d));
        wr<uint64_t>(p, 0x20, d.size());
    }
    return arr;
}

void put_online_id(uint8_t* p, const std::string& name) {
    std::memcpy(p, name.data(), std::min<size_t>(name.size(), 16));  // 16 chars + NUL + 3 pad (20 B)
}

uint8_t* make_member(Arena& a, const MemberInfo& m) {
    uint8_t* p = a.alloc(0x60);
    wr<uint64_t>(p, 8, rtc_tick(m.join_ms));
    wr<uint64_t>(p, 0x10, m.account);
    put_online_id(p + 0x20, m.name);
    wr<uint16_t>(p, 0x38, m.id);
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

uint8_t* make_room_internal(Arena& a, const Json& room) {
    uint8_t* r = a.alloc(0x80);
    wr<uint16_t>(r, 8, kServerId);                                  // (field meaning unverified)
    wr<uint16_t>(r, 0xa, uint16_t(room.get("max_slot").as_int(4)));  // (field meaning unverified)
    wr<uint32_t>(r, 0xc, uint32_t(room.get("world_id").as_int(1)));  // (field meaning unverified)
    wr<uint64_t>(r, 0x18, room.get("room_id").as_u64());
    wr<uint32_t>(r, 0x40, uint32_t(room.get("flag_attr").as_int()));
    uint32_t n = 0;
    wr<uint8_t*>(r, 0x48, make_bin_attrs_internal(a, room.get("bin_internal"), &n));
    wr<uint32_t>(r, 0x50, n);
    return r;
}

// Create/join response; also records the room locally. Caller holds g_mu.
uint8_t* make_join_response(Arena& a, const Json& body) {
    const Json& room = body.get("room");
    uint8_t* resp = a.alloc(0x30);
    wr<uint8_t*>(resp, 0, make_room_internal(a, room));
    g_room = room.get("room_id").as_u64();
    g_me = uint16_t(body.get("member_id").as_int());
    g_owner = uint16_t(room.get("owner").as_int());
    g_members.clear();
    g_signals.clear();
    uint8_t *head = nullptr, *prev = nullptr, *me = nullptr, *owner = nullptr;
    uint32_t count = 0;
    for (const auto& mj : room.get("members").elems()) {
        MemberInfo m = member_from(mj);
        g_members[m.id] = m;
        uint8_t* p = make_member(a, m);
        if (prev) wr<uint8_t*>(prev, 0, p);
        else head = p;
        prev = p;
        if (m.id == g_me) me = p;
        if (m.id == g_owner) owner = p;
        ++count;
    }
    wr<uint8_t*>(resp, 8, head);
    wr<uint32_t>(resp, 0x10, count);  // (field meaning unverified)
    wr<uint8_t*>(resp, 0x18, me);
    wr<uint8_t*>(resp, 0x20, owner);
    return resp;
}

uint8_t* make_search_response(Arena& a, const Json& body) {
    uint8_t* resp = a.alloc(0x20);
    uint8_t *head = nullptr, *prev = nullptr;
    int32_t n = 0;
    for (const auto& rj : body.get("rooms").elems()) {
        uint8_t* r = a.alloc(0xb0);
        wr<uint16_t>(r, 8, uint16_t(rj.get("max_slot").as_int(4)));       // (field meaning unverified)
        wr<uint16_t>(r, 0xa, uint16_t(rj.get("cur_members").as_int()));   // (field meaning unverified)
        wr<uint32_t>(r, 0xc, uint32_t(rj.get("world_id").as_int(1)));     // (field meaning unverified)
        wr<uint64_t>(r, 0x20, rj.get("room_id").as_u64());
        wr<uint64_t>(r, 0x40, rj.get("owner_account_id").as_u64());
        put_online_id(r + 0x50, rj.get("owner_name").as_string());
        uint8_t* ia = a.alloc(8 * 8);  // the game reads 8 values blind: attrs 0x4c..0x53 in order
        for (int k = 0; k < 8; ++k) wr<uint16_t>(ia, size_t(k) * 8, uint16_t(0x4c + k));
        for (const auto& e : rj.get("int_attrs").elems()) {
            int id = int(e.get("id").as_int());
            if (id >= 0x4c && id <= 0x53) wr<uint32_t>(ia, size_t(id - 0x4c) * 8 + 4, uint32_t(e.get("value").as_int()));
        }
        wr<uint8_t*>(r, 0x78, ia);
        wr<uint64_t>(r, 0x80, 8);
        uint32_t nb = 0;
        wr<uint8_t*>(r, 0x98, make_bin_attrs(a, rj.get("bin_external"), 0x55, &nb));
        wr<uint64_t>(r, 0xa0, nb);
        if (prev) wr<uint8_t*>(prev, 0, r);
        else head = r;
        prev = r;
        ++n;
    }
    wr<int32_t>(resp, 8, n);
    wr<uint8_t*>(resp, 0x10, head);
    return resp;
}

// Wire RoomInfo (online_room_attrs.md 2: wire = struct - 8 from 0x38 up to 0x65; the serializer FUN_00098fb0 then
// writes struct +0x68 u32 world id at wire 0x5e, +0x66 u16 at 0x62, +0x70 u64 room id at 0x64, 990dc..990ff). Flag word
// struct +0x5e -> wire 0x56 (c1af0 needs bit 0x400), host name wire 0x00 (8 B). For logs only (the game reads the blob).
uint64_t blob_room(const uint8_t* p, size_t n) { return p && n >= 0x6c ? rd<uint64_t>(p, 0x64) : 0; }
uint32_t blob_world(const uint8_t* p, size_t n) { return p && n >= 0x62 ? rd<uint32_t>(p, 0x5e) : 0; }
uint32_t blob_flags(const uint8_t* p, size_t n) { return p && n >= 0x58 ? rd<uint16_t>(p, 0x56) : 0; }
std::string blob_name(const uint8_t* p, size_t n) {
    std::string r;
    for (size_t i = 0; p && i < 8 && i < n && p[i]; ++i) r += (p[i] >= 0x20 && p[i] < 0x7f) ? char(p[i]) : '?';
    return r;
}
void log_attr55(const char* when, const Json& bin_external) {  // friends.md 7: does attr 0x55 carry the room id?
    for (const auto& e : bin_external.elems())
        if (e.get("id").as_int() == 0x55) {
            std::vector<uint8_t> d;
            net::hex_decode(e.get("data").as_string(), &d);
            std::fprintf(stderr, "MATCHING2 %s: attr 0x55 %zu B, room id at wire 0x64 = %llu, world at wire 0x5e = %u\n",
                         when, d.size(), (unsigned long long)blob_room(d.data(), d.size()), blob_world(d.data(), d.size()));
        }
}

uint32_t ip_from(const std::string& s) {
    in_addr a{};
    if (inet_pton(AF_INET, s.c_str(), &a) != 1) return 0;
    return a.s_addr;  // network order
}

// Queue a request to the lobby for a game call. Caller holds g_mu.
uint32_t issue(Kind k, const std::string& op, Json body, const void* opt, uint16_t ev) {
    Pending p;
    p.kind = k, p.ev = ev;
    if (opt) {
        p.cb = rd<ReqCb>(opt, 0);
        p.arg = rd<void*>(opt, 8);
        p.req_id = g_next_req++;
    }
    uint32_t rid = lobby().request(op, std::move(body));
    g_pending[rid] = p;
    return p.req_id;
}

// Sends our presence blob when the lobby link is up and it changed. Caller holds g_mu.
void flush_presence_locked() {
    if (!g_hello_ok || g_lobby_down || np_identity().secret.empty()) return;  // unverified: the lobby refuses it
    std::string key = g_pres_data + "/" + g_pres_status;
    if (key == g_pres_sent) return;
    g_pres_sent = key;
    Json b;
    b["game_data"] = g_pres_data;
    if (!g_pres_data.empty() && !g_pres_status.empty()) b["status"] = g_pres_status;
    issue(Kind::SetPresence, "set_presence", b, nullptr, 0);
}

void send_hello_locked() {
    if (g_hello_sent) return;
    g_hello_sent = true;
    const NpIdentity& id = np_identity();
    Json h;
    h["proto"] = net::kLobbyProtocol;
    h["version"] = net::kGameNetVersion;
    h["title"] = "CUSA10454";
    h["name"] = id.online_id;
    h["account_id"] = net::u64_str(id.account_id);
    if (!id.secret.empty()) h["secret"] = id.secret;  // verified account: friends see us online / in our room
    h["udp_port"] = int(configured_udp_port());
    g_reported_port = configured_udp_port();
    issue(Kind::Hello, "hello", h, nullptr, 0);
    std::fprintf(stderr, "MATCHING2 hello -> %s as %s (account %llu, udp %u)\n", lobby().url().c_str(),
                 id.online_id.c_str(), (unsigned long long)id.account_id, g_reported_port);
}

// ---- SysV entry points ----
DCP_SYSV int32_t sceNpMatching2Initialize(const void* param) {
    std::lock_guard<std::mutex> lk(g_mu);
    DCP_HLE_LOG("sceNpMatching2Initialize", "pool 0x%llx", param ? (unsigned long long)rd<uint64_t>(param, 0) : 0ull);
    g_init = true;
    return kOk;
}
DCP_SYSV int32_t sceNpMatching2Terminate() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_init = false;
    return kOk;
}
DCP_SYSV int32_t sceNpMatching2CreateContextA(const void* param, uint16_t* ctx) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::fprintf(stderr, "HLE sceNpMatching2CreateContextA(user %d) -> ctx %u\n", param ? rd<int32_t>(param, 0) : -1, kCtxId);
    if (!ctx) return kErrInvalid;
    *ctx = kCtxId;
    g_ctx_created = true;
    return kOk;
}
DCP_SYSV int32_t sceNpMatching2RegisterContextCallback(void* cb, void* arg) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_ctx_cb = reinterpret_cast<CtxCb>(cb), g_ctx_arg = arg;
    return kOk;
}
DCP_SYSV int32_t sceNpMatching2ContextStart(uint16_t ctx, uint64_t) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::fprintf(stderr, "HLE sceNpMatching2ContextStart(%u)\n", ctx);
    if (!g_ctx_created) return kErrNotInit;
    g_start_wanted = true;  // context event goes out from matching2_pump once the lobby answered hello
    if (g_hello_ok && !g_lobby_down) g_ctx_events.push_back({kCtxEvStart, 0});  // restart: link already up
    send_hello_locked();
    return kOk;
}
DCP_SYSV int32_t sceNpMatching2ContextStop(uint16_t ctx) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::fprintf(stderr, "HLE sceNpMatching2ContextStop(%u)\n", ctx);
    if (g_room) {
        Json b;
        b["room_id"] = net::u64_str(g_room);
        issue(Kind::Leave, "leave", b, nullptr, 0);
        g_room = 0, g_members.clear(), g_signals.clear();
        net::clear_peer_addresses();
    }
    g_start_wanted = false;
    g_ctx_events.push_back({kCtxEvStop, 0});  // the game waits for it (bb940) before DestroyContext
    return kOk;
}
DCP_SYSV int32_t sceNpMatching2DestroyContext(uint16_t ctx) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::fprintf(stderr, "HLE sceNpMatching2DestroyContext(%u)\n", ctx);
    g_ctx_created = false;
    return kOk;
}
DCP_SYSV int32_t sceNpMatching2GetServerId(uint16_t, uint16_t* server) {
    if (!server) return kErrInvalid;
    *server = kServerId;
    return kOk;
}
DCP_SYSV int32_t sceNpMatching2GetWorldInfoList(uint16_t, const void* req, const void* opt, uint32_t* req_id) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!req || !opt) return kErrInvalid;
    uint32_t r = issue(Kind::Worlds, "worlds", Json::object(), opt, kEvGetWorldInfoList);
    if (req_id) *req_id = r;
    return kOk;
}
DCP_SYSV int32_t sceNpMatching2SetUserInfo(uint16_t, const uint8_t* req, const void* opt, uint32_t* req_id) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!req || !opt) return kErrInvalid;
    Json b;
    b["bin_attrs"] = bin_attrs_json(rd<const uint8_t*>(req, 8), rd<uint64_t>(req, 0x10));
    uint32_t r = issue(Kind::SetUserInfo, "set_user_info", b, opt, kEvSetUserInfo);
    if (req_id) *req_id = r;
    return kOk;
}
DCP_SYSV int32_t sceNpMatching2CreateJoinRoomA(uint16_t, const uint8_t* req, const void* opt, uint32_t* req_id) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!req || !opt) return kErrInvalid;
    Json b;
    b["max_slot"] = int(rd<uint16_t>(req, 0));
    b["flag_attr"] = int64_t(rd<uint32_t>(req, 8));
    b["world_id"] = int64_t(rd<uint32_t>(req, 0xc));
    b["bin_internal"] = bin_attrs_json(rd<const uint8_t*>(req, 0x60), rd<uint64_t>(req, 0x68));
    b["int_attrs"] = int_attrs_json(rd<const uint8_t*>(req, 0x70), rd<uint64_t>(req, 0x78));
    b["bin_external"] = bin_attrs_json(rd<const uint8_t*>(req, 0x90), rd<uint64_t>(req, 0x98));
    b["member_bin_internal"] = bin_attrs_json(rd<const uint8_t*>(req, 0xa0), rd<uint64_t>(req, 0xa8));
    std::fprintf(stderr, "HLE sceNpMatching2CreateJoinRoomA %s\n", b.dump().c_str());
    log_attr55("create", b.get("bin_external"));
    uint32_t r = issue(Kind::CreateJoin, "create_room", b, opt, kEvCreateJoinRoom);
    if (req_id) *req_id = r;
    return kOk;
}
DCP_SYSV int32_t sceNpMatching2JoinRoomA(uint16_t, const uint8_t* req, const void* opt, uint32_t* req_id) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!req || !opt) return kErrInvalid;
    Json b;
    b["room_id"] = net::u64_str(rd<uint64_t>(req, 0));
    std::fprintf(stderr, "HLE sceNpMatching2JoinRoomA %s\n", b.dump().c_str());
    uint32_t r = issue(Kind::Join, "join", b, opt, kEvJoinRoom);
    if (req_id) *req_id = r;
    return kOk;
}
DCP_SYSV int32_t sceNpMatching2LeaveRoom(uint16_t, const uint8_t* req, const void* opt, uint32_t* req_id) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!req || !opt) return kErrInvalid;
    Json b;
    b["room_id"] = net::u64_str(rd<uint64_t>(req, 0));
    std::fprintf(stderr, "HLE sceNpMatching2LeaveRoom %s\n", b.dump().c_str());
    uint32_t r = issue(Kind::Leave, "leave", b, opt, kEvLeaveRoom);
    if (req_id) *req_id = r;
    g_room = 0, g_members.clear(), g_signals.clear();
    net::clear_peer_addresses();
    return kOk;
}
DCP_SYSV int32_t sceNpMatching2SearchRoom(uint16_t, const uint8_t* req, const void* opt, uint32_t* req_id) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!req || !opt) return kErrInvalid;
    Json b;
    b["world_id"] = int64_t(rd<uint32_t>(req, 4));
    b["start"] = int64_t(rd<uint32_t>(req, 0x10));
    b["max"] = int64_t(rd<uint32_t>(req, 0x14));
    b["flag_filter"] = int64_t(rd<uint32_t>(req, 0x18));
    b["flag_attr"] = int64_t(rd<uint32_t>(req, 0x1c));
    Json f = Json::array();
    const uint8_t* flt = rd<const uint8_t*>(req, 0x20);
    for (uint64_t i = 0, n = rd<uint64_t>(req, 0x28); flt && i < n && i < 16; ++i) {
        Json o;
        o["op"] = int(flt[i * 16]);
        o["id"] = int(rd<uint16_t>(flt, i * 16 + 8));
        o["value"] = int64_t(rd<uint32_t>(flt, i * 16 + 0xc));
        f.push(o);
    }
    b["filters"] = f;
    Json ids = Json::array();
    const uint8_t* ai = rd<const uint8_t*>(req, 0x40);
    for (uint64_t i = 0, n = rd<uint64_t>(req, 0x48); ai && i < n && i < 16; ++i) ids.push(int(rd<uint16_t>(ai, i * 2)));
    b["attr_ids"] = ids;
    if (const char* code = std::getenv("DCP_JOIN_CODE"); code && *code) {  // launcher "join by code"
        // The code names the room, so its attribute conditions do not apply: once the host is in town the game rewrites
        // the packed attr 0x53 from the save (e.g. 0x1100 -> 0x81102, net.md 3.4 bits [1:0] / [20:19]), which a search
        // built without the host's quest conditions (EQ on 0x53, b8fd0) would never match.
        b["join_code"] = code;
        b["filters"] = Json::array();
        b["flag_filter"] = int64_t(0);
    }
    std::fprintf(stderr, "HLE sceNpMatching2SearchRoom %s\n", b.dump().c_str());
    uint32_t r = issue(Kind::Search, "search", b, opt, kEvSearchRoom);
    if (req_id) *req_id = r;
    return kOk;
}
DCP_SYSV int32_t sceNpMatching2SetRoomDataInternal(uint16_t, const uint8_t* req, const void* opt, uint32_t* req_id) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!req || !opt) return kErrInvalid;
    Json b;
    b["room_id"] = net::u64_str(rd<uint64_t>(req, 0));
    b["flag_filter"] = int64_t(rd<uint32_t>(req, 8));
    b["flag_attr"] = int64_t(rd<uint32_t>(req, 0xc));
    b["bin_internal"] = bin_attrs_json(rd<const uint8_t*>(req, 0x10), rd<uint64_t>(req, 0x18));
    DCP_HLE_LOG("sceNpMatching2SetRoomDataInternal", "%s", b.dump().c_str());
    uint32_t r = issue(Kind::SetInternal, "set_room_internal", b, opt, kEvSetRoomDataInternal);
    if (req_id) *req_id = r;
    return kOk;
}
DCP_SYSV int32_t sceNpMatching2SetRoomDataExternal(uint16_t, const uint8_t* req, const void* opt, uint32_t* req_id) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!req || !opt) return kErrInvalid;
    Json b;
    b["room_id"] = net::u64_str(rd<uint64_t>(req, 0));
    b["int_attrs"] = int_attrs_json(rd<const uint8_t*>(req, 8), rd<uint64_t>(req, 0x10));
    b["bin_external"] = bin_attrs_json(rd<const uint8_t*>(req, 0x28), rd<uint64_t>(req, 0x30));
    // The host's game re-sends its external room data every ~7 frames while in town, almost always unchanged: answer
    // repeats locally so the lobby only sees real changes (each message is a billed Worker request).
    std::string s = b.dump();
    if (s == g_last_external) {
        Pending p;
        p.kind = Kind::SetExternal, p.ev = kEvSetRoomDataExternal;
        p.cb = rd<ReqCb>(opt, 0), p.arg = rd<void*>(opt, 8), p.req_id = g_next_req++;
        g_local_done.push_back(p);
        if (req_id) *req_id = p.req_id;
        return kOk;
    }
    g_last_external = s;
    std::fprintf(stderr, "HLE sceNpMatching2SetRoomDataExternal %s\n", s.c_str());
    log_attr55("set external", b.get("bin_external"));
    uint32_t r = issue(Kind::SetExternal, "set_room_external", b, opt, kEvSetRoomDataExternal);
    if (req_id) *req_id = r;
    return kOk;
}
DCP_SYSV int32_t sceNpMatching2RegisterRoomEventCallback(uint16_t, void* cb, void* arg) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_room_cb = reinterpret_cast<RoomCb>(cb), g_room_arg = arg;
    return kOk;
}
DCP_SYSV int32_t sceNpMatching2RegisterSignalingCallback(uint16_t, void* cb, void* arg) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_sig_cb = reinterpret_cast<SigCb>(cb), g_sig_arg = arg;
    return kOk;
}
DCP_SYSV int32_t sceNpMatching2SignalingGetLocalNetInfo(uint8_t* info) {
    if (!info) return kErrInvalid;
    uint32_t lo = htonl(INADDR_LOOPBACK);
    wr<uint32_t>(info, 8, lo);    // local address (stored at matching+0x538, never read: net.md 2.3)
    wr<uint32_t>(info, 0xc, lo);  // mapped address (+0x534)
    wr<int32_t>(info, 0x10, 2);   // NAT type 2 (+0x53c)
    return kOk;
}

}  // namespace

// Drains the lobby link and delivers every Matching2 callback (see the threading note at the top). Called once per
// frame from sceNpCheckCallback on the game's render thread.
void matching2_pump() {
    std::vector<std::function<void()>> calls;  // guest callbacks, run after g_mu is released
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_ctx_cb)
            for (auto [ev, err] : g_ctx_events) {
                CtxCb cb = g_ctx_cb;
                void* arg = g_ctx_arg;
                uint16_t e = ev;
                int32_t r = err;
                calls.push_back([=] { cb(kCtxId, e, 0, r, arg); });
            }
        g_ctx_events.clear();
        for (const Pending& p : g_local_done) {
            ReqCb cb = p.cb;
            void* arg = p.arg;
            uint32_t rq = p.req_id;
            uint16_t ev = p.ev;
            if (cb) calls.push_back([=] { cb(kCtxId, rq, ev, 0, nullptr, arg); });
        }
        g_local_done.clear();
        if (!g_hello_sent) return;  // never went online: no lobby traffic at all
        LobbyMessage m;
        while (lobby().poll(&m)) {
            if (m.kind == LobbyMessage::Down) {
                if (g_lobby_down) continue;
                g_lobby_down = true;
                g_last_external.clear();
                std::fprintf(stderr, "MATCHING2 lobby link down\n");
                if (g_room && g_room_cb) {
                    RoomCb cb = g_room_cb;
                    void* arg = g_room_arg;
                    uint64_t room = g_room;
                    auto a = std::make_shared<Arena>();
                    uint8_t* d = a->alloc(0x10);
                    calls.push_back([=] { cb(kCtxId, room, kRoomEvDestroyed, d, arg); (void)a; });
                }
                if (g_ctx_cb && g_hello_ok) {
                    CtxCb cb = g_ctx_cb;
                    void* arg = g_ctx_arg;
                    calls.push_back([=] { cb(kCtxId, kCtxEvStartOver, 0, LobbyClient::kErrLobbyDown, arg); });
                }
                continue;
            }
            if (m.kind == LobbyMessage::Response) {
                auto it = g_pending.find(m.rid);
                if (it == g_pending.end()) continue;
                Pending p = it->second;
                g_pending.erase(it);
                if (p.kind == Kind::Hello) {
                    g_hello_ok = m.err == 0;
                    g_hello_err = m.err;
                    if (g_hello_ok) {
                        g_your_ip = m.body.get("your_ip").as_string();
                        g_lobby_local = net::stun::is_private_or_loopback(ip_from(g_your_ip));
                    }
                    std::fprintf(stderr, "MATCHING2 hello %s (err 0x%x)%s\n", g_hello_ok ? "ok" : "FAILED", uint32_t(m.err),
                                 g_hello_ok ? (" your_ip " + m.body.get("your_ip").as_string()).c_str() : "");
                    flush_presence_locked();
                    if (g_start_wanted && g_ctx_cb) {
                        CtxCb cb = g_ctx_cb;
                        void* arg = g_ctx_arg;
                        int32_t err = m.err;
                        calls.push_back([=] { cb(kCtxId, kCtxEvStart, 0, err, arg); });
                    }
                    continue;
                }
                if (p.kind == Kind::Endpoint) {
                    g_endpoint_inflight = false;
                    std::fprintf(stderr, "MATCHING2 endpoint %s (lobby has %s:%lld)\n", m.err ? "FAILED" : "ok",
                                 m.body.get("ip").as_string().c_str(), (long long)m.body.get("port").as_int());
                }
                if (p.kind == Kind::SetPresence) {
                    if (m.err) std::fprintf(stderr, "MATCHING2 set_presence failed: 0x%08x\n", uint32_t(m.err));
                    continue;
                }
                if (p.kind == Kind::FriendList) {  // GetPresence (friends.md 5): rows = friends in a room with a blob
                    g_fl_result.clear();
                    size_t n = 0;
                    for (const Json& f : m.body.get("friends").elems()) {
                        ++n;
                        PresenceFriend pf;
                        pf.account = f.get("account_id").as_u64();
                        pf.name = f.get("name").as_string();
                        pf.room = f.get("room_id").as_u64();
                        net::hex_decode(f.get("game_data").as_string(), &pf.data);
                        net::hex_decode(f.get("status").as_string(), &pf.status);
                        bool row = f.get("online").as_bool() && pf.room && !pf.data.empty();
                        std::fprintf(stderr, "MATCHING2 friend %s (%llu): %s, room %llu, blob %zu B (room id in blob %llu, "
                                     "world %u, flags 0x%04x, host '%s')%s\n", pf.name.c_str(), (unsigned long long)pf.account,
                                     f.get("online").as_bool() ? "online" : "offline", (unsigned long long)pf.room,
                                     pf.data.size(), (unsigned long long)blob_room(pf.data.data(), pf.data.size()),
                                     blob_world(pf.data.data(), pf.data.size()), blob_flags(pf.data.data(), pf.data.size()),
                                     blob_name(pf.data.data(), pf.data.size()).c_str(), row ? " -> presence row" : "");
                        if (row) g_fl_result.push_back(std::move(pf));
                    }
                    std::fprintf(stderr, "MATCHING2 friend_list %s: %zu friend(s), %zu presence row(s)\n",
                                 m.err ? "FAILED" : "ok", n, g_fl_result.size());
                    g_fl_busy = false;
                    ++g_fl_gen;
                    continue;
                }
                if (!p.cb) continue;  // internal request (endpoint, implicit leave)
                auto a = std::make_shared<Arena>();
                const void* data = nullptr;
                if (m.err == 0) {
                    switch (p.kind) {
                    case Kind::Worlds: {
                        uint8_t* resp = a->alloc(0x10);
                        const Json& w = m.body.get("worlds");
                        uint8_t* arr = a->alloc(0x28 * std::max<size_t>(w.size(), 1));
                        for (size_t i = 0; i < w.size(); ++i) {
                            uint8_t* e = arr + i * 0x28;
                            wr<uint32_t>(e, 8, uint32_t(w.at(i).get("world_id").as_int(1)));
                            wr<uint32_t>(e, 0x18, uint32_t(w.at(i).get("rooms").as_int()));    // (field meaning unverified)
                            wr<uint32_t>(e, 0x1c, uint32_t(w.at(i).get("members").as_int()));  // (field meaning unverified)
                        }
                        wr<uint8_t*>(resp, 0, arr);
                        wr<int32_t>(resp, 8, int32_t(w.size()));
                        data = resp;
                        break;
                    }
                    case Kind::CreateJoin:
                    case Kind::Join:
                        data = make_join_response(*a, m.body);
                        std::fprintf(stderr, "MATCHING2 %s room %llu as member 0x%x (owner 0x%x, %zu members, code %s)\n",
                                     p.kind == Kind::Join ? "joined" : "created", (unsigned long long)g_room, g_me,
                                     g_owner, g_members.size(), m.body.get("room").get("join_code").as_string().c_str());
                        break;
                    case Kind::Search:
                        data = make_search_response(*a, m.body);
                        std::fprintf(stderr, "MATCHING2 search -> %lld room(s)\n", (long long)m.body.get("rooms").size());
                        break;
                    default: break;  // SetUserInfo / Leave / SetInternal / SetExternal: no response data
                    }
                } else {
                    std::fprintf(stderr, "MATCHING2 request %u (event 0x%x) failed: 0x%08x\n", p.req_id, p.ev, uint32_t(m.err));
                    if (p.kind == Kind::SetExternal) g_last_external.clear();  // resend it next time
                }
                ReqCb cb = p.cb;
                void* arg = p.arg;
                uint32_t rq = p.req_id;
                uint16_t ev = p.ev;
                int32_t err = m.err;
                calls.push_back([=] { cb(kCtxId, rq, ev, err, data, arg); (void)a; });
                continue;
            }
            // Events
            const Json& b = m.body;
            uint64_t room = b.get("room_id").as_u64();
            if (room != g_room || !g_room) continue;  // stale (left already)
            RoomCb rcb = g_room_cb;
            void* rarg = g_room_arg;
            auto a = std::make_shared<Arena>();
            if (m.ev == "member_joined") {
                MemberInfo mi = member_from(b.get("member"));
                g_members[mi.id] = mi;
                std::fprintf(stderr, "MATCHING2 member 0x%x (%s) joined\n", mi.id, mi.name.c_str());
                uint8_t* d = a->alloc(0x20);
                wr<uint8_t*>(d, 0, make_member(*a, mi));
                if (rcb) calls.push_back([=] { rcb(kCtxId, room, kRoomEvMemberJoined, d, rarg); (void)a; });
            } else if (m.ev == "member_left") {
                uint16_t id = uint16_t(b.get("member_id").as_int());
                MemberInfo mi;
                mi.id = id;
                if (g_members.count(id)) mi = g_members[id];
                g_members.erase(id);
                g_signals.erase(id);
                net::clear_peer_address(id);
                std::fprintf(stderr, "MATCHING2 member 0x%x left\n", id);
                uint8_t* d = a->alloc(0x20);
                wr<uint8_t*>(d, 0, make_member(*a, mi));
                if (rcb) calls.push_back([=] { rcb(kCtxId, room, kRoomEvMemberLeft, d, rarg); (void)a; });
            } else if (m.ev == "owner_changed") {
                uint16_t prev = uint16_t(b.get("prev").as_int()), now = uint16_t(b.get("new").as_int());
                g_owner = now;
                std::fprintf(stderr, "MATCHING2 owner 0x%x -> 0x%x\n", prev, now);
                uint8_t* d = a->alloc(0x10);
                wr<uint16_t>(d, 0, prev);
                wr<uint16_t>(d, 2, now);
                if (rcb) calls.push_back([=] { rcb(kCtxId, room, kRoomEvOwnerChanged, d, rarg); (void)a; });
            } else if (m.ev == "room_destroyed" || m.ev == "kicked") {
                bool kicked = m.ev == "kicked";
                std::fprintf(stderr, "MATCHING2 %s\n", kicked ? "kicked out" : "room destroyed");
                g_room = 0, g_members.clear(), g_signals.clear();
                net::clear_peer_addresses();
                uint8_t* d = a->alloc(0x10);
                if (rcb) calls.push_back([=] { rcb(kCtxId, room, kicked ? kRoomEvKicked : kRoomEvDestroyed, d, rarg); (void)a; });
            } else if (m.ev == "room_internal_updated") {
                uint8_t* d = a->alloc(0x50);
                uint8_t* flags = a->alloc(8);
                wr<uint32_t>(flags, 0, uint32_t(b.get("flag_attr").as_int()));
                wr<uint8_t*>(d, 8, flags);
                uint32_t n = 0;
                uint8_t* arr = make_bin_attrs_internal(*a, b.get("bin_internal"), &n);
                if (n) {
                    uint8_t* ptrs = a->alloc(8 * n);
                    for (uint32_t i = 0; i < n; ++i) wr<uint8_t*>(ptrs, i * 8, arr + i * 0x28);
                    wr<uint8_t*>(d, 0x38, ptrs);
                    wr<uint64_t>(d, 0x40, n);
                }
                if (rcb) calls.push_back([=] { rcb(kCtxId, room, kRoomEvInternalUpdated, d, rarg); (void)a; });
            } else if (m.ev == "signal") {
                uint16_t id = uint16_t(b.get("member_id").as_int());
                SignalState& s = g_signals[id];
                uint32_t ip = ip_from(b.get("ip").as_string());
                uint16_t port = htons(uint16_t(b.get("port").as_int()));
                if (s.delivered && (s.ip_be != ip || s.port_be != port))
                    std::fprintf(stderr, "MATCHING2 member 0x%x endpoint changed after Established (kept the first)\n", id);
                if (!s.delivered && (s.ip_be != ip || s.port_be != port))
                    s.ip_be = ip, s.port_be = port, s.changed_ms = steady_ms();
                if (!s.delivered) {
                    uint32_t lip = ip_from(b.get("local_ip").as_string());
                    uint16_t lport = uint16_t(b.get("local_port").as_int());
                    bool lan = lip && lport && net::stun::is_rfc1918(lip);
                    s.local_ip_be = lan ? lip : 0, s.local_port_be = lan ? htons(lport) : 0;
                }
                if (id != g_me && port) net::rudp::punch(ip, port);  // open our NAT towards it now (hole punching)
                std::fprintf(stderr, "MATCHING2 signal: member 0x%x at %s:%lld\n", id, b.get("ip").as_string().c_str(),
                             (long long)b.get("port").as_int());
            } else if (m.ev == "signal_dead") {
                uint16_t id = uint16_t(b.get("member_id").as_int());
                auto it = g_signals.find(id);
                bool was = it != g_signals.end() && it->second.delivered;
                if (it != g_signals.end()) g_signals.erase(it);
                net::clear_peer_address(id);
                SigCb scb = g_sig_cb;
                void* sarg = g_sig_arg;
                if (was && scb) calls.push_back([=] { scb(kCtxId, room, id, kSigEvDead, 0, sarg); });
            }
        }
        // Our real UDP endpoint once the game's P2P socket is bound (DCP_NET_PORT may differ from the hello guess).
        // Behind a NAT (the lobby saw a public address) the endpoint is the STUN-mapped one of the P2P socket; with a
        // LAN / local lobby, or when STUN fails, it is the local port (with the address the lobby sees). net.md 11.
        bool endpoint_final = false;
        if (g_hello_ok && !g_lobby_down) {
            uint16_t port = net::rudp::bound_port();
            if (port && !g_bound_ms) g_bound_ms = steady_ms();
            uint32_t pip = 0;
            uint16_t pport = 0;
            auto st = port ? net::rudp::public_endpoint(&pip, &pport) : net::Carrier::StunState::Idle;
            net::upnp::Status up = net::upnp::status();
            // Idle = socket bound but STUN not started yet (rudp starts it after the UPnP hook): reporting then would
            // send the local port, and a peer that settles before the STUN update keeps that wrong endpoint.
            bool stun_pending = st == net::Carrier::StunState::Running ||
                                (st == net::Carrier::StunState::Idle && !g_lobby_local);
            bool waiting = (stun_pending || up.state == net::upnp::State::Pending) &&
                           steady_ms() - g_bound_ms < kEndpointWaitMs;
            std::string want_ip;
            uint16_t want_port = port;
            const char* how = g_lobby_local ? "LAN lobby" : "no STUN/UPnP";
            // A UPnP mapping with a public WAN address accepts unsolicited packets (works even against a peer behind
            // a symmetric NAT), so it wins over the STUN mapping; behind double NAT the STUN mapping is the real one.
            uint32_t up_ip = up.state == net::upnp::State::Mapped ? ip_from(up.ext_ip) : 0;
            if (port && !g_lobby_local && up_ip && !net::stun::is_private_or_loopback(up_ip))
                want_ip = up.ext_ip, want_port = up.ext_port, how = "UPnP";
            else if (port && !g_lobby_local && st == net::Carrier::StunState::Done)
                want_ip = net::stun::ip_str(pip), want_port = ntohs(pport), how = "STUN";
            if (port && !waiting && (want_port != g_reported_port || want_ip != g_reported_ip)) {
                g_reported_port = want_port, g_reported_ip = want_ip;
                Json e;
                e["port"] = int(want_port);
                if (!want_ip.empty()) e["ip"] = want_ip;
                // LAN candidate for a peer behind the same router (net.md 11.1); not needed with a LAN lobby.
                uint32_t lan = net::stun::default_route_ip();
                if (!g_lobby_local && lan && net::stun::is_rfc1918(lan))
                    e["local_ip"] = net::stun::ip_str(lan), e["local_port"] = int(port);
                g_endpoint_inflight = true;
                std::fprintf(stderr, "MATCHING2 endpoint -> %s:%u (local UDP %u, %s)\n",
                             want_ip.empty() ? "<address the lobby sees>" : want_ip.c_str(), want_port, port,
                             how);
                issue(Kind::Endpoint, "endpoint", e, nullptr, 0);
            }
            endpoint_final = port && !waiting && !g_endpoint_inflight;
        }
        // Signaling Established per peer: only once the game can act on it (callback registered after the
        // create/join response, socket bound by RudpMgr_OpenSocket, member known from the room data).
        if (g_room && g_sig_cb && net::rudp::open_socket_count() > 0) {
            uint64_t now = steady_ms();
            for (auto& [id, s] : g_signals) {
                if (s.delivered || !s.port_be || id == g_me || !g_members.count(id)) continue;
                // Wait until our own public endpoint reached the lobby (the peer must be able to answer our SYNs) and
                // the peer's endpoint stopped changing (its STUN update may follow the join-time signal).
                if (!endpoint_final || now - s.changed_ms < kSignalSettleMs) continue;
                s.delivered = true;
                net::set_peer_address(id, s.ip_be, s.port_be);
                // Same public address as ours = probably the same router: also try its LAN address (the public one
                // needs NAT hairpinning). Different households behind one carrier-grade NAT share a public address
                // too; then the LAN SYNs go unanswered and the public path is kept.
                const std::string& mine = g_reported_ip.empty() ? g_your_ip : g_reported_ip;
                if (s.local_ip_be && !mine.empty() && ip_from(mine) == s.ip_be) {
                    net::set_peer_alt(id, s.ip_be, s.port_be, s.local_ip_be, s.local_port_be);
                    std::fprintf(stderr, "MATCHING2 member 0x%x shares our public address; LAN candidate %s:%u\n", id,
                                 net::stun::ip_str(s.local_ip_be).c_str(), ntohs(s.local_port_be));
                }
                const uint8_t* ip = reinterpret_cast<const uint8_t*>(&s.ip_be);
                std::fprintf(stderr, "MATCHING2 signaling Established: member 0x%x at %u.%u.%u.%u:%u\n", id, ip[0], ip[1],
                             ip[2], ip[3], ntohs(s.port_be));
                SigCb scb = g_sig_cb;
                void* sarg = g_sig_arg;
                uint64_t room = g_room;
                uint16_t mid = id;
                calls.push_back([=] { scb(kCtxId, room, mid, kSigEvEstablished, 0, sarg); });
            }
        }
    }
    for (auto& c : calls) c();
}

void presence_publish(const uint8_t* data, size_t n, const uint8_t* status, size_t status_n) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!data) n = 0;
    if (!status) status_n = 0;
    n = std::min<size_t>(n, 0x80), status_n = std::min<size_t>(status_n, 0xc0);
    while (status_n && !status[status_n - 1]) --status_n;  // trailing zeros carry nothing (the reader zero-fills)
    std::fprintf(stderr, "MATCHING2 SetPresence: %zu B game data, room id in blob %llu (our room %llu), world %u, "
                 "flags 0x%04x, host '%s', status %zu B%s\n", n, (unsigned long long)blob_room(data, n),
                 (unsigned long long)g_room, blob_world(data, n), blob_flags(data, n), blob_name(data, n).c_str(),
                 status_n, g_hello_ok ? "" : " (sent once the lobby answers hello)");
    g_pres_data = n ? net::hex_encode(data, n) : std::string();
    g_pres_status = status_n ? net::hex_encode(status, status_n) : std::string();
    flush_presence_locked();
}

void presence_clear() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::fprintf(stderr, "MATCHING2 presence cleared\n");
    g_pres_data.clear(), g_pres_status.clear();
    flush_presence_locked();
}

void presence_fetch() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_fl_busy) return;
    if (!g_hello_sent || g_lobby_down || np_identity().secret.empty()) {  // not online / no identity: no friends
        std::fprintf(stderr, "MATCHING2 GetPresence: %s, no friends\n",
                     np_identity().secret.empty() ? "no verified identity" : "lobby not connected");
        g_fl_result.clear();
        ++g_fl_gen;
        return;
    }
    g_fl_busy = true;  // hello may still be in flight: the lobby answers in order on one connection
    issue(Kind::FriendList, "friend_list", Json::object(), nullptr, 0);
}

bool presence_busy() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_fl_busy;
}

std::vector<PresenceFriend> presence_result(uint64_t* gen) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (gen) *gen = g_fl_gen;
    return g_fl_result;
}

void register_matching2() {
    const char* m = "libSceNpMatching2";
    REG(m, sceNpMatching2Initialize); REG(m, sceNpMatching2Terminate); REG(m, sceNpMatching2CreateContextA);
    REG(m, sceNpMatching2RegisterContextCallback); REG(m, sceNpMatching2ContextStart); REG(m, sceNpMatching2ContextStop);
    REG(m, sceNpMatching2DestroyContext); REG(m, sceNpMatching2GetServerId); REG(m, sceNpMatching2GetWorldInfoList);
    REG(m, sceNpMatching2SetUserInfo); REG(m, sceNpMatching2CreateJoinRoomA); REG(m, sceNpMatching2JoinRoomA);
    REG(m, sceNpMatching2LeaveRoom); REG(m, sceNpMatching2SearchRoom); REG(m, sceNpMatching2SetRoomDataInternal);
    REG(m, sceNpMatching2SetRoomDataExternal); REG(m, sceNpMatching2RegisterRoomEventCallback);
    REG(m, sceNpMatching2RegisterSignalingCallback); REG(m, sceNpMatching2SignalingGetLocalNetInfo);
}

}  // namespace dcp::hle
