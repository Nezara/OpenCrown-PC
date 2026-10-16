// libkernel file I/O. /app0/... -> Config::app0, read-only (the user's dump). /savedataN/... -> the mounted save
// directory (savedata.cpp), read-write. fd 1/2 -> host stdout/stderr; every other mount point reports ENOENT.
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include "render/texwatch.h"
#include "../common.h"
#include "pure.h"

namespace dcp::hle {
namespace {

struct File { HANDLE h = INVALID_HANDLE_VALUE; std::string path; bool append = false; bool writable = false; };
std::mutex g_mu;
std::vector<File*> g_fds(3, nullptr);  // 0..2 reserved (stdio)

struct Host { std::string path; bool writable; };
std::optional<Host> resolve(const char* path) {
    if (!path) return std::nullopt;
    if (auto h = map_app0(config().app0, path)) return Host{*h, false};
    if (auto h = resolve_savedata(path)) return Host{*h, true};
    return std::nullopt;
}

// FreeBSD open(2) flags as used by the PS4.
enum : int32_t { kOWronly = 1, kORdwr = 2, kOAccMode = 3, kOAppend = 8, kOCreat = 0x200, kOTrunc = 0x400, kOExcl = 0x800 };

File* get(int32_t fd) {
    std::lock_guard<std::mutex> lk(g_mu);
    return fd >= 0 && size_t(fd) < g_fds.size() ? g_fds[fd] : nullptr;
}

// PS4 (FreeBSD 9) struct stat, 0x78 bytes; only the fields the game can sensibly use are filled.
void fill_stat(uint8_t* st, bool dir, uint64_t size, bool writable) {
    std::memset(st, 0, 0x78);
    uint16_t mode = uint16_t((dir ? 0040555 : 0100444) | (writable ? 0222 : 0));
    std::memcpy(st + 0x08, &mode, 2);
    uint16_t nlink = 1;
    std::memcpy(st + 0x0A, &nlink, 2);
    std::memcpy(st + 0x48, &size, 8);
    uint64_t blocks = (size + 511) / 512;
    std::memcpy(st + 0x50, &blocks, 8);
    uint32_t blksize = 0x10000;
    std::memcpy(st + 0x58, &blksize, 4);
}

int32_t stat_host(const Host& host, uint8_t* st) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(widen(host.path).c_str(), GetFileExInfoStandard, &a)) return kErr(kENOENT);
    fill_stat(st, a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY, (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow,
              host.writable);
    return kOk;
}

int32_t win_err() {
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return kErr(kENOENT);
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return kErr(kEEXIST);
    case ERROR_DISK_FULL: return kErr(kENOSPC);
    default: return kErr(kEACCES);
    }
}

DCP_SYSV int32_t sceKernelOpen(const char* path, int32_t flags, int32_t mode) {
    auto host = resolve(path);
    bool write = (flags & kOAccMode) != 0 || (flags & (kOCreat | kOTrunc | kOAppend));
    if (!host || (write && !host->writable)) {
        std::fprintf(stderr, "HLE sceKernelOpen: '%s' flags 0x%x -> %s\n", path ? path : "(null)", flags,
                     host ? "EACCES (read-only)" : "ENOENT (unmapped)");
        return kErr(host ? kEACCES : kENOENT);
    }
    DWORD disp = OPEN_EXISTING;
    if (flags & kOCreat) disp = (flags & kOExcl) ? CREATE_NEW : (flags & kOTrunc) ? CREATE_ALWAYS : OPEN_ALWAYS;
    else if (flags & kOTrunc) disp = TRUNCATE_EXISTING;
    if (disp == OPEN_EXISTING && GetFileAttributesW(widen(host->path).c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::fprintf(stderr, "HLE sceKernelOpen: '%s' -> ENOENT\n", path);
        return kErr(kENOENT);
    }
    DWORD access = GENERIC_READ | (write ? GENERIC_WRITE : 0);
    HANDLE h = CreateFileW(widen(host->path).c_str(), access, FILE_SHARE_READ, nullptr, disp,
                           FILE_FLAG_BACKUP_SEMANTICS /* allows directories */, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        int32_t e = win_err();
        std::fprintf(stderr, "HLE sceKernelOpen: '%s' flags 0x%x -> 0x%x\n", path, flags, unsigned(e));
        return e;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_fds.push_back(new File{h, path, (flags & kOAppend) != 0, host->writable});
    int32_t fd = int32_t(g_fds.size() - 1);
    if (host->writable) std::fprintf(stderr, "HLE sceKernelOpen: '%s' flags 0x%x -> fd %d\n", path, flags, fd);
    else DCP_HLE_LOG("sceKernelOpen", "'%s' mode 0%o -> fd %d (further /app0 opens not logged)", path, mode, fd);
    return fd;
}

DCP_SYSV int32_t sceKernelClose(int32_t fd) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (fd < 3 || size_t(fd) >= g_fds.size() || !g_fds[fd]) return kErr(kEBADF);
    CloseHandle(g_fds[fd]->h);
    delete g_fds[fd];
    g_fds[fd] = nullptr;
    return kOk;
}

DCP_SYSV int64_t sceKernelRead(int32_t fd, void* buf, uint64_t n) {
    File* f = get(fd);
    if (!f) return kErr(kEBADF);
    uint64_t done = 0;
    while (done < n) {
        DWORD chunk = DWORD(n - done > 0x40000000 ? 0x40000000 : n - done), got = 0;
        texwatch::host_write(static_cast<uint8_t*>(buf) + done, chunk);  // ReadFile fails on write-protected pages
        if (!ReadFile(f->h, static_cast<uint8_t*>(buf) + done, chunk, &got, nullptr)) return kErr(kEFAULT);
        if (got == 0) break;
        done += got;
    }
    return int64_t(done);
}

DCP_SYSV int64_t sceKernelWrite(int32_t fd, const void* buf, uint64_t n) {
    if (fd == 1 || fd == 2) {
        std::fwrite(buf, 1, n, fd == 1 ? stdout : stderr);
        std::fflush(fd == 1 ? stdout : stderr);
        return int64_t(n);
    }
    File* f = get(fd);
    if (!f || !f->writable) return kErr(kEBADF);
    if (f->append) {
        LARGE_INTEGER z{};
        SetFilePointerEx(f->h, z, nullptr, FILE_END);
    }
    uint64_t done = 0;
    while (done < n) {
        DWORD chunk = DWORD(n - done > 0x40000000 ? 0x40000000 : n - done), put = 0;
        if (!WriteFile(f->h, static_cast<const uint8_t*>(buf) + done, chunk, &put, nullptr)) return win_err();
        done += put;
    }
    return int64_t(done);
}

DCP_SYSV int64_t sceKernelLseek(int32_t fd, int64_t off, int32_t whence) {
    File* f = get(fd);
    if (!f) return kErr(kEBADF);
    LARGE_INTEGER d, out;
    d.QuadPart = off;
    if (!SetFilePointerEx(f->h, d, &out, DWORD(whence))) return kErr(kEINVAL);  // SEEK_SET/CUR/END = 0/1/2 on both
    return out.QuadPart;
}

DCP_SYSV int32_t sceKernelStat(const char* path, uint8_t* st) {
    auto host = resolve(path);
    int32_t r = host ? stat_host(*host, st) : kErr(kENOENT);
    if (r != kOk) std::fprintf(stderr, "HLE sceKernelStat: '%s' -> ENOENT\n", path ? path : "(null)");
    return r;
}

DCP_SYSV int32_t sceKernelFstat(int32_t fd, uint8_t* st) {
    File* f = get(fd);
    if (!f) return kErr(kEBADF);
    BY_HANDLE_FILE_INFORMATION i;
    if (!GetFileInformationByHandle(f->h, &i)) return kErr(kEBADF);
    fill_stat(st, i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY, (uint64_t(i.nFileSizeHigh) << 32) | i.nFileSizeLow,
              f->writable);
    return kOk;
}

DCP_SYSV int32_t sceKernelFsync(int32_t fd) {
    File* f = get(fd);
    if (!f) return kErr(kEBADF);
    if (f->writable) FlushFileBuffers(f->h);
    return kOk;
}
DCP_SYSV int32_t sceKernelFtruncate(int32_t fd, int64_t len) {
    File* f = get(fd);
    if (!f) return kErr(kEBADF);
    if (!f->writable) return kErr(kEACCES);
    LARGE_INTEGER cur{}, z{}, l;
    l.QuadPart = len;
    SetFilePointerEx(f->h, z, &cur, FILE_CURRENT);
    bool ok = SetFilePointerEx(f->h, l, nullptr, FILE_BEGIN) && SetEndOfFile(f->h);
    SetFilePointerEx(f->h, cur, nullptr, FILE_BEGIN);
    return ok ? kOk : win_err();
}
DCP_SYSV int32_t sceKernelMkdir(const char* p, int32_t) {
    auto host = resolve(p);
    if (!host || !host->writable) {
        DCP_HLE_LOG("sceKernelMkdir", "'%s' -> EACCES (read-only)", p ? p : "");
        return kErr(host ? kEACCES : kENOENT);
    }
    return CreateDirectoryW(widen(host->path).c_str(), nullptr) ? kOk : win_err();
}
DCP_SYSV int32_t sceKernelRename(const char* from, const char* to) {
    auto a = resolve(from), b = resolve(to);
    if (!a || !b || !a->writable || !b->writable) return kErr(kEACCES);
    return MoveFileExW(widen(a->path).c_str(), widen(b->path).c_str(), MOVEFILE_REPLACE_EXISTING) ? kOk : win_err();
}
DCP_SYSV int32_t sceKernelUnlink(const char* p) {
    auto host = resolve(p);
    if (!host || !host->writable) return kErr(host ? kEACCES : kENOENT);
    return DeleteFileW(widen(host->path).c_str()) ? kOk : win_err();
}
DCP_SYSV int32_t sceKernelCheckReachability(const char* path) {
    auto host = resolve(path);
    return host && GetFileAttributesW(widen(host->path).c_str()) != INVALID_FILE_ATTRIBUTES ? kOk : kErr(kENOENT);
}

// POSIX variants used by libc.prx (return -1 + errno).
DCP_SYSV int64_t _write(int32_t fd, const void* buf, uint64_t n) {
    int64_t r = sceKernelWrite(fd, buf, n);
    return r < 0 ? set_errno(r & 0xFFFF) : r;
}
DCP_SYSV int64_t _read(int32_t fd, void* buf, uint64_t n) {
    int64_t r = sceKernelRead(fd, buf, n);
    return r < 0 ? set_errno(r & 0xFFFF) : r;
}

}  // namespace

std::wstring widen(const std::string& s) {
    std::wstring w(s.size() + 1, L'\0');
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), int(w.size()));
    w.resize(n > 0 ? n - 1 : 0);
    return w;
}

void register_kernel_file() {
    const char* k = "libkernel";
    REG(k, sceKernelOpen); REG(k, sceKernelClose); REG(k, sceKernelRead); REG(k, sceKernelWrite);
    REG(k, sceKernelLseek); REG(k, sceKernelStat); REG(k, sceKernelFstat); REG(k, sceKernelFsync);
    REG(k, sceKernelFtruncate); REG(k, sceKernelMkdir); REG(k, sceKernelRename); REG(k, sceKernelUnlink);
    REG(k, sceKernelCheckReachability); REG(k, _write); REG(k, _read);
}

}  // namespace dcp::hle
