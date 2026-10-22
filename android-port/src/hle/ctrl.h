// Host -> SceCtrl: the platform layer reports the pad state here (keyboard / mouse / gamepad mapped by it).
#pragma once
#include <cstdint>
#include <string>

namespace hle {

// SceCtrlButtons bits (vitasdk psp2/ctrl.h).
enum : uint32_t {
  CTRL_SELECT = 0x1,
  CTRL_START = 0x8,
  CTRL_UP = 0x10,
  CTRL_RIGHT = 0x20,
  CTRL_DOWN = 0x40,
  CTRL_LEFT = 0x80,
  CTRL_L = 0x100,  // SCE_CTRL_LTRIGGER: the Vita's L
  CTRL_R = 0x200,  // SCE_CTRL_RTRIGGER: the Vita's R
  CTRL_TRIANGLE = 0x1000,
  CTRL_CIRCLE = 0x2000,
  CTRL_CROSS = 0x4000,
  CTRL_SQUARE = 0x8000,
};

struct PadState {
  uint32_t buttons = 0;
  uint8_t lx = 128, ly = 128, rx = 128, ry = 128;  // 0..255, 128 = centre
};

void ctrl_set(const PadState& s);

// Scripted pad (mobile_run --input <file>): lines `<flip> <BUTTON|BUTTON|...|0>` (the Pro runner's format; # comments),
// each state held from that flip (sceDisplaySetFrameBuf count, display_flip_count) until the next line. Names: SELECT,
// START (OPTIONS), UP, RIGHT, DOWN, LEFT, L (L1), R (R1), TRIANGLE, CIRCLE, CROSS, SQUARE, LS_LEFT / LS_RIGHT / LS_UP /
// LS_DOWN (left stick held to that edge), or a number (button bits). While a script is loaded it replaces live pad
// input. False with a reason on a malformed line.
bool ctrl_load_script(const std::string& text, std::string& err);
// Flips so far (sceDisplaySetFrameBuf calls; display.cpp): the script clock, = the DCP_SCREENSHOT_FRAME numbering.
uint64_t display_flip_count();

}  // namespace hle
