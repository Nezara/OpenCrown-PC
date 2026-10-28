// See overlay_text.h.
#include "platform/overlay_text.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstdio>
#include <map>
#include <utility>

#include "hle/pvf.h"

namespace platform::ui {
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
}  // namespace

void Geo::rect(float x0, float y0, float x1, float y1, Color c) {
  const float p[6][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y0}, {x1, y1}, {x0, y1}};
  for (auto& v : p) out.push_back({v[0], v[1], 0, 0, c.r, c.g, c.b, c.a, -1});
}
void Geo::frame(float x0, float y0, float x1, float y1, float t, Color c) {
  rect(x0, y0, x1, y0 + t, c), rect(x0, y1 - t, x1, y1, c);
  rect(x0, y0 + t, x0 + t, y1 - t, c), rect(x1 - t, y0 + t, x1, y1 - t, c);
}

float ascent(int px) { return fonts().ascent(px); }

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

float measure(const std::u32string& t, int px) {
  float w = 0;
  for (uint32_t c : t) w += fonts().glyph(c, px).advance;
  return w;
}

// Draws `t` from pen x on `baseline`; runs outside [cl, cr) are cut. Returns the pen x after the text.
float text(Geo& g, const std::u32string& t, float x, float baseline, int px, Color c, float cl, float cr) {
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

std::vector<std::u32string> wrap(const std::u32string& t, int px, float width) {
  std::vector<std::u32string> lines;
  std::u32string cur;
  float w = 0;
  size_t last_space = std::u32string::npos;
  for (char32_t c : t) {
    float a = fonts().glyph(c, px).advance;
    if (w + a > width && !cur.empty()) {
      if (last_space != std::u32string::npos && c != U' ') {  // break at the last space, carry the word over
        std::u32string rest = cur.substr(last_space + 1);
        cur.resize(last_space);
        lines.push_back(cur);
        cur = rest;
        w = measure(cur, px);
      } else {
        lines.push_back(cur);
        cur.clear(), w = 0;
      }
      last_space = std::u32string::npos;
      if (c == U' ') continue;
    }
    if (c == U' ') last_space = cur.size();
    cur.push_back(c);
    w += a;
  }
  if (!cur.empty() || lines.empty()) lines.push_back(cur);
  return lines;
}

}  // namespace platform::ui
