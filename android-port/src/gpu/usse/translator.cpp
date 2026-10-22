// USSE -> SPIR-V translator. Field layouts, register rules and semantics: notes/map/usse_gxp.md (section numbers
// below refer to it). Anything outside what the game's programs use is refused with a reason, never guessed.
#include "gpu/usse/translator.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>

#include "gpu/spirv/builder.h"

namespace gpu::usse {
namespace {

using spirv::Id;

struct Fail {
  std::string what;
};
[[noreturn]] void fail(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  throw Fail{buf};
}

uint32_t bits(uint64_t w, int hi, int lo) { return uint32_t((w >> lo) & ((1ull << (hi - lo + 1)) - 1)); }

// Hardware constant table (5.5): entry n = the pair (T0[n], T1[n]).
constexpr uint32_t T0[64] = {
    0x00000000, 0x00000000, 0x3F800000, 0x3F800000, 0x40000000, 0x41000000, 0x42000000, 0x43000000, 0x44000000,
    0x45000000, 0x46000000, 0x47000000, 0x3F000000, 0x3E000000, 0x3D000000, 0x3C000000, 0x3B000000, 0x3A000000,
    0x39000000, 0x38000000, 0x402DF854, 0x3FB504F3, 0x40490FDB, 0x3F490FDB, 0x40C90FDB, 0x41C90FDB, 0x37800000,
    0x37800080, 0x35D00D01, 0x39888889, 0x3CAAAAAB, 0x3F000000, 0x00000000, 0x00000000, 0x3C003C00, 0x44004000,
    0x54005000, 0x64006000, 0x74007000, 0x34003800, 0x24002800, 0x14001800, 0x04000800, 0x35E2416F, 0x39A83DA8,
    0x3E484248, 0x4A484648, 0x00000000, 0x00000000, 0x30002555, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x7FFF7FFF, 0x7FFF7FFF, 0x7FFF7FFF,
    0x7FFF7FFF};
constexpr uint32_t T1[64] = {
    0x00000000, 0x3F800000, 0x00000000, 0x3F800000, 0x40800000, 0x41800000, 0x42800000, 0x43800000, 0x44800000,
    0x45800000, 0x46800000, 0x47800000, 0x3E800000, 0x3D800000, 0x3C800000, 0x3B800000, 0x3A800000, 0x39800000,
    0x38800000, 0x37800000, 0x3EBC5AB2, 0x3F3504F3, 0x3FC90FDB, 0x3EC90FDB, 0x41490FDB, 0x00000000, 0x38000000,
    0x38000100, 0x37B60B61, 0x3B2AAAAB, 0x3E000000, 0x3F800000, 0x3C000000, 0x00000000, 0x3C003C00, 0x4C004800,
    0x5C005800, 0x6C006800, 0x00007800, 0x2C003000, 0x1C002000, 0x0C001000, 0x00000000, 0x00000000, 0x00000000,
    0x36483A48, 0x00004E48, 0x00000000, 0x19550C44, 0x3C003800, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000};

// Swizzle tables (6.2-6.4). Channels: x y z w, constants 0 1 2 and h (0.5).
const char* const S4[16] = {"xxxx", "yyyy", "zzzz", "wwww", "xyzw", "yzww", "xyzz", "xxyz",
                            "xyxy", "xywz", "zxyw", "zwzw", "yzxz", "xxyy", "xzww", "xyz1"};
const char* const S4X[16] = {"yzxw", "zwxy", "xzwy", "yyww", "wyzw", "wzwz", "xyzx", "zzww",
                             "xwzx", "yyyx", "yyyz", "xzyw", "xxxy", "zyxw", "yyzz", "zzzy"};
const char* const S3[16] = {"xxx", "yyy", "zzz", "www", "xyz", "yzw", "xxy", "xyx",
                            "yyx", "yyz", "zxy", "xzy", "yzx", "zyx", "zzy", "xy1"};
const char* const S3X[16] = {"xyy", "yxy", "xxz", "yxx", "xy0", "x10", "000", "111",
                             "hhh", "222", "x00", nullptr, nullptr, nullptr, nullptr, nullptr};
const char* const M2S0[8] = {"xxxx", "yyyy", "zzzz", "wwww", "xyzw", "yzxw", "xyww", "zwxy"};  // VMAD2 sources
const char* const M2S1[8] = {"xxxx", "yyyy", "zzzz", "wwww", "xyzw", "xyyz", "yyww", "wyzw"};
const char* const M2S2[8] = {"xxxx", "yyyy", "zzzz", "wwww", "xyzw", "xzww", "xxyz", "xyzz"};
const char kChan[] = "xyzw012h";

int chan(char c) {
  const char* p = std::strchr(kChan, c);
  if (!c || !p) fail("bad swizzle channel '%c'", c);
  return int(p - kChan);
}

enum class Bank : uint8_t { T, O, PA, SA, I, C, G, Imm, Index, Idx1, Idx2 };
const char* bank_name(Bank b) {
  static const char* const n[] = {"t", "o", "pa", "sa", "i", "c", "g", "imm", "index", "idx1", "idx2"};
  return n[int(b)];
}
bool unified(Bank b) { return b == Bank::PA || b == Bank::SA || b == Bank::O; }

struct Reg {
  Bank bank;
  uint32_t n;  // 32-bit register (after doubling), internal register index, or constant table entry
};

// Bank decode (5.1) + doubling (5.2) + internal registers (5.3) + the secondary-program rule (3).
enum class Slot { Dest, Src0, Src12 };
Reg mkreg(Slot kind, uint32_t n, uint32_t bank, uint32_t ext, bool dbl, int field_bits, bool second) {
  static const Bank kD[2][4] = {{Bank::T, Bank::O, Bank::PA, Bank::Idx1}, {Bank::SA, Bank::C, Bank::Index, Bank::Idx2}};
  static const Bank kS0[2][2] = {{Bank::T, Bank::PA}, {Bank::O, Bank::SA}};
  static const Bank kS12[2][4] = {{Bank::T, Bank::O, Bank::PA, Bank::SA}, {Bank::Idx1, Bank::C, Bank::Imm, Bank::Idx2}};
  Bank b = kind == Slot::Dest ? kD[ext][bank] : kind == Slot::Src0 ? kS0[ext][bank & 1] : kS12[ext][bank];
  if (dbl && b != Bank::C && b != Bank::Imm) n = (n << 1) & 0xff;
  if (b == Bank::T) {
    uint32_t lim = (1u << field_bits) - (dbl ? 8 : 4);
    if (n >= lim) {
      n -= lim;
      if (dbl) n >>= 1;
      b = Bank::I;
    }
  }
  if (b == Bank::C && (n & 0x40)) {
    n &= ~0x40u;
    b = Bank::G;
  }
  if (second && b != Bank::I && b != Bank::C && b != Bank::Imm) b = Bank::SA;
  return {b, n};
}

std::string rs(Reg r, const std::string& sw = {}) {
  return bank_name(r.bank) + std::to_string(r.n) + (sw.empty() ? "" : "." + sw);
}
std::string msk(uint32_t m) {
  std::string s;
  for (int i = 0; i < 4; i++)
    if (m >> i & 1) s += "xyzw"[i];
  return s.empty() ? "-" : s;
}
std::string modstr(const std::string& s, uint32_t m) {  // bit0 negate, bit1 absolute
  return m == 1 ? "-" + s : m == 2 ? "|" + s + "|" : m == 3 ? "-|" + s + "|" : s;
}
std::string swz12(uint32_t v) {  // V32NMAD src1: four 3-bit selectors, lane 0 lowest
  std::string s;
  for (int i = 0; i < 4; i++) s += kChan[(v >> (3 * i)) & 7];
  return s;
}
std::string vmad_swz(uint32_t sel, uint32_t ext, bool vec4) {
  const char* s = vec4 ? (ext ? S4X : S4)[sel] : (ext ? S3X : S3)[sel];
  return s ? s : "?";
}

float half_to_float(uint16_t h) {
  uint32_t s = uint32_t(h >> 15) << 31, e = (h >> 10) & 31, m = h & 0x3ff, f;
  if (e == 0) {
    if (!m) {
      f = s;
    } else {  // subnormal: normalise
      e = 113;
      while (!(m & 0x400)) m <<= 1, e--;
      f = s | e << 23 | (m & 0x3ff) << 13;
    }
  } else if (e == 31) {
    f = s | 0x7f800000 | m << 13;
  } else {
    f = s | (e + 112) << 23 | m << 13;
  }
  float r;
  std::memcpy(&r, &f, 4);
  return r;
}

enum class Cls { VMAD2, V32NMAD, V16NMAD, VMAD, VDP, VDUAL, VCOMP, VMOV, VPCK, SOP2, PHAS, NOP, SPEC, Unknown };
Cls classify(uint64_t w) {  // most specific pattern first (6)
  if ((w & 0xff70000000000000ull) == 0xfa40000000000000ull) return Cls::PHAS;
  if ((w & 0xf87001c000000000ull) == 0xf800014000000000ull) return Cls::NOP;
  if ((w & 0xf820000000000000ull) == 0x1820000000000000ull) return Cls::VMAD;
  if ((w & 0xf820000000000000ull) == 0x1800000000000000ull) return Cls::VDP;
  switch (w >> 59) {
    case 0x00: return Cls::VMAD2;
    case 0x01: return Cls::V32NMAD;
    case 0x02: return Cls::V16NMAD;
    case 0x06: return Cls::VCOMP;
    case 0x07: return Cls::VMOV;
    case 0x08: return Cls::VPCK;
    case 0x10: return Cls::SOP2;
    case 0x1f: return Cls::SPEC;
  }
  if ((w >> 60) == 0x2) return Cls::VDUAL;
  return Cls::Unknown;
}

class Translator {
 public:
  Translator(std::span<const uint8_t> g, std::span<const uint8_t> fetch) : g_(g), fetch_(fetch) {}
  void run(Shader& out);

 private:
  // --- container access (bounds-checked) ---
  uint32_t rd(uint32_t off, uint32_t n) const {
    if (uint64_t(off) + n > g_.size()) fail("GXP read out of bounds at 0x%x", off);
    uint32_t v = 0;
    std::memcpy(&v, g_.data() + off, n);
    return v;
  }
  uint32_t u8(uint32_t o) const { return rd(o, 1); }
  uint32_t u16(uint32_t o) const { return rd(o, 2); }
  uint32_t u32(uint32_t o) const { return rd(o, 4); }
  uint32_t rel(uint32_t o) const { return o + u32(o); }  // offsets are relative to their own field
  uint64_t word(uint32_t o) const { return uint64_t(u32(o)) | uint64_t(u32(o + 4)) << 32; }

  // --- one instruction: text always, SPIR-V when emit_ ---
  std::string step(uint64_t w);
  std::string nmad(uint64_t w, bool f16);
  std::string vmad2(uint64_t w);
  std::string vmad(uint64_t w);
  std::string vdp(uint64_t w);
  std::string vdual(uint64_t w);
  std::string vcomp(uint64_t w);
  std::string vmov(uint64_t w);
  std::string vpck(uint64_t w);
  std::string sop2(uint64_t w);

  // --- register values (symbolic execution) ---
  struct Val {
    Id f = 0, u = 0;  // the register as float and / or as uint (each derived from the other on demand)
  };
  Val& slot(Bank b, uint32_t n);
  Id get_f(Bank b, uint32_t n);
  Id get_u(Bank b, uint32_t n);
  void set_f(Bank b, uint32_t n, Id v) { regs(b)[n] = Val{v, 0}; }
  void set_u(Bank b, uint32_t n, Id v) { regs(b)[n] = Val{0, v}; }
  std::map<uint32_t, Val>& regs(Bank b) {
    if (!unified(b)) fail("register bank %s is not a register file", bank_name(b));
    return regs_[b == Bank::PA ? 0 : b == Bank::SA ? 1 : 2];
  }
  Id lane_f32(Reg r, int c, uint32_t mods = 0);
  Id lane_f16(Reg r, int c, uint32_t mods = 0);
  Id apply(Id v, uint32_t mods);
  Id fconst(float v) { return b_.const_float(v); }

  struct Pend {
    Reg r;
    uint32_t lane;
    Id v;
    bool f16;
  };
  void put(Reg r, uint32_t lane, Id v, bool f16 = false);
  void commit();

  // --- shader interface ---
  void setup_vertex(Shader& out);
  void setup_fragment(Shader& out);
  void finish_vertex(Shader& out);
  void finish_fragment(Shader& out);
  Id input_vec4(uint32_t location);
  Id vertex_input(uint32_t location);
  Id output_vec4(uint32_t location, Id value);
  Id uniform_word(uint32_t word);

  std::span<const uint8_t> g_;
  std::span<const uint8_t> fetch_;  // Fetch bits per vertex input location
  spirv::Builder b_;
  bool fragment_ = false, second_ = false, emit_ = false;
  uint32_t flags_ = 0, vary_ = 0, ub_words_ = 0, ub_base_ = 0;
  Id fn_ = 0, t_void_ = 0, t_f_ = 0, t_u_ = 0, t_v2_ = 0, t_v4_ = 0, ubo_ = 0;
  std::map<uint32_t, Val> regs_[3];
  Id i_[4][4] = {};
  std::vector<Pend> pend_;
  std::map<uint32_t, Id> inputs_;  // location -> loaded vec4
  std::vector<Id> iface_;
  bool blended_ = false;  // SOP2 seen: the program has produced its colour
  Id blend_src_ = 0;      // u8x4 shader colour of the SOP2
  Blend blend_ = Blend::None;
};

// --- values ---------------------------------------------------------------------------------------------------------
Translator::Val& Translator::slot(Bank b, uint32_t n) {
  auto& m = regs(b);
  auto it = m.find(n);
  if (it != m.end()) return it->second;
  if (b == Bank::SA && n >= ub_base_ && n < ub_base_ + ub_words_) return m[n] = Val{0, uniform_word(n - ub_base_)};
  if (b == Bank::O && fragment_) fail("o%u (framebuffer colour) read outside a blend", n);
  fail("%s%u read before it is written", bank_name(b), n);
}
Id Translator::get_f(Bank b, uint32_t n) {
  Val& v = slot(b, n);
  if (!v.f) v.f = b_.bitcast(t_f_, v.u);
  return v.f;
}
Id Translator::get_u(Bank b, uint32_t n) {
  Val& v = slot(b, n);
  if (!v.u) v.u = b_.bitcast(t_u_, v.f);
  return v.u;
}

Id Translator::apply(Id v, uint32_t mods) {
  if (mods & 2) v = b_.fabs(t_f_, v);
  if (mods & 1) v = b_.fneg(t_f_, v);
  return v;
}

Id Translator::lane_f32(Reg r, int c, uint32_t mods) {
  static const float k[4] = {0.0f, 1.0f, 2.0f, 0.5f};
  if (c >= 4) return apply(fconst(k[c - 4]), mods);
  switch (r.bank) {
    case Bank::I:
      if (!i_[r.n][c]) fail("i%u.%c read before it is written", r.n, kChan[c]);
      return apply(i_[r.n][c], mods);
    case Bank::C: {
      if (r.n >= 64) fail("constant c%u", r.n);
      uint32_t bitsv = c == 1 ? T1[r.n] : T0[r.n];  // z, w read T0 (5.5, unverified; the game never does)
      float f;
      std::memcpy(&f, &bitsv, 4);
      return apply(fconst(f), mods);
    }
    case Bank::PA:
    case Bank::SA:
    case Bank::O:
      return apply(get_f(r.bank, r.n + c), mods);
    default:
      fail("f32 read of %s", rs(r).c_str());
  }
}

Id Translator::lane_f16(Reg r, int c, uint32_t mods) {
  static const float k[4] = {0.0f, 1.0f, 2.0f, 0.5f};
  if (c >= 4) return apply(fconst(k[c - 4]), mods);
  switch (r.bank) {
    case Bank::I:
      if (!i_[r.n][c]) fail("i%u.%c read before it is written", r.n, kChan[c]);
      return apply(i_[r.n][c], mods);
    case Bank::C: {
      if (r.n >= 64) fail("constant c%u", r.n);
      uint32_t wv = c < 2 ? T0[r.n] : T1[r.n];
      return apply(fconst(half_to_float(uint16_t(c & 1 ? wv >> 16 : wv))), mods);
    }
    case Bank::PA:
    case Bank::SA:
    case Bank::O: {
      Id v2 = b_.unpack_half2x16(t_v2_, get_u(r.bank, r.n + (c >> 1)));
      return apply(b_.composite_extract(t_f_, v2, {uint32_t(c & 1)}), mods);
    }
    default:
      fail("f16 read of %s", rs(r).c_str());
  }
}

void Translator::put(Reg r, uint32_t lane, Id v, bool f16) {
  if (r.bank != Bank::I && !unified(r.bank)) fail("write to %s", rs(r).c_str());
  if (r.bank == Bank::I && (r.n > 3 || lane > 3)) fail("write to i%u lane %u", r.n, lane);
  pend_.push_back({r, lane, v, f16});
}

// All sources of an instruction are read before any destination lane is written (5.4).
void Translator::commit() {
  std::map<std::pair<int, uint32_t>, std::pair<Id, Id>> halves;  // (bank, reg) -> (low, high)
  for (const Pend& p : pend_) {
    if (p.r.bank == Bank::I) {
      i_[p.r.n][p.lane] = p.v;
    } else if (p.f16) {
      auto& h = halves[{int(p.r.bank), p.r.n + (p.lane >> 1)}];
      (p.lane & 1 ? h.second : h.first) = p.v;
    } else {
      set_f(p.r.bank, p.r.n + p.lane, p.v);
    }
  }
  for (auto& [key, h] : halves) {
    Bank bank = Bank(key.first);
    if (!h.first || !h.second) {  // one half written: keep the other half of the old value
      auto& m = regs(bank);
      Id old = m.count(key.second) ? b_.unpack_half2x16(t_v2_, get_u(bank, key.second)) : 0;
      for (int k = 0; k < 2; k++) {
        Id& dst = k ? h.second : h.first;
        if (!dst) dst = old ? b_.composite_extract(t_f_, old, {uint32_t(k)}) : fconst(0.0f);
      }
    }
    set_u(bank, key.second, b_.pack_half2x16(t_u_, b_.composite_construct(t_v2_, {h.first, h.second})));
  }
  pend_.clear();
}

// --- instruction classes ----------------------------------------------------------------------------------------------
std::string Translator::nmad(uint64_t w, bool f16) {  // 6.2
  static const char* const ops[8] = {"mul", "add", "frc", "dsx", "dsy", "min", "max", "dp"};
  uint32_t pred = bits(w, 58, 56), mask = bits(w, 42, 39), s1mod = bits(w, 38, 37), s2abs = bits(w, 36, 36);
  uint32_t op = bits(w, 14, 12);
  Reg d = mkreg(Slot::Dest, bits(w, 27, 22), bits(w, 33, 32), bits(w, 51, 51), true, 7, second_);
  Reg s1 = mkreg(Slot::Src12, bits(w, 11, 6), bits(w, 31, 30), bits(w, 49, 49), true, 7, second_);
  Reg s2 = mkreg(Slot::Src12, bits(w, 5, 0), bits(w, 29, 28), bits(w, 48, 48), true, 7, second_);
  std::string sw1 = swz12(bits(w, 21, 15) | bits(w, 35, 34) << 7 | bits(w, 50, 50) << 9 | bits(w, 54, 53) << 10);
  std::string sw2 = S4[bits(w, 47, 44)];
  std::string a2 = rs(s2, sw2);
  if (s2abs) a2 = "|" + a2 + "|";
  std::string text = (pred ? "(p" + std::to_string(pred) + ")" : "") + ops[op] + (f16 ? ".f16 " : ".f32 ") +
                     rs(d, msk(mask)) + " = " + modstr(rs(s1, sw1), s1mod) + ", " + a2;
  if (!emit_) return text;
  if (pred) fail("predicated instruction");
  if (op != 0 && op != 1 && op != 5 && op != 6) fail("vector op %s", ops[op]);
  for (uint32_t j = 0; j < 4; j++) {
    if (!(mask >> j & 1)) continue;
    Id a = f16 ? lane_f16(s1, chan(sw1[j]), s1mod) : lane_f32(s1, chan(sw1[j]), s1mod);
    Id c = f16 ? lane_f16(s2, chan(sw2[j]), s2abs << 1) : lane_f32(s2, chan(sw2[j]), s2abs << 1);
    Id r = op == 0 ? b_.fmul(t_f_, a, c) : op == 1 ? b_.fadd(t_f_, a, c) : op == 5 ? b_.fmin(t_f_, a, c) : b_.fmax(t_f_, a, c);
    put(d, j, r, f16 && d.bank != Bank::I);
  }
  commit();
  return text;
}

std::string Translator::vmad2(uint64_t w) {  // 6.3
  uint32_t pred = bits(w, 57, 56), fmt16 = bits(w, 58, 58), mask = bits(w, 42, 39);
  uint32_t s0abs = bits(w, 50, 50), s1mod = bits(w, 38, 37), s2mod = bits(w, 36, 35);
  Reg d = mkreg(Slot::Dest, bits(w, 27, 22), bits(w, 33, 32), 0, true, 7, second_);
  Reg s0 = mkreg(Slot::Src0, bits(w, 17, 12), bits(w, 34, 34), 0, true, 7, second_);
  Reg s1 = mkreg(Slot::Src12, bits(w, 11, 6), bits(w, 31, 30), bits(w, 49, 49), true, 7, second_);
  Reg s2 = mkreg(Slot::Src12, bits(w, 5, 0), bits(w, 29, 28), bits(w, 48, 48), true, 7, second_);
  std::string sw0 = M2S0[bits(w, 19, 18) | bits(w, 53, 53) << 2];
  std::string sw1 = M2S1[bits(w, 21, 20) | bits(w, 44, 44) << 2];
  std::string sw2 = M2S2[bits(w, 47, 45)];
  std::string a = rs(s0, sw0);
  if (s0abs) a = "|" + a + "|";
  std::string text = (pred ? "(p" + std::to_string(pred) + ")" : "") + std::string("mad.") + (fmt16 ? "f16 " : "f32 ") +
                     rs(d, msk(mask)) + " = " + a + " * " + modstr(rs(s1, sw1), s1mod) + " + " + modstr(rs(s2, sw2), s2mod);
  if (!emit_) return text;
  if (pred) fail("predicated instruction");
  if (fmt16) fail("f16 mad");
  for (uint32_t j = 0; j < 4; j++) {
    if (!(mask >> j & 1)) continue;
    Id x = lane_f32(s0, chan(sw0[j]), s0abs << 1), y = lane_f32(s1, chan(sw1[j]), s1mod);
    Id z = lane_f32(s2, chan(sw2[j]), s2mod);
    put(d, j, b_.fadd(t_f_, b_.fmul(t_f_, x, y), z));
  }
  commit();
  return text;
}

std::string Translator::vmad(uint64_t w) {  // 6.4: dst = g0 * src1 + g1
  uint32_t pred = bits(w, 58, 56), vec4 = bits(w, 52, 52), mask = bits(w, 42, 39), rcount = bits(w, 45, 44);
  uint32_t rmode = bits(w, 48, 47), g0n = bits(w, 29, 28), g1n = bits(w, 13, 12);
  uint32_t s1mods = bits(w, 38, 38) | bits(w, 37, 37) << 1, g0mods = bits(w, 11, 11) | bits(w, 46, 46) << 1;
  uint32_t g1mods = bits(w, 36, 36) | bits(w, 35, 35) << 1;
  Reg d = mkreg(Slot::Dest, bits(w, 27, 22), bits(w, 33, 32), bits(w, 51, 51), true, 7, second_);
  Reg s1 = mkreg(Slot::Src12, bits(w, 5, 0), bits(w, 31, 30), bits(w, 49, 49), true, 7, second_);
  std::string sw0 = vmad_swz(bits(w, 21, 18), bits(w, 34, 34), vec4);
  std::string sw1 = vmad_swz(bits(w, 9, 6), bits(w, 10, 10), vec4);
  std::string sw2 = vmad_swz(bits(w, 17, 14), bits(w, 54, 54), vec4);
  auto wrap = [](std::string s, uint32_t m) {
    if (m & 2) s = "|" + s + "|";
    return m & 1 ? "-" + s : s;
  };
  std::string text = (pred ? "(p" + std::to_string(pred) + ")" : "") + "vmad" + (vec4 ? "4 " : "3 ") +
                     rs(d, msk(mask)) + " = " + wrap("i" + std::to_string(g0n) + "." + sw0, g0mods) + " * " +
                     wrap(rs(s1, sw1), s1mods) + " + " + wrap("i" + std::to_string(g1n) + "." + sw2, g1mods) +
                     (rcount ? " rpt" + std::to_string(rcount) + "/m" + std::to_string(rmode) : "");
  if (!emit_) return text;
  if (pred) fail("predicated instruction");
  if (rcount) fail("repeated vmad");
  Reg g0{Bank::I, g0n}, g1{Bank::I, g1n};
  for (uint32_t j = 0; j < 4; j++) {
    if (!(mask >> j & 1)) continue;
    if (j >= sw0.size() || sw0 == "?" || sw1 == "?" || sw2 == "?") fail("vmad3 lane %u / undefined swizzle", j);
    Id x = lane_f32(g0, chan(sw0[j]), g0mods), y = lane_f32(s1, chan(sw1[j]), s1mods);
    Id z = lane_f32(g1, chan(sw2[j]), g1mods);
    put(d, j, b_.fadd(t_f_, b_.fmul(t_f_, x, y), z));
  }
  commit();
  return text;
}

std::string Translator::vdp(uint64_t w) {  // 6.5: dot product replicated into the mask
  uint32_t pred = bits(w, 58, 56), dp4 = bits(w, 52, 52), mask = bits(w, 42, 39), clip = bits(w, 54, 54);
  uint32_t rcount = bits(w, 45, 44), g0n = bits(w, 29, 28), s1mods = bits(w, 38, 38) | bits(w, 37, 37) << 1;
  uint32_t g0mods = bits(w, 46, 46) << 1;
  Reg d = mkreg(Slot::Dest, bits(w, 27, 22), bits(w, 33, 32), bits(w, 51, 51), true, 7, second_);
  Reg s1 = mkreg(Slot::Src12, bits(w, 5, 0), bits(w, 31, 30), bits(w, 49, 49), true, 7, second_);
  std::string sw1;
  for (int hi : {8, 11, 14, 17}) sw1 += kChan[bits(w, hi, hi - 2)];
  if (!dp4) sw1.resize(3);
  std::string sw0 = vmad_swz(bits(w, 21, 18), 0, dp4);
  std::string s1s = rs(s1, sw1);
  if (s1mods & 2) s1s = "|" + s1s + "|";
  if (s1mods & 1) s1s = "-" + s1s;
  std::string text = (pred ? "(p" + std::to_string(pred) + ")" : "") + "vdp" + (dp4 ? "4 " : "3 ") + rs(d, msk(mask)) +
                     " = " + s1s + " . i" + std::to_string(g0n) + "." + sw0 +
                     (clip ? " clip" + std::to_string(bits(w, 36, 34)) : "");
  if (!emit_) return text;
  if (pred) fail("predicated instruction");
  if (clip || rcount) fail("vdp clip / repeat");
  Reg g0{Bank::I, g0n};
  Id sum = 0;
  for (size_t k = 0; k < sw1.size(); k++) {
    Id p = b_.fmul(t_f_, lane_f32(s1, chan(sw1[k]), s1mods), lane_f32(g0, chan(sw0[k]), g0mods));
    sum = sum ? b_.fadd(t_f_, sum, p) : p;
  }
  for (uint32_t j = 0; j < 4; j++)
    if (mask >> j & 1) put(d, j, sum);
  commit();
  return text;
}

std::string Translator::vdual(uint64_t w) {  // 6.6: only the one form the game uses (FADD + VMOV)
  static const struct {
    const char* name;
    int hi, lo;
  } f[] = {{"cc4", 59, 59},    {"g1neg", 58, 58},  {"svpred", 57, 56}, {"skipinv", 55, 55}, {"op1ext", 54, 54},
           {"f16", 53, 53},    {"g1swx", 52, 52},  {"usw", 51, 48},    {"uneg", 47, 47},    {"op1", 46, 44},
           {"op2ext", 43, 43}, {"primU", 42, 42},  {"g0sw", 41, 38},   {"g1sw", 37, 34},    {"pdbank", 33, 32},
           {"ubank", 31, 30},  {"pdgpi", 29, 28},  {"pdn", 27, 21},    {"op2", 20, 18},     {"srccfg", 17, 16},
           {"g2n1", 15, 15},   {"g2n0_uabs", 14, 14}, {"g1slot", 13, 12}, {"g0slot", 11, 10}, {"wmask3", 9, 7},
           {"un", 6, 0}};
  std::string text = "VDUAL";
  std::map<std::string, uint32_t> v;
  for (const auto& e : f) {
    v[e.name] = bits(w, e.hi, e.lo);
    text += std::string(" ") + e.name + "=" + std::to_string(v[e.name]);
  }
  if (!emit_) return text;
  if (v["cc4"] || v["f16"] || v["svpred"] || v["op1ext"] != 1 || v["op1"] != 1 || v["op2ext"] || v["op2"] != 5 ||
      v["srccfg"] != 2 || v["primU"] || v["uneg"] || v["g1neg"] || v["g1swx"])
    fail("vdual form other than fadd + vmov");
  // op1: i[pdgpi].mask = i[g0slot].c0 + i[g1slot].c1 (scalar)
  Id a = lane_f32({Bank::I, v["g0slot"]}, chan(S3[v["g0sw"]][0]));
  Id c = lane_f32({Bank::I, v["g1slot"]}, chan(S3[v["g1sw"]][0]));
  Id sum = b_.fadd(t_f_, a, c);
  // op2: unified-store move, 3 lanes
  Reg d = mkreg(Slot::Dest, v["pdn"], v["pdbank"], 0, true, 8, second_);
  Reg s = mkreg(Slot::Src12, v["un"], v["ubank"], 0, true, 8, second_);
  if (d.bank != Bank::I) fail("vdual vmov to %s", rs(d).c_str());
  const char* sw = S3[v["usw"]];
  for (uint32_t j = 0; j < 3; j++) put(d, j, lane_f32(s, chan(sw[j])));
  for (uint32_t j = 0; j < 3; j++)
    if (v["wmask3"] >> j & 1) put({Bank::I, v["pdgpi"]}, j, sum);
  commit();
  return text;
}

std::string Translator::vcomp(uint64_t w) {  // 6.7
  static const char* const ops[4] = {"vrcp", "vrsq", "vlog", "vexp"};
  static const char* const types[4] = {"f32", "f16", "c10", "?"};
  uint32_t pred = bits(w, 58, 56), dtype = bits(w, 54, 53), rcount = bits(w, 47, 44), op = bits(w, 42, 41);
  uint32_t stype = bits(w, 40, 39), s1mod = bits(w, 38, 37), scomp = bits(w, 36, 35), mask = bits(w, 3, 0);
  Reg d = mkreg(Slot::Dest, bits(w, 27, 21), bits(w, 33, 32), bits(w, 51, 51), true, 8, second_);
  Reg s1 = mkreg(Slot::Src12, bits(w, 13, 7), bits(w, 31, 30), bits(w, 49, 49), true, 8, second_);
  std::string text = (pred ? "(p" + std::to_string(pred) + ")" : "") + ops[op] + "." + types[stype] + " " +
                     rs(d, msk(mask)) + " = " + modstr(rs(s1, std::string(1, "xyzw"[scomp])), s1mod) +
                     (dtype ? " dty=" + std::to_string(dtype) : "");
  if (!emit_) return text;
  if (pred) fail("predicated instruction");
  if (dtype || stype || rcount) fail("vcomp non-f32 / repeat");
  Id x = lane_f32(s1, int(scomp), s1mod), r;
  switch (op) {
    case 0: r = b_.fdiv(t_f_, fconst(1.0f), x); break;
    case 1: r = b_.rsqrt(t_f_, x); break;
    case 2: r = b_.log2(t_f_, x); break;
    default: r = b_.exp2(t_f_, x); break;
  }
  for (uint32_t j = 0; j < 4; j++)
    if (mask >> j & 1) put(d, j, r);
  commit();
  return text;
}

std::string Translator::vmov(uint64_t w) {  // 6.8
  static const char* const types[8] = {"i8", "i16", "i32", "c10", "f16", "f32", "u8", "u16"};
  static const char* const kinds[4] = {"vmov", "vmovc", "vmovcu8", "?"};
  uint32_t pred = bits(w, 58, 56), mtype = bits(w, 47, 46), rcount = bits(w, 45, 44), dtype = bits(w, 42, 40);
  uint32_t mask = bits(w, 27, 24);
  bool dbl = dtype == 3 || dtype == 4 || dtype == 5;
  int nbits = dbl ? 7 : 6;
  Reg d = mkreg(Slot::Dest, bits(w, 23, 18), bits(w, 33, 32), bits(w, 51, 51), dbl, nbits, second_);
  Reg s1 = mkreg(Slot::Src12, bits(w, 11, 6), bits(w, 31, 30), bits(w, 49, 49), dbl, nbits, second_);
  std::string sw = dbl ? S4[bits(w, 38, 35)] : "";
  std::string text = (pred ? "(p" + std::to_string(pred) + ")" : "") + kinds[mtype] + "." + types[dtype] + " " +
                     rs(d, msk(mask)) + " = " + rs(s1, sw) + (mtype ? "  (conditional)" : "") +
                     (rcount ? " rpt" + std::to_string(rcount) : "");
  if (!emit_) return text;
  if (pred) fail("predicated instruction");
  if (mtype || dtype != 5) fail("vmov form %s.%s", kinds[mtype], types[dtype]);
  if (rcount && (d.bank == Bank::I || s1.bank == Bank::I)) fail("repeated vmov on internal registers");
  for (uint32_t r = 0; r <= rcount; r++) {  // each pass advances both registers by one 64-bit unit
    Reg dr{d.bank, d.n + 2 * r}, sr{s1.bank, s1.n + 2 * r};
    for (uint32_t j = 0; j < 4; j++)
      if (mask >> j & 1) put(dr, j, lane_f32(sr, chan(sw[j])));
    commit();
  }
  return text;
}

std::string Translator::vpck(uint64_t w) {  // 6.9
  static const char* const fmts[8] = {"u8", "s8", "o8", "u16", "s16", "f16", "f32", "c10"};
  uint32_t pred = bits(w, 58, 56), rcount = bits(w, 47, 44), sfmt = bits(w, 43, 41), dfmt = bits(w, 40, 38);
  uint32_t mask = bits(w, 37, 34), scale = bits(w, 18, 18), c0b1 = bits(w, 7, 7), c0b0 = bits(w, 0, 0);
  uint32_t s1n = bits(w, 13, 8), s2n = bits(w, 6, 1);
  bool fl = sfmt == 5 || sfmt == 6 || sfmt == 7;
  Reg d = mkreg(Slot::Dest, bits(w, 27, 21), bits(w, 33, 32), bits(w, 51, 51), false, 7, second_);
  Reg s1 = mkreg(Slot::Src12, fl ? s1n : (c0b1 | s1n << 1), bits(w, 31, 30), bits(w, 49, 49), fl, 7, second_);
  Reg s2 = mkreg(Slot::Src12, s2n, bits(w, 29, 28), bits(w, 48, 48), true, 7, second_);
  uint32_t sel[4] = {c0b0 | (sfmt == 6 ? c0b1 << 1 : (s2n & 1) << 1), bits(w, 17, 16), bits(w, 15, 14), bits(w, 20, 19)};
  char buf[32];
  std::snprintf(buf, sizeof buf, " sel[%u%u%u%u]", sel[0], sel[1], sel[2], sel[3]);
  std::string text = (pred ? "(p" + std::to_string(pred) + ")" : "") + "vpck." + fmts[dfmt] + "." + fmts[sfmt] + " " +
                     rs(d, msk(mask)) + " = " + rs(s1) + "," + rs(s2) + buf + (scale ? " scale" : "");
  if (!emit_) return text;
  if (pred) fail("predicated instruction");
  if (sfmt != 6 || rcount) fail("vpck from %s / repeat", fmts[sfmt]);
  auto element = [&](uint32_t k) -> Id {  // the 4-entry source list
    if (s1.bank == Bank::I && s2.bank == Bank::Imm) return lane_f32(s1, int(k));
    if (unified(s1.bank) && unified(s2.bank)) return k < 2 ? lane_f32(s1, int(k)) : lane_f32(s2, int(k - 2));
    fail("vpck sources %s, %s", rs(s1).c_str(), rs(s2).c_str());
  };
  if (dfmt == 6) {  // f32 lane gather
    if (d.bank != Bank::I) fail("vpck f32 to %s", rs(d).c_str());
    for (uint32_t j = 0; j < 4; j++)
      if (mask >> j & 1) put(d, j, element(sel[j]));
    commit();
  } else if (dfmt == 0 || dfmt == 5) {  // u8x4 into one register / f16x4 into two
    if (mask != 15 || !unified(d.bank) || (dfmt == 0 && !scale)) fail("vpck %s form", fmts[dfmt]);
    Id e[4];
    for (int j = 0; j < 4; j++) e[j] = element(sel[j]);
    if (dfmt == 0) {
      set_u(d.bank, d.n, b_.pack_unorm4x8(t_u_, b_.composite_construct(t_v4_, {e[0], e[1], e[2], e[3]})));
    } else {
      Id lo = b_.pack_half2x16(t_u_, b_.composite_construct(t_v2_, {e[0], e[1]}));
      Id hi = b_.pack_half2x16(t_u_, b_.composite_construct(t_v2_, {e[2], e[3]}));
      set_u(d.bank, d.n, lo);
      set_u(d.bank, d.n + 1, hi);
    }
  } else {
    fail("vpck to %s", fmts[dfmt]);
  }
  return text;
}

std::string Translator::sop2(uint64_t w) {  // 6.10: recognised as one of the six fixed-function blends
  uint32_t pred = bits(w, 58, 57), cmod1 = bits(w, 56, 56), asel1 = bits(w, 53, 52), cmod2 = bits(w, 47, 47);
  uint32_t count = bits(w, 46, 44), amod1 = bits(w, 43, 43), asel2 = bits(w, 42, 41), csel1 = bits(w, 40, 38);
  uint32_t csel2 = bits(w, 37, 35), amod2 = bits(w, 34, 34), s1mod = bits(w, 20, 20), cop = bits(w, 19, 18);
  uint32_t aop = bits(w, 17, 16), as1mod = bits(w, 15, 15), dmod = bits(w, 14, 14);
  Reg d = mkreg(Slot::Dest, bits(w, 27, 21), bits(w, 33, 32), bits(w, 51, 51), false, 7, second_);
  Reg s1 = mkreg(Slot::Src12, bits(w, 13, 7), bits(w, 31, 30), bits(w, 49, 49), false, 7, second_);
  Reg s2 = mkreg(Slot::Src12, bits(w, 6, 0), bits(w, 29, 28), bits(w, 48, 48), false, 7, second_);
  char buf[160];
  std::snprintf(buf, sizeof buf,
                " cop=%u aop=%u csel=%u,%u asel=%u,%u cmod=%u,%u amod=%u,%u s1mod=%u as1m=%u dmod=%u cnt=%u", cop, aop,
                csel1, csel2, asel1, asel2, cmod1, cmod2, amod1, amod2, s1mod, as1mod, dmod, count);
  std::string text = "sop2 " + rs(d) + " = " + rs(s1) + "," + rs(s2) + buf;
  if (!emit_) return text;
  if (pred || count || s1mod || as1mod || dmod || cmod1 != 1 || amod1 != 1 || cmod2 || amod2)
    fail("sop2 form outside the six blends");
  if (!fragment_ || d.bank != Bank::O || d.n || s1.bank != Bank::O || s1.n || !unified(s2.bank))
    fail("sop2 operands %s = %s,%s", rs(d).c_str(), rs(s1).c_str(), rs(s2).c_str());
  static const struct {
    uint32_t cop, c1, c2, aop, a1, a2;
    Blend mode;
  } modes[] = {{0, 4, 4, 0, 2, 2, Blend::Alpha},     {0, 0, 4, 0, 2, 2, Blend::Add},
               {1, 0, 4, 1, 2, 2, Blend::Subtract},  {0, 3, 3, 0, 1, 1, Blend::DestAlpha},
               {0, 0, 3, 0, 1, 1, Blend::DestAlphaAdd}, {1, 0, 3, 1, 1, 1, Blend::DestAlphaSubtract}};
  for (const auto& m : modes)
    if (m.cop == cop && m.c1 == csel1 && m.c2 == csel2 && m.aop == aop && m.a1 == asel1 && m.a2 == asel2) blend_ = m.mode;
  if (blend_ == Blend::None) fail("sop2 blend variant not one of the six");
  blend_src_ = get_u(s2.bank, s2.n);
  blended_ = true;
  return text;
}

std::string Translator::step(uint64_t w) {
  Cls k = classify(w);
  if (emit_ && blended_ && k != Cls::NOP) fail("instruction after the blend");
  switch (k) {
    case Cls::V32NMAD: return nmad(w, false);
    case Cls::V16NMAD: return nmad(w, true);
    case Cls::VMAD2: return vmad2(w);
    case Cls::VMAD: return vmad(w);
    case Cls::VDP: return vdp(w);
    case Cls::VDUAL: return vdual(w);
    case Cls::VCOMP: return vcomp(w);
    case Cls::VMOV: return vmov(w);
    case Cls::VPCK: return vpck(w);
    case Cls::SOP2: return sop2(w);
    case Cls::PHAS: return "PHAS wait=" + std::to_string(bits(w, 42, 40)) + " imm=" + std::to_string(bits(w, 50, 50));
    case Cls::NOP: return "NOP";
    case Cls::SPEC:
      return "SPEC special=" + std::to_string(bits(w, 54, 54)) + " cat=" + std::to_string(bits(w, 53, 52));
    case Cls::Unknown: break;
  }
  if (emit_) fail("unknown instruction %016llx", (unsigned long long)w);
  return "UNKNOWN";
}

// --- interface -------------------------------------------------------------------------------------------------------
Id Translator::input_vec4(uint32_t location) {
  auto it = inputs_.find(location);
  if (it != inputs_.end()) return it->second;
  Id var = b_.variable(b_.type_pointer(spv::StorageClassInput, t_v4_), spv::StorageClassInput);
  b_.decorate(var, spv::DecorationLocation, {location});
  iface_.push_back(var);
  return inputs_[location] = b_.load(t_v4_, var);
}

// A vertex attribute as a float vec4, through the substitute fetch the backend asked for (Fetch bits).
Id Translator::vertex_input(uint32_t location) {
  uint8_t f = location < fetch_.size() ? fetch_[location] : 0;
  if (!(f & (kFetchUint | kFetchSint))) {
    Id v = input_vec4(location);
    return f & kFetchWOne ? b_.composite_insert(t_v4_, fconst(1.0f), v, {3}) : v;
  }
  bool s = f & kFetchSint;
  Id t = b_.type_vector(s ? b_.type_sint() : t_u_, 4);
  Id var = b_.variable(b_.type_pointer(spv::StorageClassInput, t), spv::StorageClassInput);
  b_.decorate(var, spv::DecorationLocation, {location});
  iface_.push_back(var);
  Id v = s ? b_.s2f(t_v4_, b_.load(t, var)) : b_.u2f(t_v4_, b_.load(t, var));
  return f & kFetchWOne ? b_.composite_insert(t_v4_, fconst(1.0f), v, {3}) : v;
}

Id Translator::output_vec4(uint32_t location, Id value) {
  Id var = b_.variable(b_.type_pointer(spv::StorageClassOutput, t_v4_), spv::StorageClassOutput);
  b_.decorate(var, spv::DecorationLocation, {location});
  iface_.push_back(var);
  b_.store(var, value);
  return var;
}

Id Translator::uniform_word(uint32_t word) {
  if (!ubo_ || word >= kUniformVec4s * 4) fail("uniform word %u", word);
  Id p = b_.access_chain(b_.type_pointer(spv::StorageClassUniform, t_u_), ubo_,
                         {b_.const_uint(0), b_.const_uint(word / 4), b_.const_uint(word % 4)});
  return b_.load(t_u_, p);
}

void Translator::setup_vertex(Shader& out) {
  if (u32(vary_ + 8) || u32(vary_ + 12)) fail("untyped vertex attributes");
  uint32_t params = rel(0x28), count = u32(0x24), location = 0;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t p = params + 16 * i, cat = u8(p + 4) & 15, type = u8(p + 4) >> 4, comps = u8(p + 5) & 15;
    if (cat != 0) continue;
    if (type != 0 || comps < 1 || comps > 4) fail("attribute type %u x%u", type, comps);
    uint32_t reg = u32(p + 12);
    Id v = vertex_input(location);
    for (uint32_t k = 0; k < comps; k++) set_f(Bank::PA, reg + k, b_.composite_extract(t_f_, v, {k}));
    out.inputs.push_back({location++, reg, comps});
  }
}

void Translator::setup_fragment(Shader& out) {
  uint32_t ndesc = u16(vary_ + 12), desc = rel(vary_ + 16), pa = 0;
  for (uint32_t i = 0; i < ndesc; i++) {
    uint32_t ai = u32(desc + 16 * i), res = u32(desc + 16 * i + 4), size = u32(desc + 16 * i + 8);
    uint32_t ci = u32(desc + 16 * i + 12);
    uint32_t iter = (ai >> 12) & 15, tex = ai & 15;
    if (iter != 15) {  // iterated varying -> PA (4.2), f32 only
      if (!(ai >> 20 & 1) || (ai >> 28 & 3)) fail("iterator format 0x%08x", ai);
      if (ai >> 30 & 1 || ai >> 4 & 1) fail("point-sprite / centroid iterator");
      uint32_t location;
      if (iter == 0xA) {
        location = 0;
        out.varyings |= kColor0;
      } else if (iter <= 9) {
        location = 1 + iter;
        out.varyings |= kTexcoord0 << iter;
      } else {
        fail("iterated varying %u", iter);
      }
      uint32_t regs = ((size >> 4) & 3) + 1;
      Id v = input_vec4(location);
      for (uint32_t k = 0; k < regs; k++) set_f(Bank::PA, pa + k, b_.composite_extract(t_f_, v, {k}));
      pa += regs;
    }
    if (tex != 15) {  // non-dependent texture read -> PA
      if (tex > 9 || ((ai >> 8) & 3) != 1 || (ai >> 10 & 1)) fail("texture read 0x%08x", ai);
      if (res >= 16) fail("texture unit %u", res);
      Id coord = input_vec4(1 + tex);
      out.varyings |= kTexcoord0 << tex;
      Id t_si = b_.type_sampler2d();
      Id var = b_.variable(b_.type_pointer(spv::StorageClassUniformConstant, t_si), spv::StorageClassUniformConstant);
      b_.decorate(var, spv::DecorationDescriptorSet, {0});
      b_.decorate(var, spv::DecorationBinding, {kTextureBinding + res});
      Id texel = b_.image_sample(t_v4_, b_.load(t_si, var), b_.vector_shuffle(t_v2_, coord, coord, {0, 1}));
      out.texture_units |= 1u << res;
      uint32_t regs = ((size >> 6) & 3) + 1, type = (ci >> 4) & 3;
      if (type == 3) {
        for (uint32_t k = 0; k < regs && k < 4; k++) set_f(Bank::PA, pa + k, b_.composite_extract(t_f_, texel, {k}));
      } else if (type == 2 && regs == 2) {
        set_u(Bank::PA, pa, b_.pack_half2x16(t_u_, b_.vector_shuffle(t_v2_, texel, texel, {0, 1})));
        set_u(Bank::PA, pa + 1, b_.pack_half2x16(t_u_, b_.vector_shuffle(t_v2_, texel, texel, {2, 3})));
      } else {
        fail("texture result type %u x%u regs", type, regs);
      }
      pa += regs;
    }
  }
}

void Translator::finish_vertex(Shader& out) {
  uint32_t vo1 = u32(vary_ + 16), vo2 = u32(vary_ + 20);
  if ((vo1 & ~0xff001800u) || !(vo1 & 0x1000) || (vo2 & ~7u) || u32(vary_ + 24)) fail("vertex outputs %08x %08x", vo1, vo2);
  auto o = [&](uint32_t n) { return regs_[2].count(n) ? get_f(Bank::O, n) : fconst(0.0f); };
  Id pos_var = b_.variable(b_.type_pointer(spv::StorageClassOutput, t_v4_), spv::StorageClassOutput);
  b_.decorate(pos_var, spv::DecorationBuiltIn, {spv::BuiltInPosition});
  iface_.push_back(pos_var);
  for (uint32_t k = 0; k < 4; k++)
    if (!regs_[2].count(k)) fail("position o%u not written", k);
  // GXM viewport -> Vulkan clip space in the shader (push constants scale, offset): pos * scale + pos.w * offset.
  // One fixed full-target Vulkan viewport then covers both GXM modes (viewport enabled, or positions in pixels).
  Id t_pc = b_.type_struct({t_v4_, t_v4_});
  b_.decorate(t_pc, spv::DecorationBlock);
  b_.member_decorate(t_pc, 0, spv::DecorationOffset, {0});
  b_.member_decorate(t_pc, 1, spv::DecorationOffset, {16});
  Id pc = b_.variable(b_.type_pointer(spv::StorageClassPushConstant, t_pc), spv::StorageClassPushConstant);
  Id p_v4 = b_.type_pointer(spv::StorageClassPushConstant, t_v4_);
  Id scale = b_.load(t_v4_, b_.access_chain(p_v4, pc, {b_.const_uint(0)}));
  Id offset = b_.load(t_v4_, b_.access_chain(p_v4, pc, {b_.const_uint(1)}));
  Id pos = b_.composite_construct(t_v4_, {o(0), o(1), o(2), o(3)});
  b_.store(pos_var, b_.fadd(t_v4_, b_.fmul(t_v4_, pos, scale), b_.vec_times_scalar(t_v4_, offset, o(3))));
  uint32_t next = 4;
  if (vo1 & 0x800) {
    output_vec4(0, b_.composite_construct(t_v4_, {o(4), o(5), o(6), o(7)}));
    out.varyings |= kColor0;
    next = 8;
  }
  if (vo2 & 7) {
    uint32_t comps = 2 + (vo2 >> 1 & 1) + (vo2 >> 2 & 1);
    Id c[4];
    for (uint32_t k = 0; k < 4; k++) c[k] = k < comps ? o(next + k) : fconst(0.0f);
    output_vec4(1, b_.composite_construct(t_v4_, {c[0], c[1], c[2], c[3]}));
    out.varyings |= kTexcoord0;
    next += comps;
  }
  if ((vo1 >> 24) != next) fail("vertex output count %u, layout gives %u", vo1 >> 24, next);
}

void Translator::finish_fragment(Shader& out) {
  Id color;
  out.native_color = flags_ & 0x40;
  if (out.native_color) {
    Id u = blended_ ? blend_src_ : get_u(Bank::O, 0);
    color = b_.unpack_unorm4x8(t_v4_, u);
    out.blend = blend_;
  } else {
    uint32_t start = u8(vary_ + 8), type = u8(vary_ + 10), comps = u8(vary_ + 11);
    if (type != 1 || comps != 4 || blended_) fail("fragment output type %u x%u", type, comps);
    Id lo = b_.unpack_half2x16(t_v2_, get_u(Bank::PA, start)), hi = b_.unpack_half2x16(t_v2_, get_u(Bank::PA, start + 1));
    color = b_.vector_shuffle(t_v4_, lo, hi, {0, 1, 2, 3});
  }
  output_vec4(0, color);
}

void Translator::run(Shader& out) {
  if (g_.size() < 0x9c || u32(0) != 0x00505847) fail("not a GXP program");
  if (u32(8) > g_.size()) fail("GXP size field beyond the data");
  flags_ = u32(0x14);
  fragment_ = flags_ & 1;
  out.stage = fragment_ ? Stage::Fragment : Stage::Vertex;
  std::vector<uint64_t> prim(u32(0x3c)), sec(u32(0x44));
  for (size_t i = 0; i < prim.size(); i++) prim[i] = word(rel(0x40) + uint32_t(8 * i));
  for (size_t i = 0; i < sec.size(); i++) sec[i] = word(rel(0x48) + uint32_t(8 * i));

  char head[128];
  std::snprintf(head, sizeof head, "%s prim %zu sec %zu pa %u sa %u flags %x\n", fragment_ ? "frag" : "vert",
                prim.size(), sec.size(), u16(0x30), u16(0x32), flags_);
  out.listing = head;
  for (int pass = 0; pass < 2; pass++) {
    second_ = true;
    for (size_t i = 0; i < sec.size(); i++) {
      std::snprintf(head, sizeof head, " S%02zu %016llx ", i, (unsigned long long)sec[i]);
      std::string t = step(sec[i]);
      if (!emit_) out.listing += head + t + "\n";
    }
    second_ = false;
    for (size_t i = 0; i < prim.size(); i++) {
      std::snprintf(head, sizeof head, " P%02zu %016llx ", i, (unsigned long long)prim[i]);
      std::string t = step(prim[i]);
      if (!emit_) out.listing += head + t + "\n";
    }
    if (emit_) break;

    // Emission pass: interface first, then the same walk.
    emit_ = true;
    if (u32(0x2c) == 0) fail("no varyings block");
    vary_ = rel(0x2c);
    if (u32(0x18) & ~0x30000000u || u32(0x20) || u32(0x78) || u32(0x80) || u32(0x88))
      fail("uniform buffers in memory / dependent samplers");
    ub_words_ = u32(0x64);
    out.uniform_words = ub_words_;
    b_.capability(spv::CapabilityShader);
    t_void_ = b_.type_void();
    t_f_ = b_.type_float();
    t_u_ = b_.type_uint();
    t_v2_ = b_.type_vector(t_f_, 2);
    t_v4_ = b_.type_vector(t_f_, 4);
    fn_ = b_.begin_function(t_void_, b_.type_function(t_void_));
    b_.name(fn_, "main");
    b_.label();
    for (uint32_t i = 0, n = u32(0x90), c = rel(0x94); i < n; i++) {  // containers (1.5)
      uint32_t index = u16(c + 8 * i), base = u16(c + 8 * i + 4), size = u16(c + 8 * i + 6);
      if (index == 14) {
        ub_base_ = base;
        if (size != ub_words_) fail("default uniform container size %u vs %u", size, ub_words_);
      } else if (index == 19) {
        for (uint32_t l = 0, nl = u32(0x70), lo = rel(0x74); l < nl; l++) {
          uint32_t off = u32(lo + 8 * l), bitsv = u32(lo + 8 * l + 4);
          if (off >= size) fail("literal offset %u", off);
          float f;
          std::memcpy(&f, &bitsv, 4);
          set_f(Bank::SA, base + off, fconst(f));
        }
      } else {
        fail("container %u", index);
      }
    }
    if (ub_words_) {
      if (ub_words_ > kUniformVec4s * 4) fail("default uniform buffer of %u words", ub_words_);
      Id t_uv4 = b_.type_vector(t_u_, 4);
      Id t_block = b_.type_struct({b_.type_array(t_uv4, kUniformVec4s, 16)});
      b_.decorate(t_block, spv::DecorationBlock);
      b_.member_decorate(t_block, 0, spv::DecorationOffset, {0});
      ubo_ = b_.variable(b_.type_pointer(spv::StorageClassUniform, t_block), spv::StorageClassUniform);
      b_.decorate(ubo_, spv::DecorationDescriptorSet, {0});
      b_.decorate(ubo_, spv::DecorationBinding, {fragment_ ? 1u : 0u});
    }
    if (fragment_)
      setup_fragment(out);
    else
      setup_vertex(out);
  }
  if (fragment_)
    finish_fragment(out);
  else
    finish_vertex(out);
  b_.ret();
  b_.end_function();
  b_.entry_point(fragment_ ? spv::ExecutionModelFragment : spv::ExecutionModelVertex, fn_, "main",
                 std::span<const Id>(iface_));
  if (fragment_) b_.execution_mode(fn_, spv::ExecutionModeOriginUpperLeft);
  out.spirv = b_.finish();
}

}  // namespace

Shader translate(std::span<const uint8_t> gxp, std::span<const uint8_t> fetch) {
  Shader out;
  try {
    Translator t(gxp, fetch);
    t.run(out);
    out.ok = true;
  } catch (const Fail& f) {
    out.error = f.what;
  }
  return out;
}

}  // namespace gpu::usse
