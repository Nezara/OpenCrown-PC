// Shared by dcp_run and the launcher: the default input.ini (binding names, order and format) and the per-user
// settings directory. Header-only so both targets stay in lockstep without a library.
#pragma once
#include <cstdlib>
#include <string>

namespace dcp {

// Default input.ini. "action = input, input": SDL key names (SDL_GetScancodeFromName: W, Space, Left Shift, Escape,
// Up, ...) or Mouse1..Mouse5 (left, right, middle, back, forward). Empty value = unbound.
inline constexpr const char* kInputDefaults = R"(# Dragon's Crown Pro keyboard/mouse bindings (player 1). Delete this file to restore defaults.
# action = input[, input...]   inputs: SDL key names (W, Space, Left Shift, Escape, Up, F1...) or Mouse1..Mouse5
move_up = W
move_down = S
move_left = A
move_right = D
square = Mouse1
circle = Mouse2
triangle = E
cross = Space
l1 = Left Shift
r1 = Left Shift
l2 = Z
r2 = C
l3 = Left Ctrl
r3 = V
options = Escape
touchpad = Tab
# town map: touchpad click with a finger on the left half of the pad (the plain touchpad click opens the character menu)
town_map = M
dpad_up = Up
dpad_down = Down
dpad_left = Left
dpad_right = Right
look_up = I
look_down = K
look_left = J
look_right = L
# pointer_tap = R1 (the right-stick pointer's interact: doors, chests) while a look key is held or for 2 s after;
# replaces that key's normal button in that window.
pointer_tap = E
# Fairy pointer: mouse movement steers the fairy 1:1 (cursor captured) while fairy mode is on; fairy_speed scales it
# (assumes the game's default touch sensitivity; if you change that option in game, adjust fairy_speed).
# fairy = hold for fairy mode (mouse attacks off while held); fairy_toggle = optional on/off key (unbound by default);
# fairy_tap = touchpad tap (interact: doors, chests) in fairy mode, replaces that key's normal button while it is on.
fairy = Mouse3
fairy_toggle =
fairy_tap = E
fairy_speed = 1.0
# touch_screen = on: touching the screen sends the fairy there, a short tap also interacts (doors, chests).
touch_screen = on
# deck_trackpads = on (Steam Deck): right trackpad = fairy (click = interact), left trackpad click = town map. With
# Steam Input on, the right trackpad's mouse steers the fairy while the controller is in use.
deck_trackpads = on
# touch_as_mouse: Steam Deck Game Mode turns touches into left clicks unless the shortcut uses Steam's "Touchscreen
# Native Support". auto = those clicks steer the fairy like touches in Game Mode, on = always, off = never.
touch_as_mouse = auto
# mouse_attacks: mouse buttons press game buttons (Mouse1 = square ...). auto = yes, except on a Steam Deck, where the
# controller is the main input and clicks (also touches turned into clicks) never attack; keys still work. on / off.
mouse_attacks = auto
)";

// Per-user settings dir (input.ini, launcher.ini, saves/, logs/): $DCP_USER_DIR if set, else
// ${XDG_DATA_HOME:-~/.local/share}/OpenCrown (the same dir as net::default_user_dir()).
inline std::string user_dir() {
    if (const char* d = std::getenv("DCP_USER_DIR"); d && *d) return d;
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x == '/') return std::string(x) + "/OpenCrown";
    const char* home = std::getenv("HOME");
    return std::string(home && *home ? home : ".") + "/.local/share/OpenCrown";
}

}  // namespace dcp
