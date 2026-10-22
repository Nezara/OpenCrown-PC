#include "platform/touch_controls.h"

#include <algorithm>
#include <cmath>

namespace platform {
namespace {

enum Glyph { G_NONE, G_TRIANGLE, G_CIRCLE, G_CROSS, G_SQUARE, G_L, G_R, G_SELECT, G_START, G_UP, G_RIGHT, G_DOWN, G_LEFT };

struct Color {
  float r, g, b, a;
};
constexpr Color kBase{1, 1, 1, 0.22f}, kHeld{1, 1, 1, 0.5f}, kRim{1, 1, 1, 0.45f};

struct Geo {
  std::vector<gpu::OverlayVertex>& out;

  void quad(const float (&px)[4][2], const float (&uv)[4][2], Color c, float inner) {
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int i : idx) out.push_back({px[i][0], px[i][1], uv[i][0], uv[i][1], c.r, c.g, c.b, c.a, inner});
  }
  // Disc (inner = -1) or ring (inner = hole radius / r).
  void circle(float x, float y, float r, Color c, float inner = -1) {
    float px[4][2] = {{x - r, y - r}, {x + r, y - r}, {x + r, y + r}, {x - r, y + r}};
    float uv[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    quad(px, uv, c, inner);
  }
  // Solid bar from (x0, y0) to (x1, y1), `t` thick.
  void bar(float x0, float y0, float x1, float y1, float t, Color c) {
    float dx = x1 - x0, dy = y1 - y0, len = std::max(1e-3f, std::sqrt(dx * dx + dy * dy));
    float nx = -dy / len * t / 2, ny = dx / len * t / 2;
    float px[4][2] = {{x0 + nx, y0 + ny}, {x1 + nx, y1 + ny}, {x1 - nx, y1 - ny}, {x0 - nx, y0 - ny}};
    float uv[4][2] = {};
    quad(px, uv, c, -1);
  }
  void tri(float x0, float y0, float x1, float y1, float x2, float y2, Color c) {
    for (auto [x, y] : {std::pair{x0, y0}, {x1, y1}, {x2, y2}}) out.push_back({x, y, 0, 0, c.r, c.g, c.b, c.a, -1});
  }
};

void glyph(Geo& g, int kind, float x, float y, float r) {
  float s = r * 0.45f, t = std::max(2.0f, r * 0.12f);
  switch (kind) {
    case G_TRIANGLE: {  // green outline
      Color c{0.35f, 0.9f, 0.6f, 0.85f};
      float ax = x, ay = y - s, bx = x + s * 0.95f, by = y + s * 0.6f, cx = x - s * 0.95f, cy = by;
      g.bar(ax, ay, bx, by, t, c), g.bar(bx, by, cx, cy, t, c), g.bar(cx, cy, ax, ay, t, c);
      break;
    }
    case G_CIRCLE: g.circle(x, y, s, {1.0f, 0.4f, 0.4f, 0.85f}, 1 - t / s); break;
    case G_CROSS: {
      Color c{0.45f, 0.65f, 1.0f, 0.85f};
      g.bar(x - s, y - s, x + s, y + s, t, c), g.bar(x - s, y + s, x + s, y - s, t, c);
      break;
    }
    case G_SQUARE: {
      Color c{1.0f, 0.55f, 0.85f, 0.85f};
      float a = s * 0.8f;
      g.bar(x - a, y - a, x + a, y - a, t, c), g.bar(x + a, y - a, x + a, y + a, t, c);
      g.bar(x + a, y + a, x - a, y + a, t, c), g.bar(x - a, y + a, x - a, y - a, t, c);
      break;
    }
    case G_L: {
      Color c{1, 1, 1, 0.8f};
      g.bar(x - s * 0.4f, y - s, x - s * 0.4f, y + s, t, c), g.bar(x - s * 0.4f, y + s, x + s * 0.6f, y + s, t, c);
      break;
    }
    case G_R: {
      Color c{1, 1, 1, 0.8f};
      float l = x - s * 0.5f, rr = x + s * 0.5f;
      g.bar(l, y - s, l, y + s, t, c), g.bar(l, y - s, rr, y - s, t, c), g.bar(rr, y - s, rr, y, t, c);
      g.bar(rr, y, l, y, t, c), g.bar(l + s * 0.3f, y, rr + s * 0.1f, y + s, t, c);
      break;
    }
    case G_SELECT: g.bar(x - s * 0.5f, y, x + s * 0.5f, y, s * 0.7f, {1, 1, 1, 0.8f}); break;
    case G_START: g.tri(x - s * 0.4f, y - s * 0.6f, x + s * 0.6f, y, x - s * 0.4f, y + s * 0.6f, {1, 1, 1, 0.8f}); break;
    default: {  // d-pad arrows
      float dx = kind == G_RIGHT ? 1 : kind == G_LEFT ? -1 : 0, dy = kind == G_DOWN ? 1 : kind == G_UP ? -1 : 0;
      float px = -dy, py = dx;  // perpendicular
      Color c{1, 1, 1, 0.8f};
      g.tri(x + dx * s * 0.6f, y + dy * s * 0.6f, x - dx * s * 0.4f + px * s * 0.6f, y - dy * s * 0.4f + py * s * 0.6f,
            x - dx * s * 0.4f - px * s * 0.6f, y - dy * s * 0.4f - py * s * 0.6f, c);
    }
  }
}

}  // namespace

void TouchControls::resize(float w, float h) {
  w_ = std::max(1.0f, w);
  h_ = std::max(1.0f, h);
  float u = std::min(h_ / 100, w_ / 178);  // layout unit: 1% of the height on a 16:9-or-wider screen
  stick_x_ = 20 * u, stick_y_ = h_ - 26 * u, stick_r_ = 14 * u;
  float fx = w_ - 21 * u, fy = h_ - 27 * u, off = 11 * u, br = 6.5f * u;
  float dx = 48 * u, dy = h_ - 12 * u, doff = 6.5f * u, dr = 4.5f * u;
  buttons_ = {
      {fx, fy - off, br, hle::CTRL_TRIANGLE, G_TRIANGLE}, {fx + off, fy, br, hle::CTRL_CIRCLE, G_CIRCLE},
      {fx, fy + off, br, hle::CTRL_CROSS, G_CROSS},       {fx - off, fy, br, hle::CTRL_SQUARE, G_SQUARE},
      {9 * u, 9 * u, 6.5f * u, hle::CTRL_L, G_L},         {w_ - 9 * u, 9 * u, 6.5f * u, hle::CTRL_R, G_R},
      {w_ / 2 - 9 * u, 6 * u, 4 * u, hle::CTRL_SELECT, G_SELECT},
      {w_ / 2 + 9 * u, 6 * u, 4 * u, hle::CTRL_START, G_START},
      {dx, dy - doff, dr, hle::CTRL_UP, G_UP},            {dx + doff, dy, dr, hle::CTRL_RIGHT, G_RIGHT},
      {dx, dy + doff, dr, hle::CTRL_DOWN, G_DOWN},        {dx - doff, dy, dr, hle::CTRL_LEFT, G_LEFT},
  };
}

uint32_t TouchControls::button_at(float x, float y) const {
  const Button* best = nullptr;
  float best_d = 0;
  for (const Button& b : buttons_) {
    float d = std::hypot(x - b.x, y - b.y) / b.r;
    if (d <= 1.3f && (!best || d < best_d)) best = &b, best_d = d;
  }
  return best ? best->bit : 0;
}

void TouchControls::down(int64_t finger, float x, float y) {
  Finger f;
  f.x = x, f.y = y;
  if (!controls_)
    f.kind = SCREEN;
  else if (std::hypot(x - stick_x_, y - stick_y_) <= stick_r_ * 1.5f &&
      std::none_of(fingers_.begin(), fingers_.end(), [](auto& p) { return p.second.kind == STICK; }))
    f.kind = STICK;
  else if ((f.bit = button_at(x, y)))
    f.kind = BUTTON;
  else
    f.kind = SCREEN;
  fingers_[finger] = f;
}

void TouchControls::move(int64_t finger, float x, float y) {
  auto it = fingers_.find(finger);
  if (it == fingers_.end()) return;
  Finger& f = it->second;
  f.x = x, f.y = y;
  if (f.kind == BUTTON) f.bit = button_at(x, y);  // slide between buttons; off all of them = released
}

void TouchControls::up(int64_t finger) { fingers_.erase(finger); }

void TouchControls::apply(hle::PadState& s) const {
  for (auto& [id, f] : fingers_) {
    if (f.kind == BUTTON) s.buttons |= f.bit;
    if (f.kind != STICK) continue;
    float dx = (f.x - stick_x_) / stick_r_, dy = (f.y - stick_y_) / stick_r_, len = std::hypot(dx, dy);
    if (len > 1) dx /= len, dy /= len;
    s.lx = uint8_t(std::clamp(128 + int(std::lround(dx * 127)), 0, 255));
    s.ly = uint8_t(std::clamp(128 + int(std::lround(dy * 127)), 0, 255));
  }
}

std::vector<hle::TouchPoint> TouchControls::front_touches() const {
  std::vector<hle::TouchPoint> out;
  float scale = std::min(w_ / 960, h_ / 544), x0 = (w_ - 960 * scale) / 2, y0 = (h_ - 544 * scale) / 2;
  for (auto& [id, f] : fingers_) {
    if (f.kind != SCREEN) continue;
    float gx = (f.x - x0) / scale, gy = (f.y - y0) / scale;
    if (gx < 0 || gx >= 960 || gy < 0 || gy >= 544) continue;
    hle::TouchPoint p;
    p.id = uint8_t(out.size());
    p.x = int16_t(gx * 2);
    p.y = int16_t(gy * 2);
    out.push_back(p);
  }
  return out;
}

std::vector<gpu::OverlayVertex> TouchControls::geometry() const {
  std::vector<gpu::OverlayVertex> v;
  Geo g{v};
  uint32_t held = 0;
  float kx = stick_x_, ky = stick_y_;
  for (auto& [id, f] : fingers_) {
    if (f.kind == BUTTON) held |= f.bit;
    if (f.kind == STICK) {
      float dx = f.x - stick_x_, dy = f.y - stick_y_, len = std::hypot(dx, dy);
      float k = len > stick_r_ ? stick_r_ / len : 1;
      kx = stick_x_ + dx * k, ky = stick_y_ + dy * k;
    }
  }
  g.circle(stick_x_, stick_y_, stick_r_, kBase);
  g.circle(stick_x_, stick_y_, stick_r_, kRim, 0.94f);
  g.circle(kx, ky, stick_r_ * 0.45f, kx != stick_x_ || ky != stick_y_ ? kHeld : kRim);
  for (const Button& b : buttons_) {
    g.circle(b.x, b.y, b.r, held & b.bit ? kHeld : kBase);
    g.circle(b.x, b.y, b.r, kRim, 0.9f);
    glyph(g, b.glyph, b.x, b.y, b.r);
  }
  for (gpu::OverlayVertex& p : v) p.a *= opacity_;
  return v;
}

}  // namespace platform
