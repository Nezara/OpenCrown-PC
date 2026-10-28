#include "platform/chat_overlay.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "net/identity.h"
#include "platform/overlay_text.h"

namespace platform {
namespace {

namespace chat = dcp::net::chat;
using ui::Color;

constexpr uint64_t kShowMs = 8000, kFadeMs = 1500;  // a closed line stays this long, then fades out
constexpr size_t kShownClosed = 4;                   // wrapped lines shown while closed
constexpr size_t kHistory = 60;
// Screen gesture: a tap shorter than kTapMs, the second touch within kGapMs of it, held kHoldMs, all within kSlop
// picture pixels of the first.
constexpr uint64_t kTapMs = 300, kGapMs = 350, kHoldMs = 400;
constexpr float kSlop = 40;

constexpr Color kPanel{0.09f, 0.09f, 0.12f, 0.92f}, kRim{0.8f, 0.75f, 0.55f, 0.9f}, kBoxBg{0.17f, 0.17f, 0.21f, 1},
    kText{1, 1, 1, 1}, kGrey{0.65f, 0.65f, 0.7f, 1}, kEdit{0.6f, 0.85f, 1, 1}, kButton{0.24f, 0.24f, 0.3f, 1},
    kOk{0.55f, 0.42f, 0.16f, 1}, kNotice{0.75f, 0.75f, 0.75f, 1};

// The game's player name colours by slot (1P host .. 4P, sampled from the game's own name text), so a name in chat
// matches that player on screen. Slot = (member id >> 4) - 1 (notes/map/player_colours.md).
constexpr uint32_t kSlotRgb[4] = {0xe06060, 0x6060e0, 0xe0c060, 0x80e060};

Color name_color(const chat::Line& l, float a) {
  uint32_t rgb = l.slot >= 0 ? kSlotRgb[l.slot] : 0xd0d0d0;
  return {float((rgb >> 16) & 0xff) / 255, float((rgb >> 8) & 0xff) / 255, float(rgb & 0xff) / 255, a};
}

struct Box {
  float x0, y0, x1, y1;
  bool hit(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};
// The letterboxed 960x544 picture; the open panel sits in its upper half.
struct Layout {
  float s, ox, oy;
  Box panel, history, text, close, send;
  float px(float x) const { return ox + x * s; }
  float py(float y) const { return oy + y * s; }
};
Layout layout(float w, float h) {
  Layout l;
  l.s = std::min(w / 960.0f, h / 544.0f);
  l.ox = (w - 960 * l.s) / 2;
  l.oy = (h - 544 * l.s) / 2;
  l.panel = {l.px(120), l.py(16), l.px(840), l.py(254)};
  l.history = {l.px(144), l.py(28), l.px(816), l.py(150)};
  l.text = {l.px(144), l.py(158), l.px(816), l.py(202)};
  l.close = {l.px(504), l.py(210), l.px(652), l.py(244)};
  l.send = {l.px(668), l.py(210), l.px(816), l.py(244)};
  return l;
}

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

void pop_code_point(std::string& s) {
  while (!s.empty() && (uint8_t(s.back()) & 0xc0) == 0x80) s.pop_back();
  if (!s.empty()) s.pop_back();
}

}  // namespace

void ChatOverlay::init() { chat::load_mutes(dcp::net::identity_dir() + "/chat_mutes.txt"); }

void ChatOverlay::local(const std::string& text) {
  Entry e;
  e.line.text = text;
  e.line.notice = true;
  e.t = SDL_GetTicks();
  lines_.push_back(std::move(e));
  while (lines_.size() > kHistory) lines_.pop_front();
  version_++;
}

void ChatOverlay::start_input(SDL_Window* win) {
  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER, SDL_TEXTINPUT_TYPE_TEXT);
  SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_CAPITALIZATION_NUMBER, SDL_CAPITALIZE_SENTENCES);
  SDL_SetBooleanProperty(props, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN, false);
  int w = 1, h = 1;
  SDL_GetWindowSizeInPixels(win, &w, &h);
  float d = std::max(0.01f, SDL_GetWindowPixelDensity(win));
  Layout l = layout(float(w), float(h));
  SDL_Rect r{int(l.text.x0 / d), int(l.text.y0 / d), int((l.text.x1 - l.text.x0) / d), int((l.text.y1 - l.text.y0) / d)};
  SDL_SetTextInputArea(win, &r, 0);
  if (SDL_TextInputActive(win)) SDL_StopTextInput(win);  // again: brings a dismissed screen keyboard back
  if (!SDL_StartTextInputWithProperties(win, props)) std::fprintf(stderr, "[chat] text input: %s\n", SDL_GetError());
  SDL_DestroyProperties(props);
}

void ChatOverlay::open_box(SDL_Window* win) {
  open_ = true;
  text_.clear();
  editing_.clear();
#ifdef __ANDROID__
  SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "1");  // even when a gamepad reports itself as a keyboard
  SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");  // Back closes the box instead of leaving the game
#endif
  pad_hint_ = SDL_HasGamepad();
  start_input(win);
  version_++;
}

void ChatOverlay::close_box(SDL_Window* win) {
  open_ = false;
  SDL_StopTextInput(win);
#ifdef __ANDROID__
  SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "0");
#endif
  version_++;
}

void ChatOverlay::submit() {
  std::string text = chat::clamp_chars(text_, chat::kMaxChars);
  if (text.empty()) return;
  if (text[0] != '/') return chat::send(text);  // shown when the lobby's echo comes back
  size_t sp = text.find(' ');
  std::string cmd = lower(text.substr(0, sp)),
              arg = sp == std::string::npos ? "" : chat::clamp_chars(text.substr(sp + 1), 16);
  if (cmd == "/mute") {
    for (auto it = lines_.rbegin(); it != lines_.rend(); ++it) {
      const chat::Line& l = it->line;
      if (l.notice || l.self || lower(l.name) != lower(arg)) continue;
      uint64_t acc = l.account;
      std::string name = l.name;
      chat::set_muted(acc, name, true);
      lines_.erase(std::remove_if(lines_.begin(), lines_.end(),
                                  [&](const Entry& e) { return e.line.account == acc && !e.line.notice; }),
                   lines_.end());
      return local("Muted " + name + ". /unmute " + name + " to hear them again.");
    }
    return local(arg.empty() ? "Usage: /mute <name>" : "Nobody called " + arg + " has chatted recently.");
  }
  if (cmd == "/unmute") {
    for (const auto& [acc, name] : chat::mutes())
      if (lower(name) == lower(arg)) {
        chat::set_muted(acc, name, false);
        return local("Unmuted " + name + ".");
      }
    return local(arg.empty() ? "Usage: /unmute <name>" : arg + " is not muted.");
  }
  if (cmd == "/mutes") {
    auto m = chat::mutes();
    if (m.empty()) return local("Nobody is muted.");
    std::string s = "Muted:";
    for (const auto& [acc, name] : m) s += " " + name;
    return local(s);
  }
  if (cmd == "/help") {
    local("Chat goes to the players in your room.");
    return local("/mute <name>  /unmute <name>  /mutes");
  }
  local("Unknown command " + cmd + ". /help lists them.");
}

bool ChatOverlay::sync(SDL_Window* win) {
  const uint64_t v0 = version_;
  if (!chat::enabled()) return false;
  const bool in = chat::in_room();
  if (in && !was_in_room_ && !hinted_) {
    hinted_ = true;
    local("Double tap the screen and hold to talk.");
  }
  was_in_room_ = in;
  for (chat::Line l; chat::take(&l);) {
    lines_.push_back({std::move(l), SDL_GetTicks()});
    while (lines_.size() > kHistory) lines_.pop_front();
    version_++;
  }
  if (open_ && !in) close_box(win);  // left the room
  const uint64_t now = SDL_GetTicks();
  if (gesture_ == Gesture::Wait2 && now - gesture_ms_ >= kGapMs) gesture_ = Gesture::Idle;
  if (gesture_ == Gesture::Down2 && now - gesture_ms_ >= kHoldMs) {
    gesture_ = Gesture::Idle;
    if (!open_ && in) open_box(win);
  }
  if (!open_) {  // fades: redraw in 1/4 s steps while a line fades or just ran out
    bool fading = false;
    for (const Entry& e : lines_)
      if (now - e.t >= kShowMs && now - e.t < kShowMs + kFadeMs + 250) fading = true;
    if (fading && now / 250 != fade_step_) fade_step_ = now / 250, version_++;
  } else if (now / 530 != blink_phase_) {
    blink_phase_ = now / 530, version_++;
  }
  return version_ != v0 || gesture_ != Gesture::Idle;
}

void ChatOverlay::finger(SDL_Window* win, int64_t id, uint32_t type, float x, float y, bool on_picture,
                         bool entry_open) {
  int w = 1, h = 1;
  SDL_GetWindowSizeInPixels(win, &w, &h);
  const float slop = kSlop * std::min(w / 960.0f, h / 544.0f);
  const uint64_t now = SDL_GetTicks();
  const bool near = std::hypot(x - gesture_x_, y - gesture_y_) <= slop;
  switch (gesture_) {
    case Gesture::Idle:
    case Gesture::Wait2:
      if (type != SDL_EVENT_FINGER_DOWN || !on_picture) return;
      if (open_ || entry_open || !chat::enabled() || !chat::in_room()) return;
      if (gesture_ == Gesture::Wait2 && now - gesture_ms_ < kGapMs && near) {
        gesture_ = Gesture::Down2;  // sync() opens the box once it is held long enough
      } else {
        gesture_ = Gesture::Down1;
        gesture_x_ = x, gesture_y_ = y;
      }
      gesture_finger_ = id, gesture_ms_ = now;
      return;
    case Gesture::Down1:
    case Gesture::Down2:
      if (id != gesture_finger_) {
        if (type == SDL_EVENT_FINGER_DOWN) gesture_ = Gesture::Idle;  // a second finger: not the gesture
        return;
      }
      if (type == SDL_EVENT_FINGER_MOTION) {
        if (!near) gesture_ = Gesture::Idle;  // a drag (the fairy pointer)
        return;
      }
      if (type == SDL_EVENT_FINGER_UP && gesture_ == Gesture::Down1 && now - gesture_ms_ < kTapMs && near)
        gesture_ = Gesture::Wait2, gesture_ms_ = now;  // a tap: the hold may follow
      else if (type != SDL_EVENT_FINGER_DOWN)
        gesture_ = Gesture::Idle;
      return;
  }
}

bool ChatOverlay::click(SDL_Window* win, float x, float y) {
  int w = 1, h = 1;
  SDL_GetWindowSizeInPixels(win, &w, &h);
  Layout l = layout(float(w), float(h));
  if (l.send.hit(x, y)) {
    submit();
    close_box(win);
  } else if (l.close.hit(x, y)) {
    close_box(win);
  } else if (l.text.hit(x, y)) {
    start_input(win);
  }
  return true;
}

bool ChatOverlay::event(SDL_Window* win, const SDL_Event& e, bool entry_open) {
  if (!open_) {
    if (e.type != SDL_EVENT_KEY_DOWN || e.key.repeat || entry_open || !chat::enabled()) return false;
    if (e.key.scancode != SDL_SCANCODE_RETURN && e.key.scancode != SDL_SCANCODE_KP_ENTER) return false;
    if (chat::in_room()) {
      open_box(win);
      return true;
    }
    if (hinted_) local("You are not in an online room.");  // offline players never see chat lines
    return false;
  }
  switch (e.type) {
    case SDL_EVENT_TEXT_INPUT:
      for (uint32_t c : ui::decode8(e.text.text ? e.text.text : "")) {
        if (c < 0x20 || c == 0x7f) continue;
        if (chat::code_points(text_) >= chat::kMaxChars) break;
        char buf[5] = {};
        if (c < 0x80) buf[0] = char(c);
        else if (c < 0x800) buf[0] = char(0xc0 | c >> 6), buf[1] = char(0x80 | (c & 0x3f));
        else if (c < 0x10000)
          buf[0] = char(0xe0 | c >> 12), buf[1] = char(0x80 | ((c >> 6) & 0x3f)), buf[2] = char(0x80 | (c & 0x3f));
        else
          buf[0] = char(0xf0 | c >> 18), buf[1] = char(0x80 | ((c >> 12) & 0x3f)), buf[2] = char(0x80 | ((c >> 6) & 0x3f)),
          buf[3] = char(0x80 | (c & 0x3f));
        text_ += buf;
      }
      editing_.clear();
      version_++;
      return true;
    case SDL_EVENT_TEXT_EDITING:
      editing_ = e.edit.text ? e.edit.text : "";
      version_++;
      return true;
    case SDL_EVENT_KEY_DOWN:
      switch (e.key.scancode) {
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
        case SDL_SCANCODE_RETURN2:
          if (editing_.empty() && !e.key.repeat) submit(), close_box(win);
          break;
        case SDL_SCANCODE_ESCAPE:
        case SDL_SCANCODE_AC_BACK:
          if (editing_.empty() && !e.key.repeat) close_box(win);
          break;
        case SDL_SCANCODE_BACKSPACE:
          if (editing_.empty()) pop_code_point(text_), version_++;
          break;
        case SDL_SCANCODE_V:
          if (e.key.mod & SDL_KMOD_CTRL) {
            if (char* clip = SDL_GetClipboardText()) {
              std::string one = clip;
              text_ = chat::clamp_chars(text_ + " " + one.substr(0, one.find_first_of("\r\n")), chat::kMaxChars);
              SDL_free(clip);
              version_++;
            }
          }
          break;
        default: break;
      }
      return true;
    case SDL_EVENT_KEY_UP: return false;  // releases still reach the game's key state
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
      pad_hint_ = true;
      if (e.gbutton.button == SDL_GAMEPAD_BUTTON_START) submit(), close_box(win);
      else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) close_box(win);
      return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (e.button.which == SDL_TOUCH_MOUSEID) return true;
      if (e.button.button == SDL_BUTTON_LEFT) {
        float d = SDL_GetWindowPixelDensity(win);
        click(win, e.button.x * d, e.button.y * d);
      }
      return true;
    case SDL_EVENT_FINGER_DOWN: {
      int w = 1, h = 1;
      SDL_GetWindowSizeInPixels(win, &w, &h);
      click(win, e.tfinger.x * w, e.tfinger.y * h);
      return true;
    }
    case SDL_EVENT_FINGER_MOTION: return true;
    default: return false;
  }
}

const std::vector<gpu::OverlayVertex>& ChatOverlay::geometry(float w, float h) {
  if (built_version_ == version_ && built_w_ == w && built_h_ == h) return geo_;
  built_version_ = version_, built_w_ = w, built_h_ = h;
  geo_.clear();
  if (!chat::enabled()) return geo_;
  ui::Geo g{geo_};
  Layout l = layout(w, h);
  const float s = l.s, t = std::max(1.0f, std::round(2 * s));
  auto px = [&](float size) { return std::max(8, int(std::lround(size * s))); };
  const uint64_t now = SDL_GetTicks();

  if (!open_) {  // newest lines bottom-left, each on a dark strip, fading out
    const int fp = px(15);
    const float lh = fp * 1.35f, x = l.px(16), width = 400 * s;
    float y = l.py(544 * 0.80f);
    size_t shown = 0;
    for (auto it = lines_.rbegin(); it != lines_.rend() && shown < kShownClosed; ++it) {
      const uint64_t age = now - it->t;
      if (age >= kShowMs + kFadeMs) break;
      const float a = age < kShowMs ? 1.0f : 1.0f - float(age - kShowMs) / kFadeMs;
      const chat::Line& ln = it->line;
      const std::u32string prefix = ln.notice ? U"" : ui::decode8(ln.name + ": ");
      std::vector<std::u32string> rows = ui::wrap(prefix + ui::decode8(ln.text), fp, width);
      for (size_t r = rows.size(); r-- > 0 && shown < kShownClosed; shown++) {
        y -= lh;
        const float lw = ui::measure(rows[r], fp);
        g.rect(x - 5 * s, y, x + lw + 5 * s, y + lh, {0, 0, 0, 0.45f * a});
        const float base = y + lh / 2 + ui::ascent(fp) * 0.38f;
        Color body = ln.notice ? kNotice : kText;
        body.a = a;
        if (r == 0 && !prefix.empty() && rows[0].compare(0, prefix.size(), prefix) == 0) {
          float x1 = ui::text(g, prefix, x, base, fp, name_color(ln, a));
          ui::text(g, rows[0].substr(prefix.size()), x1, base, fp, body);
        } else {
          ui::text(g, rows[r], x, base, fp, body);
        }
      }
    }
    return geo_;
  }

  g.rect(l.panel.x0, l.panel.y0, l.panel.x1, l.panel.y1, kPanel);
  g.frame(l.panel.x0, l.panel.y0, l.panel.x1, l.panel.y1, t, kRim);
  // History: the newest wrapped lines that fit, bottom-aligned.
  const int hp = px(16);
  const float hlh = hp * 1.3f, hw = l.history.x1 - l.history.x0;
  float y = l.history.y1;
  for (auto it = lines_.rbegin(); it != lines_.rend() && y - hlh >= l.history.y0; ++it) {
    const chat::Line& ln = it->line;
    const std::u32string prefix = ln.notice ? U"" : ui::decode8(ln.name + ": ");
    std::vector<std::u32string> rows = ui::wrap(prefix + ui::decode8(ln.text), hp, hw);
    for (size_t r = rows.size(); r-- > 0 && y - hlh >= l.history.y0;) {
      y -= hlh;
      const float base = y + hlh / 2 + ui::ascent(hp) * 0.38f;
      if (r == 0 && !prefix.empty() && rows[0].compare(0, prefix.size(), prefix) == 0) {
        float x1 = ui::text(g, prefix, l.history.x0, base, hp, name_color(ln, 1));
        ui::text(g, rows[0].substr(prefix.size()), x1, base, hp, kText);
      } else {
        ui::text(g, rows[r], l.history.x0, base, hp, ln.notice ? kNotice : kText);
      }
    }
  }
  // Text field: its tail when longer than the box, the composition, the caret.
  g.rect(l.text.x0, l.text.y0, l.text.x1, l.text.y1, kBoxBg);
  g.frame(l.text.x0, l.text.y0, l.text.x1, l.text.y1, t, kGrey);
  const int fp = px(22);
  std::u32string body = ui::decode8(text_), comp = ui::decode8(editing_);
  float pad = 10 * s, inner0 = l.text.x0 + pad, inner1 = l.text.x1 - pad;
  float tw = ui::measure(body, fp) + ui::measure(comp, fp), caret_w = std::max(2.0f, 2 * s);
  float x = inner0 - std::max(0.0f, tw + caret_w - (inner1 - inner0));
  float base = (l.text.y0 + l.text.y1) / 2 + ui::ascent(fp) * 0.38f;
  x = ui::text(g, body, x, base, fp, kText, inner0, inner1);
  if (!comp.empty()) {
    float x1 = ui::text(g, comp, x, base, fp, kEdit, inner0, inner1);
    g.rect(std::max(inner0, x), base + 3 * s, std::min(inner1, x1), base + 3 * s + t, kEdit);
    x = x1;
  }
  if (blink_phase_ % 2 == 0 && x >= inner0 && x <= inner1)
    g.rect(x + s, base - ui::ascent(fp) * 0.85f, x + s + caret_w, base + 4 * s, kText);
  // Hint + count, the buttons.
  const int sp = px(14);
  char hint[64];
  std::snprintf(hint, sizeof hint, "%s   %zu / %zu", pad_hint_ ? "Start: send   B: close" : "Enter: send   Esc: close",
                chat::code_points(text_), chat::kMaxChars);
  ui::text(g, ui::ascii(hint), l.text.x0, (l.send.y0 + l.send.y1) / 2 + ui::ascent(sp) * 0.38f, sp, kGrey);
  auto button = [&](const Box& b, const char* label, Color c) {
    g.rect(b.x0, b.y0, b.x1, b.y1, c);
    g.frame(b.x0, b.y0, b.x1, b.y1, t, kRim);
    int bp = px(18);
    std::u32string lb = ui::ascii(label);
    ui::text(g, lb, (b.x0 + b.x1 - ui::measure(lb, bp)) / 2, (b.y0 + b.y1) / 2 + ui::ascent(bp) * 0.38f, bp, kText);
  };
  button(l.close, "Close", kButton);
  button(l.send, "Send", kOk);
  return geo_;
}

}  // namespace platform
