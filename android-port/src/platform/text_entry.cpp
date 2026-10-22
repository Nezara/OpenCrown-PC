#include "platform/text_entry.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <utility>

#include "hle/pvf.h"

namespace platform {
namespace {

// --- text: host fonts through FreeType (the ScePvf system-font list), glyphs as runs of solid overlay quads ---------
// The overlay draws coloured triangles only, so a glyph's coverage is cut into horizontal runs of equal coverage
// (4 levels): crisp at window resolution, a few hundred quads per glyph, built once per size.
struct Run {
  int16_t x0, x1, y, h;  // pixels from the pen position / baseline; h rows tall
  uint8_t level;         // 1..4
};
struct Glyph {
  float advance = 0;
  bool missing = false;
  std::vector<Run> runs;
};

struct Fonts {
  FT_Library lib = nullptr;
  std::vector<FT_Face> faces;  // first that has the glyph wins
  std::map<std::pair<uint32_t, int>, Glyph> cache;

  Fonts() {
    if (FT_Init_FreeType(&lib)) return;
    std::vector<std::string> paths = hle::pvf_system_fonts();
#ifdef __ANDROID__
    for (const char* p : {"/system/fonts/Roboto-Regular.ttf", "/system/fonts/RobotoStatic-Regular.ttf",
                          "/system/fonts/DroidSans.ttf", "/system/fonts/NotoSansCJK-Regular.ttc"})
      paths.push_back(p);
#endif
    for (size_t i = 0; i < paths.size(); i++) {
      const std::string& p = paths[i];
      if (std::find(paths.begin(), paths.begin() + i, p) != paths.begin() + i) continue;  // listed twice
      FT_Face f = nullptr;
      if (FT_New_Face(lib, p.c_str(), 0, &f) == 0) faces.push_back(f);
    }
    if (faces.empty()) std::fprintf(stderr, "[ime] no host font found: text shows as boxes (set one with --font)\n");
  }
  ~Fonts() {
    for (FT_Face f : faces) FT_Done_Face(f);
    if (lib) FT_Done_FreeType(lib);
  }

  // Baseline offset and line height for `px`.
  float ascent(int px) {
    if (faces.empty()) return px * 0.8f;
    FT_Set_Pixel_Sizes(faces[0], 0, FT_UInt(px));
    return float(faces[0]->size->metrics.ascender) / 64.0f;
  }

  const Glyph& glyph(uint32_t cp, int px) {
    auto key = std::make_pair(cp, px);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    Glyph& g = cache[key];
    for (FT_Face f : faces) {
      FT_UInt gi = FT_Get_Char_Index(f, cp);
      if (!gi) continue;
      FT_Set_Pixel_Sizes(f, 0, FT_UInt(px));
      if (FT_Load_Glyph(f, gi, FT_LOAD_RENDER)) continue;
      const FT_Bitmap& bm = f->glyph->bitmap;
      g.advance = float(f->glyph->advance.x) / 64.0f;
      if (bm.pixel_mode != FT_PIXEL_MODE_GRAY) return g;
      // Horizontal runs of equal coverage; a run continuing the same one of the row above makes it taller (stems).
      std::vector<Run> open, next;  // runs ending at the previous / this row
      for (unsigned y = 0; y < bm.rows; y++) {
        const unsigned char* row = bm.buffer + int(y) * bm.pitch;
        next.clear();
        for (unsigned x = 0; x < bm.width;) {
          uint8_t lv = uint8_t((row[x] * 4 + 127) / 255);
          unsigned e = x + 1;
          while (e < bm.width && uint8_t((row[e] * 4 + 127) / 255) == lv) e++;
          if (lv) {
            Run r{int16_t(f->glyph->bitmap_left + int(x)), int16_t(f->glyph->bitmap_left + int(e)),
                  int16_t(int(y) - f->glyph->bitmap_top), 1, lv};
            for (Run& q : open)
              if (q.level == lv && q.x0 == r.x0 && q.x1 == r.x1) {
                r.y = q.y, r.h = int16_t(q.h + 1), q.level = 0;  // taken over
                break;
              }
            next.push_back(r);
          }
          x = e;
        }
        for (const Run& q : open)
          if (q.level) g.runs.push_back(q);
        open.swap(next);
      }
      g.runs.insert(g.runs.end(), open.begin(), open.end());
      return g;
    }
    g.missing = true;  // no face has it: a hollow box
    g.advance = px * 0.6f;
    return g;
  }
};
Fonts& fonts() {
  static Fonts f;
  return f;
}

struct Color {
  float r, g, b, a;
};
constexpr Color kDim{0, 0, 0, 0.55f}, kPanel{0.09f, 0.09f, 0.12f, 0.96f}, kRim{0.8f, 0.75f, 0.55f, 0.9f},
    kBoxBg{0.17f, 0.17f, 0.21f, 1}, kText{1, 1, 1, 1}, kTitle{0.95f, 0.88f, 0.65f, 1}, kGrey{0.65f, 0.65f, 0.7f, 1},
    kEdit{0.6f, 0.85f, 1, 1}, kButton{0.24f, 0.24f, 0.3f, 1}, kOk{0.55f, 0.42f, 0.16f, 1};

struct Geo {
  std::vector<gpu::OverlayVertex>& out;
  void rect(float x0, float y0, float x1, float y1, Color c) {
    const float p[6][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y0}, {x1, y1}, {x0, y1}};
    for (auto& v : p) out.push_back({v[0], v[1], 0, 0, c.r, c.g, c.b, c.a, -1});
  }
  void frame(float x0, float y0, float x1, float y1, float t, Color c) {
    rect(x0, y0, x1, y0 + t, c), rect(x0, y1 - t, x1, y1, c);
    rect(x0, y0 + t, x0 + t, y1 - t, c), rect(x1 - t, y0 + t, x1, y1 - t, c);
  }
};

std::u32string decode16(const std::u16string& s) {
  std::u32string o;
  for (size_t i = 0; i < s.size(); i++) {
    uint32_t c = s[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
      c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
    o.push_back(c);
  }
  return o;
}
std::u32string decode8(const std::string& s) {
  std::u32string o;
  for (size_t i = 0; i < s.size();) {
    unsigned char b = s[i];
    int n = b < 0x80 ? 0 : b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : -1;
    if (n < 0 || i + n >= s.size()) {  // stray continuation byte or cut sequence
      i++;
      continue;
    }
    uint32_t c = n == 0 ? b : b & (0x3F >> n);
    for (int k = 1; k <= n; k++) c = c << 6 | (s[i + k] & 0x3F);
    o.push_back(c);
    i += n + 1;
  }
  return o;
}
std::u32string ascii(const char* s) { return decode8(s); }

float measure(const std::u32string& t, int px) {
  float w = 0;
  for (uint32_t c : t) w += fonts().glyph(c, px).advance;
  return w;
}

// Draws `t` from pen x on `baseline`; runs outside [cl, cr) are cut. Returns the pen x after the text.
float text(Geo& g, const std::u32string& t, float x, float baseline, int px, Color c, float cl = -1e9f, float cr = 1e9f) {
  for (uint32_t cp : t) {
    const Glyph& gl = fonts().glyph(cp, px);
    if (gl.missing) {
      float x0 = x + px * 0.08f, x1 = x + gl.advance - px * 0.08f, y0 = baseline - px * 0.7f;
      if (x0 >= cl && x1 <= cr) g.frame(x0, y0, x1, baseline, std::max(1.0f, px / 14.0f), c);
    }
    for (const Run& r : gl.runs) {
      float x0 = std::max(cl, x + r.x0), x1 = std::min(cr, x + r.x1);
      if (x1 <= x0) continue;
      g.rect(x0, baseline + r.y, x1, baseline + r.y + r.h, {c.r, c.g, c.b, c.a * r.level / 4.0f});
    }
    x += gl.advance;
  }
  return x;
}

size_t units16(uint32_t cp) { return cp >= 0x10000 ? 2 : 1; }

}  // namespace

TextEntry::~TextEntry() = default;

// DEV (local test runs without a keyboard at hand): DCP_IME_TEST=<answer>|<answer>|... answers the dialogs in turn (the
// last one repeats) through the normal event path: Backspace over the initial text, the answer's text as one SDL text
// event, then Enter. "{cancel}" = Escape instead; a "{click}" / "{start}" suffix confirms with a mouse click on OK /
// a gamepad Start event. DCP_IME_TEST_DUMP=<file.ppm> saves the panel as drawn just before (CPU raster of the overlay
// rectangles; the GPU draws the same vertices).
void TextEntry::dev_test(SDL_Window* win) {
  static const char* env = SDL_getenv("DCP_IME_TEST");  // UTF-8 on Windows too
  if (!env) return;
  static std::vector<std::string> answers = [] {
    std::vector<std::string> v;
    std::string all = env;
    for (size_t p = 0;;) {
      size_t bar = all.find('|', p);
      v.push_back(all.substr(p, bar - p));
      if (bar == std::string::npos) return v;
      p = bar + 1;
    }
  }();
  static size_t dialogs = 0;
  static std::string typed, confirm;
  uint64_t t = SDL_GetTicks() - opened_ms_;
  auto push = [](SDL_Event e) { SDL_PushEvent(&e); };
  auto key = [&](SDL_Scancode sc) {
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = sc;
    e.key.down = true;
    push(e);
  };
  if (test_step_ == 0 && t > 800) {
    test_step_ = 1;
    typed = answers[std::min(dialogs++, answers.size() - 1)];
    confirm.clear();
    for (const char* suffix : {"{click}", "{start}"})
      if (typed.size() >= std::strlen(suffix) && typed.compare(typed.size() - std::strlen(suffix), std::string::npos, suffix) == 0)
        confirm = suffix, typed.resize(typed.size() - std::strlen(suffix));
    if (typed != "{cancel}") {
      for (size_t i = 0; i < text_.size(); i++) key(SDL_SCANCODE_BACKSPACE);
      SDL_Event e{};
      e.type = SDL_EVENT_TEXT_INPUT;
      e.text.text = typed.c_str();
      push(e);
    }
  } else if (test_step_ == 1 && t > 1600) {
    test_step_ = 2;
    if (const char* dump = std::getenv("DCP_IME_TEST_DUMP")) {
      int w = 1, h = 1;
      SDL_GetWindowSizeInPixels(win, &w, &h);
      std::vector<float> img(size_t(w) * h * 3, 0.25f);
      std::vector<gpu::OverlayVertex> v = geometry(float(w), float(h));
      for (size_t i = 0; i + 6 <= v.size(); i += 6) {  // every shape here is a rectangle: vertices 0 and 2 are corners
        int x0 = std::max(0, int(std::lround(v[i].x))), x1 = std::min(w, int(std::lround(v[i + 2].x)));
        int y0 = std::max(0, int(std::lround(v[i].y))), y1 = std::min(h, int(std::lround(v[i + 2].y)));
        const auto& c = v[i];
        for (int y = y0; y < y1; y++)
          for (int x = x0; x < x1; x++) {
            float* p = &img[(size_t(y) * w + x) * 3];
            p[0] += (c.r - p[0]) * c.a, p[1] += (c.g - p[1]) * c.a, p[2] += (c.b - p[2]) * c.a;
          }
      }
      if (std::FILE* f = std::fopen(dump, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (float x : img) std::fputc(int(std::clamp(x, 0.0f, 1.0f) * 255 + 0.5f), f);
        std::fclose(f);
        std::fprintf(stderr, "[ime] test: panel saved to %s\n", dump);
      }
    }
  } else if (test_step_ == 2 && t > 2000) {
    test_step_ = 3;
    if (typed == "{cancel}") {
      key(SDL_SCANCODE_ESCAPE);
    } else if (confirm == "{click}") {  // OK's centre in window points, as the mouse reports it
      int w = 1, h = 1;
      SDL_GetWindowSizeInPixels(win, &w, &h);
      Layout l = layout(float(w), float(h));
      float d = std::max(0.01f, SDL_GetWindowPixelDensity(win));
      SDL_Event e{};
      e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
      e.button.button = SDL_BUTTON_LEFT;
      e.button.down = true;
      e.button.x = (l.ok.x0 + l.ok.x1) / 2 / d;
      e.button.y = (l.ok.y0 + l.ok.y1) / 2 / d;
      push(e);
    } else if (confirm == "{start}") {
      SDL_Event e{};
      e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
      e.gbutton.button = SDL_GAMEPAD_BUTTON_START;
      e.gbutton.down = true;
      push(e);
    } else {
      key(SDL_SCANCODE_RETURN);
    }
  }
}

TextEntry::Layout TextEntry::layout(float w, float h) const {
  Layout l;
  l.s = std::min(w / 960.0f, h / 544.0f);
  l.ox = (w - 960 * l.s) / 2;
  l.oy = (h - 544 * l.s) / 2;
  // Upper part of the picture: Android's keyboard covers the lower half in landscape.
  l.panel = {l.px(120), l.py(30), l.px(840), l.py(236)};
  l.text = {l.px(144), l.py(86), l.px(816), l.py(140)};
  l.ok = {l.px(668), l.py(170), l.px(816), l.py(218)};
  l.cancel = {l.px(504), l.py(170), l.px(652), l.py(218)};
  return l;
}

void TextEntry::start_input(SDL_Window* win) {
  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER, SDL_TEXTINPUT_TYPE_TEXT);
  SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_CAPITALIZATION_NUMBER,
                        (req_.option & 2) ? SDL_CAPITALIZE_NONE : SDL_CAPITALIZE_SENTENCES);  // NO_AUTO_CAPITALIZATION
  SDL_SetBooleanProperty(props, SDL_PROP_TEXTINPUT_AUTOCORRECT_BOOLEAN, false);
  SDL_SetBooleanProperty(props, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN, false);
  // Where the text is (window points), so an OS IME composes next to it.
  int w = 1, h = 1;
  SDL_GetWindowSizeInPixels(win, &w, &h);
  float d = std::max(0.01f, SDL_GetWindowPixelDensity(win));
  Layout l = layout(float(w), float(h));
  SDL_Rect r{int(l.text.x0 / d), int(l.text.y0 / d), int((l.text.x1 - l.text.x0) / d), int((l.text.y1 - l.text.y0) / d)};
  SDL_SetTextInputArea(win, &r, 0);
  if (SDL_TextInputActive(win)) SDL_StopTextInput(win);  // again: brings a dismissed screen keyboard back
  if (!SDL_StartTextInputWithProperties(win, props)) std::fprintf(stderr, "[ime] text input: %s\n", SDL_GetError());
  SDL_DestroyProperties(props);
}

bool TextEntry::sync(SDL_Window* win) {
  hle::ImeRequest r;
  bool pending = hle::ime_poll(r);
  if (open_ && (!pending || r.id != req_.id)) {  // aborted by the game (or replaced)
    open_ = false;
    SDL_StopTextInput(win);
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "0");
#endif
    std::fprintf(stderr, "[ime] dialog closed by the game\n");
    if (!pending) return true;
  }
  if (!open_ && pending) {
    open_ = true;
    req_ = r;
    text_ = r.text.substr(0, r.max_length);
    editing_.clear();
#ifdef __ANDROID__
    // The system keyboard even when a gamepad reports itself as a keyboard; Back cancels instead of leaving the game.
    SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "1");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
#endif
    pad_hint_ = SDL_HasGamepad();
    opened_ms_ = SDL_GetTicks();
    test_step_ = 0;
    start_input(win);
    std::fprintf(stderr, "[ime] dialog open (max %u)\n", r.max_length);
    return true;
  }
  if (open_) dev_test(win);
  if (open_) {  // caret blink
    uint64_t phase = SDL_GetTicks() / 530;
    if (phase != blink_phase_) return blink_phase_ = phase, true;
  }
  return false;
}

void TextEntry::finish(SDL_Window* win, bool ok) {
  if (!ok && !req_.cancelable) return;
  open_ = false;
  SDL_StopTextInput(win);
#ifdef __ANDROID__
  SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "0");
#endif
  hle::ime_answer(req_.id, ok, text_);
}

void TextEntry::insert_utf8(const char* s) {
  for (uint32_t c : decode8(s)) {
    if (c < 0x20 || c == 0x7F || (c >= 0xD800 && c < 0xE000) || c > 0x10FFFF) continue;  // controls, stray surrogates
    if (text_.size() + units16(c) > req_.max_length) break;
    if (c >= 0x10000) {
      text_.push_back(char16_t(0xD800 + ((c - 0x10000) >> 10)));
      text_.push_back(char16_t(0xDC00 + ((c - 0x10000) & 0x3FF)));
    } else {
      text_.push_back(char16_t(c));
    }
  }
}

void TextEntry::backspace() {
  if (text_.empty()) return;
  bool pair = text_.size() >= 2 && text_.back() >= 0xDC00 && text_.back() < 0xE000 &&
              text_[text_.size() - 2] >= 0xD800 && text_[text_.size() - 2] < 0xDC00;
  text_.resize(text_.size() - (pair ? 2 : 1));
}

bool TextEntry::click(SDL_Window* win, float x, float y) {
  int w = 1, h = 1;
  SDL_GetWindowSizeInPixels(win, &w, &h);
  Layout l = layout(float(w), float(h));
  if (l.ok.hit(x, y)) finish(win, true);
  else if (req_.cancelable && l.cancel.hit(x, y)) finish(win, false);
  else if (l.text.hit(x, y)) start_input(win);
  return true;
}

bool TextEntry::event(SDL_Window* win, const SDL_Event& e) {
  if (!open_) return false;
  switch (e.type) {
    case SDL_EVENT_TEXT_INPUT:
      insert_utf8(e.text.text);
      editing_.clear();
      return true;
    case SDL_EVENT_TEXT_EDITING:
      editing_ = e.edit.text ? e.edit.text : "";
      return true;
    case SDL_EVENT_KEY_DOWN:
      switch (e.key.scancode) {
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
        case SDL_SCANCODE_RETURN2:
          if (editing_.empty() && !e.key.repeat) finish(win, true);
          break;
        case SDL_SCANCODE_ESCAPE:
        case SDL_SCANCODE_AC_BACK:
          if (editing_.empty() && !e.key.repeat) finish(win, false);
          break;
        case SDL_SCANCODE_BACKSPACE:
          if (editing_.empty()) backspace();
          break;
        case SDL_SCANCODE_V:
          if (e.key.mod & SDL_KMOD_CTRL) {
            if (char* clip = SDL_GetClipboardText()) {
              std::string one = clip;
              one = one.substr(0, one.find_first_of("\r\n"));
              insert_utf8(one.c_str());
              SDL_free(clip);
            }
          }
          break;
        default: break;
      }
      return true;
    case SDL_EVENT_KEY_UP: return false;  // releases still reach the game's key state
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
      pad_hint_ = true;
      if (e.gbutton.button == SDL_GAMEPAD_BUTTON_START) finish(win, true);
      else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) finish(win, false);
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

std::vector<gpu::OverlayVertex> TextEntry::geometry(float w, float h) const {
  std::vector<gpu::OverlayVertex> out;
  if (!open_) return out;
  Geo g{out};
  Layout l = layout(w, h);
  float s = l.s, t = std::max(1.0f, std::round(2 * s));
  auto px = [&](float size) { return std::max(8, int(std::lround(size * s))); };

  g.rect(0, 0, w, h, kDim);
  g.rect(l.panel.x0, l.panel.y0, l.panel.x1, l.panel.y1, kPanel);
  g.frame(l.panel.x0, l.panel.y0, l.panel.x1, l.panel.y1, t, kRim);

  // Title (one line, cut at the panel's inner edge).
  int tp = px(22);
  std::u32string title = req_.title.empty() ? ascii("Enter text") : decode16(req_.title);
  text(g, title, l.text.x0, l.py(60) + fonts().ascent(tp) * 0.38f, tp, kTitle, l.text.x0, l.text.x1);

  // Text box: the text (its tail when longer than the box), the composition, the caret.
  g.rect(l.text.x0, l.text.y0, l.text.x1, l.text.y1, kBoxBg);
  g.frame(l.text.x0, l.text.y0, l.text.x1, l.text.y1, t, kGrey);
  int fp = px(28);
  std::u32string body = decode16(text_), comp = decode8(editing_);
  float pad = 10 * s, inner0 = l.text.x0 + pad, inner1 = l.text.x1 - pad;
  float width = measure(body, fp) + measure(comp, fp), caret_w = std::max(2.0f, 2 * s);
  float x = inner0 - std::max(0.0f, width + caret_w - (inner1 - inner0));
  float base = (l.text.y0 + l.text.y1) / 2 + fonts().ascent(fp) * 0.38f;
  x = text(g, body, x, base, fp, kText, inner0, inner1);
  if (!comp.empty()) {
    float x1 = text(g, comp, x, base, fp, kEdit, inner0, inner1);
    g.rect(std::max(inner0, x), base + 3 * s, std::min(inner1, x1), base + 3 * s + t, kEdit);
    x = x1;
  }
  if (blink_phase_ % 2 == 0 && x >= inner0 && x <= inner1)
    g.rect(x + s, base - fonts().ascent(fp) * 0.85f, x + s + caret_w, base + 4 * s, kText);

  // Length / max under the box, the key hint, the buttons.
  int sp = px(16);
  char count[32];
  std::snprintf(count, sizeof count, "%zu / %u", text_.size(), req_.max_length);
  std::u32string cnt = ascii(count);
  text(g, cnt, l.text.x1 - measure(cnt, sp), l.py(158), sp, kGrey);
  std::u32string hint = ascii(pad_hint_ ? (req_.cancelable ? "Start: OK    B: Cancel" : "Start: OK")
                                        : (req_.cancelable ? "Enter: OK    Esc: Cancel" : "Enter: OK"));
  text(g, hint, l.text.x0, (l.ok.y0 + l.ok.y1) / 2 + fonts().ascent(sp) * 0.38f, sp, kGrey);
  auto button = [&](const Box& b, const char* label, Color c) {
    g.rect(b.x0, b.y0, b.x1, b.y1, c);
    g.frame(b.x0, b.y0, b.x1, b.y1, t, kRim);
    int bp = px(20);
    std::u32string lb = ascii(label);
    text(g, lb, (b.x0 + b.x1 - measure(lb, bp)) / 2, (b.y0 + b.y1) / 2 + fonts().ascent(bp) * 0.38f, bp, kText);
  };
  if (req_.cancelable) button(l.cancel, "Cancel", kButton);
  button(l.ok, "OK", kOk);
  return out;
}

}  // namespace platform
