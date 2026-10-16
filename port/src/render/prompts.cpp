// Button-prompt glyph swap, see prompts.h and notes/map/button_prompts.md.
#include "prompts.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "gl.h"

using namespace dcp::gl;

namespace dcp::render {
namespace {

// FNV-1a 64 of the atlas's detiled BC7 blocks (1024x512, 512 KiB). Only the hash is kept in the repo (rule 1).
// The US_Master dump's atlas: see button_prompts.md. $DCP_PROMPT_ATLAS=<hex T# base> forces a texture (debug).
constexpr uint64_t kAtlasHashes[] = {0xe1d3d9ea867008b4ull};

uint64_t fnv64(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// Atlas cells (texels), button_prompts.md section 3. "seen" = drawn in a DCP_PROMPT_LOG run; the rest are read off the
// decoded atlas (DCP_RENDER_TEXDUMP) and wait for a scene that uses them.
using C = PromptCell;
const PromptCell kCells[] = {
    // Spheres (button bases; the composite draws one dimmed, then the tinted symbol over it).
    {448, 0, 544, 96, PadBtn::Circle, C::Ball, 0.9f},
    {544, 0, 640, 96, PadBtn::Triangle, C::Ball, 0.9f},   // seen: save select "Delete"
    {640, 0, 736, 96, PadBtn::Cross, C::Ball, 0.9f},
    {476, 128, 572, 224, PadBtn::Square, C::Ball, 0.9f},
    {572, 128, 668, 224, PadBtn::Circle, C::Ball, 0.9f},  // seen: world map / town HUD
    {100, 300, 196, 400, PadBtn::Triangle, C::Ball, 0.9f},
    {512, 344, 608, 440, PadBtn::Cross, C::Ball, 0.9f},
    {608, 344, 704, 436, PadBtn::Square, C::Ball, 0.9f},
    // Face symbols (white, tinted by the vertex colour).
    {880, 0, 944, 56, PadBtn::Circle, C::Symbol, 1.3f},
    {944, 0, 1008, 56, PadBtn::Cross, C::Symbol, 1.3f},
    {736, 56, 808, 120, PadBtn::Triangle, C::Symbol, 1.3f},
    {808, 56, 872, 120, PadBtn::Circle, C::Symbol, 1.3f},  // seen (over the circle sphere)
    {848, 128, 912, 188, PadBtn::Cross, C::Symbol, 1.3f},
    {912, 120, 976, 184, PadBtn::Square, C::Symbol, 1.3f},
    {776, 400, 840, 468, PadBtn::Square, C::Symbol, 1.3f},
    {632, 436, 700, 500, PadBtn::Triangle, C::Symbol, 1.3f},  // seen (over the triangle sphere)
    // Shoulder plates and the digits drawn over them.
    {408, 340, 508, 436, PadBtn::L1, C::Plate, 1},
    {0, 404, 104, 500, PadBtn::R1, C::Plate, 1},
    {700, 448, 752, 512, PadBtn::L1, C::Plate, 1},
    {880, 56, 936, 116, PadBtn::R1, C::Plate, 1},
    {628, 244, 668, 300, PadBtn::L1, C::Digit, 1},
    {848, 188, 892, 244, PadBtn::L2, C::Digit, 1},
    // Other buttons.
    {0, 0, 200, 108, PadBtn::Touchpad, C::Ball, 0.9f},      // seen: touchpad art (inn / town)
    {200, 340, 404, 392, PadBtn::Touchpad, C::Ball, 0.9f},  // "TOUCHPAD" lettering
    {200, 392, 384, 448, PadBtn::Options, C::Ball, 0.9f},   // "OPTIONS" lettering
    {200, 0, 324, 124, PadBtn::Dpad, C::Ball, 0.9f},
    {324, 0, 452, 124, PadBtn::Dpad, C::Ball, 0.9f},
};

}  // namespace

const char* pad_btn_name(PadBtn b) {
    static const char* const n[] = {"none", "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3",
                                    "options", "touchpad", "share", "dpad_up", "dpad_down", "dpad_left", "dpad_right",
                                    "dpad", "lstick", "rstick"};
    return size_t(b) < std::size(n) ? n[size_t(b)] : "?";
}

Prompts::Prompts() { log = std::getenv("DCP_PROMPT_LOG") != nullptr; }

bool Prompts::is_atlas(uint64_t base, uint32_t w, uint32_t h, const uint8_t* lin, size_t n) {
    if (w != 1024 || h != 512) return false;
    uint64_t hash = fnv64(lin, n);
    bool hit = false;
    for (uint64_t k : kAtlasHashes) hit |= k && k == hash;
    if (const char* e = std::getenv("DCP_PROMPT_ATLAS"); e && std::strtoull(e, nullptr, 16) == base) hit = true;
    if (log) std::fprintf(stderr, "prompt: 1024x512 texture %llx hash %016llx%s\n", (unsigned long long)base,
                          (unsigned long long)hash, hit ? " = button atlas" : "");
    return hit;
}

const PromptCell* Prompts::find(float u, float v) const {
    for (const PromptCell& c : kCells)
        if (c.btn != PadBtn::None && u >= c.x0 && u < c.x1 && v >= c.y0 && v < c.y1) return &c;
    return nullptr;
}

void Prompts::set_bindings(const std::map<std::string, std::string>& b) {
    binds_ = b;
    for (Glyph& g : cache_) {
        if (g.tex) glDeleteTextures(1, &g.tex);
        g = Glyph{};
    }
}

namespace {

// SDL key name -> short cap label.
std::string short_key(const std::string& k) {
    static const std::pair<const char*, const char*> kMap[] = {
        {"Left Shift", "Shift"}, {"Right Shift", "Shift"}, {"Left Ctrl", "Ctrl"}, {"Right Ctrl", "Ctrl"},
        {"Left Alt", "Alt"}, {"Right Alt", "Alt"}, {"Escape", "Esc"}, {"Return", "Enter"}, {"Backspace", "Bksp"},
        {"CapsLock", "Caps"}, {"Up", "\xe2\x86\x91"}, {"Down", "\xe2\x86\x93"}, {"Left", "\xe2\x86\x90"},
        {"Right", "\xe2\x86\x92"}, {"PageUp", "PgUp"}, {"PageDown", "PgDn"}, {"Delete", "Del"}, {"Insert", "Ins"},
    };
    for (auto& [a, b] : kMap)
        if (k == a) return b;
    return k;
}

}  // namespace

std::string Prompts::label_for(PadBtn b) const {
    auto get = [&](const char* a) {
        auto it = binds_.find(a);
        return it == binds_.end() ? std::string() : it->second;
    };
    auto four = [&](const char* u, const char* l, const char* d, const char* r) {
        std::string s;
        for (const char* a : {u, l, d, r}) {
            std::string k = get(a);
            if (k.empty()) return std::string();
            s += short_key(k);
        }
        return s;
    };
    switch (b) {
    case PadBtn::LStick: return four("move_up", "move_left", "move_down", "move_right");
    case PadBtn::RStick: return four("look_up", "look_left", "look_down", "look_right");
    case PadBtn::Dpad: return four("dpad_up", "dpad_left", "dpad_down", "dpad_right");
    case PadBtn::Share: return std::string();
    // The touchpad prompts seen so far are "Press TOUCHPAD to open Town Map" (town); on PC that is town_map, while
    // the plain touchpad key opens the character menu.
    case PadBtn::Touchpad: {
        std::string k = get("town_map");
        return k.empty() ? get("touchpad") : k;
    }
    default: return get(pad_btn_name(b));
    }
}

namespace {

struct Canvas {
    int w, h;
    std::vector<float> a, r, g, b;  // premultiplied-free straight colour, alpha separate
    Canvas(int w_, int h_) : w(w_), h(h_), a(size_t(w_) * h_), r(a.size()), g(a.size()), b(a.size()) {}
    void over(int x, int y, float cov, float cr, float cg, float cb) {
        if (x < 0 || y < 0 || x >= w || y >= h || cov <= 0) return;
        size_t i = size_t(y) * w + x;
        float na = cov + a[i] * (1 - cov);
        if (na <= 0) return;
        r[i] = (cr * cov + r[i] * a[i] * (1 - cov)) / na;
        g[i] = (cg * cov + g[i] * a[i] * (1 - cov)) / na;
        b[i] = (cb * cov + b[i] * a[i] * (1 - cov)) / na;
        a[i] = na;
    }
    // Rounded rectangle [x0,x1) x [y0,y1), radius rad, antialiased; fill colour (alpha fa).
    void rrect(float x0, float y0, float x1, float y1, float rad, float cr, float cg, float cb, float fa) {
        for (int y = int(y0) - 1; y <= int(y1) + 1; ++y)
            for (int x = int(x0) - 1; x <= int(x1) + 1; ++x) {
                float px = x + 0.5f, py = y + 0.5f;
                float qx = std::max(std::max(x0 + rad - px, px - (x1 - rad)), 0.0f);
                float qy = std::max(std::max(y0 + rad - py, py - (y1 - rad)), 0.0f);
                float d = std::sqrt(qx * qx + qy * qy) - rad;  // < 0 inside
                float cov = std::clamp(0.5f - d, 0.0f, 1.0f);
                over(x, y, cov * fa, cr, cg, cb);
            }
    }
    std::vector<uint8_t> rgba() const {
        std::vector<uint8_t> o(size_t(w) * h * 4);
        for (size_t i = 0; i < a.size(); ++i) {
            o[i * 4 + 0] = uint8_t(std::clamp(r[i], 0.0f, 1.0f) * 255 + 0.5f);
            o[i * 4 + 1] = uint8_t(std::clamp(g[i], 0.0f, 1.0f) * 255 + 0.5f);
            o[i * 4 + 2] = uint8_t(std::clamp(b[i], 0.0f, 1.0f) * 255 + 0.5f);
            o[i * 4 + 3] = uint8_t(std::clamp(a[i], 0.0f, 1.0f) * 255 + 0.5f);
        }
        return o;
    }
};

// System fonts (never shipped): bold Latin font for labels, a symbol font for arrows if the first lacks them.
struct Fonts {
    FT_Library lib = nullptr;
    std::vector<std::vector<uint8_t>> blobs;
    std::vector<FT_Face> faces;
    Fonts() {
        if (FT_Init_FreeType(&lib)) return;
        std::string win = std::getenv("WINDIR") ? std::getenv("WINDIR") : "C:/Windows";
        std::vector<std::string> paths;
        if (const char* e = std::getenv("DCP_PROMPT_FONT")) paths.push_back(e);
        for (const char* f : {"arialbd.ttf", "segoeuib.ttf", "seguisym.ttf", "arial.ttf"}) paths.push_back(win + "/Fonts/" + f);
        for (const std::string& p : paths) {
            std::ifstream in(p, std::ios::binary);
            if (!in) continue;
            blobs.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            FT_Face f;
            if (FT_New_Memory_Face(lib, blobs.back().data(), FT_Long(blobs.back().size()), 0, &f) == 0) faces.push_back(f);
        }
        if (faces.empty()) std::fprintf(stderr, "prompt: no system font found for key glyphs (set DCP_PROMPT_FONT)\n");
    }
    static Fonts& get() {
        static Fonts f;
        return f;
    }
};

std::vector<uint32_t> utf8(const std::string& s) {
    std::vector<uint32_t> o;
    for (size_t i = 0; i < s.size();) {
        uint8_t c = uint8_t(s[i]);
        int n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        uint32_t cp = n == 1 ? c : c & (0xff >> (n + 1));
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = cp << 6 | (uint8_t(s[i + k]) & 0x3f);
        o.push_back(cp);
        i += n;
    }
    return o;
}

// Draws text centred at (cx, cy) with pixel height px; returns the advance width (draw = false: measure only).
float text(Canvas* cv, const std::string& s, float px, float cx, float cy, bool draw) {
    Fonts& F = Fonts::get();
    if (F.faces.empty()) return 0;
    auto cps = utf8(s);
    auto face_for = [&](uint32_t cp) {
        for (FT_Face f : F.faces)
            if (FT_Get_Char_Index(f, cp)) return f;
        return F.faces[0];
    };
    float width = 0;
    for (uint32_t cp : cps) {
        FT_Face f = face_for(cp);
        FT_Set_Pixel_Sizes(f, 0, FT_UInt(px));
        if (FT_Load_Char(f, cp, FT_LOAD_DEFAULT)) continue;
        width += f->glyph->advance.x / 64.0f;
    }
    if (!draw) return width;
    float x = cx - width / 2;
    for (uint32_t cp : cps) {
        FT_Face f = face_for(cp);
        FT_Set_Pixel_Sizes(f, 0, FT_UInt(px));
        if (FT_Load_Char(f, cp, FT_LOAD_RENDER)) continue;
        FT_GlyphSlot gs = f->glyph;
        // Vertical centre on the cap height (~0.72 em) so letters sit in the middle of the cap.
        float base = cy + px * 0.36f;
        int ox = int(std::lround(x)) + gs->bitmap_left, oy = int(std::lround(base)) - gs->bitmap_top;
        for (unsigned y = 0; y < gs->bitmap.rows; ++y)
            for (unsigned xx = 0; xx < gs->bitmap.width; ++xx)
                cv->over(ox + int(xx), oy + int(y), gs->bitmap.buffer[y * gs->bitmap.pitch + xx] / 255.0f, 1, 1, 1);
        x += gs->advance.x / 64.0f;
    }
    return width;
}

constexpr int kH = 96;  // glyph texture height (px); the backend scales it to the original prompt's size

Canvas keycap(const std::string& label) {
    float px = utf8(label).size() <= 1 ? 60.0f : utf8(label).size() <= 3 ? 46.0f : 40.0f;
    float tw = text(nullptr, label, px, 0, 0, false);
    int w = std::max(kH, int(std::ceil(tw)) + 40);
    Canvas cv(w, kH);
    cv.rrect(2, 2, float(w - 2), float(kH - 2), 20, 0.92f, 0.92f, 0.92f, 1);    // light rim
    cv.rrect(8, 8, float(w - 8), float(kH - 12), 15, 0.13f, 0.13f, 0.15f, 1);   // dark face (thicker bottom = key depth)
    text(&cv, label, px, w / 2.0f, (kH - 4) / 2.0f, true);
    return cv;
}

// Mouse glyph: body outline, the bound button (1 left, 2 right, 3 middle) highlighted.
Canvas mouse_glyph(int button) {
    const int w = 68;
    Canvas cv(w, kH);
    const float x0 = 4, y0 = 2, x1 = w - 4, y1 = kH - 2, split = 40;
    cv.rrect(x0, y0, x1, y1, 30, 0.92f, 0.92f, 0.92f, 1);
    cv.rrect(x0 + 5, y0 + 5, x1 - 5, y1 - 5, 25, 0.13f, 0.13f, 0.15f, 1);
    const float hr = 1.0f, hg = 0.78f, hb = 0.2f;  // highlight
    float mid = w / 2.0f;
    for (int y = int(y0 + 5); y < int(split); ++y)
        for (int x = int(x0 + 5); x < int(x1 - 5); ++x) {
            bool left = x + 0.5f < mid - 3, right = x + 0.5f > mid + 3;
            if (!((button == 1 && left) || (button == 2 && right))) continue;
            size_t i = size_t(y) * w + x;
            if (cv.a[i] > 0.5f && cv.r[i] < 0.5f) cv.r[i] = hr, cv.g[i] = hg, cv.b[i] = hb;  // inside the dark face only
        }
    cv.rrect(x0 + 5, split - 2, x1 - 5, split + 2, 1, 0.92f, 0.92f, 0.92f, 1);       // button / palm line
    cv.rrect(mid - 2, y0 + 5, mid + 2, split, 1, 0.92f, 0.92f, 0.92f, 1);             // left / right line
    bool m = button == 3;
    cv.rrect(mid - 6, 14, mid + 6, 34, 6, m ? hr : 0.92f, m ? hg : 0.92f, m ? hb : 0.92f, 1);  // wheel
    return cv;
}

}  // namespace

unsigned Prompts::glyph(PadBtn b, float* aspect) {
    Glyph& g = cache_[size_t(b)];
    if (!g.tried) {
        g.tried = true;
        std::string k = label_for(b);
        if (!k.empty() && !Fonts::get().faces.empty()) {
            int mb = 0;
            if (k.size() == 6 && (k.compare(0, 5, "Mouse") == 0 || k.compare(0, 5, "mouse") == 0)) mb = k[5] - '0';
            Canvas cv = mb >= 1 && mb <= 3 ? mouse_glyph(mb) : keycap(mb ? "M" + std::to_string(mb) : short_key(k));
            std::vector<uint8_t> px = cv.rgba();
            glGenTextures(1, &g.tex);
            glBindTexture(GL_TEXTURE_2D, g.tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cv.w, cv.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            g.aspect = float(cv.w) / float(cv.h);
            if (log) std::fprintf(stderr, "prompt: glyph %s = '%s' %dx%d\n", pad_btn_name(b), k.c_str(), cv.w, cv.h);
        }
    }
    *aspect = g.aspect;
    return g.tex;
}

}  // namespace dcp::render
