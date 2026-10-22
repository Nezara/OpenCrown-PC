// Synthetic checks for gpu/texture: encode a known pattern in every supported layout with an independent encoder, decode,
// compare. Needs no game files.
#include <cstdio>
#include <cstring>
#include <vector>

#include "gpu/texture.h"

using namespace gpu::tex;

namespace {

int cases = 0, failures = 0;
void check(bool ok, const char* name, const char* what) {
  cases++;
  if (!ok) { failures++; std::printf("FAIL %s: %s\n", name, what); }
}

constexpr uint32_t kData = 0x1000, kPal = 0x8000;
std::vector<uint8_t> mem(0x10000);
ReadFn reader() {
  return [](uint32_t va, uint32_t n) -> const uint8_t* { return va + n <= mem.size() ? mem.data() + va : nullptr; };
}

uint32_t lg(uint32_t v) { uint32_t r = 0; while ((1u << r) < v) r++; return r; }

// Z-order by squares: bits interleave inside min x min squares, squares run along the longer side. Different
// construction from the decoder's bit loop.
uint32_t zorder(uint32_t x, uint32_t y, uint32_t gw, uint32_t gh) {
  uint32_t s = std::min(gw, gh), lx = x % s, ly = y % s, z = 0;
  for (uint32_t b = 0; b < 16; b++) {
    if (ly & (1u << b)) z += 1u << (2 * b);  // y in the even bits (checked on game textures)
    if (lx & (1u << b)) z += 2u << (2 * b);
  }
  uint32_t sq = gw > gh ? x / s : y / s;
  return sq * s * s + z;
}

Words* words(uint32_t type, uint32_t base, uint32_t swz, uint32_t w, uint32_t h, uint32_t mips, uint32_t extra0 = 0,
             uint32_t pal = 0) {
  static Words t;
  bool pot = type == 0;
  t[0] = ((base >> 7) & 1) << 31 | (mips - 1) << 17 | extra0;
  t[1] = (pot ? (lg(h) | lg(w) << 16) : ((h - 1) | (w - 1) << 12)) | (base & 0x1f) << 24 | type << 29;
  t[2] = kData;
  t[3] = (pal >> 6) | swz << 28;
  return &t;
}

bool same(const Decoded& d, uint32_t level, const std::vector<uint8_t>& want) {
  const Level& l = d.levels[level];
  return l.offset + want.size() <= d.data.size() && !std::memcmp(d.data.data() + l.offset, want.data(), want.size());
}
bool eq(Swizzle a, Swizzle b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

// BC3 swizzled, 16x8 with 2 mips: blocks carry their (bx,by) index so any misplacement shows.
void test_bc3() {
  uint32_t w = 16, h = 8, gw = 4, gh = 2;
  std::fill(mem.begin(), mem.end(), 0);
  std::vector<uint8_t> want0, want1;
  want0.resize(gw * gh * 16);
  for (uint32_t by = 0; by < gh; by++)
    for (uint32_t bx = 0; bx < gw; bx++) {
      uint8_t* blk = &mem[kData + zorder(bx, by, gw, gh) * 16];
      for (int i = 0; i < 16; i++) blk[i] = uint8_t(bx * 16 + by * 64 + i + 1);
      std::memcpy(&want0[(by * gw + bx) * 16], blk, 16);
    }
  uint32_t l1 = gw * gh * 16;  // mip 1: 8x4 -> 2x1 blocks, stored right after
  for (uint32_t bx = 0; bx < 2; bx++) {
    uint8_t* blk = &mem[kData + l1 + zorder(bx, 0, 2, 1) * 16];
    for (int i = 0; i < 16; i++) blk[i] = uint8_t(200 + bx * 16 + i);
    want1.insert(want1.end(), blk, blk + 16);
  }
  Decoded d;
  bool ok = decode(*words(0, 0x87, 0, w, h, 2), reader(), d);
  check(ok && d.desc.format == HostFormat::BC3 && d.levels.size() == 2, "bc3", "decode/format");
  check(ok && d.desc.width == w && d.desc.height == h && d.desc.mips == 2, "bc3", "dims");
  check(ok && same(d, 0, want0) && same(d, 1, want1), "bc3", "block placement");
  check(ok && eq(d.desc.swizzle, Swizzle{}), "bc3", "ABGR is the identity mapping");
  check(ok && d.levels[0].width == 4 && d.levels[1].width == 2, "bc3", "level sizes in blocks");
}

void test_bc1_swizzle() {
  Decoded d;
  std::fill(mem.begin(), mem.end(), 7);
  bool ok = decode(*words(0, 0x85, 1, 4, 4, 1), reader(), d);  // ARGB: R comes from the B slot
  check(ok && d.desc.format == HostFormat::BC1 && d.desc.swizzle.r == Comp::B && d.desc.swizzle.b == Comp::R &&
            d.desc.swizzle.a == Comp::A, "bc1", "ARGB mapping");
  ok = decode(*words(0, 0x85, 4, 4, 4, 1), reader(), d);  // 1BGR: alpha constant 1
  check(ok && d.desc.swizzle.a == Comp::One && d.desc.swizzle.r == Comp::R, "bc1", "1BGR mapping");
}

// 2048x2048 BC3 is the largest the game uses: decode must be exact and finish.
void test_large_size() {
  Words t = {0x80000000u, 11u | 11u << 16 | 7u << 24 | 0u << 29, kData, 0};
  Desc d = describe(t);
  check(d.ok && d.width == 2048 && d.source_bytes == 2048 * 2048, "bc3-2048", "size");
}

void test_r8_linear() {
  uint32_t w = 5, h = 3, pitch = 8;
  std::fill(mem.begin(), mem.end(), 0);
  std::vector<uint8_t> want;
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < pitch; x++) {
      mem[kData + y * pitch + x] = uint8_t(10 * y + x + 1);
      if (x < w) want.push_back(uint8_t(10 * y + x + 1));
    }
  Decoded d;
  bool ok = decode(*words(3, 0x00, 1, w, h, 1), reader(), d);  // swizzle1 000R
  check(ok && d.desc.format == HostFormat::R8 && same(d, 0, want), "r8-linear", "texels");
  check(ok && d.desc.swizzle.r == Comp::R && d.desc.swizzle.g == Comp::Zero && d.desc.swizzle.a == Comp::Zero,
        "r8-linear", "mapping 000R");
  check(ok && d.desc.source_bytes == pitch * h, "r8-linear", "source size");
}

// BC3 swizzled, taller than wide (32x64 = 8x16 blocks): squares run along y.
void test_bc3_tall() {
  uint32_t w = 32, h = 64, gw = 8, gh = 16;
  std::fill(mem.begin(), mem.end(), 0);
  std::vector<uint8_t> want;
  want.resize(gw * gh * 16);
  for (uint32_t by = 0; by < gh; by++)
    for (uint32_t bx = 0; bx < gw; bx++) {
      uint8_t* blk = &mem[kData + zorder(bx, by, gw, gh) * 16];
      for (int i = 0; i < 16; i++) blk[i] = uint8_t(bx * 16 + by * 3 + i + 1);
      std::memcpy(&want[(by * gw + bx) * 16], blk, 16);
    }
  Decoded d;
  bool ok = decode(*words(0, 0x87, 0, w, h, 1), reader(), d);
  check(ok && same(d, 0, want), "bc3-tall", "block placement");
}

void test_rgba8_swizzled_arbitrary() {
  uint32_t w = 10, h = 6, pw = 16, ph = 8;  // stored as 16x8
  std::fill(mem.begin(), mem.end(), 0);
  std::vector<uint8_t> want;
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < w; x++) {
      uint8_t px[4] = {uint8_t(x), uint8_t(y), uint8_t(x + y), 99};
      std::memcpy(&mem[kData + zorder(x, y, pw, ph) * 4], px, 4);
      want.insert(want.end(), px, px + 4);
    }
  Decoded d;
  bool ok = decode(*words(5, 0x0C, 2, w, h, 1), reader(), d);  // RGBA
  check(ok && d.desc.format == HostFormat::RGBA8 && d.desc.width == w && same(d, 0, want), "rgba8-arb", "texels");
  check(ok && d.desc.swizzle.r == Comp::A && d.desc.swizzle.a == Comp::R && d.desc.swizzle.g == Comp::B,
        "rgba8-arb", "RGBA mapping");
}

void test_565() {
  std::fill(mem.begin(), mem.end(), 0);
  uint16_t v[2] = {uint16_t(31 | 0 << 5 | 0 << 11), uint16_t(0 | 63 << 5 | 31 << 11)};  // low field full / G+high full
  std::memcpy(&mem[kData], v, 4);
  Decoded d;
  bool ok = decode(*words(3, 0x05, 0, 2, 1, 1), reader(), d);  // BGR: lowest field is R
  check(ok && d.desc.format == HostFormat::RGBA8 && d.data.size() == 8, "565", "size");
  bool a = ok && d.data[0] == 255 && d.data[1] == 0 && d.data[2] == 0 && d.data[3] == 255;
  bool b = ok && d.data[4] == 0 && d.data[5] == 255 && d.data[6] == 255 && d.data[7] == 255;
  check(a && b, "565", "BGR texels");
  ok = decode(*words(3, 0x05, 1, 2, 1, 1), reader(), d);  // RGB: R is the high field
  check(ok && d.data[0] == 0 && d.data[2] == 255 && d.data[4] == 255 && d.data[6] == 0, "565", "RGB texels");
}

void test_palette() {
  std::fill(mem.begin(), mem.end(), 0);
  for (int i = 0; i < 256; i++) {
    mem[kPal + i * 4] = uint8_t(i), mem[kPal + i * 4 + 1] = uint8_t(255 - i), mem[kPal + i * 4 + 2] = 5, mem[kPal + i * 4 + 3] = 200;
  }
  uint32_t w = 8, h = 8;
  std::vector<uint8_t> want;
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < w; x++) {
      uint8_t i = uint8_t(y * 9 + x * 3);
      mem[kData + zorder(x, y, w, h)] = i;
      uint8_t px[4] = {i, uint8_t(255 - i), 5, 200};
      want.insert(want.end(), px, px + 4);
    }
  Decoded d;
  bool ok = decode(*words(0, 0x95, 0, w, h, 1, 0, kPal), reader(), d);
  check(ok && same(d, 0, want), "p8", "texels");
  // P4 swizzled 8x8: two texels per byte, low nibble first.
  std::fill(mem.begin() + kData, mem.begin() + kData + 0x100, 0);
  want.clear();
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < w; x++) {
      uint8_t i = uint8_t((x + 2 * y) & 15);
      uint32_t t = zorder(x, y, w, h);
      mem[kData + t / 2] |= (t & 1) ? i << 4 : i;
      uint8_t px[4] = {i, uint8_t(255 - i), 5, 200};
      want.insert(want.end(), px, px + 4);
    }
  ok = decode(*words(0, 0x94, 0, w, h, 1, 0, kPal), reader(), d);
  check(ok && same(d, 0, want), "p4", "texels");
  check(!decode(*words(0, 0x95, 0, w, h, 1, 0, 0), reader(), d), "p8", "palette address required");
}

void test_strided_and_f16() {
  // Strided: stride 32 bytes = field value 7 in word0 bits 9-11 (low part) -> (7+1)*4.
  std::fill(mem.begin(), mem.end(), 0);
  for (int y = 0; y < 2; y++) for (int x = 0; x < 3; x++) { mem[kData + y * 32 + x] = uint8_t(y * 16 + x + 1); }
  Decoded d;
  bool ok = decode(*words(6, 0x00, 0, 3, 2, 1, 7u << 9), reader(), d);
  std::vector<uint8_t> want = {1, 2, 3, 17, 18, 19};
  check(ok && same(d, 0, want), "strided", "texels");
  // F16 RGBA swizzled 2x2 passes through.
  for (int i = 0; i < 32; i++) mem[kData + i] = uint8_t(i + 1);
  ok = decode(*words(0, 0x1B, 0, 2, 2, 1), reader(), d);
  check(ok && d.desc.format == HostFormat::RGBA16F && d.data.size() == 32, "f16", "size");
  check(ok && !std::memcmp(&d.data[0], &mem[kData], 8) && !std::memcmp(&d.data[8], &mem[kData + 16], 8) &&
            !std::memcmp(&d.data[16], &mem[kData + 8], 8), "f16", "texel order for 2x2 (x,y)=(0,0),(1,0),(0,1)");
}

void test_unsupported_and_key() {
  Decoded d;
  check(!decode(*words(2, 0x87, 0, 8, 8, 1), reader(), d) && !d.desc.reason.empty(), "cube", "rejected with reason");
  check(!decode(*words(4, 0x00, 0, 8, 8, 1), reader(), d), "tiled", "rejected");
  check(!decode(*words(0, 0x0A, 0, 8, 8, 1), reader(), d), "s16", "unknown base format rejected");
  check(!decode(*words(3, 0x87, 0, 8, 8, 1), reader(), d), "bc3-linear", "rejected");
  Words t;
  std::memcpy(t, *words(0, 0x87, 0, 8, 8, 1), 16);
  uint64_t k1 = cache_key(t, reader());
  mem[kData + 3] ^= 1;
  uint64_t k2 = cache_key(t, reader());
  t[1] ^= 1;  // other height
  uint64_t k3 = cache_key(t, reader());
  check(k1 && k2 && k3 && k1 != k2 && k2 != k3, "cache_key", "changes with data and words");
  t[2] = 0xffff0000;
  check(cache_key(t, reader()) == 0, "cache_key", "unreadable memory gives 0");
}


// CPU BC decompression (decompress_bc): hand-made blocks with expected texels worked out from the BC spec.
void test_decompress() {
  // BC1, 6x5 texels = 2x2 blocks (edge texels cropped). Block 0: c0 = pure red (0xf800) > c1 = pure blue (0x001f),
  // 4-colour mode; indices 0,1,2,3 repeating. Other blocks: c0 <= c1 -> index 3 = transparent black.
  Decoded d;
  d.desc.ok = true;
  d.desc.format = HostFormat::BC1;
  d.desc.width = 6;
  d.desc.height = 5;
  d.levels.push_back({0, 2, 2});
  const uint8_t b0[8] = {0x00, 0xf8, 0x1f, 0x00, 0xe4, 0xe4, 0xe4, 0xe4};  // idx row = 0b11100100 -> 0,1,2,3
  const uint8_t b1[8] = {0x1f, 0x00, 0x00, 0xf8, 0xff, 0xff, 0xff, 0xff};  // c0 < c1, all index 3
  for (int i = 0; i < 4; i++) d.data.insert(d.data.end(), i ? b1 : b0, (i ? b1 : b0) + 8);
  decompress_bc(d);
  auto px = [&](uint32_t x, uint32_t y) { return d.data.data() + (size_t(y) * 6 + x) * 4; };
  check(d.desc.format == HostFormat::RGBA8 && d.levels.size() == 1 && d.levels[0].width == 6 &&
            d.levels[0].height == 5 && d.data.size() == 6 * 5 * 4, "bc1 decompress", "layout");
  const uint8_t want[4][4] = {{255, 0, 0, 255}, {0, 0, 255, 255}, {170, 0, 85, 255}, {85, 0, 170, 255}};
  bool ok = true;
  for (int x = 0; x < 4; x++) ok &= !std::memcmp(px(x, 2), want[x], 4);
  check(ok, "bc1 decompress", "4-colour palette");
  const uint8_t zero[4] = {0, 0, 0, 0};
  check(!std::memcmp(px(5, 4), zero, 4) && !std::memcmp(px(4, 0), zero, 4), "bc1 decompress", "punch-through");

  // BC3, 4x4: alpha endpoints 255 / 0 (8-step), index 1 everywhere -> 0; colour = white c0 = c1, index 0.
  Decoded e;
  e.desc.ok = true;
  e.desc.format = HostFormat::BC3;
  e.desc.width = e.desc.height = 4;
  e.levels.push_back({0, 1, 1});
  uint8_t blk[16] = {255, 0};
  uint64_t idx = 0;
  for (int i = 0; i < 16; i++) idx |= uint64_t(i == 5 ? 2 : 1) << (3 * i);  // texel 5: index 2 = (6*255+0)/7
  for (int i = 0; i < 6; i++) blk[2 + i] = uint8_t(idx >> (8 * i));
  blk[8] = blk[9] = blk[10] = blk[11] = 0xff;
  e.data.assign(blk, blk + 16);
  decompress_bc(e);
  check(e.data.size() == 64 && e.data[3] == 0 && e.data[5 * 4 + 3] == 218 && e.data[5 * 4] == 255,
        "bc3 decompress", "alpha / colour");

  // BC2, 4x4: explicit alpha nibbles i -> i * 17.
  Decoded f;
  f.desc.ok = true;
  f.desc.format = HostFormat::BC2;
  f.desc.width = f.desc.height = 4;
  f.levels.push_back({0, 1, 1});
  uint8_t b2[16] = {};
  for (int i = 0; i < 16; i++) b2[i / 2] |= uint8_t(i << (4 * (i & 1)));
  f.data.assign(b2, b2 + 16);
  decompress_bc(f);
  ok = true;
  for (int i = 0; i < 16; i++) ok &= f.data[i * 4 + 3] == i * 17;
  check(ok, "bc2 decompress", "alpha nibbles");
}

}  // namespace

int main() {
  test_bc3();
  test_bc3_tall();
  test_bc1_swizzle();
  test_large_size();
  test_r8_linear();
  test_rgba8_swizzled_arbitrary();
  test_565();
  test_palette();
  test_strided_and_f16();
  test_unsupported_and_key();
  test_decompress();
  std::printf("%d cases, %d failures\n", cases, failures);
  return failures ? 1 : 0;
}
