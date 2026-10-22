// ScePvf (the Vita's font library) on FreeType. The Vita's own system fonts are firmware files that a game dump
// doesn't contain, so "system" fonts are the host's: a Latin face plus a Japanese fallback (pvf.h). User font
// files (scePvfOpenUserFile*) are opened through app0: like any game file. Glyphs render as 8-bit or 4-bit
// coverage into the game's buffer; metrics are 26.6 fixed point like the Vita's.
//
// Hard-float ABI: float arguments in s0/s1, float results in s0. Layouts: vitasdk psp2/pvf.h.
#include <ft2build.h>
#include FT_FREETYPE_H

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hle/common.h"
#include "hle/io.h"
#include "hle/pvf.h"

namespace hle {
namespace {

enum : uint32_t {
  PVF_ERROR_LIBID = 0x80460002,
  PVF_ERROR_ARG = 0x80460003,
  PVF_ERROR_NOFILE = 0x80460004,
  PVF_ERROR_FILEOPEN = 0x80460005,
  PVF_ERROR_NOGLYPH = 0x8046000F,
};
constexpr uint32_t LIB_ID = 0x50564601;  // our single library instance

std::mutex pvf_lock;
FT_Library ft = nullptr;
std::vector<std::string> system_fonts = default_system_fonts();
float em = 72.0f / 64.0f, hres = 72.0f, vres = 72.0f;

struct Font {
  std::vector<FT_Face> faces;  // first that has the glyph wins
  float h_points = 12.0f, v_points = 12.0f;
  std::string name;
};
std::map<uint32_t, std::unique_ptr<Font>> fonts;
uint32_t next_font = 1;

void set_err(Cpu& c, uint32_t va, uint32_t e) {
  if (va) c.mem().write<uint32_t>(va, e);
}

bool ft_ready() { return ft || FT_Init_FreeType(&ft) == 0; }

FT_Face open_face(const std::string& path, long index) {
  FT_Face f = nullptr;
  return FT_New_Face(ft, path.c_str(), index, &f) == 0 ? f : nullptr;
}

uint32_t add_font(std::unique_ptr<Font> f) {
  uint32_t id = next_font++;
  fonts[id] = std::move(f);
  return id;
}

Font* find(uint32_t id) {
  auto it = fonts.find(id);
  return it == fonts.end() ? nullptr : it->second.get();
}

// Selects the glyph for `code` at the font's size; returns its face or nullptr.
FT_Face load(Font& f, uint32_t code, bool render) {
  for (FT_Face face : f.faces) {
    FT_UInt gi = FT_Get_Char_Index(face, code);
    if (!gi) continue;
    FT_Set_Char_Size(face, FT_F26Dot6(f.h_points * 64), FT_F26Dot6(f.v_points * 64), FT_UInt(hres), FT_UInt(vres));
    if (FT_Load_Glyph(face, gi, render ? FT_LOAD_RENDER : FT_LOAD_DEFAULT) == 0) return face;
  }
  return nullptr;
}

// --- library ---------------------------------------------------------------------------------------------------
// ScePvfLibId scePvfNewLib(ScePvfInitRec*, ScePvfError*)
void NewLib(Cpu& c) {
  std::lock_guard lk(pvf_lock);
  if (!ft_ready()) {
    set_err(c, arg(c, 1), PVF_ERROR_ARG);
    return ret(c, 0);
  }
  set_err(c, arg(c, 1), 0);
  ret(c, LIB_ID);
}
void DoneLib(Cpu& c) { ret(c, arg(c, 0) == LIB_ID ? 0u : uint32_t(PVF_ERROR_LIBID)); }
void SetEM(Cpu& c) {
  std::lock_guard lk(pvf_lock);
  em = c.sreg(0);
  ret(c, 0);
}
void SetResolution(Cpu& c) {
  std::lock_guard lk(pvf_lock);
  if (c.sreg(0) > 0) hres = c.sreg(0);
  if (c.sreg(1) > 0) vres = c.sreg(1);
  ret(c, 0);
}
// ScePvfFloat32 scePvfPixelToPoint{H,V}(lib, pixel, ScePvfError*)
void PixelToPointH(Cpu& c) {
  set_err(c, arg(c, 1), 0);
  c.set_sreg(0, c.sreg(0) * 72.0f / hres);
}
void PixelToPointV(Cpu& c) {
  set_err(c, arg(c, 1), 0);
  c.set_sreg(0, c.sreg(0) * 72.0f / vres);
}

// --- fonts -----------------------------------------------------------------------------------------------------
// ScePvfFontId scePvfOpen(lib, fontIndex, mode, ScePvfError*): every index is the host system font stack.
void Open(Cpu& c) {
  std::lock_guard lk(pvf_lock);
  uint32_t err = arg(c, 3);
  if (arg(c, 0) != LIB_ID) return set_err(c, err, PVF_ERROR_LIBID), ret(c, 0);
  auto f = std::make_unique<Font>();
  for (const std::string& p : system_fonts)
    if (FT_Face face = open_face(p, 0)) f->faces.push_back(face);
  if (f->faces.empty()) {
    HLE_NOTE("scePvfOpen: no usable system font (set one with --font)\n");
    return set_err(c, err, PVF_ERROR_NOFILE), ret(c, 0);
  }
  f->name = f->faces[0]->family_name ? f->faces[0]->family_name : "system";
  HLE_NOTE("scePvfOpen(index %u): host font %s (+%zu fallback)\n", arg(c, 1), f->name.c_str(), f->faces.size() - 1);
  set_err(c, err, 0);
  ret(c, add_font(std::move(f)));
}

// ScePvfFontId scePvfOpenUserFileWithSubfontIndex(lib, const char* path, mode, subFontIndex, ScePvfError*)
void OpenUserFileWithSubfontIndex(Cpu& c) {
  std::lock_guard lk(pvf_lock);
  uint32_t err = arg(c, 4);
  if (arg(c, 0) != LIB_ID) return set_err(c, err, PVF_ERROR_LIBID), ret(c, 0);
  std::string guest = str(c, arg(c, 1), 1024);
  std::string host = io_host_path(guest, false);
  FT_Face face = host.empty() ? nullptr : open_face(host, long(arg(c, 3)));
  if (!face) {
    HLE_NOTE("scePvfOpenUserFile(\"%s\"): not found\n", guest.c_str());
    return set_err(c, err, host.empty() ? PVF_ERROR_NOFILE : PVF_ERROR_FILEOPEN), ret(c, 0);
  }
  auto f = std::make_unique<Font>();
  f->faces.push_back(face);
  f->name = guest;
  set_err(c, err, 0);
  ret(c, add_font(std::move(f)));
}

void Close(Cpu& c) {
  std::lock_guard lk(pvf_lock);
  auto it = fonts.find(arg(c, 0));
  if (it == fonts.end()) return ret(c, PVF_ERROR_ARG);
  for (FT_Face f : it->second->faces) FT_Done_Face(f);
  fonts.erase(it);
  ret(c, 0);
}

// scePvfSetCharSize(font, hSize, vSize): in points.
void SetCharSize(Cpu& c) {
  std::lock_guard lk(pvf_lock);
  Font* f = find(arg(c, 0));
  if (!f) return ret(c, PVF_ERROR_ARG);
  if (c.sreg(0) > 0) f->h_points = c.sreg(0);
  if (c.sreg(1) > 0) f->v_points = c.sreg(1);
  ret(c, 0);
}

// ScePvfIGlyphMetricsInfo (10 x s32, 26.6): width, height, ascender, descender, hBearingX, hBearingY, vBearingX,
// vBearingY, hAdvance, vAdvance.
void put_metrics(Cpu& c, uint32_t va, FT_Face face) {
  const FT_Glyph_Metrics& g = face->glyph->metrics;
  int32_t asc = int32_t(face->size->metrics.ascender), desc = int32_t(face->size->metrics.descender);
  int32_t v[10] = {int32_t(g.width),        int32_t(g.height),       asc,
                   desc,                    int32_t(g.horiBearingX), int32_t(g.horiBearingY),
                   int32_t(g.vertBearingX), int32_t(g.vertBearingY), int32_t(g.horiAdvance),
                   int32_t(g.vertAdvance)};
  for (int i = 0; i < 10; i++) c.mem().write<int32_t>(va + 4 * i, v[i]);
}

// scePvfGetCharInfo(font, charCode, ScePvfCharInfo*): bitmap width / height / pitch / left / top, then metrics.
void GetCharInfo(Cpu& c) {
  uint32_t out = arg(c, 2);
  std::lock_guard lk(pvf_lock);
  Font* f = find(arg(c, 0));
  if (!f || !out) return ret(c, PVF_ERROR_ARG);
  FT_Face face = load(*f, arg(c, 1) & 0xffff, true);
  if (!face) return ret(c, PVF_ERROR_NOGLYPH);
  const FT_Bitmap& b = face->glyph->bitmap;
  std::memset(c.mem().ptr(out), 0, 0x40);
  c.mem().write<uint32_t>(out, b.width);
  c.mem().write<uint32_t>(out + 4, b.rows);
  c.mem().write<uint32_t>(out + 8, b.width);
  c.mem().write<int32_t>(out + 12, face->glyph->bitmap_left);
  c.mem().write<int32_t>(out + 16, face->glyph->bitmap_top);
  put_metrics(c, out + 20, face);
  ret(c, 0);
}

// scePvfGetCharImageRect(font, charCode, ScePvfIrect* {u16 width, u16 height})
void GetCharImageRect(Cpu& c) {
  uint32_t out = arg(c, 2);
  std::lock_guard lk(pvf_lock);
  Font* f = find(arg(c, 0));
  if (!f || !out) return ret(c, PVF_ERROR_ARG);
  FT_Face face = load(*f, arg(c, 1) & 0xffff, true);
  if (!face) return ret(c, PVF_ERROR_NOGLYPH);
  c.mem().write<uint16_t>(out, uint16_t(face->glyph->bitmap.width));
  c.mem().write<uint16_t>(out + 2, uint16_t(face->glyph->bitmap.rows));
  ret(c, 0);
}

// scePvfGetCharGlyphImage(font, charCode, ScePvfUserImageBufferRec* {pixelFormat, xPos64, yPos64,
// rect{u16 w, u16 h}, u16 bytesPerLine, reserved, buffer*}): the pen is at (xPos64, yPos64) in 26.6, on the
// baseline; format 2 = 8-bit coverage, 0 = 4-bit (low nibble first).
void GetCharGlyphImage(Cpu& c) {
  uint32_t ib = arg(c, 2);
  std::lock_guard lk(pvf_lock);
  Font* f = find(arg(c, 0));
  if (!f || !ib) return ret(c, PVF_ERROR_ARG);
  FT_Face face = load(*f, arg(c, 1) & 0xffff, true);
  if (!face) return ret(c, PVF_ERROR_NOGLYPH);
  Memory& m = c.mem();
  uint32_t format = m.read<uint32_t>(ib);
  int32_t x0 = (m.read<int32_t>(ib + 4) >> 6) + face->glyph->bitmap_left;
  int32_t y0 = (m.read<int32_t>(ib + 8) >> 6) - face->glyph->bitmap_top;
  int32_t w = m.read<uint16_t>(ib + 12), h = m.read<uint16_t>(ib + 14);
  uint32_t bpl = m.read<uint16_t>(ib + 16), buf = m.read<uint32_t>(ib + 20);
  if (!buf || !m.valid(buf, bpl * uint32_t(h))) return ret(c, PVF_ERROR_ARG);
  const FT_Bitmap& b = face->glyph->bitmap;
  for (uint32_t row = 0; row < b.rows; row++) {
    int32_t y = y0 + int32_t(row);
    if (y < 0 || y >= h) continue;
    for (uint32_t col = 0; col < b.width; col++) {
      int32_t x = x0 + int32_t(col);
      if (x < 0 || x >= w) continue;
      uint8_t v = b.buffer[row * uint32_t(b.pitch) + col];
      if (format == 2) {
        m.write<uint8_t>(buf + uint32_t(y) * bpl + uint32_t(x), v);
      } else {
        uint32_t a = buf + uint32_t(y) * bpl + uint32_t(x) / 2;
        uint8_t old = m.read<uint8_t>(a), n = uint8_t(v >> 4);
        m.write<uint8_t>(a, (x & 1) ? uint8_t((old & 0x0f) | n << 4) : uint8_t((old & 0xf0) | n));
      }
    }
  }
  ret(c, 0);
}

// scePvfGetFontInfo(font, ScePvfFontInfo*): max metrics (int 26.6 @0, float @0x28), numChars @0x50, style info
// @0x54 {weight, family, style, subStyle, language, region, country, fontName[64] @+0x10, styleName, fileName}.
void GetFontInfo(Cpu& c) {
  uint32_t out = arg(c, 1);
  std::lock_guard lk(pvf_lock);
  Font* f = find(arg(c, 0));
  if (!f || !out) return ret(c, PVF_ERROR_ARG);
  FT_Face face = f->faces[0];
  FT_Set_Char_Size(face, FT_F26Dot6(f->h_points * 64), FT_F26Dot6(f->v_points * 64), FT_UInt(hres), FT_UInt(vres));
  Memory& m = c.mem();
  std::memset(m.ptr(out), 0, 0x130);
  const FT_Size_Metrics& s = face->size->metrics;
  int32_t asc = int32_t(s.ascender), desc = int32_t(s.descender), adv = int32_t(s.max_advance);
  int32_t height = asc - desc;
  int32_t iv[10] = {adv, height, asc, desc, 0, asc, 0, 0, adv, height};
  for (int i = 0; i < 10; i++) {
    m.write<int32_t>(out + 4 * i, iv[i]);
    m.write<float>(out + 0x28 + 4 * i, iv[i] / 64.0f);
  }
  m.write<uint32_t>(out + 0x50, uint32_t(face->num_glyphs));
  m.write<float>(out + 0x54, 1.0f);
  m.write<uint16_t>(out + 0x5E, 1);  // language: Japanese / Latin mix (unverified code)
  std::string name = f->name.substr(0, 63);
  std::memcpy(m.ptr(out + 0x54 + 0x10), name.data(), name.size());
  ret(c, 0);
}

}  // namespace

std::vector<std::string> default_system_fonts() {
#ifdef __ANDROID__
  return {"/system/fonts/Roboto-Regular.ttf", "/system/fonts/NotoSansCJK-Regular.ttc"};
#elif defined(_WIN32)
  return {"C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/YuGothM.ttc", "C:/Windows/Fonts/msgothic.ttc"};
#else
  return {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"};
#endif
}

void pvf_set_system_fonts(const std::vector<std::string>& paths) {
  std::lock_guard lk(pvf_lock);
  system_fonts = paths;
}

std::vector<std::string> pvf_system_fonts() {
  std::lock_guard lk(pvf_lock);
  return system_fonts;
}

void register_pvf() {
  REG(scePvfNewLib, NewLib);
  REG(scePvfDoneLib, DoneLib);
  REG(scePvfSetEM, SetEM);
  REG(scePvfSetResolution, SetResolution);
  REG(scePvfPixelToPointH, PixelToPointH);
  REG(scePvfPixelToPointV, PixelToPointV);
  REG(scePvfOpen, Open);
  REG(scePvfOpenUserFileWithSubfontIndex, OpenUserFileWithSubfontIndex);
  REG(scePvfClose, Close);
  REG(scePvfSetCharSize, SetCharSize);
  REG(scePvfGetCharInfo, GetCharInfo);
  REG(scePvfGetCharImageRect, GetCharImageRect);
  REG(scePvfGetCharGlyphImage, GetCharGlyphImage);
  REG(scePvfGetFontInfo, GetFontInfo);
}

}  // namespace hle
