// SceIofilemgr: guest paths ("app0:DRACRO.CPK", "savedata0:/x") mapped onto host directories. app0: is a stack
// of read-only roots (the update first, then the base game, like the Vita's patch overlay); the other devices are
// writable host directories. Files are host FILE*s; one lock guards the table and every transfer.
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "hle/common.h"
#include "hle/io.h"

namespace fs = std::filesystem;

namespace hle {
namespace {

// psp2/kernel/iofilemgr.h + SCE_ERROR_ERRNO_* (vitasdk).
enum : uint32_t {
  SCE_O_RDONLY = 0x0001,
  SCE_O_WRONLY = 0x0002,
  SCE_O_APPEND = 0x0100,
  SCE_O_CREAT = 0x0200,
  SCE_O_TRUNC = 0x0400,
  SCE_O_EXCL = 0x0800,
  ERR_ENOENT = 0x80010002,
  ERR_EIO = 0x80010005,
  ERR_EBADF = 0x80010009,
  ERR_EACCES = 0x8001000d,
  ERR_EEXIST = 0x80010011,
  ERR_EISDIR = 0x80010015,
  ERR_EINVAL = 0x80010016,
  ERR_EMFILE = 0x80010018,
  ERR_ENAMETOOLONG = 0x8001005b,
};

struct Device {
  std::vector<fs::path> roots;  // searched in order
  bool writable;
};
std::map<std::string, Device> devices;

struct File {
  std::FILE* f;
  std::string guest;
};
std::mutex io_lock;
std::map<uint32_t, File> files;  // io_lock
uint32_t next_fd = 0x100001;     // odd, positive

int64_t tell(std::FILE* f) {
#ifdef _WIN32
  return _ftelli64(f);
#else
  return ftello(f);
#endif
}
bool seek(std::FILE* f, int64_t off, int whence) {
#ifdef _WIN32
  return _fseeki64(f, off, whence) == 0;
#else
  return fseeko(f, off, whence) == 0;
#endif
}

// "dev:rest" -> device + relative path; ".." components are refused so a guest path can't leave its root.
bool split(const std::string& guest, const Device*& dev, fs::path& rel) {
  size_t colon = guest.find(':');
  if (colon == std::string::npos) return false;
  auto it = devices.find(guest.substr(0, colon));
  if (it == devices.end()) return false;
  dev = &it->second;
  rel.clear();
  for (const fs::path& part : fs::path(guest.substr(colon + 1)).relative_path()) {
    if (part == "..") return false;
    if (part != "." && part != "/" && !part.empty()) rel /= part;
  }
  return true;
}

// Host path of an existing file (first root that has it), or where a new file would go (first root).
fs::path resolve(const Device& dev, const fs::path& rel, bool must_exist) {
  std::error_code ec;
  for (const fs::path& root : dev.roots)
    if (fs::exists(root / rel, ec)) return root / rel;
  return must_exist || dev.roots.empty() ? fs::path() : dev.roots.front() / rel;
}

std::string guest_path(Cpu& c, uint32_t va, bool& too_long) {
  std::string s = str(c, va, 1025);
  too_long = s.size() > 1024;
  return s;
}

// SceUID sceIoOpen(const char* path, int flags, SceMode mode)
void Open(Cpu& c) {
  bool too_long;
  std::string guest = guest_path(c, arg(c, 0), too_long);
  uint32_t flags = arg(c, 1);
  if (too_long) return ret(c, ERR_ENAMETOOLONG);
  const Device* dev;
  fs::path rel;
  if (!split(guest, dev, rel)) {
    HLE_NOTE("sceIoOpen(\"%s\"): unknown device or bad path\n", guest.c_str());
    return ret(c, ERR_ENOENT);
  }
  // Save data opens are rare and decide whether the game sees a save: logged with their result.
  auto done = [&](uint32_t r) {
    if (dev->writable) HLE_NOTE("sceIoOpen(\"%s\", 0x%x) -> 0x%08x\n", guest.c_str(), flags, r);
    ret(c, r);
  };
  bool write = flags & SCE_O_WRONLY;
  if (write && !dev->writable) return done(ERR_EACCES);
  fs::path host = resolve(*dev, rel, !(flags & SCE_O_CREAT));
  std::error_code ec;
  bool exists = !host.empty() && fs::exists(host, ec);
  if (host.empty() || (!exists && !(flags & SCE_O_CREAT))) return done(ERR_ENOENT);
  if (exists && (flags & SCE_O_CREAT) && (flags & SCE_O_EXCL)) return done(ERR_EEXIST);
  if (exists && fs::is_directory(host, ec)) return done(ERR_EISDIR);

  const char* mode = "rb";
  if (write) {
    if (!exists || (flags & SCE_O_TRUNC)) mode = (flags & SCE_O_RDONLY) ? "w+b" : "wb";
    else mode = "r+b";
  }
  std::FILE* f = std::fopen(host.string().c_str(), mode);
  if (!f) return done(ERR_EIO);
  if (flags & SCE_O_APPEND) seek(f, 0, SEEK_END);
  std::lock_guard lk(io_lock);
  uint32_t fd = next_fd;
  next_fd += 2;
  files[fd] = {f, guest};
  done(fd);
}

void Close(Cpu& c) {
  std::lock_guard lk(io_lock);
  auto it = files.find(arg(c, 0));
  if (it == files.end()) return ret(c, ERR_EBADF);
  std::fclose(it->second.f);
  files.erase(it);
  ret(c, 0);
}

// Reads / writes at the current position, or at `off` (pread) leaving the position alone.
uint32_t transfer(Cpu& c, uint32_t fd, uint32_t buf, uint32_t n, bool write, int64_t off = -1) {
  if (int32_t(n) < 0) return ERR_EINVAL;
  if (n && !c.mem().valid(buf, n)) return ERR_EINVAL;
  std::lock_guard lk(io_lock);
  auto it = files.find(fd);
  if (it == files.end()) return ERR_EBADF;
  std::FILE* f = it->second.f;
  int64_t saved = off >= 0 ? tell(f) : 0;
  if (off >= 0 && !seek(f, off, SEEK_SET)) return ERR_EINVAL;
  seek(f, 0, SEEK_CUR);  // switching between read and write on one FILE needs a positioning call
  size_t done = n ? (write ? std::fwrite(c.mem().ptr(buf), 1, n, f) : std::fread(c.mem().ptr(buf), 1, n, f)) : 0;
  if (write) std::fflush(f);
  if (off >= 0) seek(f, saved, SEEK_SET);
  if (done < n && std::ferror(f)) {
    std::clearerr(f);
    return ERR_EIO;
  }
  return uint32_t(done);
}

void Read(Cpu& c) { ret(c, transfer(c, arg(c, 0), arg(c, 1), arg(c, 2), false)); }
void Write(Cpu& c) { ret(c, transfer(c, arg(c, 0), arg(c, 1), arg(c, 2), true)); }

// int sceIoPread(SceUID fd, void* buf, SceSize n, SceOff offset): the 64-bit offset is on the stack.
void Pread(Cpu& c) {
  int64_t off = int64_t(c.mem().read<uint64_t>(c.reg(13)));
  if (off < 0) return ret(c, ERR_EINVAL);
  ret(c, transfer(c, arg(c, 0), arg(c, 1), arg(c, 2), false, off));
}

// SceOff sceIoLseek(SceUID fd, SceOff offset, int whence): offset in r2:r3, whence on the stack.
void Lseek(Cpu& c) {
  uint32_t fd = arg(c, 0);
  int64_t off = int64_t(uint64_t(c.reg(2)) | uint64_t(c.reg(3)) << 32);
  int whence = int(arg(c, 4));
  if (whence < 0 || whence > 2) return ret64(c, uint64_t(int64_t(int32_t(ERR_EINVAL))));
  std::lock_guard lk(io_lock);
  auto it = files.find(fd);
  if (it == files.end()) return ret64(c, uint64_t(int64_t(int32_t(ERR_EBADF))));
  std::FILE* f = it->second.f;
  int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? tell(f) : (seek(f, 0, SEEK_END), tell(f));
  if (base + off < 0 || !seek(f, base + off, SEEK_SET)) return ret64(c, uint64_t(int64_t(int32_t(ERR_EINVAL))));
  ret64(c, uint64_t(base + off));
}

// SceDateTime: u16 year, month, day, hour, minute, second; u32 microsecond (UTC).
void put_datetime(Cpu& c, uint32_t va, std::time_t t) {
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  uint16_t v[6] = {uint16_t(tm.tm_year + 1900), uint16_t(tm.tm_mon + 1), uint16_t(tm.tm_mday),
                   uint16_t(tm.tm_hour),        uint16_t(tm.tm_min),     uint16_t(tm.tm_sec)};
  for (int i = 0; i < 6; i++) c.mem().write<uint16_t>(va + 2 * i, v[i]);
  c.mem().write<uint32_t>(va + 12, 0);
}

// int sceIoGetstat(const char* path, SceIoStat* st): mode, attr, size (s64 at 8), ctime/atime/mtime at
// 0x10/0x20/0x30, private[6] up to 0x58.
void Getstat(Cpu& c) {
  bool too_long;
  std::string guest = guest_path(c, arg(c, 0), too_long);
  uint32_t st = arg(c, 1);
  if (too_long) return ret(c, ERR_ENAMETOOLONG);
  if (!c.mem().valid(st, 0x58)) return ret(c, ERR_EINVAL);
  const Device* dev;
  fs::path rel;
  if (!split(guest, dev, rel)) return ret(c, ERR_ENOENT);
  fs::path host = resolve(*dev, rel, true);
  std::error_code ec;
  if (host.empty()) return ret(c, ERR_ENOENT);
  bool dir = fs::is_directory(host, ec);
  uint64_t size = dir ? 0 : fs::file_size(host, ec);
  auto ft = fs::last_write_time(host, ec);
  // file_clock -> system_clock through "now" on both clocks (clock_cast is missing from this libstdc++).
  auto sys = std::chrono::system_clock::now() +
             std::chrono::duration_cast<std::chrono::system_clock::duration>(ft - fs::file_time_type::clock::now());
  std::time_t mtime = ec ? 0 : std::chrono::system_clock::to_time_t(sys);
  std::memset(c.mem().ptr(st), 0, 0x58);
  // SCE_S_IFDIR 0x1000 / SCE_S_IFREG 0x2000, read (+ write) for user: SCE_S_IRUSR 0x100, SCE_S_IWUSR 0x80.
  uint32_t mode = (dir ? 0x1000 : 0x2000) | 0x100 | (dev->writable ? 0x80 : 0) | (dir ? 0x40 : 0);
  c.mem().write<uint32_t>(st, mode);
  c.mem().write<uint32_t>(st + 4, dir ? 0x10 : 0x20);  // SCE_SO_IFDIR / SCE_SO_IFREG
  c.mem().write<uint64_t>(st + 8, size);
  for (uint32_t o : {0x10u, 0x20u, 0x30u}) put_datetime(c, st + o, mtime);
  ret(c, 0);
}

void Remove(Cpu& c) {
  bool too_long;
  std::string guest = guest_path(c, arg(c, 0), too_long);
  if (too_long) return ret(c, ERR_ENAMETOOLONG);
  const Device* dev;
  fs::path rel;
  if (!split(guest, dev, rel)) return ret(c, ERR_ENOENT);
  if (!dev->writable) return ret(c, ERR_EACCES);
  fs::path host = resolve(*dev, rel, true);
  std::error_code ec;
  if (host.empty()) return ret(c, ERR_ENOENT);
  if (fs::is_directory(host, ec)) return ret(c, ERR_EISDIR);
  ret(c, fs::remove(host, ec) ? 0u : uint32_t(ERR_EIO));
}

}  // namespace

std::string io_host_path(const std::string& guest, bool create) {
  const Device* dev;
  fs::path rel;
  if (!split(guest, dev, rel)) return {};
  fs::path host = resolve(*dev, rel, !create);
  return host.empty() ? std::string() : host.string();
}

void io_mount(const std::string& device, const std::string& host_dir, bool writable) {
  Device& d = devices[device];
  d.writable = writable;
  d.roots.push_back(fs::path(host_dir));
  if (writable) {
    std::error_code ec;
    fs::create_directories(host_dir, ec);
  }
}

void register_io() {
  REG(sceIoOpen, Open);
  REG(sceIoClose, Close);
  REG(sceIoRead, Read);
  REG(sceIoWrite, Write);
  REG(sceIoPread, Pread);
  REG(sceIoLseek, Lseek);
  REG(sceIoGetstat, Getstat);
  REG(sceIoRemove, Remove);
}

}  // namespace hle
