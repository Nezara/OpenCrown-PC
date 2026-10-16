// PSN-side services the online path needs (notes/map/net.md 2.2, 2.5, 2.6, 5, 6): NpManager (always signed in, PS Plus,
// async requests complete at once), NetCtl (IP obtained), NpToolkit2 (all 55 private NIDs: success, empty results),
// NpCommerce / InGameMessage / WordFilter / Tus / Ssl / Http stubs. sceNpCheckCallback is the per-frame pump that also
// delivers every Matching2 callback (hle/matching2.cpp). Signatures from the call sites:
//   sceNpCreateAsyncRequest({u64 size 0x18, ..}) -> req id; sceNpPollAsync(req, int* result): 1 = running, 0 = done
//     (result -> NpHelper+0x88), else error (59bc00..59bc59)
//   sceNpCheckNpAvailabilityA(req, user) (59bce6); sceNpGetParentalControlInfoA(req, user, i8* age (+0x8f),
//     info* (+0x8c)) (59bd61..59bd74); sceNpCheckPlus(req, {u64 size 0x38, i32 user @8, u64 features @0x10 = 1},
//     result* (+0x90, byte 1 = authorized, read at c7440)) (59bdf5..59be22)
//   sceNpGetState(user, int*): 2 = signed in (c7637..c7649); sceNpGetNpId(user, SceNpId* 0x24 B);
//     sceNpGetAccountCountryA(user, char[4]); sceNpGetAccountIdA(user, u64*) (c7655..c767b)
//   sceNpRegisterStateCallbackA(cb, arg) -> id (59bec3); sceNetCtlRegisterCallback(cb, arg, int* id) (59beb8)
//   sceNetCtlGetState(int*): 3 = IP obtained = online (59bc90); sceNetCtlGetInfo(2, 6-byte ether addr) (59bbc6)
//   DcNetSignin state 0 waits for CheckPlus -> NpHelper+0x88 == 0 and +0x90 != 0 -> next state (c72b0..c7449); with
//     that the NpCommerce upsell (state 3..) is never opened.
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "common.h"
#include "net/identity.h"

namespace dcp::hle {

void matching2_pump();
void register_matching2();
void net_test_tick();  // hle/net_test.cpp ($DCP_NET_TEST only)

const NpIdentity& np_identity() {
    static NpIdentity id = [] {
        NpIdentity r;
        const char* n = std::getenv("DCP_NP_NAME");
        const std::string s = net::online_name(n ? n : "");  // PSN online id charset, same rule as the launcher
        r.online_id = s;
        net::Identity id = net::load_identity(net::identity_dir());
        if (id.ok) {
            r.account_id = id.account_id, r.secret = id.secret;
            std::fprintf(stderr, "NP identity %s: account %llu\n", id.path.c_str(), (unsigned long long)r.account_id);
            return r;
        }
        uint64_t h = 1469598103934665603ull;  // fallback, FNV-1a: stable per name (old builds' id; unverified)
        for (char c : s) h = (h ^ uint8_t(c)) * 1099511628211ull;
        r.account_id = h | 1;
        std::fprintf(stderr, "NP identity: %s; account %llu from the name (no friends)\n", id.error.c_str(),
                     (unsigned long long)r.account_id);
        return r;
    }();
    return id;
}

namespace {

template <class T> void wr(void* p, size_t off, T v) { std::memcpy(static_cast<uint8_t*>(p) + off, &v, sizeof v); }

// ---- libSceNpManager ----
std::atomic<int32_t> g_next_req{1};

DCP_SYSV int32_t sceNpSetNpTitleId(const void*, const void*) { return kOk; }
DCP_SYSV int32_t sceNpSetContentRestriction(const void*) { return kOk; }
DCP_SYSV int32_t sceNpCheckCallback() {
    net_test_tick();
    matching2_pump();
    return kOk;
}
DCP_SYSV int32_t sceNpRegisterStateCallbackA(void*, void*) { return 1; }  // never fires: the state never changes
DCP_SYSV int32_t sceNpUnregisterStateCallbackA(int32_t) { return kOk; }
DCP_SYSV int32_t sceNpCreateAsyncRequest(const void*) { return g_next_req++; }
DCP_SYSV int32_t sceNpPollAsync(int32_t req, int32_t* result) {
    DCP_HLE_LOG("sceNpPollAsync", "req %d -> done, result 0", req);
    if (result) *result = 0;
    return 0;  // finished
}
DCP_SYSV int32_t sceNpAbortRequest(int32_t) { return kOk; }
DCP_SYSV int32_t sceNpDeleteRequest(int32_t) { return kOk; }
DCP_SYSV int32_t sceNpCheckNpAvailabilityA(int32_t req, int32_t user) {
    std::fprintf(stderr, "HLE sceNpCheckNpAvailabilityA(req %d, user %d) -> available\n", req, user);
    return kOk;
}
DCP_SYSV int32_t sceNpGetParentalControlInfoA(int32_t req, int32_t user, int8_t* age, uint8_t* info) {
    std::fprintf(stderr, "HLE sceNpGetParentalControlInfoA(req %d, user %d) -> no restriction\n", req, user);
    if (age) *age = 20;
    if (info) std::memset(info, 0, 3);  // content / chat / UGC restriction flags all off
    return kOk;
}
DCP_SYSV int32_t sceNpCheckPlus(int32_t req, const void*, uint8_t* result) {
    std::fprintf(stderr, "HLE sceNpCheckPlus(req %d) -> authorized\n", req);
    if (result) result[0] = 1;
    return kOk;
}
DCP_SYSV int32_t sceNpGetState(int32_t user, int32_t* state) {
    DCP_HLE_LOG("sceNpGetState", "user %d -> signed in", user);
    if (state) *state = 2;  // SCE_NP_STATE_SIGNED_IN
    return kOk;
}
DCP_SYSV int32_t sceNpGetNpId(int32_t, uint8_t* npid) {
    if (!npid) return kErr(kEINVAL);
    std::memset(npid, 0, 0x24);  // SceNpId: SceNpOnlineId {char[16], term, pad[3]} + opt[8] + reserved[8]
    const std::string& n = np_identity().online_id;
    std::memcpy(npid, n.data(), std::min<size_t>(n.size(), 16));
    return kOk;
}
DCP_SYSV int32_t sceNpGetAccountCountryA(int32_t, char* cc) {
    if (cc) std::memcpy(cc, "us\0", 4);
    return kOk;
}
DCP_SYSV int32_t sceNpGetAccountIdA(int32_t, uint64_t* id) {
    if (id) *id = np_identity().account_id;
    return kOk;
}
DCP_SYSV int32_t sceNpRegisterPlusEventCallback(void*, void*) { return kOk; }
DCP_SYSV int32_t sceNpUnregisterPlusEventCallback() { return kOk; }
DCP_SYSV int32_t sceNpNotifyPlusFeature(const void*) { return kOk; }
// In-game messages (invites between players): accepted and dropped; nothing is ever received.
DCP_SYSV int32_t sceNpInGameMessageInitialize(uint64_t, const void*) { return 1; }
DCP_SYSV int32_t sceNpInGameMessageTerminate(int32_t) { return kOk; }
DCP_SYSV int32_t sceNpInGameMessageCreateHandle(int32_t) { return 1; }
DCP_SYSV int32_t sceNpInGameMessageDeleteHandle(int32_t, int32_t) { return kOk; }
DCP_SYSV int32_t sceNpInGameMessagePrepareA(int32_t, int32_t, const void*, void*, void*) { return kOk; }
DCP_SYSV int32_t sceNpInGameMessageSendDataA(int32_t, int32_t, const void*, const void*, const void*, uint64_t) {
    DCP_HLE_LOG("sceNpInGameMessageSendDataA", "dropped (no in-game messages)");
    return kOk;
}

// ---- libSceNetCtl ----
DCP_SYSV int32_t sceNetCtlGetState(int32_t* st) {
    if (st) *st = 3;  // SCE_NET_CTL_STATE_IPOBTAINED
    return kOk;
}
DCP_SYSV int32_t sceNetCtlGetInfo(int32_t code, uint8_t* info) {
    if (!info) return int32_t(0x80412104);
    if (code == 2) {  // ether address
        static const uint8_t mac[6] = {0x02, 0xdc, 0x50, 0x00, 0x00, 0x01};
        std::memcpy(info, mac, 6);
    } else {
        std::memset(info, 0, 16);
    }
    return kOk;
}
DCP_SYSV int32_t sceNetCtlRegisterCallback(void*, void*, int32_t* id) {
    if (id) *id = 1;
    return kOk;
}
DCP_SYSV int32_t sceNetCtlUnregisterCallback(int32_t) { return kOk; }
DCP_SYSV int32_t sceNetCtlCheckCallback() { return kOk; }

// ---- libSceNpCommerce (PS Plus upsell dialog; not reached while CheckPlus authorizes) ----
DCP_SYSV int32_t sceNpCommerceDialogInitialize() { return kOk; }
DCP_SYSV int32_t sceNpCommerceDialogOpen(const void*) {
    std::fprintf(stderr, "HLE sceNpCommerceDialogOpen (unexpected: CheckPlus said authorized)\n");
    return kOk;
}
DCP_SYSV int32_t sceNpCommerceDialogUpdateStatus() { return 3; }  // FINISHED
DCP_SYSV int32_t sceNpCommerceDialogGetResult(uint8_t* r) {
    if (r) r[4] = 1;  // the byte the game checks (c7473): "purchased / authorized"
    return kOk;
}
DCP_SYSV int32_t sceNpCommerceDialogTerminate() { return kOk; }

// ---- libSceNpUtility word filter: text passes unchanged ----
DCP_SYSV int32_t sceNpWordFilterCreateTitleCtxA(int32_t, const void*) { return 1; }
DCP_SYSV int32_t sceNpWordFilterDeleteTitleCtx(int32_t) { return kOk; }
DCP_SYSV int32_t sceNpWordFilterCreateAsyncRequest(int32_t, const void*) { return g_next_req++; }
DCP_SYSV int32_t sceNpWordFilterDeleteRequest(int32_t) { return kOk; }
DCP_SYSV int32_t sceNpWordFilterSanitizeComment(int32_t, const char* in, char* out, void*) {
    if (in && out) std::strcpy(out, in);
    return kOk;
}
DCP_SYSV int32_t sceNpWordFilterPollAsync(int32_t, int32_t* result) {
    if (result) *result = 0;
    return 0;
}

// ---- libSceNpTus (cross-save upload): contexts and requests succeed, async polls finish at once ----
DCP_SYSV int32_t sceNpTusCreateNpTitleCtxA(int32_t, int32_t) { return 1; }
DCP_SYSV int32_t sceNpTusCreateRequest(int32_t) { return g_next_req++; }
DCP_SYSV int32_t sceNpTusPollAsync(int32_t, int32_t* result) {
    if (result) *result = 0;
    return 0;
}

// ---- libSceRtc: sceRtcSetTick(SceRtcDateTime* out, const SceRtcTick* in), used by FUN_00572370 to turn a Matching2
// member joinDate into a date (u16 year, month, day, hour, minute, second, u32 microsecond; FUN_00572370 reads the
// words at +0 / +4 / +8). Tick = microseconds since 0001-01-01. ----
DCP_SYSV int32_t sceRtcSetTick(uint16_t* dt, const uint64_t* tick) {
    if (!dt || !tick) return int32_t(0x80b50002);
    uint64_t us = *tick;
    int64_t days = int64_t(us / 86400000000ull) - 719162;  // days since 1970-01-01
    uint64_t rem = us % 86400000000ull;
    days += 719468;  // civil_from_days (H. Hinnant)
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    uint64_t doe = uint64_t(days - era * 146097);
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint64_t mp = (5 * doy + 2) / 153;
    uint64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    int64_t y = int64_t(yoe) + era * 400 + (m <= 2);
    dt[0] = uint16_t(y), dt[1] = uint16_t(m), dt[2] = uint16_t(d);
    dt[3] = uint16_t(rem / 3600000000ull), dt[4] = uint16_t(rem / 60000000ull % 60), dt[5] = uint16_t(rem / 1000000ull % 60);
    uint32_t usec = uint32_t(rem % 1000000ull);
    std::memcpy(dt + 6, &usec, 4);
    return kOk;
}

// ---- Ssl / Http: initialised for Toolkit2, never used for requests (net.md 2.1) ----
DCP_SYSV int32_t sceSslInit(uint64_t) { return 1; }
DCP_SYSV int32_t sceSslTerm(int32_t) { return kOk; }
DCP_SYSV int32_t sceHttpInit(int32_t, int32_t, uint64_t) { return 1; }
DCP_SYSV int32_t sceHttpTerm(int32_t) { return kOk; }

// ---- libSceNpToolkit2: private C++ API (names unknown, roles in net.md 2.6). Everything succeeds at once: futures
// are never busy (mIinAn8TvdY), return code 0 (5-UHpg8s00I), typed results are empty containers (count fields at
// +0x10 / +0x38 read as 0 from a zero block: c6904, c6a62, c7b6a, c82d6). The event callback never fires.
// Presence (notes/map/friends.md 4.2 / 5) goes over the game's own lobby session (matching2.cpp, lobby_protocol.md 4.3):
//   SetPresence xCg+igDBo3Q(req, future) (bea20 beb54..bec07): status 0xc0 B @+0x18, game data @+0xe8, u64 size
//     @+0x168 (0x78) -> set_presence. Clear presence 0fPn7oELg8E (c7990, inferred) -> set_presence "".
//   GetPresence 7zsee5IHLis(req {u64 2 @+0x18}, future = Signin+0x15b8) (c7aaf..c7acb) -> friend_list; isBusy
//     mIinAn8TvdY(future) (c7aea) is 1 until the lobby answered; 4mdynkqoCFc(future) (c7b65..) returns the container
//     {+8 rows, +0x10 n} built below. The block list (AUW6at2PxHs, friends.md 2) stays empty. ----
alignas(16) uint8_t g_tk2_empty[0x4000];
DCP_SYSV uint64_t tk2_ok() { return 0; }
DCP_SYSV void* tk2_result() { return g_tk2_empty; }

template <class T> T rdp(const void* p, size_t off) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(p) + off, sizeof v);
    return v;
}

const void* g_pres_future = nullptr;  // the GetPresence future (Signin+0x15b8)
uint64_t g_pres_built = ~0ull;        // presence_result generation the container holds
alignas(16) uint8_t g_pres_container[0x20];
std::vector<uint8_t> g_pres_rows, g_pres_subs;  // 0x2e0-B rows / 0x1e0-B sub-entries, valid until the next fetch

// Presence container P (friends.md 5): one row per friend online in a room with a blob; one sub-entry each.
void build_presence_container() {
    uint64_t gen = 0;
    std::vector<PresenceFriend> fr = presence_result(&gen);
    if (gen == g_pres_built) return;
    g_pres_built = gen;
    g_pres_rows.assign(std::max<size_t>(fr.size(), 1) * 0x2e0, 0);
    g_pres_subs.assign(std::max<size_t>(fr.size(), 1) * 0x1e0, 0);
    for (size_t i = 0; i < fr.size(); ++i) {
        uint8_t* r = g_pres_rows.data() + i * 0x2e0;
        uint8_t* sub = g_pres_subs.data() + i * 0x1e0;
        wr<uint64_t>(r, 0, fr[i].account);
        std::memcpy(r + 8, fr[i].name.data(), std::min<size_t>(fr[i].name.size(), 16));  // online id, NUL padded to 0x14
        wr<uint32_t>(r, 0x2c8, 1);  // online -> row state 2
        wr<uint32_t>(r, 0x2cc, 2);  // presence type; sub +4 must match
        wr<uint8_t*>(r, 0x2d0, sub);
        wr<uint64_t>(r, 0x2d8, 1);
        wr<uint32_t>(sub, 4, 2);
        std::memcpy(sub + 0x98, fr[i].status.data(), std::min<size_t>(fr[i].status.size(), 0xc0));
        size_t n = std::min<size_t>(fr[i].data.size(), 0x80);
        std::memcpy(sub + 0x158, fr[i].data.data(), n);
        wr<uint64_t>(sub, 0x1d8, n);
    }
    std::memset(g_pres_container, 0, sizeof g_pres_container);
    wr<uint8_t*>(g_pres_container, 8, g_pres_rows.data());
    wr<uint64_t>(g_pres_container, 0x10, fr.size());
    std::fprintf(stderr, "HLE Toolkit2 GetPresence result: %zu row(s)\n", fr.size());
}

uint64_t signin_obj() {  // DcNetSignin = [game + 0xb8], game = DAT_00c17758 (getter FUN_00016b10)
    uint64_t base = config().eboot_base;
    uint64_t g = base ? rdp<uint64_t>(reinterpret_cast<const void*>(base + 0xc17758), 0) : 0;
    return g ? rdp<uint64_t>(reinterpret_cast<const void*>(g), 0xb8) : 0;
}

DCP_SYSV int32_t tk2_get_presence(const uint8_t* req, const void* future) {
    uint64_t si = signin_obj();
    // friends.md 7.1: the round filter of command 4 (Signin+0xa94, -1 = any round) decides which rooms are listed.
    std::fprintf(stderr, "HLE Toolkit2 GetPresence(req+0x18 = %llu, future %p): Signin+0xa94 filter = %d\n",
                 req ? (unsigned long long)rdp<uint64_t>(req, 0x18) : 0ull, future,
                 si ? rdp<int32_t>(reinterpret_cast<const void*>(si), 0xa94) : -999);
    g_pres_future = future;
    presence_fetch();
    return 0;
}
DCP_SYSV uint64_t tk2_is_busy(const void* future) {
    return future && future == g_pres_future && presence_busy() ? 1 : 0;
}
DCP_SYSV void* tk2_presence_result(const void* future) {
    if (!future || future != g_pres_future) return g_tk2_empty;
    build_presence_container();
    return g_pres_container;
}
DCP_SYSV int32_t tk2_set_presence(const uint8_t* req, const void*) {
    if (!req) return 0;
    presence_publish(req + 0xe8, size_t(rdp<uint64_t>(req, 0x168)), req + 0x18, 0xc0);
    return 0;
}
DCP_SYSV int32_t tk2_clear_presence(const void*, const void*) {
    presence_clear();
    return 0;
}

const char* const kTk2Results[] = {"aA83TCJI9Kw", "bxgy-tUd8YA", "AUW6at2PxHs"};
const char* const kTk2Other[] = {
    "RPVTcq8iqTg", "WCTsqoUEsTc", "UOVPAHM8ZHw", "mQf+iu0AG4M", "Y4+NqOIedOk", "VVWPUyolcGQ", "SLK65JLYrUE",
    "c6uPCbW4+lQ", "tlpXm5KLgqk", "30Zxe35LtBg", "H8cQuFAzgU4", "HeDIXZsguvA", "OylO3B2fCE0",
    "YeQtFtidPNw", "oyl98yuMwTs", "1bNH47yLJRs", "2783+UMEt+o", "5-UHpg8s00I", "kxzokJHZ2nY",
    "sZ8FuTrbhUc", "BNYc2zKvxyM", "7IHg-sW3Ez8", "gEvnF0rDgnA", "7Te1eeupXxQ", "qqkywbrhVtk",
    "gEoCCr0gCjQ", "AleDTt+SyDo", "PRGCDAuvO4U", "hzgYdvXB+IU", "vQhIcDhwZvg", "2w8SdVsst74",
    "XfdgmVGydYU", "K0gxiS+SBho", "LLdnqVnnNBQ", "IcuECtP3L0E", "vbh-MWtjMh4", "-bAjEe-hdCg", "s0u30pOfi9Q",
    "vycUuYWwilI", "Ht77qXUqN7U", "lWJNqXQ6+mI", "AwWC0Y+Shbg", "c9kMGltLpaQ", "dB9nauAWtgg", "7e6mE4oIQnU",
    "5H3GkWWQY5A", "cbJSAoeH-mo"};

}  // namespace

void register_np() {
    const char* n = "libSceNpManager";
    REG(n, sceNpSetNpTitleId); REG(n, sceNpSetContentRestriction); REG(n, sceNpCheckCallback);
    REG(n, sceNpRegisterStateCallbackA); REG(n, sceNpUnregisterStateCallbackA); REG(n, sceNpCreateAsyncRequest);
    REG(n, sceNpPollAsync); REG(n, sceNpAbortRequest); REG(n, sceNpDeleteRequest); REG(n, sceNpCheckNpAvailabilityA);
    REG(n, sceNpGetParentalControlInfoA); REG(n, sceNpCheckPlus); REG(n, sceNpGetState); REG(n, sceNpGetNpId);
    REG(n, sceNpGetAccountCountryA); REG(n, sceNpGetAccountIdA); REG(n, sceNpRegisterPlusEventCallback);
    REG(n, sceNpUnregisterPlusEventCallback); REG(n, sceNpNotifyPlusFeature); REG(n, sceNpInGameMessageInitialize);
    REG(n, sceNpInGameMessageTerminate); REG(n, sceNpInGameMessageCreateHandle); REG(n, sceNpInGameMessageDeleteHandle);
    REG(n, sceNpInGameMessagePrepareA); REG(n, sceNpInGameMessageSendDataA);
    const char* c = "libSceNetCtl";
    REG(c, sceNetCtlGetState); REG(c, sceNetCtlGetInfo); REG(c, sceNetCtlRegisterCallback);
    REG(c, sceNetCtlUnregisterCallback); REG(c, sceNetCtlCheckCallback);
    const char* m = "libSceNpCommerce";
    REG(m, sceNpCommerceDialogInitialize); REG(m, sceNpCommerceDialogOpen); REG(m, sceNpCommerceDialogUpdateStatus);
    REG(m, sceNpCommerceDialogGetResult); REG(m, sceNpCommerceDialogTerminate);
    const char* u = "libSceNpUtility";
    REG(u, sceNpWordFilterCreateTitleCtxA); REG(u, sceNpWordFilterDeleteTitleCtx); REG(u, sceNpWordFilterCreateAsyncRequest);
    REG(u, sceNpWordFilterDeleteRequest); REG(u, sceNpWordFilterSanitizeComment); REG(u, sceNpWordFilterPollAsync);
    const char* t = "libSceNpTus";
    REG(t, sceNpTusCreateNpTitleCtxA); REG(t, sceNpTusCreateRequest); REG(t, sceNpTusPollAsync);
    REG("libSceRtc", sceRtcSetTick);
    REG("libSceSsl", sceSslInit); REG("libSceSsl", sceSslTerm); REG("libSceHttp", sceHttpInit); REG("libSceHttp", sceHttpTerm);
    const char* k = "libSceNpToolkit2";
    loader::register_hle_nid(k, "7zsee5IHLis", reinterpret_cast<void*>(&tk2_get_presence));
    loader::register_hle_nid(k, "mIinAn8TvdY", reinterpret_cast<void*>(&tk2_is_busy));
    loader::register_hle_nid(k, "4mdynkqoCFc", reinterpret_cast<void*>(&tk2_presence_result));
    loader::register_hle_nid(k, "xCg+igDBo3Q", reinterpret_cast<void*>(&tk2_set_presence));
    loader::register_hle_nid(k, "0fPn7oELg8E", reinterpret_cast<void*>(&tk2_clear_presence));
    for (const char* nid : kTk2Results) loader::register_hle_nid("libSceNpToolkit2", nid, reinterpret_cast<void*>(&tk2_result));
    for (const char* nid : kTk2Other) loader::register_hle_nid("libSceNpToolkit2", nid, reinterpret_cast<void*>(&tk2_ok));
    register_matching2();
}

}  // namespace dcp::hle
