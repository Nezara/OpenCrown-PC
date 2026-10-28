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
    void set_ignore_mouse(bool on) { ignore_mouse_ = on; }  // Steam Input trackpad mouse: not a KB/M switch

private:
    bool kbm_, ignore_mouse_ = false;
    float motion_ = 0;  // mouse travel (px) since the last gamepad input
};

class SdlInputPump {
public:
    virtual ~SdlInputPump() = default;
    virtual void handle_event(const SDL_Event& e) = 0;
    virtual void update(SDL_Window* win) = 0;
    virtual bool kbm_active() const = 0;  // DeviceTracker state
    // touch_as_mouse is in effect: a left press / drag / release is a finger (gamescope's touch emulation).
    virtual bool left_mouse_is_touch() const = 0;
    virtual void set_blocked(bool b) = 0;  // true: the game reads idle pads (a text box has the keyboard)
};

// Steam Deck (SteamDeck=1) or a gamescope session (Game Mode): the same test as mouse_attacks = auto.
bool steam_deck_host();

// Loads bindings from <dir>/input.ini (creates dir and writes the defaults if missing). Returns the pump (owned by
// the pad shim). wrap_script: keep the scripted pad source underneath (DCP_TOUCH_TEST runs), live input on top.
SdlInputPump* install_sdl_input(const std::string& dir, bool wrap_script = false);

// input.ini action -> its first bound input name ("E", "Left Shift", "Mouse1"), defaults overlaid with <dir>/input.ini.
std::map<std::string, std::string> prompt_bindings(const std::string& dir);
