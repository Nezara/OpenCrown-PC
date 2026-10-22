// GXM texture decode (see texture.h).
//
// Textures the game sets (DCP_LOG_TEXTURES, ~70 s headless, reaches the menu; levels may add more, so the common
// formats are covered too and anything else reports "unsupported"):
//   base format  swizzle  type      sizes seen                           mips  count
//   UBC3 (BC3)   ABGR     swizzled  256x128, 256x256, 512x512, 2048x2048  1     4
// Layout facts (vitasdk psp2/gxm.h; Vita3K textures.cpp read for field positions only):
//   word0 mip filter 9, min 10-11, mag 12-13, mips-1 17-20 (as our gxm.cpp stores it), format bit 31
//   word1 pot types: log2 h 0-3, log2 w 16-19; others: h-1 0-11, w-1 12-23; base format 24-28; type 29-31
//   word2 data address; word3 palette >> 6 in 0-25, swizzle 28-30
// Morton order checked on the game's own BC3 textures (2026-10-20, 256x256 / 512x512 / 256x128): y in the even bits.
// Unverified: mip chain packing, the strided stride field, every format except BC3. Cube and tiled are rejected.
#include "gpu/texture.h"

#include <algorithm>
#include <cstring>

namespace gpu::tex {
namespace {

enum Base : uint32_t {
  U8 = 0x00, U4U4U4U4 = 0x02, U5U6U5 = 0x05, U8U8 = 0x07, U8U8U8U8 = 0x0C, F16F16F16F16 = 0x1B,
  UBC1 = 0x85, UBC2 = 0x86, UBC3 = 0x87, P4 = 0x94, P8 = 0x95, U8U8U8 = 0x98
};

// Source field of R,G,B,A for the 4-component swizzles ABGR..BGR1 (names list the fields high to low):
// 0 = lowest field, -1 = constant 1.
constexpr int kSwz4[8][4] = {{0, 1, 2, 3}, {2, 1, 0, 3}, {3, 2, 1, 0}, {1, 2, 3, 0},
                             {0, 1, 2, -1}, {2, 1, 0, -1}, {3, 2, 1, -1}, {1, 2, 3, -1}};
constexpr Comp kSlot[4] = {Comp::R, Comp::G, Comp::B, Comp::A};

Swizzle swizzle4(uint32_t code) {
  const int* s = kSwz4[code & 7];
  auto c = [&](int i) { return s[i] < 0 ? Comp::One : kSlot[s[i]]; };
  return {c(0), c(1), c(2), c(3)};
}
// Swizzle1/2 names list A,B,G,R (high to low) over the source components R, G, 0, 1.
Swizzle from_name(const char* n) {
  auto c = [](char ch) { return ch == '0' ? Comp::Zero : ch == '1' ? Comp::One : ch == 'G' ? Comp::G : Comp::R; };
  return {c(n[3]), c(n[2]), c(n[1]), c(n[0])};
}
Swizzle swizzle1(uint32_t code) {
  static const char* n[8] = {"100R", "000R", "111R", "RRRR", "0RRR", "1RRR", "R000", "R111"};
  return from_name(n[code & 7]);
}

uint32_t log2c(uint32_t v) {
  uint32_t r = 0;
  while ((1u << r) < v) r++;
  return r;
}
uint32_t align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// Index inside a power-of-two grid: x and y bits interleaved up to the shorter side, then the longer side's rest.
// Separable: index = mx[x] | my[y], so decode builds one table per axis and level instead of a bit loop per texel.
uint32_t spread_bits(uint32_t v) {  // bit i -> bit 2i (16 bits)
  v &= 0xffff;
  v = (v | v << 8) & 0x00ff00ffu;
  v = (v | v << 4) & 0x0f0f0f0fu;
  v = (v | v << 2) & 0x33333333u;
  return (v | v << 1) & 0x55555555u;
}
void morton_axes(uint32_t gw, uint32_t gh, uint32_t vw, uint32_t vh, std::vector<uint32_t>& mx,
                 std::vector<uint32_t>& my) {
  uint32_t n = std::min(log2c(gw), log2c(gh)), low = (1u << n) - 1;
  mx.resize(vw);
  my.resize(vh);
  for (uint32_t x = 0; x < vw; x++) mx[x] = spread_bits(x & low) << 1 | (gw > gh ? (x >> n) << (2 * n) : 0);
  for (uint32_t y = 0; y < vh; y++) my[y] = spread_bits(y & low) | (gw > gh ? 0 : (y >> n) << (2 * n));
}

struct Fmt {
  HostFormat host;
  uint32_t bits;                // per source texel (BC: per 4x4 block)
  bool bc, palette, convert;   // convert: output is RGBA8 built by decode(), mapping stays identity
  Swizzle swizzle;
};

bool format_of(uint32_t base, uint32_t swz, Fmt& f, std::string& why) {
  bool ok = true;
  switch (base) {
    case UBC1: f = {HostFormat::BC1, 64, true, false, false, swizzle4(swz)}; break;
    case UBC2: f = {HostFormat::BC2, 128, true, false, false, swizzle4(swz)}; break;
    case UBC3: f = {HostFormat::BC3, 128, true, false, false, swizzle4(swz)}; break;
    case U8: f = {HostFormat::R8, 8, false, false, false, swizzle1(swz)}; break;
    case U8U8: {
      static const char* n[6] = {"10GR", "00GR", "GRRR", "RGGG", "GRGR", "00RG"};
      ok = swz < 6;
      f = {HostFormat::RG8, 16, false, false, false, from_name(n[ok ? swz : 0])};
      break;
    }
    case U8U8U8U8: f = {HostFormat::RGBA8, 32, false, false, false, swizzle4(swz)}; break;
    case F16F16F16F16: f = {HostFormat::RGBA16F, 64, false, false, false, swizzle4(swz)}; break;
    case U8U8U8: f = {HostFormat::RGBA8, 24, false, false, true, {}}; ok = swz < 2; break;
    case U5U6U5: f = {HostFormat::RGBA8, 16, false, false, true, {}}; ok = swz < 2; break;
    case U4U4U4U4: f = {HostFormat::RGBA8, 16, false, false, true, {}}; break;
    case P8: f = {HostFormat::RGBA8, 8, false, true, true, {}}; break;
    case P4: f = {HostFormat::RGBA8, 4, false, true, true, {}}; break;
    default:
      why = "base format 0x" + std::to_string(base) + " not supported";
      return false;
  }
  if (!ok) why = "swizzle " + std::to_string(swz) + " not supported for base format " + std::to_string(base);
  return ok;
}

struct Fields {
  uint32_t type, base, swz;
};
Fields fields(const Words& w) { return {w[1] >> 29, (w[0] & 0x80000000) >> 24 | ((w[1] >> 24) & 0x1f), (w[3] >> 28) & 7}; }

uint32_t strided_pitch(const Words& w, uint32_t bits) {  // texels per row; the stride in bytes is split over word0
  uint32_t stride = (((w[0] >> 9) & 7) | ((w[0] >> 17) & 0x3ff) << 3) + 1;
  return stride * 4 * 8 / bits;
}

}  // namespace

Desc describe(const Words& w) {
  Desc d;
  auto [type, base, swz] = fields(w);
  d.type = Type(type == 1 ? 0 : type);
  bool pot = type == 0 || type == 2;
  d.width = pot ? 1u << ((w[1] >> 16) & 15) : ((w[1] >> 12) & 0xfff) + 1;
  d.height = pot ? 1u << (w[1] & 15) : (w[1] & 0xfff) + 1;
  d.mips = type == 6 ? 1 : ((w[0] >> 17) & 15) + 1;
  d.data = w[2] & ~3u;
  d.palette = (w[3] & 0x3ffffff) << 6;
  if (type == 1 || type == 2 || type == 7) { d.reason = "cube textures not supported"; return d; }
  if (type == 4) { d.reason = "tiled textures not supported"; return d; }
  Fmt f;
  if (!format_of(base, swz, f, d.reason)) return d;
  if (f.palette && !d.palette) { d.reason = "paletted texture without palette"; return d; }
  if (f.bc && (type == 3 || type == 6)) { d.reason = "linear block-compressed textures not supported"; return d; }
  d.format = f.host;
  d.swizzle = f.convert ? Swizzle{} : f.swizzle;
  d.mips = std::min(d.mips, log2c(std::max(d.width, d.height)) + 1);
  if (type == 3 || type == 6) {  // linear layouts: base level only
    d.mips = 1;
    uint32_t pitch = type == 6 ? strided_pitch(w, f.bits) : align(d.width, 8);
    d.source_bytes = uint32_t((uint64_t(pitch) * d.height * f.bits + 7) / 8);
  } else {  // swizzled: every level stored in its power-of-two grid, largest first
    uint32_t pw = 1u << log2c(d.width), ph = 1u << log2c(d.height), blk = f.bc ? 4 : 1;
    uint64_t total = 0;
    for (uint32_t m = 0; m < d.mips; m++)
      total += (uint64_t(std::max(1u, (pw >> m) / blk)) * std::max(1u, (ph >> m) / blk) * f.bits + 7) / 8;
    d.source_bytes = uint32_t(total);
  }
  d.ok = true;
  return d;
}

bool decode(const Words& w, const ReadFn& read, Decoded& out) {
  out = {};
  out.desc = describe(w);
  const Desc& d = out.desc;
  if (!d.ok) return false;
  auto [type, base, swz] = fields(w);
  Fmt f;
  std::string unused;
  format_of(base, swz, f, unused);
  auto fail = [&](const char* why) { out.desc.ok = false; out.desc.reason = why; return false; };
  const uint8_t* src = read(d.data, d.source_bytes);
  if (!src) return fail("texture memory unreadable");
  const uint8_t* pal = nullptr;
  if (f.palette && !(pal = read(d.palette, (base == P8 ? 256 : 16) * 4))) return fail("palette memory unreadable");

  bool swizzled = type == 0 || type == 5;
  uint32_t blk = f.bc ? 4 : 1, out_bits = f.convert ? 32 : f.bits;
  uint32_t pitch = type == 6 ? strided_pitch(w, f.bits) : align(d.width, 8);
  uint32_t pw = 1u << log2c(d.width), ph = 1u << log2c(d.height);
  uint64_t src_off = 0;

  for (uint32_t m = 0; m < d.mips; m++) {
    uint32_t lw = std::max(1u, d.width >> m), lh = std::max(1u, d.height >> m);
    uint32_t gw = (lw + blk - 1) / blk, gh = (lh + blk - 1) / blk;               // visible texels / blocks
    uint32_t mw = std::max(1u, (pw >> m) / blk), mh = std::max(1u, (ph >> m) / blk);  // stored grid
    uint32_t row_bytes = (gw * out_bits + 7) / 8;
    out.levels.push_back({uint32_t(out.data.size()), gw, gh});
    out.data.resize(out.data.size() + size_t(row_bytes) * gh);
    uint8_t* dst = out.data.data() + out.levels.back().offset;
    std::vector<uint32_t> mx, my;
    if (swizzled) morton_axes(mw, mh, gw, gh, mx, my);
    for (uint32_t y = 0; y < gh; y++) {
      for (uint32_t x = 0; x < gw; x++) {
        uint64_t idx = swizzled ? uint64_t(mx[x] | my[y]) : uint64_t(y) * pitch + x;  // texel (BC: block) index
        const uint8_t* s = src + src_off + idx * f.bits / 8;
        uint8_t* o = dst + size_t(y) * row_bytes + size_t(x) * out_bits / 8;
        if (!f.convert) { std::memcpy(o, s, f.bits / 8); continue; }
        uint8_t px[4];  // lowest field first
        switch (base) {
          case U8U8U8:
            px[0] = s[0], px[1] = s[1], px[2] = s[2], px[3] = 255;
            if (swz == 1) std::swap(px[0], px[2]);  // RGB: the lowest byte is B
            break;
          case U5U6U5: {
            uint32_t v = s[0] | s[1] << 8;
            px[0] = (v & 31) * 255 / 31, px[1] = (v >> 5 & 63) * 255 / 63, px[2] = (v >> 11) * 255 / 31, px[3] = 255;
            if (swz == 1) std::swap(px[0], px[2]);  // RGB: R is the high field
            break;
          }
          case U4U4U4U4: {
            uint32_t v = s[0] | s[1] << 8;
            uint8_t f4[4];
            for (int i = 0; i < 4; i++) f4[i] = (v >> (4 * i) & 15) * 17;
            for (int i = 0; i < 4; i++) px[i] = kSwz4[swz][i] < 0 ? 255 : f4[kSwz4[swz][i]];
            break;
          }
          default: {  // P8 / P4: palette entries are 4 bytes in the texture's 4-component order
            uint32_t i = base == P8 ? s[0] : ((idx & 1) ? s[0] >> 4 : s[0] & 15);
            for (int k = 0; k < 4; k++) px[k] = kSwz4[swz][k] < 0 ? 255 : pal[i * 4 + kSwz4[swz][k]];
          }
        }
        std::memcpy(o, px, 4);
      }
    }
    if (swizzled) src_off += (uint64_t(mw) * mh * f.bits + 7) / 8;
  }
  return true;
}

uint64_t cache_key(const Words& w, const ReadFn& read) {
  Desc d = describe(w);
  if (!d.ok) return 0;
  const uint8_t* p = read(d.data, d.source_bytes);
  if (!p) return 0;
  uint64_t h = 0xcbf29ce484222325ull;
  auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001b3ull; h ^= h >> 29; };
  for (int i = 0; i < 4; i++) mix(w[i]);
  // Four independent lanes over 32-byte chunks: a single multiply chain was latency-bound (every live texture is
  // hashed once per frame: 8% of the GPU thread on the Retroid's A75).
  uint64_t l[4] = {h, h ^ 0x9e3779b97f4a7c15ull, h ^ 0xc2b2ae3d27d4eb4full, h ^ 0x165667b19e3779f9ull};
  size_t i = 0;
  for (; i + 32 <= d.source_bytes; i += 32)
    for (int k = 0; k < 4; k++) {
      uint64_t v;
      std::memcpy(&v, p + i + 8 * k, 8);
      l[k] = (l[k] ^ v) * 0x100000001b3ull;
      l[k] ^= l[k] >> 29;
    }
  for (int k = 0; k < 4; k++) mix(l[k]);
  for (; i < d.source_bytes; i++) mix(p[i]);
  return h ? h : 1;
}

uint64_t sample_key(const Words& w, const ReadFn& read) {
  constexpr uint32_t kChunks = 64, kChunk = 32;
  Desc d = describe(w);
  if (!d.ok) return 0;
  const uint8_t* p = read(d.data, d.source_bytes);
  if (!p) return 0;
  uint64_t h = 0x84222325cbf29ce4ull;
  auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001b3ull; h ^= h >> 29; };
  for (int i = 0; i < 4; i++) mix(w[i]);
  if (d.source_bytes < kChunks * kChunk * 2) {
    for (uint32_t i = 0; i < d.source_bytes; i++) mix(p[i]);
    return h ? h : 1;
  }
  uint64_t span = d.source_bytes - kChunk;
  for (uint32_t c = 0; c < kChunks; c++) {
    const uint8_t* q = p + span * c / (kChunks - 1);
    for (uint32_t k = 0; k < kChunk; k += 8) {
      uint64_t v;
      std::memcpy(&v, q + k, 8);
      mix(v);
    }
  }
  return h ? h : 1;
}

namespace {

// One 4x4 colour block (BC1 layout) to 16 RGBA8 texels; `opaque` = always the 4-colour mode (BC2 / BC3 colour).
void bc_color(const uint8_t* b, bool opaque, uint8_t out[16][4]) {
  uint32_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
  uint8_t pal[4][4];
  auto rgb = [](uint32_t c, uint8_t* o) {
    o[0] = uint8_t((c >> 11 & 31) * 255 / 31);
    o[1] = uint8_t((c >> 5 & 63) * 255 / 63);
    o[2] = uint8_t((c & 31) * 255 / 31);
    o[3] = 255;
  };
  rgb(c0, pal[0]);
  rgb(c1, pal[1]);
  for (int k = 0; k < 3; k++) {
    if (opaque || c0 > c1) {
      pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
      pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
    } else {
      pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
      pal[3][k] = 0;
    }
  }
  pal[2][3] = 255;
  pal[3][3] = opaque || c0 > c1 ? 255 : 0;
  uint32_t idx = b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24;
  for (int i = 0; i < 16; i++) std::memcpy(out[i], pal[idx >> (2 * i) & 3], 4);
}

// BC3 alpha block: two endpoints + 16 3-bit indices.
void bc3_alpha(const uint8_t* b, uint8_t out[16][4]) {
  uint32_t a0 = b[0], a1 = b[1];
  uint8_t pal[8] = {uint8_t(a0), uint8_t(a1)};
  if (a0 > a1)
    for (uint32_t i = 1; i < 7; i++) pal[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
  else {
    for (uint32_t i = 1; i < 5; i++) pal[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
    pal[6] = 0;
    pal[7] = 255;
  }
  uint64_t idx = 0;
  for (int i = 0; i < 6; i++) idx |= uint64_t(b[2 + i]) << (8 * i);
  for (int i = 0; i < 16; i++) out[i][3] = pal[idx >> (3 * i) & 7];
}

}  // namespace

void decompress_bc(Decoded& dec) {
  HostFormat f = dec.desc.format;
  if (f != HostFormat::BC1 && f != HostFormat::BC2 && f != HostFormat::BC3) return;
  uint32_t bsize = f == HostFormat::BC1 ? 8 : 16;
  std::vector<uint8_t> data;
  std::vector<Level> levels;
  for (size_t m = 0; m < dec.levels.size(); m++) {
    const Level& l = dec.levels[m];
    uint32_t w = std::max(1u, dec.desc.width >> m), h = std::max(1u, dec.desc.height >> m);
    levels.push_back({uint32_t(data.size()), w, h});
    data.resize(data.size() + size_t(w) * h * 4);
    uint8_t* dst = data.data() + levels.back().offset;
    for (uint32_t by = 0; by < l.height; by++)
      for (uint32_t bx = 0; bx < l.width; bx++) {
        const uint8_t* b = dec.data.data() + l.offset + (size_t(by) * l.width + bx) * bsize;
        uint8_t px[16][4];
        if (f == HostFormat::BC1) bc_color(b, false, px);
        else {
          bc_color(b + 8, true, px);
          if (f == HostFormat::BC3) bc3_alpha(b, px);
          else
            for (int i = 0; i < 16; i++) px[i][3] = uint8_t((b[i / 2] >> (4 * (i & 1)) & 15) * 17);
        }
        for (uint32_t i = 0; i < 16; i++) {
          uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
          if (x < w && y < h) std::memcpy(dst + (size_t(y) * w + x) * 4, px[i], 4);
        }
      }
  }
  dec.data = std::move(data);
  dec.levels = std::move(levels);
  dec.desc.format = HostFormat::RGBA8;
}

}  // namespace gpu::tex
