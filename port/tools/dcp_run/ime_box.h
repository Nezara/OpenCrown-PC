// The PC stand-in for the PS4 system keyboard (src/hle/ime.h): a Dear ImGui text box drawn over the game while the
// game waits on sceImeDialog (hero name, item bag names). Lives on the render-loop thread.
#pragma once
#include <cstdint>
#include <string>

union SDL_Event;
struct SDL_Window;

class ImeBox {
public:
    bool init(SDL_Window* win, void* gl_context);
    void shutdown();
    void handle_event(const SDL_Event& e);
    // True while the box is open, and after it closes until every key and mouse button is up (so the Enter / click
    // that closed it does not reach the game). Game input is blocked while this is true.
    bool blocking() const { return blocking_; }
    // Once per presented frame, after the game picture and before the swap.
    void frame(SDL_Window* win);

private:
    bool ok_ = false;
    bool blocking_ = false;
    uint64_t id_ = 0;       // request being edited (0 = none)
    bool focus_ = false;    // give the text field keyboard focus on the next frame
    bool armed_ = false;    // every key / button held when the box opened has been released
    uint32_t max_ = 0;
    std::string title_;
    char buf_[512] = {};
};
