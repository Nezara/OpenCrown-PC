#pragma once
// Japanese button layout on the 1.09 eboot: Circle confirms, Cross cancels in every menu (notes/map/confirm_cancel.md).
// The action table builder (0x81073ed0) reads DcCore+0xc4: 0 = Circle decides. The constructor zeroes it and the
// startup step stores 1 (0x8101841c); with $DCP_CONFIRM=circle (launcher: Controls page, --confirm-circle) that store
// is NOPed. Prompt icons follow the flag. The player controller's "cancel" command bit (0x811f087a) is moved to
// action 0x1d (Circle), so combat stays as it was.

namespace guest {
class Memory;
}

namespace hle {

void confirm_button_install(guest::Memory& mem);  // no-op unless $DCP_CONFIRM=circle; all or nothing on unexpected bytes

}  // namespace hle
