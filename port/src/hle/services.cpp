// System-service stubs reached before the main loop. Each returns the most plausible "works, nothing happening"
// answer and logs its first call. Values marked guess are not verified against the game's checks.
#include <atomic>
#include <mutex>
#include <cstring>

#include "common.h"
#include "input.h"

namespace dcp::hle {
namespace {

// ---- libSceSysmodule: every module "loads" (the code they would provide is either in libc.prx or shimmed) ----
DCP_SYSV int32_t sceSysmoduleLoadModule(uint16_t id) {
    std::fprintf(stderr, "HLE sceSysmoduleLoadModule(0x%x) -> ok\n", id);
    return kOk;
}
DCP_SYSV int32_t sceSysmoduleUnloadModule(uint16_t) { return kOk; }
DCP_SYSV int32_t sceSysmoduleIsLoaded(uint16_t) { return kOk; }

// ---- libSceUserService: local players 1000..1003; GetEvent reports login/logout as pad_present() changes (the game
// opens/closes a pad slot per event, notes/map/input_save.md 2.2). User 1000 is always signed in. ----
DCP_SYSV int32_t sceUserServiceInitialize(const void*) { DCP_HLE_LOG("sceUserServiceInitialize", "ok"); return kOk; }
DCP_SYSV int32_t sceUserServiceTerminate() { return kOk; }
DCP_SYSV int32_t sceUserServiceGetInitialUser(int32_t* id) {
    DCP_HLE_LOG("sceUserServiceGetInitialUser", "-> %d", kUserId);
    return *id = kUserId, kOk;
}
std::mutex g_users_mu;
bool g_logged[4] = {};
DCP_SYSV int32_t sceUserServiceGetEvent(int32_t* ev) {  // {u32 eventType (0 = LOGIN, 1 = LOGOUT), i32 userId}
    if (!ev) return int32_t(0x80960009);
    std::lock_guard<std::mutex> lk(g_users_mu);
    for (int i = 0; i < 4; ++i) {
        bool p = pad_present(i);
        if (p == g_logged[i]) continue;
        g_logged[i] = p;
        ev[0] = p ? 0 : 1, ev[1] = kUserId + i;
        std::fprintf(stderr, "HLE sceUserServiceGetEvent: %s user %d\n", p ? "LOGIN" : "LOGOUT", kUserId + i);
        return kOk;
    }
    return int32_t(0x80960007);  // SCE_USER_SERVICE_ERROR_NO_EVENT
}
DCP_SYSV int32_t sceUserServiceGetLoginUserIdList(int32_t* list) {  // 4 slots, -1 = empty
    if (!list) return int32_t(0x80960009);
    std::lock_guard<std::mutex> lk(g_users_mu);
    for (int i = 0; i < 4; ++i) list[i] = g_logged[i] || i == 0 ? kUserId + i : -1;
    return kOk;
}

// ---- libSceSystemService ----
DCP_SYSV int32_t sceSystemServiceParamGetInt(int32_t id, int32_t* v) {
    if (!v) return int32_t(0x80A10003);
    int32_t r = 0;
    switch (id) {
    case 1: r = 1; break;     // LANG: English (US)
    case 2: r = 1; break;     // DATE_FORMAT: mm/dd/yyyy
    case 3: r = 0; break;     // TIME_FORMAT: 12h
    case 4: r = 0; break;     // TIME_ZONE (minutes)
    case 5: r = 0; break;     // SUMMERTIME
    case 7: r = 0; break;     // GAME_PARENTAL_LEVEL: off
    case 1000: r = 1; break;  // ENTER_BUTTON_ASSIGN: cross (western)
    default: break;
    }
    DCP_HLE_LOG("sceSystemServiceParamGetInt", "id %d -> %d", id, r);
    return *v = r, kOk;
}
DCP_SYSV int32_t sceSystemServiceGetStatus(uint8_t* st) {  // SceSystemServiceStatus: eventNum + flags, 0x84 B
    if (st) std::memset(st, 0, 0x84);
    return kOk;
}
DCP_SYSV int32_t sceSystemServiceGetDisplaySafeAreaInfo(uint8_t* info) {  // {float ratio; u8 reserved[128]}
    if (!info) return int32_t(0x80A10003);
    float ratio = 1.0f;
    std::memset(info, 0, 0x84);
    std::memcpy(info, &ratio, 4);
    return kOk;
}
DCP_SYSV int32_t sceSystemServiceHideSplashScreen() { return kOk; }
DCP_SYSV int32_t sceUserServiceGetUserName(int32_t user, char* name, uint64_t size) {
    if (name && size) std::snprintf(name, size, "Player %d", user >= kUserId && user < kUserId + 4 ? user - kUserId + 1 : 1);
    return kOk;
}

// ---- libSceLibcInternal (imported by libc.prx) ----
// libc's malloc asks for the heap-trace hooks: {u64 size; u32 flag; u32 getSegmentInfo; u64* atomic_id_mask;
// u64* mstate_table}. Hand it zeroed storage (layout as in shadPS4's notes; the fault at libc+0x64325 confirms +0x10).
uint64_t g_mspace_atomic_id_mask = 0;
uint64_t g_mstate_table[64] = {};
DCP_SYSV int32_t sceLibcHeapGetTraceInfo(uint8_t* info) {
    uint32_t zero = 0;
    uint64_t* mask = &g_mspace_atomic_id_mask;
    uint64_t* table = g_mstate_table;
    std::memcpy(info + 0x0C, &zero, 4);
    std::memcpy(info + 0x10, &mask, 8);
    std::memcpy(info + 0x18, &table, 8);
    return kOk;
}

}  // namespace

void register_services() {
    REG("libSceLibcInternal", sceLibcHeapGetTraceInfo);
    REG("libSceSysmodule", sceSysmoduleLoadModule); REG("libSceSysmodule", sceSysmoduleUnloadModule);
    REG("libSceSysmodule", sceSysmoduleIsLoaded);
    REG("libSceUserService", sceUserServiceInitialize); REG("libSceUserService", sceUserServiceTerminate);
    REG("libSceUserService", sceUserServiceGetInitialUser); REG("libSceUserService", sceUserServiceGetEvent);
    REG("libSceUserService", sceUserServiceGetUserName); REG("libSceUserService", sceUserServiceGetLoginUserIdList);
    const char* ss = "libSceSystemService";
    REG(ss, sceSystemServiceParamGetInt); REG(ss, sceSystemServiceGetStatus); REG(ss, sceSystemServiceGetDisplaySafeAreaInfo);
    REG(ss, sceSystemServiceHideSplashScreen);
}

}  // namespace dcp::hle
