// SceLibm (hard-float ABI: float args in s0/s1, double in d0, ints in r0..) and SceRtcUser.
#include <chrono>
#include <cmath>
#include <ctime>

#include "hle/common.h"

namespace hle {
namespace {

// --- SceLibm
void Acosf(Cpu& c) { c.set_sreg(0, std::acos(c.sreg(0))); }
void Asinf(Cpu& c) { c.set_sreg(0, std::asin(c.sreg(0))); }
void Atan2f(Cpu& c) { c.set_sreg(0, std::atan2(c.sreg(0), c.sreg(1))); }
void Expf(Cpu& c) { c.set_sreg(0, std::exp(c.sreg(0))); }
void Ceilf(Cpu& c) { c.set_sreg(0, std::ceil(c.sreg(0))); }
void Powf(Cpu& c) { c.set_sreg(0, std::pow(c.sreg(0), c.sreg(1))); }
void Fabsf(Cpu& c) { c.set_sreg(0, std::fabs(c.sreg(0))); }
void Sqrtf(Cpu& c) { c.set_sreg(0, std::sqrt(c.sreg(0))); }
void Fmodf(Cpu& c) { c.set_sreg(0, std::fmod(c.sreg(0), c.sreg(1))); }
// double modf(double x, double* ip): x in d0, ip in r0 (hard-float).
void Modf(Cpu& c) {
  double ip = 0, f = std::modf(c.dreg(0), &ip);
  if (arg(c, 0)) c.mem().write<double>(arg(c, 0), ip);
  c.set_dreg(0, f);
}

// Dinkumware internal _FLog(float x, int base): 0 = ln, 1 = log10 (asm: the game
// passes 0 at 0x813dfdc0), -1 = log2 (Dinkumware convention, unseen in this game).
double log_base(double x, int32_t base) { return base == 0 ? std::log(x) : base > 0 ? std::log10(x) : std::log2(x); }
void FLog(Cpu& c) { c.set_sreg(0, float(log_base(c.sreg(0), int32_t(c.reg(0))))); }

// _FSinx(float x, unsigned qoff, int quads): sin for qoff 0, cos for qoff 1 (sinf/cosf). Every call site in the
// game passes quads = 0 (x in radians); other values are not modelled.
void FSinx(Cpu& c) {
  float x = c.sreg(0);
  uint32_t qoff = c.reg(0);
  if (c.reg(1) != 0) return c.set_fault("_FSinx: quads != 0 not modelled");
  float r = (qoff & 1) ? std::cos(x) : std::sin(x);
  c.set_sreg(0, (qoff & 2) ? -r : r);
}

// --- SceRtcUser. SceRtcTick = microseconds since 0001-01-01 00:00:00 UTC; SceDateTime = u16 year, month, day,
// hour, minute, second; u32 microsecond.
constexpr uint64_t US_PER_DAY = 86400ull * 1000000;
constexpr uint64_t UNIX_EPOCH_TICK = 719162ull * US_PER_DAY;    // days from 0001-01-01 to 1970-01-01
constexpr uint64_t WIN32_EPOCH_TICK = 584388ull * US_PER_DAY;   // days from 0001-01-01 to 1601-01-01

// Proleptic Gregorian days since 0001-01-01 (Howard Hinnant's days_from_civil, shifted).
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = unsigned(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + int64_t(doe) - 719468 + 719162;
}
void civil_from_days(int64_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468 - 719162;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = unsigned(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = int(int64_t(yoe) + era * 400 + (m <= 2));
}

uint64_t now_tick() {
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
  return UNIX_EPOCH_TICK + uint64_t(us);
}

void write_datetime(Cpu& c, uint32_t va, uint64_t tick) {
  int y;
  unsigned m, d;
  civil_from_days(int64_t(tick / US_PER_DAY), y, m, d);
  uint64_t t = tick % US_PER_DAY;
  uint16_t v[6] = {uint16_t(y), uint16_t(m), uint16_t(d), uint16_t(t / 3600000000ull),
                   uint16_t(t / 60000000ull % 60), uint16_t(t / 1000000 % 60)};
  for (int i = 0; i < 6; i++) c.mem().write<uint16_t>(va + 2 * i, v[i]);
  c.mem().write<uint32_t>(va + 12, uint32_t(t % 1000000));
}

uint64_t read_datetime(Cpu& c, uint32_t va) {
  auto h = [&](int i) { return c.mem().read<uint16_t>(va + 2 * i); };
  uint64_t days = uint64_t(days_from_civil(h(0), h(1), h(2)));
  return days * US_PER_DAY + (h(3) * 3600ull + h(4) * 60ull + h(5)) * 1000000 + c.mem().read<uint32_t>(va + 12);
}

constexpr uint32_t RTC_ERROR_INVALID_POINTER = 0x80251000;  // unverified value

void GetCurrentTick(Cpu& c) {
  uint32_t p = arg(c, 0);
  if (!p) return ret(c, RTC_ERROR_INVALID_POINTER);
  c.mem().write<uint64_t>(p, now_tick());
  ret(c, 0);
}

// int sceRtcGetCurrentClockLocalTime(SceDateTime* time)
void GetCurrentClockLocalTime(Cpu& c) {
  uint32_t p = arg(c, 0);
  if (!p) return ret(c, RTC_ERROR_INVALID_POINTER);
  std::time_t t = std::time(nullptr);
  std::tm lt{}, ut{};
#ifdef _WIN32
  localtime_s(&lt, &t);
  gmtime_s(&ut, &t);
#else
  localtime_r(&t, &lt);
  gmtime_r(&t, &ut);
#endif
  ut.tm_isdst = lt.tm_isdst;
  int64_t offset_us = int64_t(std::difftime(std::mktime(&lt), std::mktime(&ut))) * 1000000;
  write_datetime(c, p, uint64_t(int64_t(now_tick()) + offset_us));
  ret(c, 0);
}

// int sceRtcSetTick(SceDateTime* time, const SceRtcTick* tick)
void SetTick(Cpu& c) {
  if (!arg(c, 0) || !arg(c, 1)) return ret(c, RTC_ERROR_INVALID_POINTER);
  write_datetime(c, arg(c, 0), c.mem().read<uint64_t>(arg(c, 1)));
  ret(c, 0);
}

// int sceRtcGetWin32FileTime(const SceDateTime* time, SceUInt64* win32): 100 ns units since 1601-01-01.
void GetWin32FileTime(Cpu& c) {
  if (!arg(c, 0) || !arg(c, 1)) return ret(c, RTC_ERROR_INVALID_POINTER);
  uint64_t tick = read_datetime(c, arg(c, 0));
  c.mem().write<uint64_t>(arg(c, 1), tick < WIN32_EPOCH_TICK ? 0 : (tick - WIN32_EPOCH_TICK) * 10);
  ret(c, 0);
}

// int sceRtcSetWin32FileTime(SceDateTime* time, SceUInt64 win32): the 64-bit value is in r2:r3.
void SetWin32FileTime(Cpu& c) {
  if (!arg(c, 0)) return ret(c, RTC_ERROR_INVALID_POINTER);
  uint64_t ft = uint64_t(c.reg(2)) | uint64_t(c.reg(3)) << 32;
  write_datetime(c, arg(c, 0), WIN32_EPOCH_TICK + ft / 10);
  ret(c, 0);
}

// int sceRtcGetCurrentClock(SceDateTime* time, int timeZoneMinutes): UTC shifted by the given offset.
void GetCurrentClock(Cpu& c) {
  uint32_t p = arg(c, 0);
  if (!p) return ret(c, RTC_ERROR_INVALID_POINTER);
  write_datetime(c, p, uint64_t(int64_t(now_tick()) + int64_t(int32_t(arg(c, 1))) * 60000000));
  ret(c, 0);
}

// int sceRtcGetTick(const SceDateTime* time, SceRtcTick* tick)
void GetTick(Cpu& c) {
  if (!arg(c, 0) || !arg(c, 1)) return ret(c, RTC_ERROR_INVALID_POINTER);
  c.mem().write<uint64_t>(arg(c, 1), read_datetime(c, arg(c, 0)));
  ret(c, 0);
}

}  // namespace

void register_libm() {
  REG(acosf, Acosf);
  REG(asinf, Asinf);
  REG(atan2f, Atan2f);
  REG(expf, Expf);
  REG(ceilf, Ceilf);
  REG(powf, Powf);
  REG(fabsf, Fabsf);
  REG(sqrtf, Sqrtf);
  REG(fmodf, Fmodf);
  REG(modf, Modf);
  REG(_FLog, FLog);
  REG(_FSinx, FSinx);
}
void register_rtc() {
  REG(sceRtcGetCurrentTick, GetCurrentTick);
  REG(sceRtcGetCurrentClock, GetCurrentClock);
  REG(sceRtcGetTick, GetTick);
  REG(sceRtcGetCurrentClockLocalTime, GetCurrentClockLocalTime);
  REG(sceRtcSetTick, SetTick);
  REG(sceRtcGetWin32FileTime, GetWin32FileTime);
  REG(sceRtcSetWin32FileTime, SetWin32FileTime);
}

}  // namespace hle
