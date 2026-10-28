// The PC stand-in for the PS4 system keyboard (src/hle/ime.h): a Dear ImGui text box drawn over the game while the
// game waits on sceImeDialog (hero name, item bag names). Lives on the render-loop thread. It owns the ImGui context,
// which the room chat overlay (chat_box.h) draws into as well: one ImGui frame per presented frame, only when either
// has something to show (begin_frame / end_frame around both draws).
#pragma once
#include <cstdint>
#include <string>

union SDL_Event;
struct SDL_Window;
struct SDL_GPUDevice;

// Any key or mouse button held right now (a box that closed blocks game input until everything is released).
bool overlay_input_held();

class ImeBox {
public:
    bool init(SDL_Window* win, void* gl_context);
    // SDL GPU renderer: ImGui draws into the swapchain pass (gpu_prepare / gpu_render from the present overlay).
    bool init_gpu(SDL_Window* win, SDL_GPUDevice* dev);
    void shutdown();
    bool ok() const { return ok_; }
    void handle_event(const SDL_Event& e);
    // True while the box is open, and after it closes until every key and mouse button is up (so the Enter / click
    // that closed it does not reach the game). Game input is blocked while this is true.
    bool blocking() const { return blocking_; }
    // Once per presented frame: takes / drops the game's request; true = the box is open and draw() must run.
    bool update(SDL_Window* win);
    void draw(SDL_Window* win);  // between begin_frame and end_frame
    // The shared ImGui frame (after the game picture, before the swap).
    void begin_frame(SDL_Window* win);
    // GL: end_frame draws at once (after the game picture, before the swap). SDL GPU: the frame is built before the
    // present, which then calls gpu_prepare (outside any pass) and gpu_render (in the swapchain pass).
    void end_frame();
    void gpu_prepare(void* cmd);
    void gpu_render(void* cmd, void* pass);

private:
    bool ok_ = false;
    bool gpu_ = false;
    bool drawn_ = false;    // SDL GPU: this frame built draw data still to be rendered
    bool blocking_ = false;
    uint64_t id_ = 0;       // request being edited (0 = none)
    bool focus_ = false;    // give the text field keyboard focus on the next frame
    bool armed_ = false;    // every key / button held when the box opened has been released
    uint32_t max_ = 0;
    std::string title_;
    char buf_[512] = {};
};
