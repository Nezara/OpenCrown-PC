// Story skip and tutorial skip questions for every new character, the 1.09 eboot's own dialogs and branches
// (notes/map/vita_story_skip_109.md; the PC port's port/src/hle/skip_offers.h does the same on the PS4 eboot).
// Always on, like the PC port; each one is turned off by its environment variable set to "0" (on Android also
// through `adb shell setprop debug.opencrown.env "DCP_STORY_SKIP=0"`):
//   DCP_STORY_SKIP     on confirming a new character the game asks (SystemMsg 0x3cc) whether to start from the
//                      talisman collecting at level 15 (story counter 0x2d). Normally only once a class on the file
//                      has cleared the game: the cbz at 0x812aebbc is NOPed. The new-hero test at 0x812aebba stays,
//                      so editing an advanced hero never gets the offer.
//   DCP_TUTORIAL_SKIP  "Begin the tutorial?" (SystemMsg 0x5d8) is normally not asked for the first character of a
//                      file (the tutorial starts by itself): the bne at 0x812abda8 becomes an unconditional b.
//   DCP_SKIP_WARNING   (with the story skip) the first "no" asks again, the question text replaced by a warning that
//                      the character cannot play online until the first half of the story is finished (online
//                      needs story counter >= 0x2d, which the skip sets); the second answer is final.
// Every site is checked against its expected bytes first; a mismatch (another eboot) leaves the game code untouched
// and logs it. DCP_SKIP_DUMP=1 logs the stub bytes.
#pragma once

namespace guest {
class Memory;
}

namespace hle {

// Before any guest code runs (the JIT translates the patched bytes on first use).
void skip_offers_install(guest::Memory& mem);

}  // namespace hle
