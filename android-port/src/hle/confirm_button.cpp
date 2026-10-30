// See confirm_button.h; asm (capstone, Work/vita/update109.elf):
//   0x8101841c  str.w r1, [r6, #0xc4]   (DcCore startup, r1 = 1)        -> nop ; nop (the ctor's 0 stays)
//   0x811f087a  movs r1, #1             (PlayerCtrl_ReadActions: act 1)  -> movs r1, #0x1d
#include "hle/confirm_button.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "guest/memory.h"

namespace hle {
namespace {

struct Site {
  uint32_t at;
  uint8_t n;
  uint8_t expect[4];
  uint8_t repl[4];
};

constexpr Site kSites[] = {
    {0x8101841c, 4, {0xc6, 0xf8, 0xc4, 0x10}, {0x00, 0xbf, 0x00, 0xbf}},
    {0x811f087a, 2, {0x01, 0x21}, {0x1d, 0x21}},
};

}  // namespace

void confirm_button_install(guest::Memory& mem) {
  const char* v = std::getenv("DCP_CONFIRM");
  if (!v || std::strcmp(v, "circle") != 0) return;
  for (const Site& s : kSites)  // all or nothing: half the layout would be worse than none
    if (!mem.valid(s.at, s.n) || std::memcmp(mem.ptr(s.at), s.expect, s.n) != 0) {
      std::fprintf(stderr, "[confirm] unexpected bytes at 0x%08x; Japanese layout not installed\n", s.at);
      return;
    }
  for (const Site& s : kSites) mem.copy_in(s.at, s.repl, s.n);
  std::fprintf(stderr, "[confirm] Japanese layout (Circle confirms, Cross cancels)\n");
}

}  // namespace hle
