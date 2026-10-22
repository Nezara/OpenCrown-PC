// Helpers for writing HLE functions: AAPCS argument access, results, guest strings, logging.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

#include "hle/nids.h"
#include "guest/cpu.h"
#include "guest/hle.h"

// Registers an HLE function under the generated NID of the import with that exact name.
#define REG(name, fn) guest::hle_register(hle::NID_##name, #name, fn)

namespace hle {

using guest::Cpu;
using guest::Memory;

// Argument i (32-bit) of a non-variadic call: r0-r3, then [sp], [sp+4], ...
inline uint32_t arg(Cpu& c, int i) { return i < 4 ? c.reg(i) : c.mem().read<uint32_t>(c.reg(13) + 4u * (i - 4)); }
inline void ret(Cpu& c, uint32_t v) { c.set_reg(0, v); }
inline void ret64(Cpu& c, uint64_t v) {
  c.set_reg(0, uint32_t(v));
  c.set_reg(1, uint32_t(v >> 32));
}

// Sequential AAPCS argument reader for variadic calls (core registers first, then the stack) or for a va_list
// (a guest pointer to the saved arguments). 64-bit values are 8-aligned and never split between r3 and the stack.
class Args {
 public:
  // Variadic call: the first `fixed` arguments are named and already consumed.
  Args(Cpu& c, int fixed) : c_(c), ncrn_(fixed), nsaa_(c.reg(13)) {}
  // va_list: arguments start at guest address `ap`.
  Args(Cpu& c, uint32_t ap, bool) : c_(c), ncrn_(4), nsaa_(ap) {}

  uint32_t u32() {
    if (ncrn_ < 4) return c_.reg(ncrn_++);
    uint32_t v = c_.mem().read<uint32_t>(nsaa_);
    nsaa_ += 4;
    return v;
  }
  uint64_t u64() {
    if (ncrn_ & 1) ncrn_++;
    if (ncrn_ <= 2) {
      uint64_t lo = c_.reg(ncrn_), hi = c_.reg(ncrn_ + 1);
      ncrn_ += 2;
      return lo | hi << 32;
    }
    ncrn_ = 4;
    nsaa_ = (nsaa_ + 7) & ~7u;
    uint64_t v = c_.mem().read<uint64_t>(nsaa_);
    nsaa_ += 8;
    return v;
  }

 private:
  Cpu& c_;
  int ncrn_;
  uint32_t nsaa_;
};

inline std::string str(Cpu& c, uint32_t va, size_t max = 4096) { return c.mem().cstr(va, max); }

// The game's heap (SceLibc malloc / free, libc.cpp) for guest-visible blocks HLE code hands to guest callbacks.
uint32_t guest_malloc(Memory& m, uint32_t n);
void guest_free(uint32_t a);

// One-line log for HLE paths that are simplified or not fully known yet.
#define HLE_NOTE(...) std::fprintf(stderr, "[hle] " __VA_ARGS__)

void register_kernel();
void register_threads();
void register_sync();
void register_ult();
void register_libc();
void register_sysmodule();
void register_ctrl();
void register_power();
void register_io();
void register_libm();
void register_rtc();
void register_ngs();
void register_audio();
void register_display();
void register_gxm();
// True on the GXM display queue thread (runs the game's display callbacks; gxm.cpp).
bool gxm_on_display_queue_thread();
// [timing] line: how long sceGxmDisplayQueueAddEntry waited for the queue, per frame since the last call.
std::string gxm_display_queue_stats(double frames);
void register_apputil();
void register_dialog();
void register_np();
void register_net();
void register_matching2();
void register_touch();
void register_pvf();

inline void register_all() {
  register_kernel();
  register_threads();
  register_sync();
  register_ult();
  register_libc();
  register_sysmodule();
  register_ctrl();
  register_power();
  register_io();
  register_libm();
  register_rtc();
  register_ngs();
  register_audio();
  register_display();
  register_gxm();
  register_apputil();
  register_dialog();
  register_np();
  register_net();
  register_matching2();
  register_touch();
  register_pvf();
}

}  // namespace hle
