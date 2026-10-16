// dcp_viewer: SDL3 + OpenGL 3.3 viewer for Dragon's Crown Pro sprite banks (.mbs + .ftx) and stages (.bsb),
// reading straight from the user's CPK. First native milestone; semantics marked GUESS are unconfirmed.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "anim.h"
#include "formats/bsb.h"
#include "formats/cpk.h"
#include "formats/ftx.h"
#include "formats/mbs.h"
#include "render/gl.h"
#include "render/png.h"

namespace fs = std::filesystem;
using namespace dcp::formats;
using namespace dcp::gl;
using dcp::render::write_png;
using dcp::viewer::Affine;
using dcp::viewer::DrawPart;
using dcp::viewer::DrawVert;
using dcp::viewer::MbsPlayer;

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

// ---- assets ----
struct Bank {
    std::vector<uint8_t> bytes;
    Mbs mbs;
    std::vector<GLuint> tex;  // per FTX0 child; 0 = missing
};

GLuint white_texture() {
    static GLuint t = 0;
    if (!t) {
        uint8_t px[4] = {255, 255, 255, 255};
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    return t;
}

GLuint upload(const FtxTexture& t) {
    auto info = gnm_format_info(t.tex.data_format());
    GLenum fmt = 0;
    switch (t.tex.data_format()) {
        case 41: fmt = GL_COMPRESSED_RGBA_BPTC_UNORM; break;
        case 35: fmt = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; break;
        case 36: fmt = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; break;
        case 37: fmt = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; break;
        case 10: fmt = GL_RGBA8; break;
        default: break;
    }
    if (!fmt) {
        std::fprintf(stderr, "texture %s: unsupported format %s\n", t.name.c_str(), info.name ? info.name : "?");
        return 0;
    }
    std::vector<uint8_t> lin;
    try { lin = ftx_detile(t); } catch (const std::exception& e) {
        std::fprintf(stderr, "texture %s: %s\n", t.name.c_str(), e.what());
        return 0;
    }
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (info.compressed)
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, fmt, t.tex.width(), t.tex.height(), 0, GLsizei(lin.size()), lin.data());
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.tex.width(), t.tex.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, lin.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return id;
}

class Assets {
public:
    explicit Assets(const fs::path& cpk) : cpk_(cpk) {
        for (const auto& e : cpk_.entries()) index_[lower(e.path())] = &e;
    }
    const CpkEntry* find(const std::string& p) const {
        auto it = index_.find(lower(p));
        return it == index_.end() ? nullptr : it->second;
    }
    std::vector<uint8_t> read(const std::string& p) const {
        const CpkEntry* e = find(p);
        if (!e) throw std::runtime_error("not in archive: " + p);
        return cpk_.read(*e);
    }
    Bank* bank(const std::string& mbs, const std::string& ftx) {
        std::string key = lower(mbs) + "|" + lower(ftx);
        auto it = banks_.find(key);
        if (it != banks_.end()) return it->second.get();
        auto b = std::make_unique<Bank>();
        Bank* r = nullptr;
        try {
            b->bytes = read(mbs);
            b->mbs = parse_mbs(b->bytes);
            if (find(ftx)) {
                auto fb = read(ftx);
                auto tx = parse_ftx(fb);
                for (const auto& t : tx) {
                    if (t.index != 0) continue;  // first texture of each FTX0 child
                    if (b->tex.size() <= t.chunk) b->tex.resize(t.chunk + 1, 0);
                    b->tex[t.chunk] = upload(t);
                }
            } else {
                std::fprintf(stderr, "no ftx %s\n", ftx.c_str());
            }
            r = b.get();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "bank %s: %s\n", mbs.c_str(), e.what());
        }
        banks_[key] = r ? std::move(b) : nullptr;
        return r;
    }

private:
    CpkArchive cpk_;
    std::unordered_map<std::string, const CpkEntry*> index_;
    std::map<std::string, std::unique_ptr<Bank>> banks_;
};

// ---- renderer ----
const char* kVs = R"(#version 330 core
layout(location=0) in vec2 aPos; layout(location=1) in vec2 aUv; layout(location=2) in vec4 aCol;
uniform mat4 uProj; out vec2 vUv; out vec4 vCol;
void main(){ vUv=aUv; vCol=aCol; gl_Position=uProj*vec4(aPos,0.0,1.0); })";
const char* kFs = R"(#version 330 core
in vec2 vUv; in vec4 vCol; uniform sampler2D uTex; out vec4 o;
void main(){ o=texture(uTex,vUv)*vCol; })";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        throw std::runtime_error(std::string("shader: ") + log);
    }
    return s;
}

struct Renderer {
    GLuint prog = 0, vao = 0, vbo = 0;
    GLint u_proj = -1;
    std::vector<DrawVert> batch;
    GLuint cur_tex = 0;
    int cur_blend = -1;

    void init() {
        prog = glCreateProgram();
        glAttachShader(prog, compile(GL_VERTEX_SHADER, kVs));
        glAttachShader(prog, compile(GL_FRAGMENT_SHADER, kFs));
        glLinkProgram(prog);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) throw std::runtime_error("program link failed");
        u_proj = glGetUniformLocation(prog, "uProj");
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glVertexAttribPointer(0, 2, GL_FLOAT, 0, sizeof(DrawVert), (void*)offsetof(DrawVert, x));
        glVertexAttribPointer(1, 2, GL_FLOAT, 0, sizeof(DrawVert), (void*)offsetof(DrawVert, u));
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, 1, sizeof(DrawVert), (void*)offsetof(DrawVert, rgba));
        for (int i = 0; i < 3; ++i) glEnableVertexAttribArray(i);
    }
    void begin(const float proj[16]) {
        glUseProgram(prog);
        glUniformMatrix4fv(u_proj, 1, 0, proj);
        glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glEnable(GL_BLEND);
        cur_tex = 0;
        cur_blend = -1;
    }
    void flush() {
        if (batch.empty()) return;
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(batch.size() * sizeof(DrawVert)), batch.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(batch.size()));
        batch.clear();
    }
    // Part byte +6 blend mode, from CB_BLEND0_CONTROL set by FUN_005aa470 (notes/map/render_cmds.md):
    // 0/6 alpha, 1 additive (S*Sa + D), 2 reverse-subtract (D - S*Sa); 3/4/5 are the same three with destination
    // alpha as the factor. The game draws 3-5 into an offscreen group with its own alpha; this viewer has no such
    // target, so dst-alpha of the main framebuffer stands in (approximation).
    static void set_blend(int b) {
        bool dst = b >= 3 && b <= 5;
        int row = dst ? b - 3 : (b == 6 ? 0 : b);
        GLenum f = dst ? GL_DST_ALPHA : GL_SRC_ALPHA;
        glBlendEquation(row == 2 ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
        glBlendFunc(f, row == 0 ? (dst ? GL_ONE_MINUS_DST_ALPHA : GL_ONE_MINUS_SRC_ALPHA) : GL_ONE);
    }
    void draw(const Bank& bank, const std::vector<DrawPart>& parts) {
        for (const auto& p : parts) {
            GLuint t = white_texture();
            if (p.texture >= 0) {
                if (size_t(p.texture) >= bank.tex.size() || !bank.tex[p.texture]) continue;
                t = bank.tex[p.texture];
            }
            if (t != cur_tex || p.blend != cur_blend) {
                flush();
                glBindTexture(GL_TEXTURE_2D, t);
                if (p.blend != cur_blend) set_blend(p.blend);
                cur_tex = t;
                cur_blend = p.blend;
            }
            static const int fan[12] = {0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5};
            for (int i : fan) batch.push_back(p.v[i]);
        }
    }
};

// ---- scene ----
struct Instance {
    Bank* bank = nullptr;
    std::unique_ptr<MbsPlayer> player;
    Affine world;
    std::array<uint8_t, 4> tint{255, 255, 255, 255};
    int z = 0, order = 0;
};

struct Camera {
    float cx = 0, cy = 0, zoom = 1;  // world centre, pixels per world unit
    void proj(int w, int h, float out[16]) const {
        std::memset(out, 0, 16 * sizeof(float));
        out[0] = 2 * zoom / w;
        out[5] = -2 * zoom / h;  // world y grows downwards (screen convention; GUESS checked by screenshots)
        out[10] = 1;
        out[12] = -cx * out[0];
        out[13] = -cy * out[5];
        out[15] = 1;
    }
};

struct Options {
    fs::path cpk;
    std::string mbs, bsb, shot;
    float view[3] = {0, 0, 0};  // --view cx,cy,zoom override (zoom 0 = fit)
    int motion = -1, frames = 0, width = 1280, height = 720;
};

int usage() {
    std::fprintf(stderr,
                 "usage: dcp_viewer <DRACRO.CPK> --mbs <path.mbs> [--motion N]\n"
                 "       dcp_viewer <DRACRO.CPK> --bsb <stage/name.bsb>\n"
                 "  options: --shot out.png --frames N (render N 1/60 s steps offscreen, save, exit) --size WxH --view cx,cy,zoom\n"
                 "  keys: left/right motion (sprite mode), space pause, arrows/drag pan, wheel or +/- zoom, R reset, Esc quit\n");
    return 2;
}

int run(const Options& o) {
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    bool shot = !o.shot.empty();
    SDL_Window* win = SDL_CreateWindow("dcp_viewer", o.width, o.height,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (shot ? SDL_WINDOW_HIDDEN : 0));
    if (!win) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { std::fprintf(stderr, "GL context: %s\n", SDL_GetError()); return 1; }
    if (const char* miss = dcp::gl::load([](const char* n) { return (void*)SDL_GL_GetProcAddress(n); })) {
        std::fprintf(stderr, "missing GL entry point %s\n", miss);
        return 1;
    }
    SDL_GL_SetSwapInterval(1);
    std::printf("GL %s on %s\n", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));

    Assets assets(o.cpk);
    Renderer ren;
    ren.init();
    std::vector<Instance> scene;
    Camera cam, home;
    Bank* sprite_bank = nullptr;
    int motion = o.motion;

    if (!o.mbs.empty()) {
        std::string ftx = o.mbs.substr(0, o.mbs.size() - 4) + ".ftx";
        sprite_bank = assets.bank(o.mbs, ftx);
        if (!sprite_bank) return 1;
        const auto& m = sprite_bank->mbs;
        for (size_t i = 0; i < m.motions.size(); ++i)
            std::printf("motion %3zu %-24s tracks %u%s\n", i, m.motions[i].name.c_str(), m.motions[i].track_count,
                        m.motions[i].flags & 1 ? " (disabled)" : "");
        if (motion < 0)  // default: first motion with tracks (motion 0 is often an empty placeholder)
            for (motion = 0; size_t(motion) + 1 < m.motions.size() && !m.motions[motion].track_count; ++motion) {}
        Instance in;
        in.bank = sprite_bank;
        in.player = std::make_unique<MbsPlayer>(m);
        in.player->set_motion(motion);
        scene.push_back(std::move(in));
        // Fit the motion's bounding rect (x0,y0,x1,y1), else a default box.
        float x0 = -200, y0 = -300, x1 = 200, y1 = 100;
        if (size_t(motion) < m.motions.size()) {
            auto r = m.motions[motion].rect;
            if (r[2] > r[0] && r[3] > r[1]) { x0 = r[0]; y0 = r[1]; x1 = r[2]; y1 = r[3]; }
        }
        cam.cx = (x0 + x1) / 2; cam.cy = (y0 + y1) / 2;
        cam.zoom = 0.8f * std::min(o.width / (x1 - x0), o.height / (y1 - y0));
    } else {
        auto bytes = assets.read(o.bsb);
        Bsb s = parse_bsb(bytes);
        int order = 0, missing = 0;
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (const auto& it : s.items) {
            if (!it.has_mone || !s.name(it.mone.mbs)) continue;
            const std::string* ftx = s.name(it.mone.ftx);
            Bank* b = assets.bank(*s.name(it.mone.mbs), ftx ? *ftx : std::string());
            if (!b) { ++missing; continue; }
            Instance in;
            in.bank = b;
            in.player = std::make_unique<MbsPlayer>(b->mbs);
            in.player->set_motion(it.mone.motion);  // payload dword 3 = motion index (names match the item names)
            // Stored positions are absolute (the loader makes them parent-relative); z only orders (GUESS: no parallax).
            float sx = it.scale_x * it.mone.scale_x, sy = it.scale_y * it.mone.scale_y;
            in.world = Affine{sx, 0, 0, sy, float(it.x), float(it.y)};
            uint32_t c = it.mone.color;
            in.tint = {uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16), uint8_t(c >> 24)};  // GUESS: dword 7 = RGBA tint
            in.z = it.z;
            in.order = order++;
            x0 = std::min(x0, float(it.x)); x1 = std::max(x1, float(it.x));
            y0 = std::min(y0, float(it.y)); y1 = std::max(y1, float(it.y));
            scene.push_back(std::move(in));
        }
        // GUESS: smaller (more negative) z is farther back, drawn first; ties in file order.
        std::stable_sort(scene.begin(), scene.end(), [](const Instance& a, const Instance& b) { return a.z < b.z; });
        if (s.bounds[0] < s.bounds[2] && s.bounds[0] > -10000000) {
            x0 = float(s.bounds[0]); x1 = float(s.bounds[2]); y0 = float(s.bounds[1]); y1 = float(s.bounds[3]);
        }
        if (x1 <= x0) { x0 = -1000; x1 = 1000; }
        if (y1 <= y0) { y0 = -500; y1 = 500; }
        cam.cx = (x0 + x1) / 2; cam.cy = (y0 + y1) / 2;
        cam.zoom = 0.95f * std::min(o.width / (x1 - x0), o.height / (y1 - y0));
        std::printf("%s: %zu art instances, %d unavailable, bounds %d,%d..%d,%d\n", o.bsb.c_str(), scene.size(), missing,
                    s.bounds[0], s.bounds[1], s.bounds[2], s.bounds[3]);
    }
    if (o.view[2] > 0) { cam.cx = o.view[0]; cam.cy = o.view[1]; cam.zoom = o.view[2]; }
    home = cam;

    auto render = [&](int w, int h) {
        glViewport(0, 0, w, h);
        glClearColor(0.12f, 0.12f, 0.14f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        float proj[16];
        cam.proj(w, h, proj);
        ren.begin(proj);
        std::vector<DrawPart> parts;
        for (const auto& in : scene) {
            parts.clear();
            in.player->emit(in.world, in.tint, parts);
            ren.draw(*in.bank, parts);
        }
        ren.flush();
    };
    auto step = [&](double ticks) { for (auto& in : scene) in.player->advance(ticks); };

    if (shot) {
        for (int i = 0; i < o.frames; ++i) step(1.0);
        GLuint fbo, tex;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, o.width, o.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { std::fprintf(stderr, "fbo incomplete\n"); return 1; }
        render(o.width, o.height);
        std::vector<uint8_t> px(size_t(o.width) * o.height * 4), rgb;
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, o.width, o.height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        for (int y = o.height - 1; y >= 0; --y)
            for (int x = 0; x < o.width; ++x) rgb.insert(rgb.end(), &px[(size_t(y) * o.width + x) * 4], &px[(size_t(y) * o.width + x) * 4 + 3]);
        if (GLenum e = glGetError()) std::fprintf(stderr, "GL error 0x%x\n", e);
        bool ok = write_png(o.shot, o.width, o.height, rgb);
        std::printf("%s %s after %d frames\n", ok ? "wrote" : "FAILED to write", o.shot.c_str(), o.frames);
        SDL_GL_DestroyContext(ctx);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return ok ? 0 : 1;
    }

    bool running = true, paused = false;
    uint64_t last = SDL_GetTicksNS();
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_MOUSE_WHEEL) cam.zoom *= e.wheel.y > 0 ? 1.15f : 1 / 1.15f;
            else if (e.type == SDL_EVENT_MOUSE_MOTION && (e.motion.state & SDL_BUTTON_LMASK)) {
                cam.cx -= e.motion.xrel / cam.zoom;
                cam.cy -= e.motion.yrel / cam.zoom;
            } else if (e.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode k = e.key.key;
                float pan = 100 / cam.zoom;
                if (k == SDLK_ESCAPE) running = false;
                else if (k == SDLK_SPACE) paused = !paused;
                else if (k == SDLK_R) cam = home;
                else if (k == SDLK_EQUALS || k == SDLK_KP_PLUS) cam.zoom *= 1.25f;
                else if (k == SDLK_MINUS || k == SDLK_KP_MINUS) cam.zoom /= 1.25f;
                else if (k == SDLK_UP) cam.cy -= pan;
                else if (k == SDLK_DOWN) cam.cy += pan;
                else if (sprite_bank && (k == SDLK_LEFT || k == SDLK_RIGHT)) {
                    int n = int(sprite_bank->mbs.motions.size());
                    motion = (motion + (k == SDLK_RIGHT ? 1 : n - 1)) % n;
                    scene[0].player->set_motion(motion);
                    std::printf("motion %d %s\n", motion, sprite_bank->mbs.motions[motion].name.c_str());
                } else if (k == SDLK_LEFT) cam.cx -= pan;
                else if (k == SDLK_RIGHT) cam.cx += pan;
            }
        }
        uint64_t now = SDL_GetTicksNS();
        double dt = double(now - last) / 1e9;
        last = now;
        if (!paused) step(std::min(dt, 0.1) * dcp::viewer::kTicksPerSecond);
        int w, h;
        SDL_GetWindowSizeInPixels(win, &w, &h);
        render(w, h);
        SDL_GL_SwapWindow(win);
    }
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) return usage();
    Options o;
    o.cpk = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--mbs") o.mbs = next();
        else if (a == "--bsb") o.bsb = next();
        else if (a == "--motion") o.motion = std::atoi(next().c_str());
        else if (a == "--shot") o.shot = next();
        else if (a == "--view") {
            std::string s = next();
            if (std::sscanf(s.c_str(), "%f,%f,%f", &o.view[0], &o.view[1], &o.view[2]) != 3) return usage();
        } else if (a == "--frames") o.frames = std::atoi(next().c_str());
        else if (a == "--size") {
            std::string s = next();
            if (std::sscanf(s.c_str(), "%dx%d", &o.width, &o.height) != 2) return usage();
        } else return usage();
    }
    if (o.mbs.empty() == o.bsb.empty()) return usage();
    try {
        return run(o);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
