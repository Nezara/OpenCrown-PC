// dcp_run's GL side: owns the SDL3 window + GL context on the process main thread and replays the frames the
// capture hooks hand over at each flip (src/render/README.md).
#include "render_loop.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "sdl_input.h"
#include "render/backend.h"
#include "render/capture.h"
#include "render/gl.h"
#include "render/png.h"
#include "../common/input_defaults.h"

namespace {

// F12 capture: a rolling buffer of small snapshots (every 3rd replayed frame, 640x360, ~3 s) so a glitch that has
// already passed when the key is hit is still on disk. F12 writes it plus the current frame at full size to
// <user dir>/captures/<time>/ on a worker thread.
class Capture {
public:
    static constexpr int kW = 640, kH = 360, kSlots = 60, kEvery = 3;
    bool init() {
        using namespace dcp::gl;
        glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenFramebuffers(1, &fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        ring_.assign(kSlots, {});
        return true;
    }
    // After Backend::draw: shown target (rows top first) -> ring slot.
    void sample(const dcp::render::Backend& be, uint64_t vblank) {
        using namespace dcp::gl;
        if (++tick_ % kEvery) return;
        int sw = 0, sh = 0;
        if (!be.shown_texture(&sw, &sh)) return;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, be.shown_fbo());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
        glBlitFramebuffer(0, 0, sw, sh, 0, 0, kW, kH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
        px_.resize(size_t(kW) * kH * 4);
        glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px_.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        Slot& sl = ring_[head_];
        sl.vblank = vblank;
        sl.rgb.resize(size_t(kW) * kH * 3);
        for (size_t i = 0, n = size_t(kW) * kH; i < n; ++i) std::memcpy(&sl.rgb[i * 3], &px_[i * 4], 3);
        head_ = (head_ + 1) % kSlots;
    }
    void save(const dcp::render::Backend& be, uint64_t vblank) {
        int w = 0, h = 0;
        auto full = be.read_rgb(&w, &h);
        std::vector<Slot> ring;
        for (int i = 0; i < kSlots; ++i)
            if (!ring_[(head_ + i) % kSlots].rgb.empty()) ring.push_back(ring_[(head_ + i) % kSlots]);
        char stamp[32];
        std::time_t t = std::time(nullptr);
        std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&t));
        std::string dir = dcp::user_dir() + "/captures/" + stamp;
        std::thread([dir, full = std::move(full), w, h, ring = std::move(ring), vblank] {
            SDL_CreateDirectory(dir.c_str());
            char name[64];
            std::snprintf(name, sizeof name, "/now_v%llu.png", (unsigned long long)vblank);
            if (w) dcp::render::write_png(dir + name, w, h, full);
            for (const Slot& sl : ring) {
                std::snprintf(name, sizeof name, "/v%06llu.png", (unsigned long long)sl.vblank);
                dcp::render::write_png(dir + name, kW, kH, sl.rgb);
            }
            std::fprintf(stderr, "capture: %zu frames + current -> %s\n", ring.size(), dir.c_str());
        }).detach();
    }

private:
    struct Slot { uint64_t vblank = 0; std::vector<uint8_t> rgb; };
    unsigned tex_ = 0, fbo_ = 0;
    uint64_t tick_ = 0;
    int head_ = 0;
    std::vector<Slot> ring_;
    std::vector<uint8_t> px_;
};

}  // namespace

int run_render_loop(const RenderOptions& o, void* game_thread, void (*finish)(int)) {
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window* win = SDL_CreateWindow("Dragon's Crown Pro (dcp_run)", o.width, o.height,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (o.window ? 0 : SDL_WINDOW_HIDDEN) |
                                       (o.window && o.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));  // no mode set = borderless desktop
    if (!win) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { std::fprintf(stderr, "GL context: %s\n", SDL_GetError()); return 1; }
    if (const char* miss = dcp::gl::load([](const char* n) { return (void*)SDL_GL_GetProcAddress(n); })) {
        std::fprintf(stderr, "missing GL entry point %s\n", miss);
        return 1;
    }
    SDL_GL_SetSwapInterval(0);  // the game paces itself (vblank shim)
    std::fprintf(stderr, "render: GL %s on %s\n", (const char*)dcp::gl::glGetString(GL_VERSION),
                 (const char*)dcp::gl::glGetString(GL_RENDERER));
    dcp::render::Backend be;
    if (!be.init()) return 1;
    be.set_prompt_bindings(prompt_bindings(o.input_dir.empty() ? dcp::user_dir() : o.input_dir));
    // Button prompts follow the last-used device (DeviceTracker in the live input source). Test hook for scripted runs:
    // DCP_PROMPT_EVENTS=1100=key,1300=pad feeds a synthetic key press (E) / gamepad button (south) at those vblanks
    // through the same tracker code.
    DeviceTracker test_dev(false);
    std::vector<std::pair<uint64_t, bool>> prompt_events;  // (vblank, is key)
    if (const char* p = std::getenv("DCP_PROMPT_EVENTS")) {
        unsigned long long v;
        char kind[8];
        int n;
        while (std::sscanf(p, "%llu=%7[a-z]%n", &v, kind, &n) == 2) {
            prompt_events.push_back({v, std::strcmp(kind, "key") == 0});
            p += n;
            if (*p == ',') ++p;
        }
    }
    size_t next_prompt_event = 0;

    size_t next_shot = 0;
    Capture cap;
    if (o.window) cap.init();
    bool cap_req = false;
    // F1: frame stats in the window title (game flips/s, mean flip interval, host replay+present cost per frame).
    const char* kTitle = "Dragon's Crown Pro (dcp_run)";
    const bool stats_log = std::getenv("DCP_STATS") != nullptr;  // stats on from the start, also written to stderr
    bool stats = stats_log;
    uint64_t stat_t0 = SDL_GetTicksNS(), stat_flips = 0, stat_work = 0, stat_draws = 0;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) finish(0);
            if (o.input) o.input->handle_event(e);
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F1 && !e.key.repeat) {
                stats = !stats;
                stat_t0 = SDL_GetTicksNS(), stat_flips = stat_work = stat_draws = 0;
                if (!stats) SDL_SetWindowTitle(win, kTitle);
            }
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F12 && !e.key.repeat) cap_req = true;
        }
        if (o.input) o.input->update(win);
        if (WaitForSingleObject(static_cast<HANDLE>(game_thread), 0) == WAIT_OBJECT_0) finish(0);
        auto f = dcp::render::take_frame(50);
        if (!f) continue;
        uint64_t work0 = SDL_GetTicksNS();
        for (; next_prompt_event < prompt_events.size() && f->vblank >= prompt_events[next_prompt_event].first; ++next_prompt_event) {
            SDL_Event ev{};
            if (prompt_events[next_prompt_event].second) {
                ev.type = SDL_EVENT_KEY_DOWN, ev.key.scancode = SDL_SCANCODE_E, ev.key.down = true;
            } else {
                ev.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN, ev.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH, ev.gbutton.down = true;
            }
            if (o.input) o.input->handle_event(ev);
            else test_dev.on_event(ev);
            std::fprintf(stderr, "prompt: test %s event at vblank %llu\n", prompt_events[next_prompt_event].second ? "key" : "pad",
                         (unsigned long long)f->vblank);
        }
        be.set_prompt_kbm(o.input ? o.input->kbm_active() : test_dev.kbm());
        be.draw(*f);
        if (o.window) {
            cap.sample(be, f->vblank);
            static const long cap_at = std::getenv("DCP_CAPTURE_AT") ? std::atol(std::getenv("DCP_CAPTURE_AT")) : -1;
            if (cap_at >= 0 && f->vblank == uint64_t(cap_at)) cap_req = true;  // test hook for the F12 path
            if (cap_req) cap.save(be, f->vblank), cap_req = false;
        }
        if (next_shot < o.shots.size() && f->vblank >= o.shots[next_shot].first) {
            int w, h;
            auto rgb = be.read_rgb(&w, &h);
            const std::string& path = o.shots[next_shot].second;
            bool ok = w && dcp::render::write_png(path, w, h, rgb);
            std::fprintf(stderr, "render: %s %s (flip %llu, vblank %llu, %zu draws)\n", ok ? "wrote" : "FAILED to write",
                         path.c_str(), (unsigned long long)f->flip, (unsigned long long)f->vblank, f->draws.size());
            if (++next_shot == o.shots.size() && !o.window) finish(ok ? 0 : 1);
        }
        if (o.window) {
            int w, h;
            SDL_GetWindowSizeInPixels(win, &w, &h);
            be.present(w, h);
            SDL_GL_SwapWindow(win);
        }
        if (stats) {
            uint64_t now = SDL_GetTicksNS();
            ++stat_flips, stat_work += now - work0, stat_draws += f->draws.size();
            if (now - stat_t0 >= 500000000ull) {
                double sec = double(now - stat_t0) / 1e9;
                char t[160];
                std::snprintf(t, sizeof t, "%s | %.1f fps  %.2f ms/frame | replay %.2f ms  %llu draws", kTitle,
                              stat_flips / sec, sec * 1000.0 / double(stat_flips), stat_work / 1e6 / double(stat_flips),
                              (unsigned long long)(stat_draws / stat_flips));
                SDL_SetWindowTitle(win, t);
                if (stats_log) std::fprintf(stderr, "stats v%llu %s\n", (unsigned long long)f->vblank, t + std::strlen(kTitle) + 3);
                stat_t0 = now, stat_flips = stat_work = stat_draws = 0;
            }
        }
    }
}
