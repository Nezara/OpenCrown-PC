// SceNpTrophy (unlocks kept locally) and the network / PSN libraries around matching: SceNetCtl, SceNpManager,
// SceNpCommon, SceNpBasic, SceSsl, SceHttp, SceNpWebApi, SceNpMessage, SceNpTus, SceNpUtility, SceLibJson.
// Offline (default): never connected, never signed in. Online (hle/online.h, a lobby URL given): NetCtl connected
// (state 3), NpManager signed in (service state 3) with our identity's online id; the PSN web services (WebApi, Tus,
// Message, ...) stay offline either way. SceNet sockets + SceLibRudp: hle/net.cpp; SceNpMatching2: hle/matching2.cpp.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "hle/common.h"
#include "hle/io.h"
#include "hle/online.h"
#include "net/identity.h"
#include "net/stun.h"

namespace fs = std::filesystem;

namespace hle {
namespace {

// --- trophies ------------------------------------------------------------------------------------------------------
enum : uint32_t {
  TROPHY_ERROR_NOT_INITIALIZED = 0x80551601,
  TROPHY_ERROR_INVALID_ARGUMENT = 0x80551604,
  TROPHY_ERROR_INVALID_CONTEXT = 0x80551609,
  TROPHY_ERROR_INVALID_TROPHY_ID = 0x8055160E,
  TROPHY_ERROR_TROPHY_ALREADY_UNLOCKED = 0x8055160F,
};
constexpr int32_t INVALID_TROPHY_ID = -1;

std::mutex trophy_lock;
bool trophy_init = false;
uint32_t trophy_count = 0;
std::set<int32_t> unlocked;
const char* const TROPHY_FILE = "savedata0:/sce_sys/oc_trophies.txt";

// TROPHY.TRP (big-endian): magic 0xDCA24D00, entry count @0x10, entry size @0x14, entries from 0x40 with a
// 32-byte name. Each trophy has a TROPnnn.PNG icon, so their number is the trophy count (no decryption needed).
uint32_t count_trophies(const std::string& host) {
  std::ifstream f(host, std::ios::binary);
  std::string d((std::istreambuf_iterator<char>(f)), {});
  auto be32 = [&](size_t o) {
    return o + 4 <= d.size() ? uint32_t(uint8_t(d[o])) << 24 | uint32_t(uint8_t(d[o + 1])) << 16 |
                                   uint32_t(uint8_t(d[o + 2])) << 8 | uint8_t(d[o + 3])
                             : 0u;
  };
  if (be32(0) != 0xDCA24D00) return 0;
  uint32_t n = be32(0x10), size = be32(0x14), count = 0;
  for (uint32_t i = 0; i < n && 0x40 + size_t(i + 1) * size <= d.size(); i++) {
    std::string name = d.substr(0x40 + size_t(i) * size, 11);
    if (name.size() == 11 && name.compare(0, 4, "TROP") == 0 && name.compare(7, 4, ".PNG") == 0 &&
        isdigit(uint8_t(name[4])) && isdigit(uint8_t(name[5])) && isdigit(uint8_t(name[6])))
      count++;
  }
  return count;
}

void load_unlocked() {
  unlocked.clear();
  std::ifstream f(io_host_path(TROPHY_FILE, false));
  for (int32_t id; f >> id;) unlocked.insert(id);
}
void save_unlocked() {
  std::string path = io_host_path(TROPHY_FILE, true);
  if (path.empty()) return;
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  std::ofstream f(path, std::ios::trunc);
  for (int32_t id : unlocked) f << id << '\n';
}

void TrophyInit(Cpu& c) {
  std::lock_guard lk(trophy_lock);
  trophy_init = true;
  load_unlocked();
  ret(c, 0);
}
void TrophyTerm(Cpu& c) {
  std::lock_guard lk(trophy_lock);
  trophy_init = false;
  ret(c, 0);
}

// sceNpTrophyCreateContext(context*, const SceNpCommunicationId* {data[9], term, num}, signature*, u64 options)
void TrophyCreateContext(Cpu& c) {
  uint32_t out = arg(c, 0), comm = arg(c, 1);
  std::lock_guard lk(trophy_lock);
  if (!trophy_init) return ret(c, TROPHY_ERROR_NOT_INITIALIZED);
  if (!out || !comm) return ret(c, TROPHY_ERROR_INVALID_ARGUMENT);
  char dir[16];
  std::snprintf(dir, sizeof dir, "%.9s_%02u", str(c, comm, 9).c_str(), c.mem().read<uint8_t>(comm + 10));
  std::string trp = io_host_path(std::string("app0:sce_sys/trophy/") + dir + "/TROPHY.TRP", false);
  trophy_count = trp.empty() ? 0 : count_trophies(trp);
  HLE_NOTE("sceNpTrophyCreateContext(%s): %u trophies, %zu unlocked\n", dir, trophy_count, unlocked.size());
  c.mem().write<uint32_t>(out, 1);
  ret(c, 0);
}
void TrophyCreateHandle(Cpu& c) {
  if (!arg(c, 0)) return ret(c, TROPHY_ERROR_INVALID_ARGUMENT);
  c.mem().write<uint32_t>(arg(c, 0), 1);
  ret(c, 0);
}
void TrophyDestroy(Cpu& c) { ret(c, 0); }

// sceNpTrophyUnlockTrophy(context, handle, trophyId, platinumId*)
void TrophyUnlock(Cpu& c) {
  int32_t id = int32_t(arg(c, 2));
  uint32_t plat = arg(c, 3);
  std::lock_guard lk(trophy_lock);
  if (!trophy_init) return ret(c, TROPHY_ERROR_NOT_INITIALIZED);
  if (arg(c, 0) != 1) return ret(c, TROPHY_ERROR_INVALID_CONTEXT);
  if (id < 0 || (trophy_count && uint32_t(id) >= trophy_count)) return ret(c, TROPHY_ERROR_INVALID_TROPHY_ID);
  if (plat) c.mem().write<int32_t>(plat, INVALID_TROPHY_ID);
  if (!unlocked.insert(id).second) return ret(c, TROPHY_ERROR_TROPHY_ALREADY_UNLOCKED);
  save_unlocked();
  HLE_NOTE("[trophy] unlocked %d (%zu/%u)\n", id, unlocked.size(), trophy_count);
  ret(c, 0);
}

// sceNpTrophyGetTrophyUnlockState(context, handle, SceNpTrophyFlagArray* (128 bits), count*)
void TrophyGetUnlockState(Cpu& c) {
  uint32_t flags = arg(c, 2), count = arg(c, 3);
  std::lock_guard lk(trophy_lock);
  if (!trophy_init) return ret(c, TROPHY_ERROR_NOT_INITIALIZED);
  if (!flags || !count) return ret(c, TROPHY_ERROR_INVALID_ARGUMENT);
  uint32_t words[4] = {};
  for (int32_t id : unlocked)
    if (id < 128) words[id / 32] |= 1u << (id % 32);
  for (int i = 0; i < 4; i++) c.mem().write<uint32_t>(flags + 4 * i, words[i]);
  c.mem().write<uint32_t>(count, trophy_count);
  ret(c, 0);
}

// --- online mode -----------------------------------------------------------------------------------------------
std::string lobby_url, configured_name;  // set once by online_configure, before guest code runs

// --- NetCtl / NpManager / NpCommon / NpBasic ------------------------------------------------------------------
void ok(Cpu& c) { ret(c, 0); }
constexpr uint32_t NETCTL_ERROR_NOT_CONNECTED = 0x80412108;
constexpr uint32_t NP_ERROR_INVALID_ARGUMENT = 0x80550003;  // (value unverified)
constexpr uint32_t NP_CMP_NOT_EQUAL = 0x80550609;  // sceNpCmpNpId "different" (asm: the block-list loops go on with it)

// sceNetCtlInetGetState(int* state): 0 = DISCONNECTED offline, 3 = IP_OBTAINED online (the gate f813b803e wants 3).
void NetCtlInetGetState(Cpu& c) {
  if (arg(c, 0)) c.mem().write<uint32_t>(arg(c, 0), online_enabled() ? 3 : 0);
  ret(c, 0);
}
void NetCtlAdhocGetState(Cpu& c) {
  if (arg(c, 0)) c.mem().write<uint32_t>(arg(c, 0), 0);
  ret(c, 0);
}

// sceNetCtlInetGetInfo(code, SceNetCtlInfo* union): codes from vitasdk netctl.h. Online the game reads code 3
// (ether address, f813b8352) when it goes online and the rest on its network-information screen (f813b2260). Each
// answer writes only its own field; an unknown code leaves the buffer alone.
void NetCtlInetGetInfo(Cpu& c) {
  if (!online_enabled()) return ret(c, NETCTL_ERROR_NOT_CONNECTED);
  uint32_t code = arg(c, 0), info = arg(c, 1);
  Memory& m = c.mem();
  if (!info || !m.valid(info, 4)) return ret(c, 0x80412104);  // SCE_NET_CTL_ERROR_INVALID_ADDR (unverified)
  auto put_str = [&](const std::string& v, uint32_t field) {
    if (!m.valid(info, field)) return;
    std::memset(m.ptr(info), 0, field);
    std::memcpy(m.ptr(info), v.data(), std::min<size_t>(v.size(), field - 1));
  };
  uint32_t ip = dcp::net::stun::default_route_ip();
  if (!ip) ip = 0x0100007f;  // 127.0.0.1 (network order)
  switch (code) {
    case 1: put_str("OpenCrown", 65); break;  // CNF_NAME
    case 3:                                   // ETHER_ADDR: locally administered, from the account id
    case 6: {                                 // BSSID
      uint64_t a = np_identity().account_id;
      uint8_t mac[6] = {0x02, uint8_t(a >> 8), uint8_t(a >> 16), uint8_t(a >> 24), uint8_t(a >> 32), uint8_t(a >> 40)};
      if (m.valid(info, 6)) std::memcpy(m.ptr(info), mac, 6);
      break;
    }
    case 4: m.write<uint32_t>(info, 1500); break;   // MTU
    case 5: m.write<uint32_t>(info, 1); break;      // LINK: up
    case 7: put_str("", 33); break;                 // SSID
    case 10: m.write<uint32_t>(info, 100); break;   // RSSI_PERCENTAGE
    case 2: case 8: case 9: case 11: case 12: case 20: case 22:  // DEVICE wired, security, RSSI dBm, channel,
      m.write<uint32_t>(info, 0);                                // IP_CONFIG DHCP, proxy off / port
      break;
    case 13: case 21: put_str("", 256); break;  // DHCP_HOSTNAME, HTTP_PROXY_SERVER
    case 14: put_str("", 128); break;           // PPPOE_AUTH_NAME
    case 15: put_str(dcp::net::stun::ip_str(ip), 16); break;  // IP_ADDRESS
    case 16: put_str("255.255.255.0", 16); break;             // NETMASK
    case 17: case 18:                                         // DEFAULT_ROUTE, PRIMARY_DNS: x.x.x.1
      put_str(dcp::net::stun::ip_str((ip & 0x00ffffffu) | 0x01000000u), 16);
      break;
    case 19: put_str("0.0.0.0", 16); break;  // SECONDARY_DNS
    default: HLE_NOTE("sceNetCtlInetGetInfo(%u): unknown code, nothing written\n", code); break;
  }
  ret(c, 0);
}
// sceNetCtlGetNatInfo(SceNetCtlNatInfo* {size, stun_status, nat_type, mapped_addr}): network-information screen only.
void NetCtlGetNatInfo(Cpu& c) {
  if (!online_enabled()) return ret(c, NETCTL_ERROR_NOT_CONNECTED);
  uint32_t p = arg(c, 0);
  if (p && c.mem().valid(p, 16)) {
    c.mem().write<uint32_t>(p + 4, 1);  // STUN done
    c.mem().write<uint32_t>(p + 8, 2);  // NAT type 2
    c.mem().write<uint32_t>(p + 12, 0);
  }
  ret(c, 0);
}
// sceNetCtl*RegisterCallback(func, arg, int* cid). The handlers never run: the connection state never changes (the
// online Inet handler 0x813b7bcf would take any event for a disconnect).
void NetCtlRegisterCallback(Cpu& c) {
  if (arg(c, 2)) c.mem().write<uint32_t>(arg(c, 2), 1);
  ret(c, 0);
}
// Ad-hoc (local wireless) is not provided: never connected.
void NetCtlAdhocNotConnected(Cpu& c) { ret(c, NETCTL_ERROR_NOT_CONNECTED); }

// sceNpRegisterServiceStateCallback(cb, arg) (boot: 0x813b28cb posts {type 2, state} to the game's event queue).
// Online, the player signs in right after start-up as on a console that is connected: the next sceNpCheckCallback
// (a game thread) reports ONLINE (3) once to each registration. Offline the state never changes, so it never runs.
std::mutex np_cb_lock;
struct ServiceStateCb {
  uint32_t fn = 0, arg = 0;
  bool told = false;
};
std::vector<ServiceStateCb> service_state_cbs;
void NpRegisterServiceStateCallback(Cpu& c) {
  std::lock_guard lk(np_cb_lock);
  service_state_cbs.push_back({arg(c, 0), arg(c, 1), false});
  ret(c, 0);
}
void NpUnregisterServiceStateCallback(Cpu& c) {
  std::lock_guard lk(np_cb_lock);
  service_state_cbs.clear();
  ret(c, 0);
}
void NpCheckCallback(Cpu& c) {
  if (online_enabled()) {
    std::vector<ServiceStateCb> due;
    {
      std::lock_guard lk(np_cb_lock);
      for (ServiceStateCb& s : service_state_cbs)
        if (!s.told && s.fn) due.push_back(s), s.told = true;
    }
    for (const ServiceStateCb& s : due) {
      HLE_NOTE("sceNpCheckCallback: service state ONLINE -> 0x%08x\n", s.fn);
      c.call(s.fn, {3, s.arg});
      if (!c.fault().empty()) return;
    }
  }
  ret(c, 0);
}
// sceNpGetServiceState(SceNpServiceState*): 1 = SIGNED_OUT offline, 3 = ONLINE (f810767a6 needs 3 to go online).
void NpGetServiceState(Cpu& c) {
  if (arg(c, 0)) c.mem().write<uint32_t>(arg(c, 0), online_enabled() ? 3 : 1);
  ret(c, 0);
}
// sceNpManagerGetNpId(SceNpId*): our own id, built like every lobby member's (the game asks once signed in; the
// matching context gets it too).
void NpManagerGetNpId(Cpu& c) {
  uint32_t p = arg(c, 0);
  if (!p || !c.mem().valid(p, NP_ID_SIZE)) return ret(c, NP_ERROR_INVALID_ARGUMENT);
  make_np_id(c.mem().ptr(p), np_identity().online_id, np_identity().account_id);
  ret(c, 0);
}
// sceNpManagerGetAccountRegion(SceNpCountryCode* {char data[2], term, pad}, int* language): "us", English (1).
void NpManagerGetAccountRegion(Cpu& c) {
  if (uint32_t p = arg(c, 0); p && c.mem().valid(p, 4)) std::memcpy(c.mem().ptr(p), "us\0", 4);
  if (arg(c, 1)) c.mem().write<uint32_t>(arg(c, 1), 1);
  ret(c, 0);
}
// sceNpManagerGetChatRestrictionFlag(int*): 0 = not restricted (f813a5004 shows message 0 otherwise).
void NpManagerGetChatRestrictionFlag(Cpu& c) {
  if (arg(c, 0)) c.mem().write<uint32_t>(arg(c, 0), 0);
  ret(c, 0);
}
// sceNpManagerGetContentRatingFlag(int* isRestricted, int* age): not restricted (f813a50ee only fails a restricted
// account under 13).
void NpManagerGetContentRatingFlag(Cpu& c) {
  if (arg(c, 0)) c.mem().write<uint32_t>(arg(c, 0), 0);
  if (arg(c, 1)) c.mem().write<uint32_t>(arg(c, 1), 0);
  ret(c, 0);
}
// sceNpCmpNpId(a, b): 0 when equal, else NP_CMP_NOT_EQUAL. Ours carry the lobby account id (make_np_id), so equal =
// same account (names can collide); two ids without one (zeroed records) compare by the 16-char handle.
void NpCmpNpId(Cpu& c) {
  uint32_t a = arg(c, 0), b = arg(c, 1);
  Memory& m = c.mem();
  if (!a || !b || !m.valid(a, NP_ID_SIZE) || !m.valid(b, NP_ID_SIZE)) return ret(c, NP_ERROR_INVALID_ARGUMENT);
  uint64_t ka = np_id_account(m.ptr(a)), kb = np_id_account(m.ptr(b));
  bool same = ka || kb ? ka == kb
                       : std::strncmp(reinterpret_cast<const char*>(m.ptr(a)), reinterpret_cast<const char*>(m.ptr(b)),
                                      16) == 0;
  ret(c, same ? 0u : NP_CMP_NOT_EQUAL);
}
// sceNpBasicGetPlaySessionLogSize(type, u32* count) (f81076f54, f813b0b34): the "players met" log stays empty, so
// sceNpBasicGetPlaySessionLog(type, index, buf) is never asked for an entry.
void NpBasicGetPlaySessionLogSize(Cpu& c) {
  if (arg(c, 1)) c.mem().write<uint32_t>(arg(c, 1), 0);
  ret(c, 0);
}
void NpBasicGetPlaySessionLog(Cpu& c) { ret(c, 0x80020001); }  // SCE_KERNEL_ERROR_ERROR
// sceNpBasicSendInGameDataMessage (invitations): nothing carries them yet; reported as sent.
void NpBasicSendInGameDataMessage(Cpu& c) {
  HLE_NOTE("sceNpBasicSendInGameDataMessage: dropped (no invitations over the lobby yet)\n");
  ret(c, 0);
}

// --- online services of 1.09 (WebApi, Message, Tus, Utility, Json): never signed in ----------------------------
// Set-up calls succeed (context / handle / callback ids are 1) so the game's boot path runs; anything that would
// reach PSN fails with a generic error (the libraries' own offline codes are not known; SCE_KERNEL_ERROR_ERROR).
constexpr uint32_t OFFLINE = 0x80020001;
void one(Cpu& c) { ret(c, 1); }
void offline(Cpu& c) { ret(c, OFFLINE); }
void this_(Cpu& c) { ret(c, arg(c, 0)); }
void none(Cpu&) {}
// Dialog status SCE_COMMON_DIALOG_STATUS_NONE; also the 0 / null answer of the Json accessors.
void zero_status(Cpu& c) { ret(c, 0); }

// sceNpWebApiCreateRequest(const char* apiGroup, const char* path, method, const void* content, int64_t* requestId)
// (1.09 0x813ae2ce / 0x813ae2f8). Only the block list and friend list GETs are answered: the block list
// (/v1/users/<id>/blockList?&limit=100&offset=0, built at 0x813b4380) is fetched at online start and before every
// JoinRoom (DcNetMatching mode 5 step 3, 0x8139ef68); a failure latches DcNetMatching+0x7e8 / DcNet+0x18c = -1 and
// the finished join then shows "Network Error: unknown Error" (text 0x37, notes/map/vita_join_109.md). HTTP 200 with
// an empty body: the stub Json leaves the value type 0, so both parsers (block list 0x813b4406, friends 0x813b3b12)
// take their "not an object" path and return 0 with an empty list.
constexpr uint32_t WEBAPI_FRIENDS_REQ = 0x46524e44;  // any id > 0 (DeleteRequest is called only for ids > 0)
void WebApiCreateRequest(Cpu& c) {
  const std::string group = str(c, arg(c, 0), 0x40), path = str(c, arg(c, 1), 0x680);
  const uint32_t out = arg(c, 4);
  const bool list = path.find("/blockList?") != std::string::npos || path.find("/friendList?") != std::string::npos;
  HLE_NOTE("sceNpWebApiCreateRequest(%s, %s, method %u)%s\n", group.c_str(), path.c_str(), arg(c, 2),
           list ? " -> empty list" : " -> offline");
  if (group != "userProfile" || !list || !out) return ret(c, OFFLINE);
  c.mem().write<uint32_t>(out, WEBAPI_FRIENDS_REQ);
  c.mem().write<uint32_t>(out + 4, 0);
  ret(c, 0);
}
// sceNpWebApiSendRequest2(int64_t requestId, const void* data, size_t size, ResponseInformationOption* info)
void WebApiSendRequest2(Cpu& c) {
  if (arg(c, 0) != WEBAPI_FRIENDS_REQ || arg(c, 1) != 0) return ret(c, OFFLINE);
  if (uint32_t info = arg(c, 4)) c.mem().write<uint32_t>(info, 200);  // +0 httpStatus
  ret(c, 0);
}
// sceNpWebApiReadData(int64_t requestId, void* buf, size_t size): empty body, 0 = end of data.
void WebApiReadData(Cpu& c) { ret(c, arg(c, 0) == WEBAPI_FRIENDS_REQ && arg(c, 1) == 0 ? 0 : OFFLINE); }

// sceNpWordFilterSanitizeCommentAsync(reqId, const char* comment, char* sanitized, option): no PSN filter, the
// comment comes back unchanged. The caller (online start, 1.09 0x8107716c) zeroes a 0x401-byte out buffer
// first and fails online with -100 on a negative return.
void WordFilterSanitize(Cpu& c) {
  const std::string s = str(c, arg(c, 1), 0x400);
  const uint32_t out = arg(c, 2);
  for (size_t i = 0; i < s.size(); i++) c.mem().write<uint8_t>(out + uint32_t(i), uint8_t(s[i]));
  c.mem().write<uint8_t>(out + uint32_t(s.size()), 0);
  ret(c, 0);
}
// sceNpWordFilterPollAsync(reqId, int* result): 0 = finished (1 = still running, which the caller re-polls).
void WordFilterPoll(Cpu& c) {
  if (arg(c, 1)) c.mem().write<uint32_t>(arg(c, 1), 0);
  ret(c, 0);
}

}  // namespace

void online_configure(const std::string& url, const std::string& name) {
  lobby_url = url;
  configured_name = name;
  if (url.empty()) return;
  const NpIdentity& id = np_identity();
  HLE_NOTE("online: lobby %s as %s (account %llu%s)\n", url.c_str(), id.online_id.c_str(),
           (unsigned long long)id.account_id, id.secret.empty() ? ", unverified" : "");
}
bool online_enabled() { return !lobby_url.empty(); }
const std::string& online_lobby_url() { return lobby_url; }

const NpIdentity& np_identity() {
  static const NpIdentity id = [] {
    NpIdentity r;
    r.online_id = dcp::net::online_name(configured_name);  // PSN charset, same rule as the launcher
    dcp::net::Identity f = dcp::net::load_identity(dcp::net::identity_dir());
    if (f.ok) {
      r.account_id = f.account_id, r.secret = f.secret;
      return r;
    }
    uint64_t h = 1469598103934665603ull;  // FNV-1a of the name: unverified account (no friends), as port/'s fallback
    for (char ch : r.online_id) h = (h ^ uint8_t(ch)) * 1099511628211ull;
    r.account_id = h | 1;
    HLE_NOTE("online identity: %s; account from the name\n", f.error.c_str());
    return r;
  }();
  return id;
}

void make_np_id(uint8_t* out, const std::string& online_id, uint64_t account_id) {
  std::memset(out, 0, NP_ID_SIZE);
  std::memcpy(out, online_id.data(), std::min<size_t>(online_id.size(), 16));
  for (int i = 0; i < 8; i++) out[NP_ID_ACCOUNT + i] = uint8_t(account_id >> (8 * i));
}
uint64_t np_id_account(const uint8_t* np_id) {
  uint64_t a = 0;
  for (int i = 0; i < 8; i++) a |= uint64_t(np_id[NP_ID_ACCOUNT + i]) << (8 * i);
  return a;
}

void register_np() {
  REG(sceNpCheckCallback, NpCheckCallback);
  REG(sceNpBasicRegisterInGameDataMessageHandler, ok);
  REG(sceNpBasicUnregisterInGameDataMessageHandler, ok);
  REG(sceNetCtlGetNatInfo, NetCtlGetNatInfo);
  REG(sceNetCtlInetGetResult, ok);

  REG(sceNpWebApiInitialize, one);
  REG(sceNpWebApiTerminate, ok);
  REG(sceNpWebApiSetNpTitleId, ok);
  REG(sceNpWebApiCheckCallback, ok);
  REG(sceNpWebApiCreateHandle, one);
  REG(sceNpWebApiAbortRequest, ok);
  REG(sceNpWebApiDeleteRequest, ok);
  REG(sceNpWebApiCreateRequest, WebApiCreateRequest);
  REG(sceNpWebApiSendRequest2, WebApiSendRequest2);
  REG(sceNpWebApiReadData, WebApiReadData);
  REG(sceNpWebApiGetHttpResponseHeaderValue, offline);
  REG(sceNpWebApiGetHttpResponseHeaderValueLength, offline);
  REG(sceNpWebApiCreatePushEventFilter, one);
  REG(sceNpWebApiCreateServicePushEventFilter, one);
  REG(sceNpWebApiDeletePushEventFilter, ok);
  REG(sceNpWebApiRegisterPushEventCallback, one);
  REG(sceNpWebApiRegisterServicePushEventCallback, one);
  REG(sceNpWebApiUnregisterPushEventCallback, ok);
  REG(sceNpWebApiUnregisterServicePushEventCallback, ok);
  REG(sceNpWebApiUtilityParseNpId, offline);

  REG(sceNpMessageInit, ok);
  REG(sceNpMessageTerm, ok);
  REG(sceNpMessageSyncMessage, offline);
  REG(sceNpMessageGetAttachedData, offline);
  REG(sceNpMessageSetAttachedDataUsedFlag, offline);
  REG(sceNpMessageDialogGetStatus, zero_status);
  REG(sceNpMessageDialogGetResult, offline);
  REG(sceNpMessageDialogAbort, offline);
  REG(sceNpMessageDialogTerm, offline);

  // Called from a task switch with (1, 0x10000100, 0, 0) when the town map opens; the return value is not read
  // (1.09 0x813b5860).
  REG(sceNpLookupInit, ok);
  REG(sceNpLookupTerm, ok);

  // TUS (cloud slots: the periodic upload 0x813a4bd0, cross-save 0x814f7d10): init is init step 4 of the online
  // start (0 or 0x80550701 = already initialised accepted, 1.09 0x813af22a); a title context fails, so both users
  // take their error / clean-up path, which deletes only ids > 0.
  REG(sceNpTusInit, ok);
  REG(sceNpTusTerm, ok);
  REG(sceNpTusCreateTitleCtx, offline);
  REG(sceNpTusDeleteTitleCtx, ok);
  REG(sceNpTusCreateRequest, offline);
  REG(sceNpTusDeleteRequest, ok);
  REG(sceNpTusAbortRequest, ok);
  REG(sceNpTusChangeModeForOtherSaveDataOwners, offline);
  REG(sceNpTusGetData, offline);
  REG(sceNpTusSetData, offline);
  REG(sceNpTusSetDataAsync, offline);
  REG(sceNpTusPollAsync, offline);

  // Word filter: comments pass unchanged (WordFilterSanitize).
  REG(sceNpWordFilterInit, ok);
  REG(sceNpWordFilterTerm, ok);
  REG(sceNpWordFilterCreateTitleCtx, one);
  REG(sceNpWordFilterDeleteTitleCtx, ok);
  REG(sceNpWordFilterCreateRequest, one);
  REG(sceNpWordFilterDeleteRequest, ok);
  REG(sceNpWordFilterSanitizeCommentAsync, WordFilterSanitize);
  REG(sceNpWordFilterPollAsync, WordFilterPoll);
  REG(sceNpBandwidthTestInitStart, offline);
  REG(sceNpBandwidthTestGetStatus, offline);
  REG(sceNpBandwidthTestAbort, ok);
  REG(sceNpBandwidthTestShutdown, offline);

  // SceLibJson: only used on WebApi responses, which never arrive. Constructors / destructors return `this`.
  REG(_ZN3sce4Json11InitializerC1Ev, this_);
  REG(_ZN3sce4Json11InitializerD1Ev, this_);
  REG(_ZN3sce4Json11Initializer10initializeEPKNS0_13InitParameterE, ok);
  REG(_ZN3sce4Json11Initializer9terminateEv, ok);
  REG(_ZN3sce4Json12MemAllocatorC2Ev, this_);
  REG(_ZN3sce4Json12MemAllocatorD2Ev, this_);
  REG(_ZN3sce4Json12MemAllocator11notifyErrorEijPv, none);
  REG(_ZN3sce4Json5ValueC1Ev, this_);
  REG(_ZN3sce4Json5ValueC1ERKNS0_6ObjectE, this_);
  REG(_ZN3sce4Json5ValueD1Ev, this_);
  REG(_ZN3sce4Json5Value5clearEv, none);
  REG(_ZN3sce4Json5ValueaSERKS1_, this_);
  REG(_ZN3sce4Json6StringC1Ev, this_);
  REG(_ZN3sce4Json6StringC1EPKc, this_);
  REG(_ZN3sce4Json6StringD1Ev, this_);
  REG(_ZN3sce4Json6StringaSERKS1_, this_);
  REG(_ZN3sce4Json6ObjectC1ERKS1_, this_);
  REG(_ZN3sce4Json6ObjectD1Ev, this_);
  REG(_ZN3sce4Json6Parser5parseERNS0_5ValueEPKcj, offline);
  REG(_ZN3sce4Json5Array8iteratorD1Ev, this_);
  REG(_ZN3sce4Json5Array8iteratorppEv, this_);
  REG(_ZN3sce4Json6Object8iteratorD1Ev, this_);
  REG(_ZN3sce4Json6Object8iteratorppEv, this_);
  // Accessors: never reached without a parsed document; 0 / null.
  REG(_ZN3sce4Json6ObjectixERKNS0_6StringE, zero_status);
  REG(_ZNK3sce4Json5Array3endEv, zero_status);
  REG(_ZNK3sce4Json5Array4sizeEv, zero_status);
  REG(_ZNK3sce4Json5Array5beginEv, zero_status);
  REG(_ZNK3sce4Json5Array8iteratordeEv, zero_status);
  REG(_ZNK3sce4Json5Array8iteratorneES2_, zero_status);
  REG(_ZNK3sce4Json5Array8iteratorptEv, zero_status);
  REG(_ZNK3sce4Json5Value10getIntegerEv, zero_status);
  REG(_ZNK3sce4Json5Value7getTypeEv, zero_status);
  REG(_ZNK3sce4Json5Value8getArrayEv, zero_status);
  REG(_ZNK3sce4Json5Value9getObjectEv, zero_status);
  REG(_ZNK3sce4Json5Value9getStringEv, zero_status);
  REG(_ZNK3sce4Json5ValueixEPKc, zero_status);
  REG(_ZNK3sce4Json6Object3endEv, zero_status);
  REG(_ZNK3sce4Json6Object5beginEv, zero_status);
  REG(_ZNK3sce4Json6Object8iteratorneES2_, zero_status);
  REG(_ZNK3sce4Json6Object8iteratorptEv, zero_status);
  REG(_ZNK3sce4Json6String4sizeEv, zero_status);
  REG(_ZNK3sce4Json6String5c_strEv, zero_status);
  REG(_ZNK3sce4Json6String6lengthEv, zero_status);
  REG(_ZNK3sce4Json6String7compareEPKc, zero_status);
  // Invitation / game custom data (PSN messages) dialogs: refused offline.
  REG(sceInvitationDialogInit, offline);
  REG(sceInvitationDialogGetStatus, zero_status);
  REG(sceInvitationDialogGetResult, offline);
  REG(sceInvitationDialogTerm, offline);
  REG(sceGameCustomDataDialogInit, offline);
  REG(sceGameCustomDataDialogGetStatus, zero_status);
  REG(sceGameCustomDataDialogGetResult, offline);
  REG(sceGameCustomDataDialogTerm, offline);

  REG(sceNpTrophyInit, TrophyInit);
  REG(sceNpTrophyTerm, TrophyTerm);
  REG(sceNpTrophyCreateContext, TrophyCreateContext);
  REG(sceNpTrophyDestroyContext, TrophyDestroy);
  REG(sceNpTrophyCreateHandle, TrophyCreateHandle);
  REG(sceNpTrophyDestroyHandle, TrophyDestroy);
  REG(sceNpTrophyUnlockTrophy, TrophyUnlock);
  REG(sceNpTrophyGetTrophyUnlockState, TrophyGetUnlockState);

  REG(sceNetCtlInit, ok);
  REG(sceNetCtlTerm, ok);
  REG(sceNetCtlCheckCallback, ok);
  REG(sceNetCtlInetGetState, NetCtlInetGetState);
  REG(sceNetCtlAdhocGetState, NetCtlAdhocGetState);
  REG(sceNetCtlInetGetInfo, NetCtlInetGetInfo);
  REG(sceNetCtlInetRegisterCallback, NetCtlRegisterCallback);
  REG(sceNetCtlInetUnregisterCallback, ok);
  REG(sceNetCtlAdhocRegisterCallback, NetCtlRegisterCallback);
  REG(sceNetCtlAdhocUnregisterCallback, ok);
  REG(sceNetCtlAdhocGetInAddr, NetCtlAdhocNotConnected);
  REG(sceNetCtlAdhocDisconnect, NetCtlAdhocNotConnected);
  REG(sceNpInit, ok);
  REG(sceNpTerm, ok);
  REG(sceNpGetServiceState, NpGetServiceState);
  REG(sceNpManagerGetNpId, NpManagerGetNpId);
  REG(sceNpManagerGetAccountRegion, NpManagerGetAccountRegion);
  REG(sceNpManagerGetChatRestrictionFlag, NpManagerGetChatRestrictionFlag);
  REG(sceNpManagerGetContentRatingFlag, NpManagerGetContentRatingFlag);
  REG(sceNpCmpNpId, NpCmpNpId);
  REG(sceNpSetPlatformType, ok);
  REG(sceNpRegisterServiceStateCallback, NpRegisterServiceStateCallback);
  REG(sceNpUnregisterServiceStateCallback, NpUnregisterServiceStateCallback);
  REG(sceNpAuthInit, ok);
  REG(sceNpAuthTerm, ok);
  REG(sceNpBasicInit, ok);
  REG(sceNpBasicTerm, ok);
  REG(sceNpBasicCheckCallback, ok);
  REG(sceNpBasicRecordPlaySessionLog, ok);
  REG(sceNpBasicGetPlaySessionLogSize, NpBasicGetPlaySessionLogSize);
  REG(sceNpBasicGetPlaySessionLog, NpBasicGetPlaySessionLog);
  REG(sceNpBasicSendInGameDataMessage, NpBasicSendInGameDataMessage);
  REG(sceSslInit, ok);
  REG(sceSslTerm, ok);
  REG(sceHttpInit, ok);
  REG(sceHttpTerm, ok);
}

}  // namespace hle
