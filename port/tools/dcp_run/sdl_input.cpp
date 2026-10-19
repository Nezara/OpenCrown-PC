// dcp_run's SDL3 input source. Slot 0 = keyboard/mouse merged with the first gamepad; gamepad k -> slot k (up to 4,
// hot-plug drives the user-service login/logout). The main thread snapshots devices (update); game threads read the
// snapshot. Rumble/light-bar requests from the game are queued and applied on the main thread.
//
// Keyboard/mouse bindings come from input.ini (see dcp::kInputDefaults in tools/common/input_defaults.h for names and format). Fairy mode (hold "fairy" or
// press "fairy_toggle") turns mouse motion into touchpad finger motion; "fairy_tap" is the touchpad click there.
#include "sdl_input.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "../common/input_defaults.h"
#include "hle/input.h"

namespace {

// ScePadData.buttons bits (SDK).
enum : uint32_t {
    kL3 = 0x2, kR3 = 0x4, kOptions = 0x8, kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80, kL2 = 0x100,
    kR2 = 0x200, kL1 = 0x400, kR1 = 0x800, kTriangle = 0x1000, kCircle = 0x2000, kCross = 0x4000, kSquare = 0x8000,
    kTouchpad = 0x100000,
};

const std::pair<const char*, uint32_t> kButtonActions[] = {
    {"square", kSquare}, {"circle", kCircle}, {"triangle", kTriangle}, {"cross", kCross}, {"l1", kL1}, {"r1", kR1},
    {"l2", kL2}, {"r2", kR2}, {"l3", kL3}, {"r3", kR3}, {"options", kOptions}, {"touchpad", kTouchpad},
    {"dpad_up", kUp}, {"dpad_down", kDown}, {"dpad_left", kLeft}, {"dpad_right", kRight},
};

float g_fairy_speed = 1.0f;  // touchpad units per window pixel of mouse motion, x (1920 / window width)

struct Bind {
    int scancode = -1;  // SDL_Scancode, or
    int mouse = 0;      // SDL mouse button 1..5
};

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::map<std::string, std::vector<Bind>> parse(const std::string& text, const std::string& src) {
    std::map<std::string, std::vector<Bind>> out;
    std::istringstream in(text);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) { std::fprintf(stderr, "input: %s:%d: no '='\n", src.c_str(), n); continue; }
        std::string action = trim(line.substr(0, eq));
        if (action == "fairy_speed") {
            g_fairy_speed = std::strtof(trim(line.substr(eq + 1)).c_str(), nullptr);
            if (!(g_fairy_speed > 0.0f)) g_fairy_speed = 1.0f;
            continue;
        }
        auto& v = out[action];
        v.clear();
        std::istringstream vals(line.substr(eq + 1));
        std::string tok;
        while (std::getline(vals, tok, ',')) {
            tok = trim(tok);
            if (tok.empty()) continue;
            Bind b;
            if (tok.size() == 6 && SDL_strncasecmp(tok.c_str(), "mouse", 5) == 0 && tok[5] >= '1' && tok[5] <= '5') {
                static const int kSdlButton[] = {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE, SDL_BUTTON_X1,
                                                 SDL_BUTTON_X2};  // Mouse1..5 = left, right, middle, back, forward
                b.mouse = kSdlButton[tok[5] - '1'];
            } else {
                b.scancode = SDL_GetScancodeFromName(tok.c_str());
                if (b.scancode == SDL_SCANCODE_UNKNOWN) {
                    std::fprintf(stderr, "input: %s:%d: unknown key '%s'\n", src.c_str(), n, tok.c_str());
                    continue;
                }
            }
            v.push_back(b);
        }
    }
    return out;
}

uint8_t axis_u8(int16_t v) { return uint8_t((int(v) + 32768) >> 8); }

class SdlInput final : public dcp::hle::InputSource, public SdlInputPump {
public:
    explicit SdlInput(std::map<std::string, std::vector<Bind>> b) : binds_(std::move(b)) { rescan(); }

    dcp::hle::PadState read(int pad, uint64_t) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (pad == 0) lift_seen_ = lift_;  // the game has read a lifted-finger state: re-centring may proceed
        return pad >= 0 && pad < 4 ? snap_[pad] : dcp::hle::PadState{};
    }
    bool present(int pad) override {
        std::lock_guard<std::mutex> lk(mu_);
        return pad == 0 || (pad > 0 && pad < 4 && pad < int(pads_.size()));
    }
    void set_vibration(int pad, uint8_t large, uint8_t small) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (pad >= 0 && pad < 4) rumble_[pad] = {large, small, true};
    }
    void set_light_bar(int pad, uint8_t r, uint8_t g, uint8_t b) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (pad >= 0 && pad < 4) led_[pad] = {r, g, b, true};
    }

    void handle_event(const SDL_Event& e) override {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) rescan();
        dev_.on_event(e);
    }
    bool kbm_active() const override { return dev_.kbm(); }
    void set_blocked(bool b) override { blocked_ = b; }

    void update(SDL_Window* win) override {
        dcp::hle::PadState s[4];
        if (blocked_) {
            if (rel_) SDL_SetWindowRelativeMouseMode(win, false), rel_ = false;
            std::lock_guard<std::mutex> lk(mu_);
            for (int i = 0; i < 4; ++i) snap_[i] = s[i];
            finger_ = false;
            return;
        }
        std::vector<SDL_Gamepad*> pads;
        {
            std::lock_guard<std::mutex> lk(mu_);
            pads = pads_;
        }
        for (size_t i = 0; i < pads.size() && i < 4; ++i) read_gamepad(pads[i], &s[i]);
        read_kbm(win, &s[0]);
        std::lock_guard<std::mutex> lk(mu_);
        for (int i = 0; i < 4; ++i) snap_[i] = s[i];
        for (size_t i = 0; i < pads.size() && i < 4; ++i) {
            if (rumble_[i].dirty) {
                SDL_RumbleGamepad(pads[i], uint16_t(rumble_[i].large * 257), uint16_t(rumble_[i].small * 257), 1000);
                rumble_[i].dirty = false;
            }
            if (led_[i].dirty) {
                SDL_SetGamepadLED(pads[i], led_[i].r, led_[i].g, led_[i].b);
                led_[i].dirty = false;
            }
        }
    }

private:
    void rescan() {
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        std::vector<SDL_Gamepad*> pads;
        for (int i = 0; i < n && pads.size() < 4; ++i) {
            SDL_Gamepad* g = SDL_GetGamepadFromID(ids[i]);
            if (!g) g = SDL_OpenGamepad(ids[i]);
            if (g) pads.push_back(g);
        }
        SDL_free(ids);
        std::lock_guard<std::mutex> lk(mu_);
        if (pads.size() != pads_.size()) std::fprintf(stderr, "input: %zu gamepad(s)\n", pads.size());
        pads_ = pads;
    }

    static void read_gamepad(SDL_Gamepad* g, dcp::hle::PadState* s) {
        static const std::pair<SDL_GamepadButton, uint32_t> kMap[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, kCross}, {SDL_GAMEPAD_BUTTON_EAST, kCircle}, {SDL_GAMEPAD_BUTTON_WEST, kSquare},
            {SDL_GAMEPAD_BUTTON_NORTH, kTriangle}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kL1},
            {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kR1}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, kL3},
            {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kR3}, {SDL_GAMEPAD_BUTTON_START, kOptions},
            {SDL_GAMEPAD_BUTTON_BACK, kTouchpad}, {SDL_GAMEPAD_BUTTON_TOUCHPAD, kTouchpad},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight},
        };
        for (auto& [b, bit] : kMap)
            if (SDL_GetGamepadButton(g, b)) s->buttons |= bit;
        s->lx = axis_u8(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX));
        s->ly = axis_u8(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));
        s->rx = axis_u8(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX));
        s->ry = axis_u8(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY));
        s->l2 = uint8_t(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
        s->r2 = uint8_t(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
        if (s->l2 > 0x20) s->buttons |= kL2;
        if (s->r2 > 0x20) s->buttons |= kR2;
        // DualShock 4 / DualSense touchpad: normalised fingers -> 1920 x 943 touchpad coordinates.
        if (SDL_GetNumGamepadTouchpads(g) > 0) {
            for (int f = 0; f < 2 && f < SDL_GetNumGamepadTouchpadFingers(g, 0); ++f) {
                bool down = false;
                float x = 0, y = 0;
                if (SDL_GetGamepadTouchpadFinger(g, 0, f, &down, &x, &y, nullptr) && down) {
                    int k = s->touch_num++;
                    s->tx[k] = uint16_t(x * 1919.0f), s->ty[k] = uint16_t(y * 942.0f), s->tid[k] = uint8_t(f);
                }
            }
        }
    }

    bool held(const char* action, const bool* keys, SDL_MouseButtonFlags mouse, bool mouse_ok) const {
        auto it = binds_.find(action);
        if (it == binds_.end()) return false;
        for (const Bind& b : it->second) {
            if (b.scancode >= 0 && keys[b.scancode]) return true;
            if (b.mouse && mouse_ok && (mouse & SDL_BUTTON_MASK(b.mouse))) return true;
        }
        return false;
    }

    void read_kbm(SDL_Window* win, dcp::hle::PadState* s) {
        if (!(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) return;
        const bool* keys = SDL_GetKeyboardState(nullptr);
        float mx = 0, my = 0;
        SDL_MouseButtonFlags mouse = SDL_GetMouseState(&mx, &my);
        bool toggle_now = held("fairy_toggle", keys, mouse, true);
        if (toggle_now && !toggle_was_) fairy_on_ = !fairy_on_;
        toggle_was_ = toggle_now;
        bool hold = held("fairy", keys, mouse, true);
        bool fairy = hold || fairy_on_;
        bool tap = fairy && held("fairy_tap", keys, mouse, true);
        // Right-stick pointer (look keys): pointer_tap is R1, its interact, while a look key is held and for 2 s after.
        const uint64_t now = SDL_GetTicks();
        if (held("look_up", keys, mouse, !hold) || held("look_down", keys, mouse, !hold) ||
            held("look_left", keys, mouse, !hold) || held("look_right", keys, mouse, !hold))
            look_ms_ = now;
        const bool pointer = !fairy && look_ms_ && now - look_ms_ < 2000;
        bool ptap = pointer && held("pointer_tap", keys, mouse, true);
        // Keys bound to fairy_tap / pointer_tap stop sending their normal button while that mode is on.
        auto bound_to = [&](const char* action, const Bind& b) {
            auto it = binds_.find(action);
            if (it == binds_.end()) return false;
            for (const Bind& t : it->second)
                if (t.scancode == b.scancode && t.mouse == b.mouse) return true;
            return false;
        };
        auto tap_key = [&](const Bind& b) {
            return (fairy && bound_to("fairy_tap", b)) || (pointer && bound_to("pointer_tap", b));
        };
        auto down = [&](const char* action, bool mouse_ok) {
            auto it = binds_.find(action);
            if (it == binds_.end()) return false;
            for (const Bind& b : it->second) {
                if (tap_key(b)) continue;
                if (b.scancode >= 0 && keys[b.scancode]) return true;
                if (b.mouse && mouse_ok && (mouse & SDL_BUTTON_MASK(b.mouse))) return true;
            }
            return false;
        };
        for (auto& [name, bit] : kButtonActions)
            if (down(name, !hold)) s->buttons |= bit;
        if (ptap) s->buttons |= kR1;
        if (s->buttons & kL2) s->l2 = 0xff;
        if (s->buttons & kR2) s->r2 = 0xff;
        auto stick = [&](const char* neg, const char* pos, uint8_t* v) {
            int d = int(down(pos, !hold)) - int(down(neg, !hold));
            if (d) *v = d > 0 ? 0xff : 0x00;
        };
        stick("move_left", "move_right", &s->lx);
        stick("move_up", "move_down", &s->ly);
        stick("look_left", "look_right", &s->rx);
        stick("look_up", "look_down", &s->ry);
        // Town map: on PS4 a touchpad click with a finger on the left half of the pad (debugger, Lucain/town repro);
        // a click with no finger opens the character menu instead.
        if (!fairy && held("town_map", keys, mouse, true)) {
            s->buttons |= kTouchpad;
            s->touch_num = 1;
            s->tx[0] = 300, s->ty[0] = 471, s->tid[0] = 0;
        }

        // Fairy pointer: the game reads touch motion per tick (deltas of a held finger), so mouse motion moves a
        // virtual finger; near the pad edge it lifts for at least one game read and re-touches at the centre (a lift
        // breaks the delta chain, so the fairy does not jump). Relative mouse mode keeps the cursor from leaving.
        float dx = 0, dy = 0;
        SDL_GetRelativeMouseState(&dx, &dy);
        if (fairy != rel_) SDL_SetWindowRelativeMouseMode(win, fairy), rel_ = fairy;
        if (!fairy) { finger_ = false; return; }
        if (tap) s->buttons |= kTouchpad;
        std::lock_guard<std::mutex> lk(mu_);
        if (lift_) {
            if (!lift_seen_) return;  // keep the finger up until the game has seen it
            lift_ = lift_seen_ = false;
            finger_ = false;
        }
        int w = 1, h = 1;
        SDL_GetWindowSize(win, &w, &h);
        // Mouse pixels -> game screen pixels (letterboxed 16:9 picture) -> touchpad units the game turns back into
        // screen pixels (x 1920/1920, y 1080/943), divided by the game's relative cursor gain 0.2 * sens (default sens 3
        // -> 0.6; FUN_0039a1e0, notes/map/fairy_cursor.md), so the fairy moves 1:1 with the mouse. fairy_speed scales.
        float vw = float(w), vh = float(h);
        if (vw * 9 > vh * 16) vw = vh * 16 / 9;
        else vh = vw * 9 / 16;
        constexpr float kGameGain = 0.6f;
        float kx = g_fairy_speed * (1920.0f / vw) / kGameGain;
        float ky = g_fairy_speed * (1080.0f / vh) / kGameGain * (943.0f / 1080.0f);
        if (!finger_) fx_ = 960.0f, fy_ = 471.0f, finger_ = true, ++finger_id_;
        fx_ += dx * kx, fy_ += dy * ky;
        if (fx_ < 64 || fx_ > 1856 || fy_ < 48 || fy_ > 895) { lift_ = true; return; }
        s->touch_num = 1;
        s->tx[0] = uint16_t(fx_), s->ty[0] = uint16_t(fy_), s->tid[0] = finger_id_;
    }

    struct Rumble { uint8_t large = 0, small = 0; bool dirty = false; };
    struct Led { uint8_t r = 0, g = 0, b = 0; bool dirty = false; };

    std::map<std::string, std::vector<Bind>> binds_;
    DeviceTracker dev_{!SDL_HasGamepad()};  // start on KB/M unless a gamepad is plugged in
    bool fairy_on_ = false, toggle_was_ = false, rel_ = false;
    bool blocked_ = false;  // ImeBox open (render-loop thread only)
    uint64_t look_ms_ = 0;  // SDL_GetTicks when a look key was last held (pointer_tap window)
    bool finger_ = false, lift_ = false, lift_seen_ = false;  // lift_*: guarded by mu_
    float fx_ = 0, fy_ = 0;
    uint8_t finger_id_ = 0;
    std::mutex mu_;
    std::vector<SDL_Gamepad*> pads_;
    dcp::hle::PadState snap_[4];
    Rumble rumble_[4];
    Led led_[4];
};

}  // namespace

void DeviceTracker::on_event(const SDL_Event& e) {
    constexpr float kMotion = 24.0f;   // mouse travel (window px) that counts as "using the mouse"
    constexpr int kStick = 16000;      // stick deadzone (of 32767); triggers count past half that
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        if (e.key.scancode == SDL_SCANCODE_F1) break;  // stats key
        kbm_ = true;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_WHEEL: kbm_ = true; break;
    case SDL_EVENT_MOUSE_MOTION:
        motion_ += std::fabs(e.motion.xrel) + std::fabs(e.motion.yrel);
        if (motion_ >= kMotion) kbm_ = true;
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: kbm_ = false, motion_ = 0; break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        bool trig = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        if (std::abs(int(e.gaxis.value)) > (trig ? kStick / 2 : kStick)) kbm_ = false, motion_ = 0;
        break;
    }
    default: break;
    }
}

std::map<std::string, std::string> prompt_bindings(const std::string& dir) {
    std::map<std::string, std::string> out;
    auto scan = [&](const std::string& text) {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            line = trim(line.substr(0, line.find('#')));
            size_t eq = line.find('=');
            if (line.empty() || eq == std::string::npos) continue;
            std::string v = trim(line.substr(eq + 1));
            out[trim(line.substr(0, eq))] = trim(v.substr(0, v.find(',')));
        }
    };
    scan(dcp::kInputDefaults);
    if (std::ifstream f(dir + "/input.ini"); f) {
        std::stringstream ss;
        ss << f.rdbuf();
        scan(ss.str());
    }
    return out;
}

SdlInputPump* install_sdl_input(const std::string& dir) {
    SDL_CreateDirectory(dir.c_str());
    std::string ini_path = dir + "/input.ini";
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) std::fprintf(stderr, "input: gamepads unavailable: %s\n", SDL_GetError());
    auto binds = parse(dcp::kInputDefaults, "defaults");
    std::ifstream f(ini_path);
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        for (auto& [k, v] : parse(ss.str(), ini_path)) binds[k] = v;
        std::fprintf(stderr, "input: bindings from %s\n", ini_path.c_str());
    } else if (std::ofstream o(ini_path); o) {
        o << dcp::kInputDefaults;
        std::fprintf(stderr, "input: wrote default bindings to %s\n", ini_path.c_str());
    }
    auto src = std::make_unique<SdlInput>(std::move(binds));
    SdlInputPump* pump = src.get();
    dcp::hle::set_input_source(std::move(src));
    return pump;
}
