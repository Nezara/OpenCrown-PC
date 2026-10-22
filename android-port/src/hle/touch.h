// Host -> SceTouch: the platform layer reports the current touches of each panel here.
#pragma once
#include <cstdint>
#include <vector>

namespace hle {

struct TouchPoint {
  uint8_t id = 0, force = 128;
  int16_t x = 0, y = 0;  // panel units: 0..1919 x 0..1087
};

// Replaces the current touches of `port` (0 front, 1 rear).
void touch_set(int port, const std::vector<TouchPoint>& now);

}  // namespace hle
