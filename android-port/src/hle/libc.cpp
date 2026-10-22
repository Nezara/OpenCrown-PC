// SceLibc: memory / string functions, the printf and scanf families, number parsing, C++ runtime hooks, rand.
// Guest memory is one contiguous host range, so a validated guest range is used through a plain host pointer.
#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>
#include <map>
#include <mutex>
#include <vector>

#include "hle/common.h"
#include "hle/io.h"

namespace hle {
namespace {

// --- guest memory helpers ------------------------------------------------------------------------------------
char* host(Cpu& c, uint32_t va, uint32_t n, const char* who) {
  if (!c.mem().valid(va, n)) {
    char b[96];
    std::snprintf(b, sizeof b, "%s: bad guest range 0x%08x+0x%x", who, va, n);
    c.set_fault(b);
    return nullptr;
  }
  return reinterpret_cast<char*>(c.mem().ptr(va));
}

// Length of the guest C string at va; faults (returns ~0u) if it runs into unmapped memory.
uint32_t gstrlen(Cpu& c, uint32_t va, const char* who) {
  for (uint32_t n = 0;; n++) {
    const uint8_t* p = c.mem().ptr(va + n);
    if (!p) {
      char b[96];
      std::snprintf(b, sizeof b, "%s: unterminated string at 0x%08x", who, va);
      c.set_fault(b);
      return ~0u;
    }
    if (!*p) return n;
  }
}

// Host view of a guest string, or nullptr after a fault.
const char* gstr(Cpu& c, uint32_t va, const char* who) {
  uint32_t n = gstrlen(c, va, who);
  return n == ~0u ? nullptr : reinterpret_cast<const char*>(c.mem().ptr(va));
}

// --- memory / string -----------------------------------------------------------------------------------------
void Memcpy(Cpu& c) {
  uint32_t d = arg(c, 0), s = arg(c, 1), n = arg(c, 2);
  char *hd = host(c, d, n, "memcpy"), *hs = host(c, s, n, "memcpy");
  if (hd && hs) std::memmove(hd, hs, n);  // memmove: overlapping guest memcpy must not corrupt host state
  ret(c, d);
}
void Memmove(Cpu& c) {
  uint32_t d = arg(c, 0), s = arg(c, 1), n = arg(c, 2);
  char *hd = host(c, d, n, "memmove"), *hs = host(c, s, n, "memmove");
  if (hd && hs) std::memmove(hd, hs, n);
  ret(c, d);
}
void Memset(Cpu& c) {
  uint32_t d = arg(c, 0), n = arg(c, 2);
  if (char* hd = host(c, d, n, "memset")) std::memset(hd, int(arg(c, 1)), n);
  ret(c, d);
}
void Memcmp(Cpu& c) {
  uint32_t n = arg(c, 2);
  char *a = host(c, arg(c, 0), n, "memcmp"), *b = host(c, arg(c, 1), n, "memcmp");
  ret(c, a && b ? uint32_t(std::memcmp(a, b, n)) : 0);
}
void Memchr(Cpu& c) {
  uint32_t s = arg(c, 0), n = arg(c, 2);
  char* hs = host(c, s, n, "memchr");
  const void* r = hs ? std::memchr(hs, int(arg(c, 1)), n) : nullptr;
  ret(c, r ? s + uint32_t(static_cast<const char*>(r) - hs) : 0);
}
void Strlen(Cpu& c) {
  uint32_t n = gstrlen(c, arg(c, 0), "strlen");
  ret(c, n == ~0u ? 0 : n);
}
void Strcmp(Cpu& c) {
  const char *a = gstr(c, arg(c, 0), "strcmp"), *b = gstr(c, arg(c, 1), "strcmp");
  ret(c, a && b ? uint32_t(std::strcmp(a, b)) : 0);
}
void Strncmp(Cpu& c) {  // bounded: the strings may end in unmapped memory past n
  uint32_t a = arg(c, 0), b = arg(c, 1), n = arg(c, 2);
  int r = 0;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *pa = c.mem().ptr(a + i), *pb = c.mem().ptr(b + i);
    if (!pa || !pb) return c.set_fault("strncmp: unmapped");
    if (*pa != *pb) { r = *pa - *pb; break; }
    if (!*pa) break;
  }
  ret(c, uint32_t(r));
}
void Strcasecmp(Cpu& c) {
  const char *a = gstr(c, arg(c, 0), "strcasecmp"), *b = gstr(c, arg(c, 1), "strcasecmp");
  ret(c, a && b ? uint32_t(strcasecmp(a, b)) : 0);
}
void Strncasecmp(Cpu& c) {
  uint32_t a = arg(c, 0), b = arg(c, 1), n = arg(c, 2);
  int r = 0;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *pa = c.mem().ptr(a + i), *pb = c.mem().ptr(b + i);
    if (!pa || !pb) return c.set_fault("strncasecmp: unmapped");
    int x = std::tolower(*pa), y = std::tolower(*pb);
    if (x != y) { r = x - y; break; }
    if (!x) break;
  }
  ret(c, uint32_t(r));
}
void Strcpy(Cpu& c) {
  uint32_t d = arg(c, 0), s = arg(c, 1), n = gstrlen(c, s, "strcpy");
  if (n == ~0u) return;
  if (char* hd = host(c, d, n + 1, "strcpy")) std::memmove(hd, c.mem().ptr(s), n + 1);
  ret(c, d);
}
void Strncpy(Cpu& c) {
  uint32_t d = arg(c, 0), s = arg(c, 1), n = arg(c, 2);
  char* hd = host(c, d, n, "strncpy");
  if (!hd) return;
  uint32_t i = 0;
  for (; i < n; i++) {
    const uint8_t* p = c.mem().ptr(s + i);
    if (!p) return c.set_fault("strncpy: unmapped source");
    if (!(hd[i] = char(*p))) break;
  }
  for (; i < n; i++) hd[i] = 0;
  ret(c, d);
}
void Strcat(Cpu& c) {
  uint32_t d = arg(c, 0), s = arg(c, 1);
  uint32_t dl = gstrlen(c, d, "strcat"), sl = gstrlen(c, s, "strcat");
  if (dl == ~0u || sl == ~0u) return;
  if (char* hd = host(c, d, dl + sl + 1, "strcat")) std::memmove(hd + dl, c.mem().ptr(s), sl + 1);
  ret(c, d);
}
void Strncat(Cpu& c) {
  uint32_t d = arg(c, 0), s = arg(c, 1), n = arg(c, 2);
  uint32_t dl = gstrlen(c, d, "strncat");
  if (dl == ~0u) return;
  uint32_t i = 0;
  for (; i < n; i++) {
    const uint8_t* p = c.mem().ptr(s + i);
    if (!p) return c.set_fault("strncat: unmapped source");
    if (!*p) break;
  }
  if (char* hd = host(c, d, dl + i + 1, "strncat")) {
    std::memmove(hd + dl, c.mem().ptr(s), i);
    hd[dl + i] = 0;
  }
  ret(c, d);
}
void Strstr(Cpu& c) {
  uint32_t h = arg(c, 0);
  const char *hh = gstr(c, h, "strstr"), *nn = gstr(c, arg(c, 1), "strstr");
  const char* r = hh && nn ? std::strstr(hh, nn) : nullptr;
  ret(c, r ? h + uint32_t(r - hh) : 0);
}
void Abs(Cpu& c) {
  int32_t v = int32_t(arg(c, 0));
  ret(c, uint32_t(v < 0 ? -v : v));
}

// --- printf family -------------------------------------------------------------------------------------------
// Formats guest `fmt` with arguments from `a` (ARM: int/long/pointer 32-bit, long long/double 64-bit).
// Returns false after a fault.
bool format(Cpu& c, uint32_t fmt, Args& a, std::string& out) {
  const char* f = gstr(c, fmt, "printf format");
  if (!f) return false;
  out.clear();
  while (*f) {
    if (*f != '%') { out += *f++; continue; }
    const char* start = f++;
    if (*f == '%') { out += '%'; f++; continue; }
    std::string spec = "%";
    while (*f && std::strchr("-+ #0", *f)) spec += *f++;
    auto num = [&](void) {
      if (*f == '*') { spec += std::to_string(int32_t(a.u32())); f++; }
      else while (std::isdigit(uint8_t(*f))) spec += *f++;
    };
    num();
    if (*f == '.') { spec += *f++; num(); }
    int len = 0;  // 0 int, 1 hh, 2 h, 3 l, 4 ll, 5 z/j/t (32-bit), 6 L
    if (f[0] == 'h' && f[1] == 'h') { len = 1; f += 2; }
    else if (*f == 'h') { len = 2; f++; }
    else if (f[0] == 'l' && f[1] == 'l') { len = 4; f += 2; }
    else if (*f == 'l') { len = 3; f++; }
    else if (*f == 'j' || *f == 'q') { len = 4; f++; }
    else if (*f == 'z' || *f == 't') { len = 5; f++; }
    else if (*f == 'L') { len = 6; f++; }
    char conv = *f ? *f++ : 0;
    char buf[512];
    switch (conv) {
      case 'd': case 'i': {
        int64_t v = len == 4 ? int64_t(a.u64()) : int64_t(int32_t(a.u32()));
        if (len == 1) v = int8_t(v);
        if (len == 2) v = int16_t(v);
        std::snprintf(buf, sizeof buf, (spec + PRId64).c_str(), v);
        out += buf;
        break;
      }
      case 'u': case 'x': case 'X': case 'o': {
        uint64_t v = len == 4 ? a.u64() : uint64_t(a.u32());
        if (len == 1) v = uint8_t(v);
        if (len == 2) v = uint16_t(v);
        const char* m = conv == 'u' ? PRIu64 : conv == 'x' ? PRIx64 : conv == 'X' ? PRIX64 : PRIo64;
        std::snprintf(buf, sizeof buf, (spec + m).c_str(), v);
        out += buf;
        break;
      }
      case 'c':
        std::snprintf(buf, sizeof buf, (spec + 'c').c_str(), int(a.u32()));
        out += buf;
        break;
      case 'p':
        std::snprintf(buf, sizeof buf, "0x%08x", a.u32());
        out += buf;
        break;
      case 's': {
        uint32_t s = a.u32();
        const char* hs = s ? gstr(c, s, "printf %s") : "(null)";
        if (!hs) return false;
        std::string r(std::snprintf(nullptr, 0, (spec + 's').c_str(), hs) + 1, '\0');
        std::snprintf(r.data(), r.size(), (spec + 's').c_str(), hs);
        out += r.c_str();
        break;
      }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
        uint64_t bits = a.u64();  // double (and long double, which is the same size on ARM)
        double d;
        std::memcpy(&d, &bits, 8);
        std::snprintf(buf, sizeof buf, (spec + conv).c_str(), d);
        out += buf;
        break;
      }
      case 'n': {
        uint32_t p = a.u32();
        if (len == 1) c.mem().write<uint8_t>(p, uint8_t(out.size()));
        else if (len == 2) c.mem().write<uint16_t>(p, uint16_t(out.size()));
        else if (len == 4) c.mem().write<uint64_t>(p, out.size());
        else c.mem().write<uint32_t>(p, uint32_t(out.size()));
        break;
      }
      default:
        out.append(start, f);  // unknown conversion: copy it through
        break;
    }
  }
  return true;
}

// Copies `s` into a guest buffer of `size` bytes (snprintf semantics) and returns the full length.
void put(Cpu& c, uint32_t dst, uint32_t size, const std::string& s) {
  if (size) {
    uint32_t n = std::min<uint32_t>(uint32_t(s.size()), size - 1);
    if (char* hd = host(c, dst, n + 1, "sprintf")) {
      std::memcpy(hd, s.data(), n);
      hd[n] = 0;
    }
  }
  ret(c, uint32_t(s.size()));
}

void Printf(Cpu& c) {
  Args a(c, 1);
  std::string s;
  if (!format(c, arg(c, 0), a, s)) return;
  std::fprintf(stdout, "[guest] %s", s.c_str());
  std::fflush(stdout);
  ret(c, uint32_t(s.size()));
}
// fprintf(FILE*, fmt, ...): the guest FILE objects are not modelled yet; everything goes to our stderr.
void Fprintf(Cpu& c) {
  Args a(c, 2);
  std::string s;
  if (!format(c, arg(c, 1), a, s)) return;
  std::fprintf(stderr, "[guest] %s", s.c_str());
  ret(c, uint32_t(s.size()));
}
void Sprintf(Cpu& c) {
  Args a(c, 2);
  std::string s;
  if (format(c, arg(c, 1), a, s)) put(c, arg(c, 0), uint32_t(s.size()) + 1, s);
}
void Snprintf(Cpu& c) {
  Args a(c, 3);
  std::string s;
  if (format(c, arg(c, 2), a, s)) put(c, arg(c, 0), arg(c, 1), s);
}
void Vsnprintf(Cpu& c) {
  Args a(c, arg(c, 3), true);
  std::string s;
  if (format(c, arg(c, 2), a, s)) put(c, arg(c, 0), arg(c, 1), s);
}

// --- heap: malloc / free -------------------------------------------------------------------------------------
// Our allocator over guest arenas (16 MiB each, from Memory::alloc): first fit on a free list, 16-byte aligned,
// neighbours merged on free.
std::mutex heap_lock;
std::map<uint32_t, uint32_t> heap_free, heap_used;  // address -> size
constexpr uint32_t ARENA = 16u << 20;

uint32_t heap_alloc(Memory& m, uint32_t n) {
  n = std::max<uint32_t>(16, (n + 15) & ~15u);
  std::lock_guard lk(heap_lock);
  for (int pass = 0; pass < 2; pass++) {
    for (auto it = heap_free.begin(); it != heap_free.end(); ++it) {
      if (it->second < n) continue;
      uint32_t a = it->first, left = it->second - n;
      heap_free.erase(it);
      if (left) heap_free[a + n] = left;
      heap_used[a] = n;
      return a;
    }
    uint32_t size = std::max(ARENA, (n + 0xfff) & ~0xfffu);
    uint32_t a = m.alloc(size, "SceLibc heap");
    if (!a) return 0;
    heap_free[a] = size;
  }
  return 0;
}
void heap_release(uint32_t a) {
  std::lock_guard lk(heap_lock);
  auto it = heap_used.find(a);
  if (it == heap_used.end()) return;
  uint32_t size = it->second;
  heap_used.erase(it);
  auto nx = heap_free.lower_bound(a);
  if (nx != heap_free.end() && a + size == nx->first) {
    size += nx->second;
    nx = heap_free.erase(nx);
  }
  if (nx != heap_free.begin()) {
    auto pv = std::prev(nx);
    if (pv->first + pv->second == a) {
      pv->second += size;
      return;
    }
  }
  heap_free[a] = size;
}
void Malloc(Cpu& c) { ret(c, heap_alloc(c.mem(), arg(c, 0))); }
void Free(Cpu& c) {
  if (arg(c, 0)) heap_release(arg(c, 0));
}

// --- stdio: fopen / fread / fclose over host files --------------------------------------------------------------
// A guest FILE* is a 16-byte slot in one guest page; slot i is host file i.
uint32_t file_slots = 0;
std::FILE* files[256] = {};

void Fopen(Cpu& c) {
  const char* path = gstr(c, arg(c, 0), "fopen");
  const char* mode = gstr(c, arg(c, 1), "fopen");
  if (!path || !mode) return;
  std::string m = mode;
  bool write = m.find_first_of("wa+") != std::string::npos;
  std::string host = io_host_path(path, write);
  std::FILE* f = host.empty() ? nullptr : std::fopen(host.c_str(), (m.find('b') == std::string::npos ? m + "b" : m).c_str());
  HLE_NOTE("fopen(\"%s\", \"%s\") -> %s\n", path, mode, f ? "ok" : "failed");
  if (!f) return ret(c, 0);
  std::lock_guard lk(heap_lock);
  if (!file_slots) file_slots = c.mem().alloc(0x1000, "FILE slots");
  for (uint32_t i = 0; i < 256; i++)
    if (!files[i]) {
      files[i] = f;
      return ret(c, file_slots + 16 * i);
    }
  std::fclose(f);
  ret(c, 0);
}
std::FILE* file_of(uint32_t fp) {
  if (!file_slots || fp < file_slots || (fp - file_slots) % 16 || (fp - file_slots) / 16 >= 256) return nullptr;
  return files[(fp - file_slots) / 16];
}
// size_t fread(void* buf, size_t size, size_t n, FILE* fp)
void Fread(Cpu& c) {
  std::FILE* f = file_of(arg(c, 3));
  uint32_t size = arg(c, 1), n = arg(c, 2);
  if (!f || !size || !n) return ret(c, 0);
  char* dst = host(c, arg(c, 0), size * n, "fread");
  if (!dst) return;
  ret(c, uint32_t(std::fread(dst, size, n, f)));
}
void Fclose(Cpu& c) {
  std::FILE* f = file_of(arg(c, 0));
  if (!f) return ret(c, uint32_t(-1));
  std::fclose(f);
  files[(arg(c, 0) - file_slots) / 16] = nullptr;
  ret(c, 0);
}
// vfprintf(FILE*, fmt, va_list): like fprintf, to our stderr.
void Vfprintf(Cpu& c) {
  Args a(c, arg(c, 2), true);
  std::string s;
  if (!format(c, arg(c, 1), a, s)) return;
  std::fprintf(stderr, "[guest] %s", s.c_str());
  ret(c, uint32_t(s.size()));
}
void ClibPrintf(Cpu& c) { Printf(c); }

void StackChkFail(Cpu& c) { c.set_fault("guest stack check failed (__stack_chk_fail)"); }
void Ldiv0(Cpu& c) { ret64(c, 0); }   // __sce_aeabi_ldiv0: the result of a 64-bit division by zero
void AtQuickExit(Cpu& c) { ret(c, 0); }  // quick_exit handlers are never run

// --- SceLibstdcxx -----------------------------------------------------------------------------------------------
// Static-initialiser hooks of the C++ streams (std::ios_base::Init, std::_Winit): the game never uses the standard
// streams beyond taking cout / cerr addresses, so these do nothing and return `this`.
void ReturnThis(Cpu& c) { ret(c, arg(c, 0)); }
void IosClear(Cpu&) {}
// std::_Syserror_map(int): message for an errno value; one fixed guest string.
uint32_t syserror_text = 0;
void SyserrorMap(Cpu& c) {
  if (!syserror_text) {
    syserror_text = c.mem().alloc(0x1000, "_Syserror_map");
    c.mem().copy_in(syserror_text, "unknown error", 14);
  }
  ret(c, syserror_text);
}

// --- sscanf --------------------------------------------------------------------------------------------------
// Each conversion runs through the host sscanf on its own (with %n to learn how much it consumed), and the result
// is stored at the guest pointer with the guest's width.
void Sscanf(Cpu& c) {
  const char *in = gstr(c, arg(c, 0), "sscanf"), *f = gstr(c, arg(c, 1), "sscanf");
  if (!in || !f) return;
  Args a(c, 2);
  const char* p = in;
  int assigned = 0;
  bool any = false;
  while (*f) {
    if (std::isspace(uint8_t(*f))) {
      while (std::isspace(uint8_t(*p))) p++;
      f++;
      continue;
    }
    if (*f != '%' || f[1] == '%') {
      if (*f == '%') f++;
      if (*p != *f) break;
      p++, f++;
      continue;
    }
    std::string spec = "%";
    f++;
    bool suppress = *f == '*';
    if (suppress) f++;
    while (std::isdigit(uint8_t(*f))) spec += *f++;
    int len = 0;  // 1 hh, 2 h, 3 l, 4 ll
    if (f[0] == 'h' && f[1] == 'h') { len = 1; f += 2; }
    else if (*f == 'h') { len = 2; f++; }
    else if (f[0] == 'l' && f[1] == 'l') { len = 4; f += 2; }
    else if (*f == 'l') { len = 3; f++; }
    char conv = *f ? *f++ : 0;
    if (conv == 'n') {
      if (!suppress) c.mem().write<uint32_t>(a.u32(), uint32_t(p - in));
      continue;
    }
    if (!*p && conv != 'c') break;
    int used = 0;
    if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
      long long v = 0;
      char fmt2[32];
      char hc = conv == 'X' ? 'x' : conv;
      std::snprintf(fmt2, sizeof fmt2, "%sll%c%%n", spec.c_str(), hc);
      if (std::sscanf(p, fmt2, &v, &used) < 1) break;
      if (!suppress) {
        uint32_t dst = a.u32();
        if (len == 1) c.mem().write<uint8_t>(dst, uint8_t(v));
        else if (len == 2) c.mem().write<uint16_t>(dst, uint16_t(v));
        else if (len == 4) c.mem().write<uint64_t>(dst, uint64_t(v));
        else c.mem().write<uint32_t>(dst, uint32_t(v));
      }
    } else if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'E' || conv == 'G') {
      double v = 0;
      if (std::sscanf(p, (spec + "lf%n").c_str(), &v, &used) < 1) break;
      if (!suppress) {
        uint32_t dst = a.u32();
        if (len == 3) c.mem().write<double>(dst, v);
        else c.mem().write<float>(dst, float(v));
      }
    } else if (conv == 's' || conv == 'c') {
      std::string tmp(4096, '\0');
      if (std::sscanf(p, (spec + conv + "%n").c_str(), tmp.data(), &used) < 1) break;
      if (!suppress) {
        uint32_t dst = a.u32();
        uint32_t n = conv == 's' ? uint32_t(std::strlen(tmp.c_str())) + 1 : uint32_t(used);
        if (char* hd = host(c, dst, n, "sscanf")) std::memcpy(hd, tmp.data(), n);
      }
    } else {
      HLE_NOTE("sscanf: conversion %%%c not supported\n", conv);
      break;
    }
    p += used;
    any = true;
    if (!suppress) assigned++;
  }
  ret(c, uint32_t(!any && !*p ? -1 : assigned));
}

// --- number parsing (Dinkumware internals) -------------------------------------------------------------------
// double _Stod(const char* s, char** endptr, long pten): result in d0 (hard-float, call site 0x813c1b1a).
void Stod(Cpu& c) {
  uint32_t s = arg(c, 0), endp = arg(c, 1);
  int32_t pten = int32_t(arg(c, 2));
  const char* hs = gstr(c, s, "_Stod");
  if (!hs) return;
  char* end = nullptr;
  double v = std::strtod(hs, &end);
  if (pten) HLE_NOTE("_Stod: pten %d ignored\n", pten);
  if (endp) c.mem().write<uint32_t>(endp, s + uint32_t(end - hs));
  c.set_dreg(0, v);
}
// unsigned long _Stoul(const char* s, char** endptr, int base)
void Stoul(Cpu& c) {
  uint32_t s = arg(c, 0), endp = arg(c, 1);
  const char* hs = gstr(c, s, "_Stoul");
  if (!hs) return;
  char* end = nullptr;
  unsigned long long v = std::strtoull(hs, &end, int(arg(c, 2)));
  if (endp) c.mem().write<uint32_t>(endp, s + uint32_t(end - hs));
  ret(c, v > 0xffffffffull ? 0xffffffffu : uint32_t(v));
}

// --- C++ runtime ---------------------------------------------------------------------------------------------
uint32_t atexit_count = 0;
void SetDsoHandleMain(Cpu&) {}
// __aeabi_atexit(obj, dtor, dso) / __cxa_atexit(dtor, obj, dso): destructors at exit are not run (yet).
void AeabiAtexit(Cpu& c) { atexit_count++, ret(c, 0); }
void CxaAtexit(Cpu& c) { atexit_count++, ret(c, 0); }
// Static-local guards (one guest thread for now): acquire returns 1 when the caller must run the initialiser.
void GuardAcquire(Cpu& c) { ret(c, (c.mem().read<uint8_t>(arg(c, 0)) & 1) ? 0 : 1); }
void GuardRelease(Cpu& c) { c.mem().write<uint32_t>(arg(c, 0), 1); }
void LdTlsRegisterModuleInfo(Cpu& c) {
  HLE_NOTE("_sceLdTlsRegisterModuleInfo: module TLS not set up yet\n");
  ret(c, 0);
}

void Exit(Cpu& c) { c.set_fault("guest called exit(" + std::to_string(int32_t(arg(c, 0))) + ")"); }
void Abort(Cpu& c) { c.set_fault("guest called abort()"); }
// qsort(base, n, size, cmp): sorted here with the guest comparator called through Cpu::call. The element order of
// equal keys may differ from SceLibc's own qsort (stable merge sort here; unverified what SceLibc uses).
void Qsort(Cpu& c) {
  uint32_t base = arg(c, 0), n = arg(c, 1), size = arg(c, 2), cmp = arg(c, 3);
  if (n < 2 || size == 0) return;
  if (uint64_t(n) * size > 0x10000000u) return c.set_fault("qsort: array too large");
  char* hb = host(c, base, n * size, "qsort");
  if (!hb) return;
  // Sort a copy of the elements by index, comparing them in place through a scratch area on the guest stack.
  std::vector<char> copy(hb, hb + size_t(n) * size);
  std::vector<uint32_t> idx(n);
  for (uint32_t i = 0; i < n; i++) idx[i] = i;
  bool failed = false;
  std::stable_sort(idx.begin(), idx.end(), [&](uint32_t x, uint32_t y) {
    if (failed) return false;
    int32_t r = int32_t(c.call(cmp, {base + x * size, base + y * size}));
    if (!c.fault().empty()) failed = true;
    return r < 0;
  });
  if (failed) return;
  for (uint32_t i = 0; i < n; i++) std::memcpy(hb + size_t(i) * size, copy.data() + size_t(idx[i]) * size, size);
}

// --- rand ----------------------------------------------------------------------------------------------------
// Dinkumware's LCG (unverified against the Vita SceLibc): seed = seed * 1103515245 + 12345, result bits 30:16.
uint32_t rand_seed = 1;
void Rand(Cpu& c) {
  rand_seed = rand_seed * 1103515245u + 12345u;
  ret(c, (rand_seed >> 16) & 0x7fff);
}
void Srand(Cpu& c) { rand_seed = arg(c, 0); }

}  // namespace

uint32_t guest_malloc(Memory& m, uint32_t n) { return heap_alloc(m, n); }
void guest_free(uint32_t a) {
  if (a) heap_release(a);
}

// --- variables (NIDs from the vitasdk db, SceLibc) -------------------------------------------------------------
// _Touptab / _Tolotab: Dinkumware `const short*` pointing at a 257-entry table whose element [-1] is EOF, so code
// indexes it with an unsigned char directly. C locale: only a-z / A-Z map. _Stderr: the FILE object itself; our
// stdio treats every FILE as the host stderr, so it is just a zeroed block.
// _Ctype: Dinkumware `const short*` to the classification table, indexed with a *signed* char (asm 0x810410aa:
// ldrsb then ldrsh [table, c, lsl #1]), so 128 entries sit before index 0 (zero: no class) plus EOF at [-1].
// Flags as the game tests them: 0x20 digit, 0x02 upper, 0x10 lower (hex-digit conversion at 0x810410b8).
uint32_t ctype_table(Memory& m) {
  enum : int16_t { XD = 0x01, UP = 0x02, SP = 0x04, PU = 0x08, LO = 0x10, DI = 0x20, CN = 0x40, BB = 0x80 };
  uint32_t t = m.alloc(0x1000, "_Ctype");
  if (!t) return 0;
  uint32_t zero = t + 0x10 + 2 * 128;
  for (int ch = 0; ch < 128; ch++) {
    int16_t v = 0;
    if (ch < 32 || ch == 127) v |= BB;
    if (ch >= 9 && ch <= 13) v |= CN;
    if (ch == ' ') v |= SP;
    if (ch >= '0' && ch <= '9') v |= DI | XD;
    if (ch >= 'A' && ch <= 'Z') v |= UP | (ch <= 'F' ? XD : 0);
    if (ch >= 'a' && ch <= 'z') v |= LO | (ch <= 'f' ? XD : 0);
    if (ch > 32 && ch < 127 && !(v & (DI | UP | LO))) v |= PU;
    m.write<int16_t>(zero + 2 * uint32_t(ch), v);
  }
  m.write<uint32_t>(t, zero);
  return t;
}

uint32_t case_table(Memory& m, bool upper) {
  uint32_t t = m.alloc(0x1000, upper ? "_Touptab" : "_Tolotab");
  if (!t) return 0;
  m.write<int16_t>(t + 0x10, -1);  // EOF
  uint32_t first = t + 0x12;
  for (int ch = 0; ch < 256; ch++) {
    int v = ch;
    if (upper && ch >= 'a' && ch <= 'z') v = ch - 32;
    if (!upper && ch >= 'A' && ch <= 'Z') v = ch + 32;
    m.write<int16_t>(first + 2 * uint32_t(ch), int16_t(v));
  }
  m.write<uint32_t>(t, first);  // the variable: the pointer
  return t;
}

void register_libc() {
  guest::hle_register_var(0x36878958, "_Touptab", [](Memory& m) { return case_table(m, true); });
  guest::hle_register_var(0xD662E07C, "_Tolotab", [](Memory& m) { return case_table(m, false); });
  guest::hle_register_var(0xDDF9BB09, "_Stderr", [](Memory& m) { return m.alloc(0x1000, "_Stderr"); });
  guest::hle_register_var(0x3CE6109D, "_Ctype", ctype_table);
  // std::cout / std::cerr: only their addresses are taken (stream objects never used through imports).
  guest::hle_register_var(0xB3867BB4, "_ZSt4cout", [](Memory& m) { return m.alloc(0x1000, "std::cout"); });
  guest::hle_register_var(0x14B35A37, "_ZSt4cerr", [](Memory& m) { return m.alloc(0x1000, "std::cerr"); });
  // SceLibKernel __stack_chk_guard: the canary value the game's function epilogues compare.
  guest::hle_register_var(0x93B8AA67, "__stack_chk_guard", [](Memory& m) {
    uint32_t a = m.alloc(0x1000, "__stack_chk_guard");
    if (a) m.write<uint32_t>(a, 0x2f8a6b1du);
    return a;
  });
  REG(malloc, Malloc);
  REG(free, Free);
  REG(fopen, Fopen);
  REG(fread, Fread);
  REG(fclose, Fclose);
  REG(vfprintf, Vfprintf);
  REG(sceClibPrintf, ClibPrintf);
  REG(__stack_chk_fail, StackChkFail);
  REG(__sce_aeabi_ldiv0, Ldiv0);
  REG(__at_quick_exit, AtQuickExit);
  REG(_ZNSt8ios_base4InitC1Ev, ReturnThis);
  REG(_ZNSt8ios_base4InitD1Ev, ReturnThis);
  REG(_ZNSt6_WinitC1Ev, ReturnThis);
  REG(_ZNSt6_WinitD1Ev, ReturnThis);
  REG(_ZNSt8ios_base5clearENSt5_IosbIiE8_IostateEb, IosClear);
  REG(_ZSt13_Syserror_mapi, SyserrorMap);
  REG(memcpy, Memcpy);
  REG(memmove, Memmove);
  REG(memset, Memset);
  REG(memcmp, Memcmp);
  REG(memchr, Memchr);
  REG(strlen, Strlen);
  REG(strcmp, Strcmp);
  REG(strncmp, Strncmp);
  REG(strcasecmp, Strcasecmp);
  REG(strncasecmp, Strncasecmp);
  REG(strcpy, Strcpy);
  REG(strncpy, Strncpy);
  REG(strcat, Strcat);
  REG(strncat, Strncat);
  REG(strstr, Strstr);
  REG(abs, Abs);
  REG(printf, Printf);
  REG(fprintf, Fprintf);
  REG(sprintf, Sprintf);
  REG(snprintf, Snprintf);
  REG(vsnprintf, Vsnprintf);
  REG(sscanf, Sscanf);
  REG(_Stod, Stod);
  REG(_Stoul, Stoul);
  REG(__cxa_set_dso_handle_main, SetDsoHandleMain);
  REG(__aeabi_atexit, AeabiAtexit);
  REG(__cxa_atexit, CxaAtexit);
  REG(__cxa_guard_acquire, GuardAcquire);
  REG(__cxa_guard_release, GuardRelease);
  REG(_sceLdTlsRegisterModuleInfo, LdTlsRegisterModuleInfo);
  REG(exit, Exit);
  REG(abort, Abort);
  REG(qsort, Qsort);
  REG(rand, Rand);
  REG(srand, Srand);
}

}  // namespace hle
