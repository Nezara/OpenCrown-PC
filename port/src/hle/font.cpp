// libSceFont + libSceFontFt (firmware modules, not in the dump): our own implementation over FreeType.
// Only the semantics the game relies on are modelled; they were read from its callers (asm, eboot vaddrs):
//   FUN_00080bb0  MemoryInit, Select*Ft, CreateLibrary/RendererWithEdition x2, Support*Fonts, AttachDeviceCacheBuffer
//   FUN_00080d70  OpenFontMemory (the game's .OTF) -> font A; OpenFontSet (system font) -> font B (must succeed,
//                 else the glyph surfaces are never set up); RenderSurfaceInit 2048x2048 1 B/px; BindRenderer
//   FUN_00080fd0  SetScalePixel(w,h); GetHorizontalLayout (reads +4 = lineHeight); SetupRenderScalePixel(w*k, h*k)
//                 with k = h / lineHeight, i.e. the rendered line height equals the cell height.
//   FUN_000810d0  clears a cell, RenderCharGlyphImage(font, code, surf, x = cellX+1, y = cellY+1, metrics, out);
//                 font A first, font B if A returns non-zero; reads metrics +0x10 (horizontal advance) and then
//                 copies the cell's coverage bytes into the glyph texture itself (CPU, as alpha of 0x00ffffff).
// So (x, y) is the top-left of the line box: baseline = y + baseLineY (GUESS consistent with the cell layout).
// The system font set is served by the game's own OTF (user decision), or by the file named in $DCP_FONT_FALLBACK.
// Glyphs missing from every font are logged once each ("font: missing U+XXXX") for a coverage report.
// Note: in-game text (menus, dialogue, HUD) does NOT come from here: it is X1 quads over the pre-baked BC7 font atlas
// in the CPK. Through TOWN the game only sets libSceFont up; RenderCharGlyphImage (FUN_000810d0, reached via the text
// object methods FUN_0006e360 / FUN_0006f3a0) was never called. It is presumably for runtime strings (e.g. online names).
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "common.h"

namespace dcp::hle {
namespace {

constexpr int32_t kFontErrInvalid = int32_t(0x80460002);   // guess: any non-zero makes the game take its fallback
constexpr int32_t kFontErrNoGlyph = int32_t(0x80460010);   // guess

std::mutex g_mu;
FT_Library g_ft = nullptr;
const uint8_t* g_game_otf = nullptr;  // last OpenFontMemory blob (the game's own font)
size_t g_game_otf_size = 0;
std::vector<uint8_t> g_fallback_file;  // $DCP_FONT_FALLBACK contents
std::set<uint32_t> g_missing;

struct Font {
    uint32_t magic = 0x464f4e54;  // 'FONT'
    FT_Face face = nullptr;
    float sx = 16, sy = 16;       // SetScalePixel
    float rx = 0, ry = 0;         // SetupRenderScalePixel (0 = use sx/sy)
    const char* what = "";
};

struct Surface {  // our layout inside the caller's SceFontRenderSurface storage (the game reserves 0x80 B)
    uint8_t* buf;
    int32_t pitch, bpp, w, h;
};

bool ft_init() {
    if (!g_ft && FT_Init_FreeType(&g_ft)) return false;
    return true;
}

Font* open_face(const uint8_t* data, size_t size, const char* what) {
    if (!ft_init() || !data || !size) return nullptr;
    FT_Face face;
    if (FT_New_Memory_Face(g_ft, data, FT_Long(size), 0, &face)) return nullptr;
    FT_Select_Charmap(face, FT_ENCODING_UNICODE);
    Font* f = new Font;
    f->face = face, f->what = what;
    std::fprintf(stderr, "HLE font: opened %s (%s %s, %ld glyphs, upem %u)\n", what, face->family_name ? face->family_name : "?",
                 face->style_name ? face->style_name : "", face->num_glyphs, face->units_per_EM);
    return f;
}

Font* as_font(void* p) {
    Font* f = static_cast<Font*>(p);
    return f && f->magic == 0x464f4e54 ? f : nullptr;
}

// Layout in pixels for a font at the given em size (horizontal: baseLineY, lineHeight, effectHeight).
void hlayout(const Font* f, float sy, float out[3]) {
    float k = sy / float(f->face->units_per_EM ? f->face->units_per_EM : 1000);
    out[0] = float(f->face->ascender) * k;
    out[1] = float(f->face->ascender - f->face->descender) * k;  // guess: ascender - descender (no line gap)
    out[2] = 0;
}

// ---- library / renderer objects: opaque tokens, nothing in them is read by the game ----
uint8_t g_lib_sel[16], g_rend_sel[16];
DCP_SYSV int32_t sceFontMemoryInit(void* mem, void*, uint32_t, const void*, void*, const void*) {
    DCP_HLE_LOG("sceFontMemoryInit", "ok");
    if (mem) std::memset(mem, 0, 0x40);
    return kOk;
}
DCP_SYSV int32_t sceFontMemoryTerm(void*) { return kOk; }
DCP_SYSV const void* sceFontSelectLibraryFt(int32_t) { return g_lib_sel; }
DCP_SYSV const void* sceFontSelectRendererFt(int32_t) { return g_rend_sel; }
DCP_SYSV int32_t sceFontCreateLibraryWithEdition(void*, const void*, uint64_t, void** lib) {
    if (!lib) return kFontErrInvalid;
    std::lock_guard lk(g_mu);
    ft_init();
    *lib = new uint64_t(0x4c4942);  // 'LIB'
    DCP_HLE_LOG("sceFontCreateLibraryWithEdition", "ok");
    return kOk;
}
DCP_SYSV int32_t sceFontCreateRendererWithEdition(void*, const void*, uint64_t, void** r) {
    if (!r) return kFontErrInvalid;
    *r = new uint64_t(0x524e44);  // 'RND'
    return kOk;
}
DCP_SYSV int32_t sceFontDestroyLibrary(void**) { return kOk; }
DCP_SYSV int32_t sceFontDestroyRenderer(void**) { return kOk; }
DCP_SYSV int32_t sceFontSupportSystemFonts(void*) { return kOk; }
DCP_SYSV int32_t sceFontSupportExternalFonts(void*, uint32_t, uint32_t) { return kOk; }
DCP_SYSV int32_t sceFontAttachDeviceCacheBuffer(void*, void*, uint32_t) { return kOk; }
DCP_SYSV int32_t sceFontBindRenderer(void* f, void*) { return as_font(f) ? kOk : kFontErrInvalid; }
DCP_SYSV int32_t sceFontUnbindRenderer(void*) { return kOk; }

// ---- fonts ----
DCP_SYSV int32_t sceFontOpenFontMemory(void*, const void* addr, uint32_t size, const void*, void** out) {
    if (!out) return kFontErrInvalid;
    std::lock_guard lk(g_mu);
    Font* f = open_face(static_cast<const uint8_t*>(addr), size, "game font (memory)");
    if (!f) {
        std::fprintf(stderr, "HLE sceFontOpenFontMemory(%p, %u): FreeType cannot open it\n", addr, size);
        return kFontErrInvalid;
    }
    g_game_otf = static_cast<const uint8_t*>(addr), g_game_otf_size = size;
    *out = f;
    return kOk;
}

DCP_SYSV int32_t sceFontOpenFontSet(void*, uint32_t type, uint32_t mode, const void*, void** out) {
    if (!out) return kFontErrInvalid;
    std::lock_guard lk(g_mu);
    Font* f = nullptr;
    if (const char* path = std::getenv("DCP_FONT_FALLBACK")) {
        if (g_fallback_file.empty()) {
            if (FILE* fp = std::fopen(path, "rb")) {
                std::fseek(fp, 0, SEEK_END);
                long n = std::ftell(fp);
                std::fseek(fp, 0, SEEK_SET);
                g_fallback_file.resize(n > 0 ? size_t(n) : 0);
                if (n > 0 && std::fread(g_fallback_file.data(), 1, size_t(n), fp) != size_t(n)) g_fallback_file.clear();
                std::fclose(fp);
            }
        }
        f = open_face(g_fallback_file.data(), g_fallback_file.size(), "system font set (DCP_FONT_FALLBACK)");
        if (!f) std::fprintf(stderr, "HLE sceFontOpenFontSet: DCP_FONT_FALLBACK=%s unusable, using the game font\n", path);
    }
    if (!f) f = open_face(g_game_otf, g_game_otf_size, "system font set (served by the game font)");
    std::fprintf(stderr, "HLE sceFontOpenFontSet(type 0x%x, mode %u) -> %s\n", type, mode, f ? "ok" : "no font");
    if (!f) return kFontErrInvalid;
    *out = f;
    return kOk;
}

DCP_SYSV int32_t sceFontCloseFont(void* p) {
    std::lock_guard lk(g_mu);
    if (Font* f = as_font(p)) {
        FT_Done_Face(f->face);
        f->magic = 0;
        delete f;
    }
    return kOk;
}

DCP_SYSV int32_t sceFontSetScalePixel(void* p, float w, float h) {
    Font* f = as_font(p);
    if (!f || !(w > 0) || !(h > 0)) return kFontErrInvalid;
    f->sx = w, f->sy = h;
    return kOk;
}
DCP_SYSV int32_t sceFontSetupRenderScalePixel(void* p, float w, float h) {
    Font* f = as_font(p);
    if (!f || !(w > 0) || !(h > 0)) return kFontErrInvalid;
    f->rx = w, f->ry = h;
    DCP_HLE_LOG("sceFontSetupRenderScalePixel", "%.1f x %.1f (scale %.1f x %.1f)", w, h, f->sx, f->sy);
    return kOk;
}
DCP_SYSV int32_t sceFontGetHorizontalLayout(void* p, float* out) {
    Font* f = as_font(p);
    if (!f || !out) return kFontErrInvalid;
    hlayout(f, f->sy, out);
    return kOk;
}
DCP_SYSV int32_t sceFontGetVerticalLayout(void* p, float* out) {  // {baseLineX, columnWidth, effectWidth}; unread by the game
    Font* f = as_font(p);
    if (!f || !out) return kFontErrInvalid;
    out[0] = f->sx * 0.5f, out[1] = f->sx, out[2] = 0;
    return kOk;
}

DCP_SYSV int32_t sceFontRenderSurfaceInit(void* surf, void* buf, int32_t pitch, int32_t bpp, int32_t w, int32_t h) {
    if (!surf) return kFontErrInvalid;
    std::memset(surf, 0, 0x80);
    Surface s{static_cast<uint8_t*>(buf), pitch, bpp, w, h};
    std::memcpy(surf, &s, sizeof s);
    DCP_HLE_LOG("sceFontRenderSurfaceInit", "%p pitch %d bpp %d %dx%d", buf, pitch, bpp, w, h);
    return kOk;
}

// metrics: {width, height, h.bearingX, h.bearingY, h.advance, v.bearingX, v.bearingY, v.advance} (the game reads +0x10).
DCP_SYSV int32_t sceFontRenderCharGlyphImage(void* p, uint32_t code, void* surfp, float x, float y, float* metrics,
                                             void* result) {
    Font* f = as_font(p);
    if (!f || !surfp) return kFontErrInvalid;
    Surface s;
    std::memcpy(&s, surfp, sizeof s);
    std::lock_guard lk(g_mu);
    FT_Face face = f->face;
    FT_UInt gi = FT_Get_Char_Index(face, code);
    if (!gi) {
        // Font A misses it: the game retries with font B. Log when B (the set) misses too.
        if (std::strstr(f->what, "system") && g_missing.insert(code).second)
            std::fprintf(stderr, "font: missing U+%04X in every font (%zu missing so far)\n", code, g_missing.size());
        return kFontErrNoGlyph;
    }
    float rx = f->rx > 0 ? f->rx : f->sx, ry = f->ry > 0 ? f->ry : f->sy;
    if (FT_Set_Char_Size(face, FT_F26Dot6(std::lround(rx * 64)), FT_F26Dot6(std::lround(ry * 64)), 72, 72) ||
        FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT) || FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL))
        return kFontErrNoGlyph;
    FT_GlyphSlot g = face->glyph;
    float lay[3];
    hlayout(f, ry, lay);
    int ox = int(std::lround(x)) + g->bitmap_left;
    int oy = int(std::lround(y + lay[0])) - g->bitmap_top;
    const FT_Bitmap& bm = g->bitmap;
    if (s.buf && s.bpp == 1 && bm.pixel_mode == FT_PIXEL_MODE_GRAY) {
        for (unsigned r = 0; r < bm.rows; ++r) {
            int yy = oy + int(r);
            if (yy < 0 || yy >= s.h) continue;
            const uint8_t* src = bm.buffer + int(r) * bm.pitch;
            uint8_t* dst = s.buf + int64_t(yy) * s.pitch;
            for (unsigned c = 0; c < bm.width; ++c) {
                int xx = ox + int(c);
                if (xx >= 0 && xx < s.w && src[c] > dst[xx]) dst[xx] = src[c];
            }
        }
    } else if (s.bpp != 1) {
        DCP_HLE_LOG("sceFontRenderCharGlyphImage", "surface %d B/px not handled", s.bpp);
    }
    if (metrics) {
        metrics[0] = float(g->metrics.width) / 64, metrics[1] = float(g->metrics.height) / 64;
        metrics[2] = float(g->metrics.horiBearingX) / 64, metrics[3] = float(g->metrics.horiBearingY) / 64;
        metrics[4] = float(g->metrics.horiAdvance) / 64;
        metrics[5] = float(g->metrics.vertBearingX) / 64, metrics[6] = float(g->metrics.vertBearingY) / 64;
        metrics[7] = float(g->metrics.vertAdvance) / 64;
    }
    if (result) std::memset(result, 0, 0x30);  // SceFontRenderOutput: not read by the game (0x40 B reserved)
    DCP_HLE_LOG("sceFontRenderCharGlyphImage", "U+%04X at %.0f,%.0f size %.1f adv %.1f", code, x, y, ry,
                float(g->metrics.horiAdvance) / 64);
    return kOk;
}

}  // namespace

void register_font() {
    const char* L = "libSceFont";
    REG(L, sceFontMemoryInit); REG(L, sceFontMemoryTerm);
    REG(L, sceFontCreateLibraryWithEdition); REG(L, sceFontCreateRendererWithEdition);
    REG(L, sceFontDestroyLibrary); REG(L, sceFontDestroyRenderer);
    REG(L, sceFontSupportSystemFonts); REG(L, sceFontSupportExternalFonts); REG(L, sceFontAttachDeviceCacheBuffer);
    REG(L, sceFontBindRenderer); REG(L, sceFontUnbindRenderer);
    REG(L, sceFontOpenFontMemory); REG(L, sceFontOpenFontSet); REG(L, sceFontCloseFont);
    REG(L, sceFontSetScalePixel); REG(L, sceFontSetupRenderScalePixel);
    REG(L, sceFontGetHorizontalLayout); REG(L, sceFontGetVerticalLayout);
    REG(L, sceFontRenderSurfaceInit); REG(L, sceFontRenderCharGlyphImage);
    REG("libSceFontFt", sceFontSelectLibraryFt); REG("libSceFontFt", sceFontSelectRendererFt);
}

}  // namespace dcp::hle
