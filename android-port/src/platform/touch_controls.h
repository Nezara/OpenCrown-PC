// On-screen touch controls (Android without a gamepad; `--touch-controls` on desktop, mouse = one finger).
// Platform-neutral: the caller feeds finger events in window pixels, reads back the pad bits / stick and the
// front-panel touches (every finger that does not land on a control is a Vita front-touch point, i.e. the fairy
// pointer), and hands the overlay geometry to the renderer.
#pragma once
#include <cstdint>
#include <map>
#include <vector>

#include "gpu/overlay.h"
#include "hle/ctrl.h"
#include "hle/touch.h"

namespace platform {

class TouchControls {
 public:
  void resize(float w, float h);  // window size in pixels; lays the controls out again
  void down(int64_t finger, float x, float y);
  void move(int64_t finger, float x, float y);
  void up(int64_t finger);
  void clear() { fingers_.clear(); }
  // false: the controls are hidden; new fingers are all front-panel touches.
  void set_controls(bool on) { controls_ = on; }
  void set_opacity(float o) { opacity_ = o < 0 ? 0 : o > 1 ? 1 : o; }  // scales every alpha (input.ini touch_opacity)

  // Adds the held buttons to s.buttons; sets lx / ly while the stick is held.
  void apply(hle::PadState& s) const;
  // Fingers on the game picture, in front-panel units (letterboxed 960x544 picture -> 1920x1088).
  std::vector<hle::TouchPoint> front_touches() const;
  std::vector<gpu::OverlayVertex> geometry() const;

 private:
  enum Kind { NONE, STICK, BUTTON, SCREEN };
  struct Button {
    float x, y, r;
    uint32_t bit;
    int glyph;
  };
  struct Finger {
    Kind kind = NONE;
    float x = 0, y = 0;
    uint32_t bit = 0;  // BUTTON: the button under the finger (fingers slide between buttons)
  };
  uint32_t button_at(float x, float y) const;

  float w_ = 1, h_ = 1, opacity_ = 1;
  bool controls_ = true;
  float stick_x_ = 0, stick_y_ = 0, stick_r_ = 1;
  std::vector<Button> buttons_;
  std::map<int64_t, Finger> fingers_;
};

}  // namespace platform
