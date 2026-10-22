// Touch screen / Steam Deck input logic (tools/dcp_run/touch_math.h) against a model of the game's relative cursor,
// and the touch-as-mouse rule (Steam Deck Game Mode).
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "../tools/dcp_run/touch_math.h"

using namespace dcp::touch;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), ++g_fail; \
    } while (0)

static bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

// The game per frame (notes/map/fairy_cursor.md): pad read -> Cursor_Update (hook samples the position at its entry,
// then applies the held finger's delta * k; first read of a touch and lifts give no delta), clamp +-960 / +-540.
struct Game {
    float k = 0.6f, x = 0, y = 0;
    bool prev_on = false;
    float prev_fx = 0, prev_fy = 0;
    float sx = 0, sy = 0;
    uint32_t seq = 0;
    bool button_was = false;
    int presses = 0;
    TouchLoop::Out frame(TouchLoop& t) {
        TouchLoop::Out o = t.read(seq != 0, sx, sy, seq);
        sx = x, sy = y, ++seq;  // hook at Cursor_Update entry
        if (o.finger && prev_on) {
            x += (o.x - prev_fx) * k;
            y += (o.y - prev_fy) * (1080.0f / 943.0f) * k;
        }
        prev_on = o.finger, prev_fx = o.x, prev_fy = o.y;
        x = std::fmax(-960.0f, std::fmin(960.0f, x)), y = std::fmax(-540.0f, std::fmin(540.0f, y));
        if (o.button && !button_was) ++presses;
        button_was = o.button;
        return o;
    }
};

// Frames until the cursor stays within 8 px of the target; -1 if never within max frames.
static int converge(Game& g, TouchLoop& t, float tx, float ty, int max, float* err) {
    t.press(tx, ty);
    int first = -1;
    for (int i = 0; i < max; ++i) {
        g.frame(t);
        bool in = std::fabs(g.x - tx) < 8 && std::fabs(g.y - ty) < 8;
        if (in && first < 0) first = i + 1;
        if (!in) first = -1;
    }
    *err = std::hypot(g.x - tx, g.y - ty);
    return first;
}

int main() {
    float gx, gy;
    window_to_game(640, 400, 1280, 800, &gx, &gy);  // Steam Deck 16:10: bars of 40 px top and bottom
    CHECK(near(gx, 0) && near(gy, 0));
    window_to_game(0, 40, 1280, 800, &gx, &gy);
    CHECK(near(gx, -960) && near(gy, -540));
    window_to_game(1280, 0, 1280, 800, &gx, &gy);  // on the bar: clamped to the picture edge
    CHECK(near(gx, 960) && near(gy, -540));
    window_to_game(1000, 360, 2000, 720, &gx, &gy);  // pillarbox 1280x720 picture at x 360
    CHECK(near(gx, 0) && near(gy, 0));
    window_to_game(360 + 320, 180, 2000, 720, &gx, &gy);
    CHECK(near(gx, -480) && near(gy, -270));

    auto r = route_touchpads(1, true);  // DualShock 4 / DualSense: unchanged
    CHECK(r.pad == 0 && !r.touchpad_btn_is_map && !r.misc2_is_touchpad);
    r = route_touchpads(2, true);  // Steam Deck (HIDAPI): right pad, left click = map, right click = touchpad
    CHECK(r.pad == 1 && r.touchpad_btn_is_map && r.misc2_is_touchpad);
    r = route_touchpads(2, false);  // deck_trackpads = off: the old behaviour (pad 0, its click)
    CHECK(r.pad == 0 && !r.touchpad_btn_is_map && !r.misc2_is_touchpad);
    CHECK(route_touchpads(0, true).pad == -1);

    CHECK(is_steam_virtual_pad(0x28de, 0x11ff));
    CHECK(!is_steam_virtual_pad(0x28de, 0x1205));  // the Deck's own controller (Steam Input off) is not the virtual pad
    CHECK(!is_steam_virtual_pad(0x054c, 0x09cc));
    CHECK(steam_mouse_mode(true, true, false));
    CHECK(!steam_mouse_mode(true, true, true));   // keyboard/mouse in use: the mouse is a mouse
    CHECK(!steam_mouse_mode(false, true, false));
    CHECK(!steam_mouse_mode(true, false, false));

    // Closed loop: targets from the centre, a corner region and a far edge; also with a different in-game touch
    // sensitivity (k 0.4 / 1.0 / 2.0) than the assumed 0.6.
    const float targets[][2] = {{0, 0}, {-700, -380}, {940, 100}, {-200, 520}};
    for (float k : {0.6f, 0.4f, 1.0f, 2.0f}) {
        Game g;
        g.k = k, g.x = 300, g.y = -200;
        TouchLoop t;
        for (auto& tg : targets) {
            float err;
            int n = converge(g, t, tg[0], tg[1], 120, &err);
            std::printf("k %.1f target (%.0f, %.0f): %d frames to 8 px, final error %.2f px\n", k, tg[0], tg[1], n, err);
            CHECK(n > 0 && n <= (k == 2.0f ? 60 : 40) && err < 4);
            t.release(false);
            for (int i = 0; i < 60; ++i) g.frame(t);
            CHECK(!t.active());
        }
    }

    // Tap: the fairy flies there, then exactly one touchpad press while the finger is on, then the finger lifts.
    {
        Game g;
        TouchLoop t;
        t.press(400, 200);
        t.release(true);
        int arrived_at = -1, press_at = -1;
        for (int i = 0; i < 90; ++i) {
            auto o = g.frame(t);
            if (arrived_at < 0 && std::fabs(g.x - 400) < 8 && std::fabs(g.y - 200) < 8) arrived_at = i;
            if (o.button) {
                CHECK(o.finger);
                if (press_at < 0) press_at = i;
            }
        }
        std::printf("tap: arrived frame %d, press frame %d, presses %d\n", arrived_at, press_at, g.presses);
        CHECK(g.presses == 1 && press_at >= arrived_at && arrived_at >= 0 && !t.active());
    }
    // No cursor readings (hook off / menu): dead reckoning still moves the finger toward the target and finishes.
    {
        TouchLoop t;
        t.press(300, 0);
        t.release(false);
        float fx0 = -1;
        for (int i = 0; i < 80; ++i) {
            auto o = t.read(false, 0, 0, 0);
            if (o.finger && fx0 < 0) fx0 = o.x;
        }
        CHECK(fx0 == 960 && !t.active());
    }
    // input.ini auto/on/off values and the touch-as-mouse rule.
    {
        CHECK(parse_tri("auto") == Tri::Auto && parse_tri("On") == Tri::On && parse_tri("OFF") == Tri::Off &&
              parse_tri("0") == Tri::Off && parse_tri("junk") == Tri::Auto);
        CHECK(mouse_is_touch(Tri::Auto, true, false, false));    // Game Mode, no native touch yet
        CHECK(!mouse_is_touch(Tri::Auto, true, true, false));    // native touches arrive: clicks are clicks again
        CHECK(!mouse_is_touch(Tri::Auto, false, false, false));  // desktop
        CHECK(!mouse_is_touch(Tri::On, true, false, true));      // mouse captured for the fairy
        CHECK(mouse_is_touch(Tri::On, false, true, false) && !mouse_is_touch(Tri::Off, true, false, false));
    }
    std::printf(g_fail ? "test_touch: %d FAILED\n" : "test_touch: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
