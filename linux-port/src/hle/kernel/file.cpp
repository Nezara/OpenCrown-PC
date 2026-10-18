// libkernel file I/O. /app0/... -> Config::app0, read-only (the user's dump). /savedataN/... -> the mounted save
// directory (savedata.cpp), read-write. fd 1/2 -> host stdout/stderr; every other mount point reports ENOENT.
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "render/texwatch.h"
#include "../common.h"
#include "pure.h"

namespace dcp::hle {
namespace {

struct File { int h = -1; std::string path; bool append = false; bool writable = false; };
std::mutex g_mu;
std::vector<File*> g_fds(3, nullptr);  // 0..2 reserved (stdio)

struct Host { std::string path; bool writable; };

// The dump may come from a case-insensitive filesystem (the game opens "/app0/DRACRO.cpk", the file is DRACRO.CPK).
// When the exact host path is missing, match each component under the app0 root without case. Cached by path.
std::string app0_case_fallback(const std::string& root, const std::string& exact) {
    static std::mutex mu;
    static std::map<std::string, std::string> cache;
    struct stat st;
    if (::stat(exact.c_str(), &st) == 0 || exact.compare(0, root.size(), root) != 0) return exact;
    std::lock_guard<std::mutex> lk(mu);
    if (auto it = cache.find(exact); it != cache.end()) return it->second;
    std::string out = root;
    for (size_t i = root.size(); i < exact.size();) {
        size_t j = exact.find('/', i + 1);
        if (j == std::string::npos) j = exact.size();
        std::string comp = exact.substr(i + 1, j - i - 1), match;
        i = j;
        if (DIR* d = opendir(out.c_str())) {
            while (dirent* e = readdir(d))
                if (strcasecmp(e->d_name, comp.c_str()) == 0) {
                    match = e->d_name;
                    break;
                }
            closedir(d);
        }
        if (match.empty()) return cache[exact] = exact;  // genuinely missing: callers report ENOENT
        out += '/' + match;
    }
    return cache[exact] = out;
}

std::optional<Host> resolve(const char* path) {
    if (!path) return std::nullopt;
    if (auto h = map_app0(config().app0, path)) return Host{app0_case_fallback(config().app0, *h), false};
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
    struct stat a;
    if (::stat(host.path.c_str(), &a) != 0) return kErr(kENOENT);
    fill_stat(st, S_ISDIR(a.st_mode), uint64_t(a.st_size), host.writable);
    return kOk;
}

bool host_exists(const std::string& p) {
    struct stat a;
    return ::stat(p.c_str(), &a) == 0;
}

int32_t host_err() {  // host errno -> SCE error (FreeBSD numbering)
    switch (errno) {
    case ENOENT: case ENOTDIR: return kErr(kENOENT);
    case EEXIST: case ENOTEMPTY: return kErr(kEEXIST);
    case ENOSPC: return kErr(kENOSPC);
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
    int oflags = (write ? O_RDWR : O_RDONLY) | O_CLOEXEC;
    if (flags & kOCreat) oflags |= O_CREAT | ((flags & kOExcl) ? O_EXCL : 0);
    if (flags & kOTrunc) oflags |= O_TRUNC;
    if (!(flags & (kOCreat | kOTrunc)) && !host_exists(host->path)) {
        std::fprintf(stderr, "HLE sceKernelOpen: '%s' -> ENOENT\n", path);
        return kErr(kENOENT);
    }
    int h = ::open(host->path.c_str(), oflags, 0644);  // O_RDONLY opens directories too
    if (h < 0) {
        int32_t e = host_err();
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
    ::close(g_fds[fd]->h);
    delete g_fds[fd];
    g_fds[fd] = nullptr;
    return kOk;
}

DCP_SYSV int64_t sceKernelRead(int32_t fd, void* buf, uint64_t n) {
    File* f = get(fd);
    if (!f) return kErr(kEBADF);
    uint64_t done = 0;
    while (done < n) {
        size_t chunk = size_t(n - done > 0x40000000 ? 0x40000000 : n - done);
        texwatch::host_write(static_cast<uint8_t*>(buf) + done, chunk);  // read() gives EFAULT on write-protected pages
        ssize_t got = ::read(f->h, static_cast<uint8_t*>(buf) + done, chunk);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return kErr(kEFAULT);
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
    if (f->append) ::lseek(f->h, 0, SEEK_END);
    uint64_t done = 0;
    while (done < n) {
        size_t chunk = size_t(n - done > 0x40000000 ? 0x40000000 : n - done);
        ssize_t put = ::write(f->h, static_cast<const uint8_t*>(buf) + done, chunk);
        if (put < 0 && errno == EINTR) continue;
        if (put < 0) return host_err();
        done += uint64_t(put);
    }
    return int64_t(done);
}

DCP_SYSV int64_t sceKernelLseek(int32_t fd, int64_t off, int32_t whence) {
    File* f = get(fd);
    if (!f) return kErr(kEBADF);
    off_t r = ::lseek(f->h, off_t(off), whence);  // SEEK_SET/CUR/END = 0/1/2 on both
    return r < 0 ? kErr(kEINVAL) : int64_t(r);
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
    struct stat i;
    if (::fstat(f->h, &i) != 0) return kErr(kEBADF);
    fill_stat(st, S_ISDIR(i.st_mode), uint64_t(i.st_size), f->writable);
    return kOk;
}

DCP_SYSV int32_t sceKernelFsync(int32_t fd) {
    File* f = get(fd);
    if (!f) return kErr(kEBADF);
    if (f->writable) ::fsync(f->h);
    return kOk;
}
DCP_SYSV int32_t sceKernelFtruncate(int32_t fd, int64_t len) {
    File* f = get(fd);
    if (!f) return kErr(kEBADF);
    if (!f->writable) return kErr(kEACCES);
    return ::ftruncate(f->h, off_t(len)) == 0 ? kOk : host_err();
}
DCP_SYSV int32_t sceKernelMkdir(const char* p, int32_t) {
    auto host = resolve(p);
    if (!host || !host->writable) {
        DCP_HLE_LOG("sceKernelMkdir", "'%s' -> EACCES (read-only)", p ? p : "");
        return kErr(host ? kEACCES : kENOENT);
    }
    return ::mkdir(host->path.c_str(), 0755) == 0 ? kOk : host_err();
}
DCP_SYSV int32_t sceKernelRename(const char* from, const char* to) {
    auto a = resolve(from), b = resolve(to);
    if (!a || !b || !a->writable || !b->writable) return kErr(kEACCES);
    return ::rename(a->path.c_str(), b->path.c_str()) == 0 ? kOk : host_err();
}
DCP_SYSV int32_t sceKernelUnlink(const char* p) {
    auto host = resolve(p);
    if (!host || !host->writable) return kErr(host ? kEACCES : kENOENT);
    return ::unlink(host->path.c_str()) == 0 ? kOk : host_err();
}
DCP_SYSV int32_t sceKernelCheckReachability(const char* path) {
    auto host = resolve(path);
    return host && host_exists(host->path) ? kOk : kErr(kENOENT);
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

void register_kernel_file() {
    const char* k = "libkernel";
    REG(k, sceKernelOpen); REG(k, sceKernelClose); REG(k, sceKernelRead); REG(k, sceKernelWrite);
    REG(k, sceKernelLseek); REG(k, sceKernelStat); REG(k, sceKernelFstat); REG(k, sceKernelFsync);
    REG(k, sceKernelFtruncate); REG(k, sceKernelMkdir); REG(k, sceKernelRename); REG(k, sceKernelUnlink);
    REG(k, sceKernelCheckReachability); REG(k, _write); REG(k, _read);
}

}  // namespace dcp::hle
