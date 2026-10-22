// Pure parts of the touch screen / Steam Deck input (no SDL), unit-tested in tests/test_touch.cpp.
// Fairy cursor model (notes/map/fairy_cursor.md): the game's default touch mode is relative; a held finger's per-read
// delta (touchpad units, 1920 x 943) becomes a cursor move of delta * (1920/1920, 1080/943) * k, k = 0.2 * sens
// (default sens 3 -> 0.6). The cursor position (centred 1920x1080 px, y down) is read back from the game each frame
// (Cursor_Update hook), so TouchLoop drives the fairy to an absolute point in closed loop.
#pragma once
#include <cmath>
#include <cstdint>

namespace dcp::touch {

constexpr float kGameGain = 0.6f;  // the game's relative touch gain at the default sensitivity

// Window point (px, top-left origin) -> centred game screen px (1920x1080, y down), through the same letterbox as
// Backend::present (16:9 picture scaled to fit, centred). Points on the bars clamp to the picture edge.
inline void window_to_game(float wx, float wy, float w, float h, float* gx, float* gy) {
    float s = std::fmin(w / 1920.0f, h / 1080.0f);
    if (!(s > 0)) { *gx = *gy = 0; return; }
    float ox = (w - 1920.0f * s) * 0.5f, oy = (h - 1080.0f * s) * 0.5f;
    *gx = std::fmax(-960.0f, std::fmin(960.0f, (wx - ox) / s - 960.0f));
    *gy = std::fmax(-540.0f, std::fmin(540.0f, (wy - oy) / s - 540.0f));
}

// Which gamepad inputs feed the PS4 touchpad. One touchpad (DualShock 4 / DualSense): pad 0, its click = touchpad
// button. Two touchpads (Steam Deck through SDL's HIDAPI driver: 0 = left, 1 = right; the mapping puts the LEFT pad
// click on SDL_GAMEPAD_BUTTON_TOUCHPAD and the RIGHT pad click on SDL_GAMEPAD_BUTTON_MISC2) with deck_trackpads on:
// right pad = PS4 touchpad, right click = touchpad button, left click = town map, left finger ignored.
struct TouchpadRoute {
    int pad = 0;                   // gamepad touchpad index used as the PS4 touchpad, -1 = none
    bool touchpad_btn_is_map = false;  // SDL_GAMEPAD_BUTTON_TOUCHPAD -> town map (else -> touchpad button)
    bool misc2_is_touchpad = false;    // SDL_GAMEPAD_BUTTON_MISC2 -> touchpad button
};
inline TouchpadRoute route_touchpads(int num_touchpads, bool deck_trackpads) {
    if (num_touchpads >= 2 && deck_trackpads) return {1, true, true};
    return {num_touchpads > 0 ? 0 : -1, false, false};
}

// Steam's virtual gamepad (Steam Input on; SDL: USB_VENDOR_VALVE 0x28de, USB_PRODUCT_STEAM_VIRTUAL_GAMEPAD 0x11ff).
// Its default template turns the Deck's right trackpad into a mouse (click = left button), so while it is the
// active device mouse motion steers the fairy and the left button is the interact.
inline bool is_steam_virtual_pad(uint16_t vendor, uint16_t product) { return vendor == 0x28de && product == 0x11ff; }
inline bool steam_mouse_mode(bool deck_trackpads, bool has_steam_virtual_pad, bool kbm_active) {
    return deck_trackpads && has_steam_virtual_pad && !kbm_active;
}

// "auto" | "on" | "off" settings in input.ini (touch_as_mouse, mouse_attacks); anything else = auto.
enum class Tri { Auto, On, Off };
inline Tri parse_tri(const char* v) {
    auto eq = [&](const char* s) {
        const char* p = v;
        for (; *p && *s; ++p, ++s)
            if ((*p | 0x20) != *s) return false;
        return !*p && !*s;
    };
    if (eq("on") || eq("1") || eq("true")) return Tri::On;
    if (eq("off") || eq("0") || eq("false")) return Tri::Off;
    return Tri::Auto;
}

// touch_as_mouse: gamescope (Steam Deck Game Mode) turns touches into left-mouse clicks for programs that do not ask
// for native touch, so a left press / drag / release stands for a finger on the fairy path. auto = only under
// gamescope and only until a real touch event has been seen (then the screen delivers touches itself). Never while
// the mouse is captured for the fairy (relative mode: no absolute position; the Steam Input trackpad mouse lives there).
inline bool mouse_is_touch(Tri setting, bool gamescope, bool native_touch_seen, bool relative_mode) {
    if (relative_mode || setting == Tri::Off) return false;
    return setting == Tri::On || (gamescope && !native_touch_seen);
}

// Closed loop: virtual finger that moves the game's cursor to (tx, ty). One step per new cursor reading. The step
// sent last is not in the reading yet (the hook samples the position before the game applies that read's delta), so
// it counts as "in flight" when the error is taken. The game's gain k (in-game touch sensitivity) is estimated from
// how far the cursor moved for the step before last, so other sensitivities than the default converge as well.
struct TouchLoop {
    float gain = 0.7f;   // fraction of the remaining error sent per step
    float k = kGameGain; // estimated game gain (screen px per touchpad unit, x)
    // target
    bool want = false;   // a target is held (finger down, or lifted but not yet arrived)
    bool held = false;   // the real finger is still down
    float tx = 0, ty = 0;
    int tap = 0;         // 1 = tap requested (interact once arrived), 2.. = pressing
    int wait = 0;        // steps left before giving up on arriving (then tap anyway / lift)
    // virtual finger
    bool on = false, lift = false;
    float fx = 960, fy = 471;
    uint8_t id = 0;
    float px = 0, py = 0;  // last step (screen px, at the estimated k), not yet in a reading
    float qx = 0, qy = 0;  // last step in touchpad units (y scaled by 1080/943)
    float ux = 0, uy = 0;  // step before last, same units
    float ex = 0, ey = 0;  // dead-reckoned cursor (all steps sent) when no readings arrive
    float lx = 0, ly = 0;  // previous reading
    bool lvalid = false;
    uint32_t seq = 0;
    int stale = 0;

    void press(float x, float y) { want = held = true, tx = x, ty = y, tap = 0, wait = 0; }
    void move(float x, float y) { if (held) tx = x, ty = y; }
    void release(bool is_tap) {
        held = false;
        wait = 40;  // ~0.67 s at 60 Hz to arrive
        if (is_tap) tap = 1;
    }
    bool active() const { return want || on; }

    struct Out { bool finger = false; uint16_t x = 0, y = 0; uint8_t id = 0; bool button = false; };
    // One game pad read. have: (cx, cy) is a cursor reading with counter rseq (0 = none ever).
    Out read(bool have, float cx, float cy, uint32_t rseq) {
        constexpr float kY = 1080.0f / 943.0f;  // touchpad y unit -> screen y unit
        Out o;
        if (!want && !on) return o;
        if (lift) { lift = on = false; return o; }  // finger up for one read
        if (!want) { on = false; return o; }
        if (!on) {  // touch down at the pad centre: the first read of a touch has no delta
            on = true, fx = 960, fy = 471, ++id, px = py = qx = qy = ux = uy = 0, lvalid = false;
            return finger(o);
        }
        bool fresh = have && rseq != seq;
        if (have && rseq == seq && ++stale > 8) have = false;  // the cursor is not updating (menu): dead reckoning
        if (fresh) seq = rseq, stale = 0;
        if (!have) cx = ex - px, cy = ey - py, fresh = true, lvalid = false;
        if (!fresh) return finger(o);
        // Gain estimate: the reading moved by k * (step before last), unless near the clamp (+-960 / +-540).
        if (lvalid && ux * ux + uy * uy > 30 * 30 && std::fabs(cx) < 940 && std::fabs(cy) < 520) {
            float kobs = ((cx - lx) * ux + (cy - ly) * uy) / (ux * ux + uy * uy);
            if (kobs > 0.1f && kobs < 3.0f) k += 0.6f * (kobs - k);
        }
        lx = cx, ly = cy, lvalid = have;
        float dx = tx - (cx + px), dy = ty - (cy + py);
        ex = cx + px, ey = cy + py;
        ux = qx, uy = qy;  // the in-flight step becomes the step before last
        bool arrived = std::fabs(tx - cx) < 8 && std::fabs(ty - cy) < 8;
        if (!held) {
            if (wait > 0) --wait;
            if (tap == 1 && (arrived || wait == 0)) tap = 2;
            if (tap >= 2) {  // press for 4 reads, then one read with the finger still on, then lift
                ++tap;
                o.button = tap <= 6;
                if (tap > 7) { want = false, tap = 0, lift = true; return o; }
                px = py = qx = qy = 0;
                return finger(o);
            }
            if (!tap && (arrived || wait == 0)) { want = false, lift = true; return o; }
        }
        // Finger step for gain * error, kept inside the pad; at the pad edge lift and re-touch at the centre.
        bool edge = false;
        auto limit = [&](float f, float d, float lo, float hi) {
            float n = f + d;
            if (n < lo) edge |= f - lo < 32, n = lo;
            if (n > hi) edge |= hi - f < 32, n = hi;
            return n;
        };
        float nfx = limit(fx, gain * dx / k, 64, 1856), nfy = limit(fy, gain * dy / (k * kY), 48, 895);
        if (edge) { lift = true, px = py = qx = qy = 0; return o; }
        qx = nfx - fx, qy = (nfy - fy) * kY;
        px = qx * k, py = qy * k;
        fx = nfx, fy = nfy;
        ex += px, ey += py;
        return finger(o);
    }

private:
    Out finger(Out o) const {
        o.finger = true, o.x = uint16_t(fx), o.y = uint16_t(fy), o.id = id;
        return o;
    }
};

}  // namespace dcp::touch
