// libSceSaveData: directory-backed save data. Each save directory (e.g. SAVEDATA00) is a host folder under
// Config::save_root (PLAN rule 0: %USERPROFILE%\Documents\Dragons Crown Pro\saves). Mount2 maps it to "/savedataN",
// which the kernel file shims then read/write. The game's SAVEn.DAT payload goes through those shims untouched; this
// file only adds the sidecars sce_sys/param.bin (raw 0x530 B SceSaveDataParam) and sce_sys/icon0.png.
// Struct layouts follow the SDK headers (as also documented by shadPS4); offsets are noted where they are read.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include "common.h"
#include "kernel/pure.h"

namespace dcp::hle {
namespace {

constexpr int32_t kSdErrParameter = int32_t(0x809F0000);
constexpr int32_t kSdErrNotMounted = int32_t(0x809F0004);
constexpr int32_t kSdErrExists = int32_t(0x809F0007);
constexpr int32_t kSdErrNotFound = int32_t(0x809F0008);
constexpr int32_t kSdErrMountFull = int32_t(0x809F000C);
constexpr int32_t kSdErrInternal = int32_t(0x809F000B);

enum : uint32_t { kModeRdonly = 1, kModeRdwr = 2, kModeCreate = 4, kModeCopyIcon = 0x10, kModeCreate2 = 0x20 };
constexpr uint64_t kParamSize = 0x530;
constexpr uint64_t kBlockSize = 0x8000;
constexpr uint64_t kDefaultBlocks = 0x8000;  // 1 GiB quota reported for every directory (generous; game stores nothing)

std::mutex g_mu;
SaveMountTable g_mounts;
std::vector<uint64_t> g_mount_blocks(SaveMountTable::kMax, kDefaultBlocks);

std::string root() { return config().save_root; }
std::string dir_path(const std::string& name) { return root() + "/" + name; }

bool is_dir(const std::string& p) {
    DWORD a = GetFileAttributesW(widen(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool make_dirs(const std::string& p) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(widen(p)), ec);
    return is_dir(p);
}

bool write_file(const std::string& p, const void* data, uint64_t n) {
    HANDLE h = CreateFileW(widen(p).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, data, DWORD(n), &put, nullptr) && put == n;
    CloseHandle(h);
    return ok;
}

bool read_file(const std::string& p, void* data, uint64_t n) {
    HANDLE h = CreateFileW(widen(p).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    bool ok = ReadFile(h, data, DWORD(n), &got, nullptr) && got == n;
    CloseHandle(h);
    return ok;
}

uint64_t used_blocks(const std::string& dir) {
    uint64_t bytes = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(std::filesystem::path(widen(dir)), ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) bytes += it->file_size(ec);
    return (bytes + kBlockSize - 1) / kBlockSize;
}

int mount_index(const char* mp) {  // "/savedataN" -> N
    return mp && std::strncmp(mp, "/savedata", 9) == 0 ? std::atoi(mp + 9) : -1;
}

// SceSaveDataMount2: +0 userId, +8 dirName*, +0x10 blocks, +0x18 mountMode.
// SceSaveDataMountResult: +0 mountPoint char[16], +0x10 requiredBlocks, +0x18 unused, +0x1c mountStatus.
DCP_SYSV int32_t sceSaveDataMount2(const uint8_t* m, uint8_t* res) {
    if (!m || !res) return kSdErrParameter;
    int32_t user;
    const char* dir;
    uint64_t blocks;
    uint32_t mode;
    std::memcpy(&user, m, 4);
    std::memcpy(&dir, m + 8, 8);
    std::memcpy(&blocks, m + 0x10, 8);
    std::memcpy(&mode, m + 0x18, 4);
    if (!valid_savedata_dirname(dir)) {
        std::fprintf(stderr, "HLE sceSaveDataMount2: bad dir name -> PARAMETER\n");
        return kSdErrParameter;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    std::string host = dir_path(dir);
    bool exists = is_dir(host), create = mode & (kModeCreate | kModeCreate2);
    int32_t r = kOk;
    if (!exists && !create) r = kSdErrNotFound;
    else if (exists && (mode & kModeCreate)) r = kSdErrExists;
    else if (!exists && !make_dirs(host + "/sce_sys")) r = kSdErrInternal;
    std::string mp;
    if (r == kOk && (mp = g_mounts.mount(dir, host)).empty()) r = kSdErrMountFull;  // also: already mounted (guess: BUSY)
    std::fprintf(stderr, "HLE sceSaveDataMount2: user %d dir '%s' blocks 0x%llx mode 0x%x -> 0x%x %s (%s)\n", user, dir,
                 (unsigned long long)blocks, mode, unsigned(r), mp.c_str(), host.c_str());
    if (r != kOk) return r;
    std::memset(res, 0, 0x20);
    std::memcpy(res, mp.c_str(), mp.size() + 1);
    uint32_t status = exists ? 0 : 1;  // SCE_SAVE_DATA_MOUNT_STATUS_CREATED
    std::memcpy(res + 0x1c, &status, 4);
    g_mount_blocks[mount_index(mp.c_str())] = blocks > kDefaultBlocks ? blocks : kDefaultBlocks;
    return kOk;
}

DCP_SYSV int32_t sceSaveDataUmount(const char* mp) {
    std::lock_guard<std::mutex> lk(g_mu);
    bool ok = mp && g_mounts.umount(mp);
    std::fprintf(stderr, "HLE sceSaveDataUmount: '%s' -> %s\n", mp ? mp : "", ok ? "ok" : "NOT_MOUNTED");
    return ok ? kOk : kSdErrNotMounted;
}

// SceSaveDataMountInfo: +0 blocks, +8 freeBlocks, +0x10 reserved[32].
DCP_SYSV int32_t sceSaveDataGetMountInfo(const char* mp, uint8_t* info) {
    std::lock_guard<std::mutex> lk(g_mu);
    const std::string* host = mp ? g_mounts.host_of(mp) : nullptr;
    if (!host || !info) return kSdErrNotMounted;
    uint64_t total = g_mount_blocks[mount_index(mp)], used = used_blocks(*host);
    uint64_t free_blocks = used < total ? total - used : 0;
    std::memset(info, 0, 0x30);
    std::memcpy(info, &total, 8);
    std::memcpy(info + 8, &free_blocks, 8);
    DCP_HLE_LOG("sceSaveDataGetMountInfo", "'%s' -> blocks 0x%llx free 0x%llx", mp, (unsigned long long)total,
                (unsigned long long)free_blocks);
    return kOk;
}

// sceSaveDataSetParam(mountPoint, paramType, buf, size): type 0 = the whole SceSaveDataParam (title +0, subTitle
// +0x80, detail +0x100, userParam +0x500, mtime +0x508).
DCP_SYSV int32_t sceSaveDataSetParam(const char* mp, uint32_t type, const uint8_t* buf, uint64_t size) {
    std::lock_guard<std::mutex> lk(g_mu);
    const std::string* host = mp ? g_mounts.host_of(mp) : nullptr;
    if (!host) return kSdErrNotMounted;
    if (!buf) return kSdErrParameter;
    uint8_t p[kParamSize] = {};
    read_file(*host + "/sce_sys/param.bin", p, kParamSize);
    if (type == 0) std::memcpy(p, buf, size < kParamSize ? size : kParamSize);
    else std::fprintf(stderr, "HLE sceSaveDataSetParam: type %u not stored\n", type);
    make_dirs(*host + "/sce_sys");
    bool ok = write_file(*host + "/sce_sys/param.bin", p, kParamSize);
    std::fprintf(stderr, "HLE sceSaveDataSetParam: '%s' type %u title '%.32s' sub '%.32s' -> %s\n", mp, type,
                 reinterpret_cast<const char*>(p), reinterpret_cast<const char*>(p + 0x80), ok ? "ok" : "write failed");
    return ok ? kOk : kSdErrInternal;
}

// SceSaveDataIcon: +0 buf*, +8 bufSize, +0x10 dataSize.
DCP_SYSV int32_t sceSaveDataSaveIcon(const char* mp, const uint8_t* icon) {
    std::lock_guard<std::mutex> lk(g_mu);
    const std::string* host = mp ? g_mounts.host_of(mp) : nullptr;
    if (!host) return kSdErrNotMounted;
    const void* buf;
    uint64_t size;
    std::memcpy(&buf, icon, 8);
    std::memcpy(&size, icon + 0x10, 8);
    if (!buf || size > (16u << 20)) return kSdErrParameter;
    make_dirs(*host + "/sce_sys");
    bool ok = write_file(*host + "/sce_sys/icon0.png", buf, size);
    DCP_HLE_LOG("sceSaveDataSaveIcon", "'%s' %llu bytes -> %s", mp, (unsigned long long)size, ok ? "ok" : "failed");
    return ok ? kOk : kSdErrInternal;
}

// SceSaveDataDelete: +0 userId, +8 titleId*, +0x10 dirName*.
DCP_SYSV int32_t sceSaveDataDelete(const uint8_t* d) {
    if (!d) return kSdErrParameter;
    const char* dir;
    std::memcpy(&dir, d + 0x10, 8);
    if (!valid_savedata_dirname(dir)) return kSdErrParameter;
    std::lock_guard<std::mutex> lk(g_mu);
    std::string host = dir_path(dir);
    if (!is_dir(host)) return kSdErrNotFound;
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(widen(host)), ec);
    std::fprintf(stderr, "HLE sceSaveDataDelete: '%s' -> %s\n", dir, ec ? "failed" : "deleted");
    return ec ? kSdErrInternal : kOk;
}

// Cond: +0 userId, +8 titleId*, +0x10 dirName* (pattern), +0x18 key, +0x1c order.
// Result: +0 hitNum, +8 dirNames* (char[32] each), +0x10 dirNamesNum, +0x14 setNum, +0x18 params*, +0x20 infos*.
DCP_SYSV int32_t sceSaveDataDirNameSearch(const uint8_t* cond, uint8_t* res) {
    if (!cond || !res) return kSdErrParameter;
    const char* pattern;
    uint32_t order;
    std::memcpy(&pattern, cond + 0x10, 8);
    std::memcpy(&order, cond + 0x1c, 4);
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(widen(root() + "/*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                char n[260];
                WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, n, sizeof n, nullptr, nullptr);
                if (valid_savedata_dirname(n) && savedata_name_match(pattern, n)) names.push_back(n);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    std::sort(names.begin(), names.end());
    if (order == 1) std::reverse(names.begin(), names.end());  // key = dir name (only key supported), descending
    char* dirs;
    uint32_t cap;
    uint8_t *params, *infos;
    std::memcpy(&dirs, res + 8, 8);
    std::memcpy(&cap, res + 0x10, 4);
    std::memcpy(&params, res + 0x18, 8);
    std::memcpy(&infos, res + 0x20, 8);
    uint32_t hit = uint32_t(names.size()), set = hit < cap ? hit : cap;
    for (uint32_t i = 0; dirs && i < set; ++i) {
        std::memset(dirs + i * 32, 0, 32);
        std::memcpy(dirs + i * 32, names[i].c_str(), names[i].size());
        if (params) {
            std::memset(params + i * kParamSize, 0, kParamSize);
            read_file(dir_path(names[i]) + "/sce_sys/param.bin", params + i * kParamSize, kParamSize);
        }
        if (infos) {
            uint64_t used = used_blocks(dir_path(names[i])), free_blocks = kDefaultBlocks - used;
            std::memset(infos + i * 0x30, 0, 0x30);
            std::memcpy(infos + i * 0x30, &kDefaultBlocks, 8);
            std::memcpy(infos + i * 0x30 + 8, &free_blocks, 8);
        }
    }
    std::memcpy(res, &hit, 4);
    std::memcpy(res + 0x14, &set, 4);
    std::fprintf(stderr, "HLE sceSaveDataDirNameSearch: '%s' -> %u hit(s)%s%s\n", pattern ? pattern : "", hit,
                 hit ? ", first " : "", hit ? names[0].c_str() : "");
    return kOk;
}

DCP_SYSV int32_t sceSaveDataInitialize3(const void*) {
    std::fprintf(stderr, "HLE sceSaveDataInitialize3: save root '%s'\n", root().c_str());
    return kOk;
}
DCP_SYSV int32_t sceSaveDataTerminate() { return kOk; }

}  // namespace

std::optional<std::string> resolve_savedata(const std::string& path) {
    if (path.compare(0, 9, "/savedata") != 0) return std::nullopt;
    std::lock_guard<std::mutex> lk(g_mu);
    return g_mounts.resolve(path);
}

std::string default_save_root() {
    if (const char* r = std::getenv("DCP_SAVE_ROOT"); r && *r) return r;
    if (const char* u = std::getenv("DCP_USER_DIR"); u && *u) return std::string(u) + "/saves";  // = dcp::user_dir()
    const char* home = std::getenv("USERPROFILE");
    return std::string(home ? home : ".") + "/Documents/Dragons Crown Pro/saves";
}

void register_savedata() {
    const char* l = "libSceSaveData";
    REG(l, sceSaveDataInitialize3); REG(l, sceSaveDataTerminate); REG(l, sceSaveDataMount2); REG(l, sceSaveDataUmount);
    REG(l, sceSaveDataGetMountInfo); REG(l, sceSaveDataSetParam); REG(l, sceSaveDataSaveIcon); REG(l, sceSaveDataDelete);
    REG(l, sceSaveDataDirNameSearch);
}

}  // namespace dcp::hle
