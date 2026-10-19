// dcp_run's GL side: owns the SDL3 window + GL context on the process main thread and replays the frames the
// capture hooks hand over at each flip (src/render/README.md).
#include "render_loop.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ime_box.h"
#include "sdl_input.h"
#include "render/backend.h"
#include "render/capture.h"
#include "render/gl.h"
#include "render/png.h"
#include "../common/input_defaults.h"

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
    // Name entry (sceImeDialog): only drawn with live input; scripted / hidden runs finish it headless (hle/ime.h).
    ImeBox ime;
    if (o.window && o.input) ime.init(win, ctx);
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
            ime.handle_event(e);
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F1 && !e.key.repeat) {
                stats = !stats;
                stat_t0 = SDL_GetTicksNS(), stat_flips = stat_work = stat_draws = 0;
                if (!stats) SDL_SetWindowTitle(win, kTitle);
            }
        }
        if (o.input) {
            o.input->set_blocked(ime.blocking());
            o.input->update(win);
        }
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
            if (o.input) ime.frame(win);
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
