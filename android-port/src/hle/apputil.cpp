// SceAppUtil (system parameters, save data slots and files, app events, DRM / store) and SceAppMgrUser.
// Save data lives under the savedata0: mount (see io.h). Slot parameters are kept by us as the raw 0x34C-byte
// SceAppUtilSaveDataSlotParam in savedata0:/sce_sys/oc_slot_<id>.bin (our own layout; the Vita keeps them in its
// own sce_sys files).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "hle/common.h"
#include "hle/io.h"

namespace fs = std::filesystem;

namespace hle {
namespace {

enum : uint32_t {
  APPUTIL_ERROR_PARAMETER = 0x80100600,
  APPUTIL_ERROR_NOT_INITIALIZED = 0x80100601,
  APPUTIL_ERROR_NOT_MOUNTED = 0x80100604,
  APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND = 0x80100641,
  APPUTIL_ERROR_DRM_NO_ENTITLEMENT = 0x80100660,
};
constexpr uint32_t SLOT_PARAM_SIZE = 0x34C;

std::mutex apputil_lock;
bool initialized = false;

// --- system parameters ---------------------------------------------------------------------------------------------
// Ids: 1 language, 2 enter button, 3 username, 4 date format, 5 time format, 6 time zone (minutes), 7 summer time.
// Defaults follow a US console; the launcher will make them settings.
int32_t timezone_minutes() {
  std::time_t now = std::time(nullptr);
  std::tm lt{}, ut{};
#ifdef _WIN32
  localtime_s(&lt, &now);
  gmtime_s(&ut, &now);
#else
  localtime_r(&now, &lt);
  gmtime_r(&now, &ut);
#endif
  ut.tm_isdst = 0;
  lt.tm_isdst = 0;
  return int32_t(std::difftime(std::mktime(&lt), std::mktime(&ut)) / 60);
}

void SystemParamGetInt(Cpu& c) {
  uint32_t id = arg(c, 0), out = arg(c, 1);
  if (!out) return ret(c, APPUTIL_ERROR_PARAMETER);
  int32_t v;
  switch (id) {
    case 1: v = 1; break;  // SCE_SYSTEM_PARAM_LANG_ENGLISH_US
    case 2: v = 1; break;  // SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS
    case 4: v = 2; break;  // MM/DD/YYYY
    case 5: v = 0; break;  // 12-hour clock
    case 6: v = timezone_minutes(); break;
    case 7: v = 0; break;
    default: return ret(c, APPUTIL_ERROR_PARAMETER);
  }
  c.mem().write<int32_t>(out, v);
  ret(c, 0);
}

// sceAppUtilSystemParamGetString(id, buf, size): 3 = username (UTF-8).
void SystemParamGetString(Cpu& c) {
  uint32_t id = arg(c, 0), buf = arg(c, 1), size = arg(c, 2);
  if (id != 3 || !buf || !size || !c.mem().valid(buf, size)) return ret(c, APPUTIL_ERROR_PARAMETER);
  static const char name[] = "Player";
  uint32_t n = std::min<uint32_t>(size - 1, sizeof name - 1);
  std::memcpy(c.mem().ptr(buf), name, n);
  c.mem().write<uint8_t>(buf + n, 0);
  ret(c, 0);
}

// --- lifecycle / events --------------------------------------------------------------------------------------------
void Init(Cpu& c) {
  std::lock_guard lk(apputil_lock);
  initialized = true;
  if (uint32_t boot = arg(c, 1)) std::memset(c.mem().ptr(boot), 0, 0x28);  // boot attribute 0, version 0
  ret(c, 0);
}
void Shutdown(Cpu& c) {
  std::lock_guard lk(apputil_lock);
  initialized = false;
  ret(c, 0);
}
// sceAppUtilReceiveAppEvent(SceAppUtilAppEventParam*): no pending event (type 0).
void ReceiveAppEvent(Cpu& c) {
  if (!arg(c, 0)) return ret(c, APPUTIL_ERROR_PARAMETER);
  std::memset(c.mem().ptr(arg(c, 0)), 0, 0x404);
  ret(c, 0);
}

// Additional content and the PS Store are not available: no DLC is installed.
void DrmOpen(Cpu& c) { ret(c, APPUTIL_ERROR_DRM_NO_ENTITLEMENT); }
void DrmClose(Cpu& c) { ret(c, 0); }
void StoreBrowse(Cpu& c) {
  HLE_NOTE("sceAppUtilStoreBrowse: no store\n");
  ret(c, APPUTIL_ERROR_PARAMETER);
}

// --- save data -----------------------------------------------------------------------------------------------------
std::string slot_file(uint32_t id, bool create) {
  return io_host_path("savedata0:/sce_sys/oc_slot_" + std::to_string(id) + ".bin", create);
}

bool read_slot(uint32_t id, std::vector<uint8_t>& out) {
  std::string path = slot_file(id, false);
  std::ifstream f(path, std::ios::binary);
  if (path.empty() || !f) return false;
  out.assign(SLOT_PARAM_SIZE, 0);
  f.read(reinterpret_cast<char*>(out.data()), SLOT_PARAM_SIZE);
  return true;
}

bool write_slot(uint32_t id, const uint8_t* param) {
  std::string path = slot_file(id, true);
  if (path.empty()) return false;
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(param), SLOT_PARAM_SIZE);
  return bool(f);
}

// sceAppUtilSaveDataSlotGetParam(slotId, SceAppUtilSaveDataSlotParam* param, mountPoint*)
void SaveDataSlotGetParam(Cpu& c) {
  uint32_t id = arg(c, 0), out = arg(c, 1);
  if (!out) return ret(c, APPUTIL_ERROR_PARAMETER);
  std::lock_guard lk(apputil_lock);
  std::vector<uint8_t> p;
  if (!read_slot(id, p)) return ret(c, APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND);
  std::memcpy(c.mem().ptr(out), p.data(), SLOT_PARAM_SIZE);
  ret(c, 0);
}

// sceAppUtilSaveDataDataSave(SceAppUtilSaveDataFileSlot* slot, SceAppUtilSaveDataFile* files, count, mountPoint*,
// SceSize* requiredSizeKB). Slot {id, slotParam*}: with a param the slot is created / updated (sizeKB and modified
// time filled in), without one it must exist. Files {path, buf, size, s64 offset @0x10, mode @0x18}: mode 0 writes
// at the offset keeping the rest, 1 truncates first, 2 creates a directory.
void SaveDataDataSave(Cpu& c) {
  uint32_t slot = arg(c, 0), files = arg(c, 1), count = arg(c, 2), required = arg(c, 4);
  if (count && !files) return ret(c, APPUTIL_ERROR_PARAMETER);
  Memory& m = c.mem();
  std::lock_guard lk(apputil_lock);
  if (required) m.write<uint32_t>(required, 0);
  std::vector<uint8_t> slot_param;
  uint32_t slot_id = 0;
  if (slot) {
    slot_id = m.read<uint32_t>(slot);
    uint32_t param = m.read<uint32_t>(slot + 4);
    if (param) slot_param.assign(m.ptr(param), m.ptr(param) + SLOT_PARAM_SIZE);
    else if (!read_slot(slot_id, slot_param)) return ret(c, APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND);
  }
  uint64_t total = 0;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t f = files + 0x40 * i;
    uint32_t path_va = m.read<uint32_t>(f), buf = m.read<uint32_t>(f + 4), size = m.read<uint32_t>(f + 8);
    int64_t offset = m.read<int64_t>(f + 0x10);
    uint32_t mode = m.read<uint32_t>(f + 0x18);
    if (!path_va) continue;
    std::string guest = "savedata0:" + str(c, path_va, 1024);
    std::string host = io_host_path(guest, true);
    if (host.empty() || (size && !m.valid(buf, size))) return ret(c, APPUTIL_ERROR_PARAMETER);
    std::error_code ec;
    if (mode == 2) {
      fs::create_directories(host, ec);
      continue;
    }
    fs::create_directories(fs::path(host).parent_path(), ec);
    bool exists = fs::exists(host, ec);
    std::fstream out(host, std::ios::binary | std::ios::in | std::ios::out |
                               (mode == 1 || !exists ? std::ios::trunc : std::ios::openmode{}));
    if (!out) return ret(c, APPUTIL_ERROR_PARAMETER);
    out.seekp(offset);
    if (size) out.write(reinterpret_cast<const char*>(m.ptr(buf)), size);
    total += size;
    HLE_NOTE("sceAppUtilSaveDataDataSave: %s (%u bytes at %lld)\n", guest.c_str(), size, (long long)offset);
  }
  if (slot) {
    uint32_t kb = uint32_t((total + 1023) / 1024);
    // SceAppUtilSaveDataSlotParam: status 0, title 0x04, subTitle 0x44, detail 0xC4, iconPath 0x2C4,
    // userParam 0x304, sizeKB 0x308, modifiedTime 0x30C (SceDateTime), reserved.
    std::memcpy(slot_param.data() + 0x308, &kb, 4);
    std::time_t t = std::time(nullptr);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    uint16_t dt[6] = {uint16_t(lt.tm_year + 1900), uint16_t(lt.tm_mon + 1), uint16_t(lt.tm_mday),
                      uint16_t(lt.tm_hour),        uint16_t(lt.tm_min),     uint16_t(lt.tm_sec)};
    std::memcpy(slot_param.data() + 0x30C, dt, sizeof dt);
    std::memset(slot_param.data() + 0x318, 0, 4);
    if (!write_slot(slot_id, slot_param.data())) return ret(c, APPUTIL_ERROR_PARAMETER);
  }
  ret(c, 0);
}

// sceAppUtilSaveDataDataRemove(slot*, SceAppUtilSaveDataRemoveItem* items {path, mode}, count, mountPoint*):
// removes the files; with a slot, mode 0 (DEFAULT) also deletes the slot, 1 (NO_SLOT) keeps it.
void SaveDataDataRemove(Cpu& c) {
  uint32_t slot = arg(c, 0), items = arg(c, 1), count = arg(c, 2);
  Memory& m = c.mem();
  std::lock_guard lk(apputil_lock);
  bool keep_slot = true;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t it = items + 0x2C * i;
    uint32_t path_va = m.read<uint32_t>(it);
    if (m.read<uint32_t>(it + 4) == 0) keep_slot = false;
    if (!path_va) continue;
    std::string host = io_host_path("savedata0:" + str(c, path_va, 1024), false);
    std::error_code ec;
    if (!host.empty()) fs::remove_all(host, ec);
  }
  if (slot && !keep_slot) {
    std::error_code ec;
    std::string path = slot_file(m.read<uint32_t>(slot), false);
    if (!path.empty()) fs::remove(path, ec);
  }
  ret(c, 0);
}

// --- SceAppMgrUser -------------------------------------------------------------------------------------------------
// _sceAppMgrGetAppState(SceAppMgrAppState*, size, version): nothing pending (all zero).
void GetAppState(Cpu& c) {
  uint32_t p = arg(c, 0), size = arg(c, 1);
  if (!p || !c.mem().valid(p, size)) return ret(c, 0x80802016);  // SCE_APPMGR_ERROR_NULL_POINTER
  std::memset(c.mem().ptr(p), 0, size);
  ret(c, 0);
}
// sceAppMgrReceiveSystemEvent(SceAppMgrSystemEvent*): no event. The game zeroes the buffer itself and only reacts
// to event codes (asm 0x813b71b4), so the buffer is left as it is.
void ReceiveSystemEvent(Cpu& c) { ret(c, 0); }

}  // namespace

void register_apputil() {
  REG(sceAppUtilInit, Init);
  REG(sceAppUtilShutdown, Shutdown);
  REG(sceAppUtilReceiveAppEvent, ReceiveAppEvent);
  REG(sceAppUtilSystemParamGetInt, SystemParamGetInt);
  REG(sceAppUtilSystemParamGetString, SystemParamGetString);
  REG(sceAppUtilDrmOpen, DrmOpen);
  REG(sceAppUtilDrmClose, DrmClose);
  REG(sceAppUtilStoreBrowse, StoreBrowse);
  REG(sceAppUtilSaveDataSlotGetParam, SaveDataSlotGetParam);
  REG(sceAppUtilSaveDataDataSave, SaveDataDataSave);
  REG(sceAppUtilSaveDataDataRemove, SaveDataDataRemove);
  REG(_sceAppMgrGetAppState, GetAppState);
  REG(sceAppMgrReceiveSystemEvent, ReceiveSystemEvent);
}

// Shared with the save-data dialog: does slot `id` exist, and copy its parameters to `out` (guest, may be 0).
bool apputil_slot_exists(Cpu& c, uint32_t id, uint32_t out) {
  std::lock_guard lk(apputil_lock);
  std::vector<uint8_t> p;
  if (!read_slot(id, p)) return false;
  if (out) std::memcpy(c.mem().ptr(out), p.data(), SLOT_PARAM_SIZE);
  return true;
}

}  // namespace hle
