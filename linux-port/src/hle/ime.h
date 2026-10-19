// libSceImeDialog, the PS4 system keyboard: the game opens it to name a hero and to rename an item bag
// (FUN_00080400 opens it, FUN_00080760 polls it). On PC the host draws its own text box: the render loop takes the
// open request, shows it and hands the result back. With no host (headless runs) the dialog finishes at once, OK,
// with the text unchanged.
#pragma once
#include <cstdint>
#include <string>

namespace dcp::hle {

struct ImeRequest {
    uint64_t id = 0;     // changes with every sceImeDialogInit
    std::string title;   // UTF-8 (may be empty)
    std::string text;    // UTF-8, the current name
    uint32_t max_len = 0;  // in UTF-16 units, as the game counts
};

void ime_set_host(bool present);  // call before the game thread starts
bool ime_pending(ImeRequest* out);  // the dialog the game is waiting on, if any
// text: UTF-8, nullptr = cancelled (the game keeps the old name). Ignored if `id` is no longer the open dialog.
void ime_finish(uint64_t id, const std::string* text);

}  // namespace dcp::hle
