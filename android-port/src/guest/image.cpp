#include "guest/image.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace guest {
namespace {

template <typename T>
bool rd(const std::vector<uint8_t>& d, uint64_t off, T& v) {
  if (off + sizeof(T) > d.size()) return false;
  std::memcpy(&v, d.data() + off, sizeof(T));
  return true;
}

constexpr uint32_t PT_LOAD = 1;

// Our inflate (RFC 1950 / 1951) for zlib-compressed SELF segments. Returns false on malformed data.
struct Inflater {
  const uint8_t* p;
  size_t n;
  std::vector<uint8_t>& out;
  size_t pos = 0;
  uint32_t bitbuf = 0;
  int bitcnt = 0;
  bool bad = false;

  struct Huff {
    uint16_t count[16] = {};
    uint16_t sym[320] = {};
  };

  int bits(int k) {
    while (bitcnt < k) {
      if (pos >= n) {
        bad = true;
        return 0;
      }
      bitbuf |= uint32_t(p[pos++]) << bitcnt;
      bitcnt += 8;
    }
    int v = int(bitbuf & ((1u << k) - 1));
    bitbuf >>= k;
    bitcnt -= k;
    return v;
  }
  // Canonical code from code lengths; false if over-subscribed (incomplete codes are allowed).
  static bool build(Huff& h, const uint8_t* len, int num) {
    uint16_t offs[16];
    for (int i = 0; i < 16; i++) h.count[i] = 0;
    for (int i = 0; i < num; i++) h.count[len[i]]++;
    h.count[0] = 0;
    int left = 1;
    for (int i = 1; i < 16; i++) {
      left = (left << 1) - h.count[i];
      if (left < 0) return false;
    }
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = uint16_t(offs[i] + h.count[i]);
    for (int i = 0; i < num; i++)
      if (len[i]) h.sym[offs[len[i]]++] = uint16_t(i);
    return true;
  }
  int decode(const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
      code |= bits(1);
      if (bad) return -1;
      int c = h.count[l];
      if (code - c < first) return h.sym[index + (code - first)];
      index += c;
      first = (first + c) << 1;
      code <<= 1;
    }
    bad = true;
    return -1;
  }
  bool codes(const Huff& lit, const Huff& dist) {
    static const uint16_t lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
      int s = decode(lit);
      if (s < 0) return false;
      if (s < 256) {
        out.push_back(uint8_t(s));
        continue;
      }
      if (s == 256) return true;
      s -= 257;
      if (s >= 29) return false;
      size_t len = lbase[s] + bits(lext[s]);
      int d = decode(dist);
      if (d < 0 || d >= 30) return false;
      size_t back = dbase[d] + bits(dext[d]);
      if (bad || back > out.size()) return false;
      size_t from = out.size() - back;
      for (size_t i = 0; i < len; i++) out.push_back(out[from + i]);
    }
  }
  bool run() {
    if (n < 2 || (p[0] & 0x0f) != 8 || ((p[0] << 8) | p[1]) % 31 || (p[1] & 0x20)) return false;  // zlib header
    pos = 2;
    for (int last = 0; !last;) {
      last = bits(1);
      int type = bits(2);
      if (bad) return false;
      if (type == 0) {  // stored
        bitbuf = 0;
        bitcnt = 0;
        if (pos + 4 > n) return false;
        size_t len = p[pos] | p[pos + 1] << 8;
        if ((len ^ size_t(p[pos + 2] | p[pos + 3] << 8)) != 0xffff || pos + 4 + len > n) return false;
        pos += 4;
        out.insert(out.end(), p + pos, p + pos + len);
        pos += len;
      } else if (type == 1) {  // fixed codes
        Huff lit, dist;
        uint8_t l[288];
        for (int i = 0; i < 288; i++) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        build(lit, l, 288);
        for (int i = 0; i < 30; i++) l[i] = 5;
        build(dist, l, 30);
        if (!codes(lit, dist)) return false;
      } else if (type == 2) {  // dynamic codes
        static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (bad || nlen > 286 || ndist > 30) return false;
        uint8_t l[19] = {};
        for (int i = 0; i < ncode; i++) l[order[i]] = uint8_t(bits(3));
        Huff lc;
        if (!build(lc, l, 19)) return false;
        uint8_t ll[320] = {};
        for (int i = 0; i < nlen + ndist;) {
          int s = decode(lc);
          if (s < 0) return false;
          if (s < 16) {
            ll[i++] = uint8_t(s);
            continue;
          }
          int rep, val = 0;
          if (s == 16) {
            if (!i) return false;
            val = ll[i - 1];
            rep = 3 + bits(2);
          } else if (s == 17) {
            rep = 3 + bits(3);
          } else {
            rep = 11 + bits(7);
          }
          if (i + rep > nlen + ndist) return false;
          while (rep--) ll[i++] = uint8_t(val);
        }
        Huff lit, dist;
        if (!build(lit, ll, nlen) || !build(dist, ll + nlen, ndist)) return false;
        if (!codes(lit, dist)) return false;
      } else {
        return false;
      }
    }
    return true;
  }
};

// ELF32 header + program headers starting at `elf` / `phdr` inside `d`. For a SELF, segment bytes come from the
// section-info table (`seginfo` != 0); for a plain ELF from p_offset.
bool parse(const std::vector<uint8_t>& d, uint64_t elf, uint64_t phdr, uint64_t seginfo, Image& out, std::string& err) {
  if (elf + 0x34 > d.size() || std::memcmp(d.data() + elf, "\x7f" "ELF", 4) != 0) return err = "no ELF header", false;
  if (d[elf + 4] != 1 || d[elf + 5] != 1) return err = "not a 32-bit little-endian ELF", false;
  uint16_t machine = 0, phnum = 0, phentsize = 0;
  rd(d, elf + 0x10, out.type);
  rd(d, elf + 0x12, machine);
  rd(d, elf + 0x18, out.entry);
  rd(d, elf + 0x2a, phentsize);
  rd(d, elf + 0x2c, phnum);
  if (machine != 40) return err = "not an ARM executable", false;
  // ET_SCE_RELEXEC is loaded at its linked addresses, where every stored pointer is already final (checked on
  // Dragon's Crown 1.09: all 436,811 ABS32 / THM_MOVW / THM_MOVT relocations match the stored values), so the
  // relocation segments are not applied.
  if (out.type != ET_SCE_EXEC && out.type != ET_SCE_RELEXEC && out.type != 2) return err = "unexpected ELF type", false;
  if (phentsize != 0x20) return err = "unexpected program header size", false;
  for (uint16_t i = 0; i < phnum; i++) {
    uint32_t p[8];
    if (!rd(d, phdr + i * 0x20ull, p)) return err = "truncated program headers", false;
    if (p[0] != PT_LOAD) continue;
    uint64_t off = p[1];
    if (seginfo) {
      uint64_t si[4];  // offset, length, compression (1 plain, 2 zlib), encryption (1 encrypted, 2 plain)
      if (!rd(d, seginfo + i * 0x20ull, si)) return err = "truncated SELF segment info", false;
      if (si[3] == 1) return err = "the eboot is still encrypted; decrypt it on the Vita first", false;
      if (si[2] == 2) {
        if (si[0] + si[1] > d.size()) return err = "segment out of range", false;
        Segment s;
        s.vaddr = p[2];
        s.memsz = p[5];
        s.flags = p[6];
        s.data.reserve(p[4]);
        Inflater z{d.data() + si[0], size_t(si[1]), s.data};
        if (!z.run() || s.data.size() != p[4] || p[4] > p[5]) return err = "corrupt compressed SELF segment", false;
        out.segments.push_back(std::move(s));
        continue;
      }
      if (si[2] != 1) return err = "unknown SELF segment compression", false;
      off = si[0];
    }
    if (p[4] > p[5] || off + p[4] > d.size()) return err = "segment out of range", false;
    Segment s;
    s.vaddr = p[2];
    s.memsz = p[5];
    s.flags = p[6];
    s.data.assign(d.begin() + off, d.begin() + off + p[4]);
    out.segments.push_back(std::move(s));
  }
  if (out.segments.empty()) return err = "no loadable segments", false;
  return true;
}

}  // namespace

bool load_image(const std::string& path, Image& out, std::string& err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return err = "cannot open " + path, false;
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  out = {};
  if (d.size() >= 0x60 && std::memcmp(d.data(), "SCE\0", 4) == 0) {
    uint64_t elf = 0, phdr = 0, seginfo = 0;
    rd(d, 0x40, elf);
    rd(d, 0x48, phdr);
    rd(d, 0x58, seginfo);
    return parse(d, elf, phdr, seginfo, out, err);
  }
  uint32_t phoff = 0;
  rd(d, 0x1c, phoff);
  return parse(d, 0, phoff, 0, out, err);
}

}  // namespace guest
