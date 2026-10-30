// Japanese button layout: Circle confirms, Cross cancels in every menu (notes/map/confirm_cancel.md).
// The game has both layouts: its action table builder reads DcCore+0x134 (0 = Circle decides, else Cross), and the
// startup step stores 1 there on every path. DCP_CONFIRM=circle (launcher: Controls page) stores 0 instead; the
// prompt icons follow the same flag. Combat keeps its own fixed actions, except the player controller's "cancel"
// command bit, which is moved to action 0x1d (Circle) so it stays where it was.
#pragma once
#include <cstdint>

namespace dcp::confirm_button {

void install(uint8_t* eboot_base);  // no-op unless DCP_CONFIRM=circle; skipped (and logged) on unexpected bytes

}  // namespace dcp::confirm_button
