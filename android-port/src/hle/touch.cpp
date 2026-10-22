// SceTouch: front (port 0) and rear (port 1) panels. Points come from the host input layer through touch_set
// (finger / mouse); with none, reads report no touches.
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

#include "hle/common.h"
#include "hle/touch.h"

namespace hle {
namespace {

constexpr uint32_t TOUCH_ERROR_INVALID_ARG = 0x80700001;  // unverified value
constexpr uint32_t MAX_REPORT = 8;

std::mutex touch_lock;
std::array<std::vector<TouchPoint>, 2> points;
const auto t0 = std::chrono::steady_clock::now();

// SceTouchPanelInfo {s16 minAaX, minAaY, maxAaX, maxAaY, minDispX, minDispY, maxDispX, maxDispY; u8 minForce,
// maxForce}: the panels report in 1920x1088 units (front) / 1920x890 from y 108 (rear).
void GetPanelInfo(Cpu& c) {
  uint32_t port = arg(c, 0), out = arg(c, 1);
  if (port > 1 || !out) return ret(c, TOUCH_ERROR_INVALID_ARG);
  int16_t front[8] = {0, 0, 1919, 1087, 0, 0, 1919, 1087};
  int16_t rear[8] = {0, 108, 1919, 889, 0, 0, 1919, 1087};
  std::memset(c.mem().ptr(out), 0, 0x30);
  std::memcpy(c.mem().ptr(out), port ? rear : front, sizeof front);
  c.mem().write<uint8_t>(out + 16, 1);
  c.mem().write<uint8_t>(out + 17, 128);
  ret(c, 0);
}

void SetSamplingState(Cpu& c) { ret(c, arg(c, 0) > 1 ? TOUCH_ERROR_INVALID_ARG : 0u); }
void EnableTouchForce(Cpu& c) { ret(c, arg(c, 0) > 1 ? TOUCH_ERROR_INVALID_ARG : 0u); }

// int sceTouchRead(port, SceTouchData* data, nBufs): SceTouchData = {u64 timestamp, u32 status, u32 reportNum,
// 8 x {u8 id, u8 force, s16 x, s16 y, u8 reserved[8], u16 info}} (0x90 bytes). Returns the buffers filled.
void Read(Cpu& c) {
  uint32_t port = arg(c, 0), data = arg(c, 1), n = arg(c, 2);
  if (port > 1 || !data || n == 0 || n > 64 || !c.mem().valid(data, 0x90 * n)) return ret(c, TOUCH_ERROR_INVALID_ARG);
  uint64_t us = uint64_t(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
  std::lock_guard lk(touch_lock);
  for (uint32_t b = 0; b < n; b++) {
    uint32_t d = data + 0x90 * b;
    std::memset(c.mem().ptr(d), 0, 0x90);
    c.mem().write<uint64_t>(d, us);
    uint32_t count = std::min<uint32_t>(uint32_t(points[port].size()), MAX_REPORT);
    c.mem().write<uint32_t>(d + 12, count);
    for (uint32_t i = 0; i < count; i++) {
      const TouchPoint& p = points[port][i];
      uint32_t r = d + 16 + 16 * i;
      c.mem().write<uint8_t>(r, p.id);
      c.mem().write<uint8_t>(r + 1, p.force);
      c.mem().write<int16_t>(r + 2, p.x);
      c.mem().write<int16_t>(r + 4, p.y);
    }
  }
  ret(c, n);
}

}  // namespace

void touch_set(int port, const std::vector<TouchPoint>& now) {
  if (port < 0 || port > 1) return;
  std::lock_guard lk(touch_lock);
  points[size_t(port)] = now;
}

void register_touch() {
  REG(sceTouchGetPanelInfo, GetPanelInfo);
  REG(sceTouchSetSamplingState, SetSamplingState);
  REG(sceTouchEnableTouchForce, EnableTouchForce);
  REG(sceTouchRead, Read);
}

}  // namespace hle
