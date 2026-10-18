// Story skip and tutorial skip questions for every new character (notes/map/story_progress.md 2.3, 2.4, 3, 7).
// Both are the game's own dialogs and branches; we only remove the conditions that hide them. Each is set
// separately by an environment variable, on unless "0" (no launcher option; the manual switch):
//   DCP_STORY_SKIP     on confirming a new character the game asks (SystemMsg 0x3cc) whether to start from the
//                      talisman collecting at level 15 (story counter 0x2d, stable unlocked). Normally only once
//                      a class on the file has cleared the game: the jne at 0x3aeda9 is NOPed. The new-hero test
//                      at 0x3aed91 stays, so editing an advanced hero never gets the offer.
//   DCP_TUTORIAL_SKIP  "Begin the tutorial?" (SystemMsg 0x5d8) is normally skipped for the first character of a
//                      file (the tutorial starts by itself): the je at 0x3a8adb (FUN_00477f80 false) is NOPed.
//   DCP_SKIP_WARNING   (with the story skip) the first "no" asks again, the question text replaced by a warning that
//                      the character cannot play online until the first half of the story is finished (online
//                      needs story counter >= 0x2d, which the skip sets); the second answer is final.
#pragma once
#include <cstdint>

namespace dcp::skip_offers {

void install(uint8_t* eboot_base);  // each patch is skipped (and logged) if its expected bytes are not there

}  // namespace dcp::skip_offers
