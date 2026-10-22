// Host <-> SceImeDialog: the game's text-entry dialog (hero name, saved chat messages) answered by a host UI.
// The platform layer (a window) registers itself, polls for an open request and answers it from its event thread;
// without a host (headless runs) the dialog answers its initial text at once, as before. All calls are thread-safe.
#pragma once
#include <cstdint>
#include <string>

namespace hle {

struct ImeRequest {
  uint64_t id = 0;            // new for every sceImeDialogInit
  std::u16string title, text;  // UTF-16 as the game passed them; text = the initial text
  uint32_t max_length = 0;     // UTF-16 units, without the terminator
  uint32_t type = 0, option = 0;  // SceImeType / SceImeOption bits (the game: DEFAULT, 0)
  bool cancelable = true;         // dialogMode WITH_CANCEL
};

// true: a host UI answers text entry (window builds); false (default): the initial text is entered at once.
void ime_set_host(bool present);
// The open, unanswered request, if any (false once answered, aborted by the game or terminated).
bool ime_poll(ImeRequest& out);
// Answers request `id`: ok = ENTER with `text` (cut to max_length), else cancelled (the initial text stays).
void ime_answer(uint64_t id, bool ok, const std::u16string& text);

}  // namespace hle
