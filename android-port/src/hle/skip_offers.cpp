// Story / tutorial skip questions on the 1.09 eboot, see skip_offers.h and notes/map/vita_story_skip_109.md for the
// asm evidence (capstone, Work/vita/update109.elf).
//
// Story skip, creation screen 0x812ae7f6, state 1, command 7 (confirm), asm 0x812aeb90..0x812aebda:
//   r6 = any of the six "class k cleared the game" header bytes (slot+0x20+k, getter 0x812f664e) > 0
//   0x812aebba  cbz r0, ->state 8      (r0 = obj+0xea66: new hero, or an edited hero with story counter <= 2)
//   0x812aebbc  cbz r6, ->state 8      <- NOPed
//   0x812aebc8  movs r2, #9 ; blx [vt+0x4c]   (state 9 = the offer, msg 0x3cc; "yes" = the game's own writes)
// Tutorial skip, hero menu 0x812ab56c, asm 0x812abd9c..:
//   0x812abda2  bl 0x812f6750 (story counter > 0 or file tutorial mark slot+0x48) ; cmp r0, #0
//   0x812abda8  bne 0x812abdf6 (dialog 0x5d8)      <- b: always the dialog. The fall-through is the auto-start (wait
//               ~3 s, then state 9 = round/Tutorial.brb). The dialog's "no" writes hero+0x103 = 1 (0x812a99dc).
//
// No-skip warning (with the story skip). Online needs hero+0x6f >= 0x2d (0x812f6472 -> 0x8137e6a2), which the skip
// sets, so the first "no" asks again with a warning; the second answer is final. State 9 of 0x812ae7f6:
//   0x812af2e6  bl 0x81325c8c (dialog text by id: -> 0x8105d46e, resolver call at 0x8105d528)
//   0x8105d528  bl 0x813ea3a8 (id -> sheet string, then vsnprintf'd)     <- bl stub L
//   0x812af47c  "no" (answer 0 cancel / 3 no): SetState(kind, 8) via [vt+0x4c], then 0x812af488 (return -1)
//               <- b.w stub B (replaces ldr r1,[r7] ; adds r0,r7,#0)
//   0x812af306  first "yes" instruction str.w r7, [fp, #0x2f8]        <- b.w stub Y
// Two flag bytes: [0] text_due (cleared by the one lookup it serves), [1] in_second. Stub B: second answer -> clear,
// state 8 as before; first -> set both, SetState(kind, 9). vt+0x4c = 0x813e89ca -> vt+0x44 = 0x813e8550, which
// raises the first-frame bit 0x800 << kind even for the same state, so the next frame builds a fresh YN dialog
// (0x813799fc) whose 0x3cc text is ours. Stub Y clears both. Stubs live in the zero tail of the text segment's last
// page (text ends 0x81772664), within b.w / bl range of every site; flags and text in a page of our own.
// Registers: L uses r0 (result), r2, r3 (caller-saved at a call; the caller reloads r2 from r6); B and Y use r0-r3,
// which the replaced code sets before any read.
#include "hle/skip_offers.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "guest/memory.h"

namespace hle {
namespace {

using guest::Memory;

struct Site {
  const char* env;
  const char* what;
  uint32_t at;
  uint8_t expect[2];
  uint8_t repl[2];
};

constexpr Site kSites[] = {
    {"DCP_STORY_SKIP", "story skip offered to every new character", 0x812aebbc, {0x3e, 0xb1}, {0x00, 0xbf}},  // cbz r6 -> nop
    {"DCP_TUTORIAL_SKIP", "tutorial question asked on the first character too", 0x812abda8, {0x25, 0xd1},
     {0x25, 0xe0}},  // bne 0x812abdf6 -> b 0x812abdf6
};

// The second ask: our text replaces message 0x3cc once. ASCII + '\n' only, no '%' (it is a vsnprintf format).
constexpr char kWarning[] =
    "Without the skip, this character can't play online\n"
    "until the first half of the story is finished.\n"
    "Skip to the talisman collecting? (Player starts at LV15)";

constexpr uint32_t kMsgId = 0x3cc;
constexpr uint32_t kLookupCall = 0x8105d528, kLookup = 0x813ea3a8;
constexpr uint32_t kNo = 0x812af47c, kNoCont = 0x812af488;
constexpr uint32_t kYes = 0x812af306, kYesCont = 0x812af30a;
constexpr uint32_t kCave = 0x81772700, kCaveEnd = 0x81773000;
// bl 0x813ea3a8
constexpr uint8_t kLookupCallBytes[] = {0x8c, 0xf3, 0x3e, 0xff};
// ldr r1,[r7] ; adds r0,r7,#0 ; ldr r3,[r1,#0x4c] ; mov r1,sb ; movs r2,#8 ; blx r3
constexpr uint8_t kNoBytes[] = {0x39, 0x68, 0x38, 0x1c, 0xcb, 0x6c, 0x49, 0x46, 0x08, 0x22, 0x98, 0x47};
// str.w r7, [fp, #0x2f8]
constexpr uint8_t kYesBytes[] = {0xcb, 0xf8, 0xf8, 0x72};

bool off(const char* env) {
  const char* v = std::getenv(env);
  return v && std::strcmp(v, "0") == 0;
}

// Thumb-2 BL / B.W (T4) from `at` to `to`, both Thumb; false when out of the +-16 MiB range.
bool enc32(uint32_t at, uint32_t to, bool link, uint8_t out[4]) {
  int64_t off = int64_t(to) - int64_t(at) - 4;
  if (off < -(int64_t(1) << 24) || off >= (int64_t(1) << 24) || (off & 1)) return false;
  uint32_t u = uint32_t(off);
  uint32_t s = (u >> 24) & 1, i1 = (u >> 23) & 1, i2 = (u >> 22) & 1;
  uint32_t j1 = ~(i1 ^ s) & 1, j2 = ~(i2 ^ s) & 1;
  uint16_t h1 = uint16_t(0xf000 | s << 10 | ((u >> 12) & 0x3ff));
  uint16_t h2 = uint16_t((link ? 0xd000 : 0x9000) | j1 << 13 | j2 << 11 | ((u >> 1) & 0x7ff));
  out[0] = uint8_t(h1), out[1] = uint8_t(h1 >> 8), out[2] = uint8_t(h2), out[3] = uint8_t(h2 >> 8);
  return true;
}

// A small Thumb assembler: 16-bit opcodes as given, labels for short branches, a literal pool at the end.
struct Thumb {
  enum Kind { kBne, kB, kCbz, kCbnz, kLdrLit };
  struct Fix {
    size_t pos;
    Kind kind;
    size_t target;
    uint16_t reg;
  };
  explicit Thumb(uint32_t at) : base(at) {}
  uint32_t base;  // 4-aligned
  std::vector<uint8_t> b;
  std::vector<Fix> fixes;
  std::vector<long> labels;
  std::vector<uint32_t> lits;
  bool ok = true;

  uint32_t here() const { return base + uint32_t(b.size()); }
  void h(uint16_t v) { b.push_back(uint8_t(v)), b.push_back(uint8_t(v >> 8)); }
  void bytes(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
  size_t label() { return labels.push_back(-1), labels.size() - 1; }
  void bind(size_t l) { labels[l] = long(b.size()); }
  void branch(Kind k, size_t l, uint16_t rn = 0) { fixes.push_back({b.size(), k, l, rn}), h(0); }
  void ldr_lit(uint16_t rt, uint32_t v) { lits.push_back(v), fixes.push_back({b.size(), kLdrLit, lits.size() - 1, rt}), h(0); }
  void far(uint32_t to, bool link) {
    uint8_t w[4];
    ok = ok && enc32(here(), to, link, w);
    bytes(w, 4);
  }
  bool finish() {
    if (b.size() & 2) h(0xbf00);  // nop: the pool is word aligned
    size_t pool = b.size();
    for (uint32_t v : lits) h(uint16_t(v)), h(uint16_t(v >> 16));
    for (const Fix& f : fixes) {
      uint32_t at = base + uint32_t(f.pos);
      uint32_t v;
      if (f.kind == kLdrLit) {  // ldr rt, [pc, #imm8*4]: from Align(at + 4, 4)
        long d = long(base + pool + 4 * f.target) - long((at + 4) & ~3u);
        if (d < 0 || d > 1020) return false;
        v = 0x4800u | f.reg << 8 | uint32_t(d / 4);
      } else {
        if (labels[f.target] < 0) return false;
        long d = long(base + labels[f.target]) - long(at + 4);
        if (f.kind == kBne) {
          if (d < -256 || d > 254) return false;
          v = 0xd100u | (uint32_t(d >> 1) & 0xff);
        } else if (f.kind == kB) {
          if (d < -2048 || d > 2046) return false;
          v = 0xe000u | (uint32_t(d >> 1) & 0x7ff);
        } else {  // cbz / cbnz: forward 0..126
          if (d < 0 || d > 126) return false;
          v = 0xb100u | (f.kind == kCbnz ? 0x800u : 0) | uint32_t((d >> 6) & 1) << 9 | uint32_t((d >> 1) & 0x1f) << 3 | f.reg;
        }
      }
      b[f.pos] = uint8_t(v), b[f.pos + 1] = uint8_t(v >> 8);
    }
    return ok;
  }
};

bool same(const Memory& mem, uint32_t at, const uint8_t* bytes, uint32_t n) {
  return mem.valid(at, n) && std::memcmp(mem.ptr(at), bytes, n) == 0;
}

void dump(const char* name, const Thumb& t) {
  std::fprintf(stderr, "[skip] stub %s at 0x%08x: ", name, t.base);
  for (uint8_t x : t.b) std::fprintf(stderr, "%02x", x);
  std::fprintf(stderr, "\n");
}

void install_warning(Memory& mem) {
  uint8_t self[4];
  if (!same(mem, kLookupCall, kLookupCallBytes, sizeof kLookupCallBytes) || !same(mem, kNo, kNoBytes, sizeof kNoBytes) ||
      !same(mem, kYes, kYesBytes, sizeof kYesBytes) || !enc32(kLookupCall, kLookup, true, self) ||
      std::memcmp(self, kLookupCallBytes, 4) != 0) {
    std::fprintf(stderr, "[skip] unexpected bytes at the warning sites; no-skip warning not installed\n");
    return;
  }
  if (!mem.valid(kCave, kCaveEnd - kCave)) {
    std::fprintf(stderr, "[skip] no stub room after the text segment; no-skip warning not installed\n");
    return;
  }
  for (uint32_t a = kCave; a < kCaveEnd; a++)
    if (mem.read<uint8_t>(a) != 0) {
      std::fprintf(stderr, "[skip] stub room after the text segment is in use; no-skip warning not installed\n");
      return;
    }
  uint32_t data = mem.alloc(guest::PAGE, "skip warning");
  if (!data) return;
  const uint32_t flags = data, text = data + 0x10;  // [0] text_due, [1] in_second
  mem.copy_in(text, kWarning, sizeof kWarning);

  Thumb L{kCave}, B{kCave + 0x80}, Y{kCave + 0x100};
  {  // id lookup: if (id == 0x3cc && text_due) { text_due = 0; return text; } else the original resolver
    size_t orig = L.label();
    L.ldr_lit(2, kMsgId);
    L.h(0x4291);  // cmp r1, r2
    L.branch(Thumb::kBne, orig);
    L.ldr_lit(2, flags);
    L.h(0x7813);  // ldrb r3, [r2]
    L.branch(Thumb::kCbz, orig, 3);
    L.h(0x2300);  // movs r3, #0
    L.h(0x7013);  // strb r3, [r2]
    L.ldr_lit(0, text);
    L.h(0x4770);  // bx lr
    L.bind(orig);
    L.far(kLookup, false);  // b.w 0x813ea3a8 (lr = the caller's)
  }
  {  // a "no": second -> final (state 8); first -> both flags, SetState(kind, 9)
    size_t final_no = B.label(), common = B.label();
    B.ldr_lit(0, flags);
    B.h(0x7841);  // ldrb r1, [r0, #1]
    B.branch(Thumb::kCbnz, final_no, 1);
    B.h(0x2101);  // movs r1, #1
    B.h(0x7001);  // strb r1, [r0]
    B.h(0x7041);  // strb r1, [r0, #1]
    B.h(0x2209);  // movs r2, #9
    B.branch(Thumb::kB, common);
    B.bind(final_no);
    B.h(0x2100);  // movs r1, #0
    B.h(0x7001);  // strb r1, [r0]
    B.h(0x7041);  // strb r1, [r0, #1]
    B.h(0x2208);  // movs r2, #8
    B.bind(common);
    B.bytes(kNoBytes, 8);       // ldr r1,[r7] ; adds r0,r7,#0 ; ldr r3,[r1,#0x4c] ; mov r1,sb
    B.bytes(kNoBytes + 10, 2);  // blx r3
    B.far(kNoCont, false);      // b.w 0x812af488 (movs.w r0, #-1 ; epilogue)
  }
  {  // a "yes": clear both, replay str.w r7, [fp, #0x2f8]
    Y.ldr_lit(0, flags);
    Y.h(0x2100);  // movs r1, #0
    Y.h(0x7001);  // strb r1, [r0]
    Y.h(0x7041);  // strb r1, [r0, #1]
    Y.bytes(kYesBytes, sizeof kYesBytes);
    Y.far(kYesCont, false);  // b.w 0x812af30a (movs r1, #0x2d)
  }
  uint8_t jl[4], jb[4], jy[4];
  if (!L.finish() || !B.finish() || !Y.finish() || L.b.size() > 0x80 || B.b.size() > 0x80 || Y.b.size() > 0x80 ||
      !enc32(kLookupCall, L.base, true, jl) || !enc32(kNo, B.base, false, jb) || !enc32(kYes, Y.base, false, jy)) {
    std::fprintf(stderr, "[skip] stub assembly failed; no-skip warning not installed\n");
    return;
  }
  for (const Thumb* t : {&L, &B, &Y}) mem.copy_in(t->base, t->b.data(), uint32_t(t->b.size()));
  mem.copy_in(kLookupCall, jl, 4);
  mem.copy_in(kNo, jb, 4);
  mem.copy_in(kYes, jy, 4);
  if (const char* d = std::getenv("DCP_SKIP_DUMP"); d && *d && *d != '0') {
    dump("L", L), dump("B", B), dump("Y", Y);
    for (uint32_t at : {kLookupCall, kNo, kYes}) {
      const uint8_t* p = mem.ptr(at);
      std::fprintf(stderr, "[skip] site 0x%08x: %02x%02x%02x%02x\n", at, p[0], p[1], p[2], p[3]);
    }
  }
  std::fprintf(stderr, "[skip] the first \"no\" to the story skip warns and asks again (flags 0x%08x)\n", flags);
}

}  // namespace

void skip_offers_install(Memory& mem) {
  for (const Site& s : kSites) {
    if (off(s.env)) continue;  // off: game code untouched
    if (!same(mem, s.at, s.expect, 2)) {
      std::fprintf(stderr, "[skip] unexpected bytes at 0x%08x; %s not installed\n", s.at, s.what);
      continue;
    }
    mem.copy_in(s.at, s.repl, 2);
    std::fprintf(stderr, "[skip] %s (0x%08x)\n", s.what, s.at);
  }
  if (!off("DCP_STORY_SKIP") && !off("DCP_SKIP_WARNING")) install_warning(mem);
}

}  // namespace hle
