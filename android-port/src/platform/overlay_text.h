// Text and shapes for the host overlay (gpu/overlay.h), shared by the text-entry dialog and the room chat: host fonts
// through FreeType (the ScePvf system-font list + Android's), glyphs cut into runs of solid quads (the overlay draws
// coloured triangles only).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gpu/overlay.h"

namespace platform::ui {

struct Color {
  float r, g, b, a;
};

struct Geo {
  std::vector<gpu::OverlayVertex>& out;
  void rect(float x0, float y0, float x1, float y1, Color c);
  void frame(float x0, float y0, float x1, float y1, float t, Color c);
};

std::u32string decode16(const std::u16string& s);
std::u32string decode8(const std::string& s);
inline std::u32string ascii(const char* s) { return decode8(s); }

float ascent(int px);  // baseline offset for `px`
float measure(const std::u32string& t, int px);
// Draws `t` from pen x on `baseline`; runs outside [cl, cr) are cut. Returns the pen x after the text.
float text(Geo& g, const std::u32string& t, float x, float baseline, int px, Color c, float cl = -1e9f,
           float cr = 1e9f);
// Splits `t` into lines no wider than `width` (at spaces where possible).
std::vector<std::u32string> wrap(const std::u32string& t, int px, float width);

}  // namespace platform::ui
