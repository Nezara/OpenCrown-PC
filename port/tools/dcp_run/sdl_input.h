// SDL3 input for dcp_run (replaces the scripted/idle pad source). Install before the game thread starts; the render
// loop (main thread) feeds it events and calls update() each iteration.
#pragma once
#include <map>
#include <string>

union SDL_Event;
struct SDL_Window;

// Last-used device for the button prompts (notes/map/button_prompts.md): a key press, a mouse button or mouse motion
// past a small threshold -> keyboard/mouse; a gamepad button, or a stick / trigger past its deadzone -> gamepad.
class DeviceTracker {
public:
    explicit DeviceTracker(bool kbm) : kbm_(kbm) {}
    void on_event(const SDL_Event& e);
    bool kbm() const { return kbm_; }

private:
    bool kbm_;
    float motion_ = 0;  // mouse travel (px) since the last gamepad input
};

class SdlInputPump {
public:
    virtual ~SdlInputPump() = default;
    virtual void handle_event(const SDL_Event& e) = 0;
    virtual void update(SDL_Window* win) = 0;
    virtual bool kbm_active() const = 0;  // DeviceTracker state
    virtual void set_blocked(bool b) = 0;  // true: the game reads idle pads (a text box has the keyboard)
};

// Loads bindings from <dir>/input.ini (creates dir and writes the defaults if missing). Returns the pump (owned by
// the pad shim).
SdlInputPump* install_sdl_input(const std::string& dir);

// input.ini action -> its first bound input name ("E", "Left Shift", "Mouse1"), defaults overlaid with <dir>/input.ini.
std::map<std::string, std::string> prompt_bindings(const std::string& dir);
