// dcp_run's render side: owns the SDL3 window + SDL GPU device (Vulkan; OpenGL context as the fallback) on the process
// main thread and replays the frames the capture hooks hand over at each flip (src/render/README.md).
#include "render_loop.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "chat_box.h"
#include "ime_box.h"
#include "sdl_input.h"
#include "render/backend.h"
#include "render/capture.h"
#include "render/gl.h"
#include "render/png.h"
#include "../common/input_defaults.h"

namespace {

const char* kTitle = "Dragon's Crown Pro (dcp_run)";

SDL_WindowFlags window_flags(const RenderOptions& o) {
    return SDL_WINDOW_RESIZABLE | (o.window ? 0 : SDL_WINDOW_HIDDEN) |
           (o.window && o.fullscreen ? SDL_WINDOW_FULLSCREEN : 0);  // no mode set = borderless desktop
}

// SDL GPU renderer (Vulkan on Linux: SPIR-V shaders only). Hidden runs (--render / --shot without --window) need no
// swapchain: the window is not claimed. On failure everything made here is destroyed and *why says what failed.
// DCP_GPU_FAIL=1 makes it fail after device creation (test switch for the OpenGL fallback).
bool open_gpu(const RenderOptions& o, SDL_Window** win, SDL_GPUDevice** dev, std::unique_ptr<dcp::render::Device>* out,
              std::string* why) {
    *win = SDL_CreateWindow(kTitle, o.width, o.height, window_flags(o));
    if (!*win) { *why = std::string("window: ") + SDL_GetError(); return false; }
    *dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
    bool claimed = false;
    auto undo = [&](const std::string& reason) {
        *why = reason;
        out->reset();
        if (claimed) SDL_ReleaseWindowFromGPUDevice(*dev, *win);
        if (*dev) SDL_DestroyGPUDevice(*dev), *dev = nullptr;
        SDL_DestroyWindow(*win), *win = nullptr;
        return false;
    };
    if (!*dev) return undo(std::string("no SDL GPU device: ") + SDL_GetError());
    const char* fail = std::getenv("DCP_GPU_FAIL");
    if (fail && *fail && *fail != '0') return undo("DCP_GPU_FAIL is set (test switch)");
    if (o.window) {
        if (!SDL_ClaimWindowForGPUDevice(*dev, *win)) return undo(std::string("swapchain: ") + SDL_GetError());
        claimed = true;
        // Uncapped like the GL path's swap interval 0 (the game paces itself): mailbox, else immediate, else vsync.
        SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
        const char* name = "vsync";
        if (SDL_WindowSupportsGPUPresentMode(*dev, *win, SDL_GPU_PRESENTMODE_MAILBOX))
            mode = SDL_GPU_PRESENTMODE_MAILBOX, name = "mailbox";
        else if (SDL_WindowSupportsGPUPresentMode(*dev, *win, SDL_GPU_PRESENTMODE_IMMEDIATE))
            mode = SDL_GPU_PRESENTMODE_IMMEDIATE, name = "immediate";
        if (!SDL_SetGPUSwapchainParameters(*dev, *win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode))
            return undo(std::string("swapchain parameters: ") + SDL_GetError());
        std::fprintf(stderr, "render: SDL GPU present mode %s\n", name);
    }
    std::string err;
    *out = dcp::render::make_gpu_device(*dev, o.window ? *win : nullptr, &err);
    if (!*out) return undo(err);
    const char* gpu_name = SDL_GetStringProperty(SDL_GetGPUDeviceProperties(*dev), SDL_PROP_GPU_DEVICE_NAME_STRING, nullptr);
    std::fprintf(stderr, "render: SDL GPU (%s) on %s\n", SDL_GetGPUDeviceDriver(*dev), gpu_name ? gpu_name : "?");
    return true;
}

// The IME box's ImGui pass inside the SDL GPU swapchain pass.
struct ImeOverlay final : dcp::render::Device::Overlay {
    ImeBox* ime;
    explicit ImeOverlay(ImeBox* i) : ime(i) {}
    void prepare(void* cmd) override { ime->gpu_prepare(cmd); }
    void render(void* cmd, void* pass) override { ime->gpu_render(cmd, pass); }
};

}  // namespace

int run_render_loop(const RenderOptions& o, pthread_t game_thread, void (*finish)(int)) {
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_Window* win = nullptr;
    SDL_GPUDevice* gpu = nullptr;
    SDL_GLContext ctx = nullptr;
    dcp::render::Backend be;
    if (o.renderer != "gl") {
        std::unique_ptr<dcp::render::Device> dev;
        std::string why;
        if (!open_gpu(o, &win, &gpu, &dev, &why)) {
            if (o.renderer == "gpu") {
                std::fprintf(stderr, "render: SDL GPU failed (--renderer gpu, no fallback): %s\n", why.c_str());
                return 1;
            }
            std::fprintf(stderr, "render: OpenGL fallback: %s\n", why.c_str());
        } else if (!be.init(std::move(dev))) {
            std::fprintf(stderr, "render: SDL GPU backend init failed\n");
            return 1;
        }
    }
    if (!gpu) {  // OpenGL 3.3 (--renderer gl, or the auto fallback)
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        win = SDL_CreateWindow(kTitle, o.width, o.height, SDL_WINDOW_OPENGL | window_flags(o));
        if (!win) { std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
        ctx = SDL_GL_CreateContext(win);
        if (!ctx) { std::fprintf(stderr, "GL context: %s\n", SDL_GetError()); return 1; }
        if (const char* miss = dcp::gl::load([](const char* n) { return (void*)SDL_GL_GetProcAddress(n); })) {
            std::fprintf(stderr, "missing GL entry point %s\n", miss);
            return 1;
        }
        SDL_GL_SetSwapInterval(0);  // the game paces itself (vblank shim)
        std::fprintf(stderr, "render: GL %s on %s\n", (const char*)dcp::gl::glGetString(GL_VERSION),
                     (const char*)dcp::gl::glGetString(GL_RENDERER));
        if (!be.init()) return 1;
    }
    be.set_prompt_bindings(prompt_bindings(o.input_dir.empty() ? dcp::user_dir() : o.input_dir));
    // Name entry (sceImeDialog): only drawn with live input; scripted / hidden runs finish it headless (hle/ime.h).
    ImeBox ime;
    ImeOverlay ime_overlay(&ime);
    if (o.window && o.input) {
        if (gpu) ime.init_gpu(win, gpu);
        else ime.init(win, ctx);
    }
    // Room text chat overlay (net/chat.h), drawn into the name box's ImGui context.
    ChatBox chat;
    chat.init(ime.ok(), o.input_dir.empty() ? dcp::user_dir() : o.input_dir, o.input);
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
    // Test hook for the touch screen: DCP_TOUCH_TEST=6300=d:0.5:0.5,6310=m:0.7:0.5,6320=u:0.7:0.5 pushes a finger
    // down / motion / up at window-normalised (x, y) when that vblank is reached (finger 1 of a virtual touch device);
    // D / M / U = left mouse button down / drag / up there instead (touch_as_mouse).
    struct TouchEv { uint64_t v; char kind; float x, y; };
    std::vector<TouchEv> touch_events;
    if (const char* p = std::getenv("DCP_TOUCH_TEST")) {
        unsigned long long v;
        char kind;
        float x, y;
        int n;
        while (std::sscanf(p, "%llu=%c:%f:%f%n", &v, &kind, &x, &y, &n) == 4) {
            touch_events.push_back({v, kind, x, y});
            p += n;
            if (*p == ',') ++p;
        }
        std::fprintf(stderr, "touch: %zu test events\n", touch_events.size());
    }
    size_t next_touch_event = 0;

    size_t next_shot = 0;
    // F1: frame stats in the window title (game flips/s, mean flip interval, host replay+present cost per frame).
    const bool stats_log = std::getenv("DCP_STATS") != nullptr;  // stats on from the start, also written to stderr
    bool stats = stats_log;
    uint64_t stat_t0 = SDL_GetTicksNS(), stat_flips = 0, stat_work = 0, stat_draws = 0;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                std::fprintf(stderr, "render: window closed\n");
                finish(0);
            }
            if (o.input) o.input->handle_event(e);
            ime.handle_event(e);
            chat.handle_event(win, e, ime.blocking());
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F1 && !e.key.repeat) {
                stats = !stats;
                stat_t0 = SDL_GetTicksNS(), stat_flips = stat_work = stat_draws = 0;
                if (!stats) SDL_SetWindowTitle(win, kTitle);
            }
        }
        if (o.input) {
            o.input->set_blocked(ime.blocking() || chat.blocking());
            o.input->update(win);
        }
        if (pthread_tryjoin_np(game_thread, nullptr) == 0) finish(0);  // GNU: non-blocking join
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
        for (; next_touch_event < touch_events.size() && f->vblank >= touch_events[next_touch_event].v; ++next_touch_event) {
            const TouchEv& t = touch_events[next_touch_event];
            SDL_Event ev{};
            if (t.kind == 'D' || t.kind == 'M' || t.kind == 'U') {  // left mouse (gamescope's touch emulation)
                int w = 1, h = 1;
                SDL_GetWindowSize(win, &w, &h);
                ev.type = t.kind == 'D' ? SDL_EVENT_MOUSE_BUTTON_DOWN : t.kind == 'U' ? SDL_EVENT_MOUSE_BUTTON_UP
                                                                       : SDL_EVENT_MOUSE_MOTION;
                if (t.kind == 'M') {
                    ev.motion.timestamp = SDL_GetTicksNS(), ev.motion.windowID = SDL_GetWindowID(win), ev.motion.which = 1;
                    ev.motion.state = SDL_BUTTON_LMASK, ev.motion.x = t.x * w, ev.motion.y = t.y * h;
                } else {
                    ev.button.timestamp = SDL_GetTicksNS(), ev.button.windowID = SDL_GetWindowID(win), ev.button.which = 1;
                    ev.button.button = SDL_BUTTON_LEFT, ev.button.down = t.kind == 'D', ev.button.clicks = 1;
                    ev.button.x = t.x * w, ev.button.y = t.y * h;
                }
                SDL_PushEvent(&ev);
                std::fprintf(stderr, "touch: test mouse %c %.3f %.3f at vblank %llu\n", t.kind, t.x, t.y,
                             (unsigned long long)f->vblank);
                continue;
            }
            ev.type = t.kind == 'd' ? SDL_EVENT_FINGER_DOWN : t.kind == 'u' ? SDL_EVENT_FINGER_UP : SDL_EVENT_FINGER_MOTION;
            ev.tfinger.timestamp = SDL_GetTicksNS();
            ev.tfinger.touchID = 1, ev.tfinger.fingerID = 1, ev.tfinger.x = t.x, ev.tfinger.y = t.y, ev.tfinger.pressure = 1;
            ev.tfinger.windowID = SDL_GetWindowID(win);
            SDL_PushEvent(&ev);
            std::fprintf(stderr, "touch: test %c %.3f %.3f at vblank %llu\n", t.kind, t.x, t.y, (unsigned long long)f->vblank);
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
            // Name box + chat share one ImGui frame. SDL GPU: built first, drawn inside the present's swapchain pass.
            // GL: drawn after the game picture, before the swap.
            auto overlay = [&] {
                if (!o.input) return;
                const bool ime_on = ime.update(win), chat_on = chat.update(win, ime_on);
                if (ime_on || chat_on) {
                    ime.begin_frame(win);
                    if (chat_on) chat.draw(win);
                    if (ime_on) ime.draw(win);
                    ime.end_frame();
                }
            };
            if (gpu) {
                overlay();
                be.present(w, h, &ime_overlay);
            } else {
                be.present(w, h);
                overlay();
                SDL_GL_SwapWindow(win);
            }
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
