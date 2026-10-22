# Vita 1.09 text entry: SceImeDialog (asm-checked)

Target `Work/vita/update109.elf`. Addresses are 1.09 Thumb VAs; `f<addr>` = function start. Tools: `Work/vita/dis_thumb.py`,
`notes/raw/vita_net_109/v9.py` (`imp`, `r`, `fn`, `callers`, `xr`). HLE: `android-port/src/hle/dialog.cpp` (IME section),
host bridge `src/hle/ime.h`, UI `src/platform/text_entry.*`.

## 1. Where the game asks for text
Only SceImeDialog (via SceCommonDialog); no SceIme imports. One wrapper object ("ime box", below), opened through
`f8103ea3e(box, maxLen, initial u16string*, title std::string*)` (sets `box+0x50 = maxLen`, copies the initial text into
`box+0x58`, then calls the opener `f810749a8`). Two callers:

| caller | max | title | what | result use |
|---|---|---|---|---|
| f812ae7f6 (call 812aef96) | 30 (0x1e) | "Enter your message." | the 4 saved chat messages on the Create / Edit character screen (Greeting / Selection / Resurrection / Dying, slot `fp+0x2f4` = 3..6 -> `f8137c460` / `f8137c6e2` / `f8137c610` / `f8137c53a`) | result 0 only: text copied, every `%` (0x25) replaced by U+FF05 (812af04e) |
| f8131003c state 0x2b (call 81310462) | 10 (0xa) | message id 0x4ed | entered from state 1 (81310384) when `f814dc89c(r6, 3)` && `[r8+0xb24] == 0` && input action 7 (`f8103e40a(.., 7)`); writes through `f810761bc` | result 0 only, same `%` swap (813104b2) |

Not text entry through the IME: the hero **name** on the Create character screen is the game's own letter grid
(Edit name -> grid; SELECT "Edit type" there = a per-letter style editor). In our runs (empty save, Fighter) action 7
in state 1 of f8131003c was never triggered by Triangle / Square / L / R / Select / Start on the empty player list,
so the 10-character dialog's trigger is **unconfirmed** (inferred: needs an existing character in the list).

## 2. Opener f810749a8 (box, initial u16string*, title std::string*)
* `sceAppUtilSystemParamGetInt(1 = LANG, &lang)` (810749e0); non-zero -> return without opening.
* Frees / allocates the result buffer `box+0xb0` = `(max + 1) * 2` bytes (810749fa..81074a00).
* `memset(param, 0, 0xa8)` (81074a14), then writes (param = `fp+0x1b0`):

| off | value | field (SDK name) |
|---|---|---|
| 0x00 | 0x03570071 | sdkVersion |
| 0x08/0x0c | u64 language mask from the game language `[f81017cf4()+0xc0]` (tbb 81074a54): 0 -> 0x2000, 3 -> 0x10, 4 -> 0x8, 5 -> 0x2, 6 -> 0x20, 7 -> 0x4000, 8 -> 0x10000, 9 -> 0x8000, else 0x40000 if system LANG == 0x12 else 0x4 | supportedLanguages |
| 0x10 | 0 | languagesForced |
| 0x14 | 0 (memset) | type = DEFAULT |
| 0x18 | 0 | option |
| 0x1c | 0 (memset) | filter* = none |
| 0x20 | 1 | dialogMode = WITH_CANCEL |
| 0x24 | 0 | textBoxMode = DEFAULT |
| 0x28 | `box+0x94` (title converted to UTF-16 into the u16string at `box+0x90`, 81074b4e / 81074b58), only if a title was passed | title* |
| 0x2c | `[box+0x50]` | maxTextLength |
| 0x30 | `box+0x78` (u16string at `box+0x74`, SSO < 8), only if an initial text was passed | initialText* |
| 0x34 | `[box+0xb0]` | inputTextBuffer* |
| 0x38..0x84 | commonParam (0x4c): magic at 0x80 = `&commonParam + 0xc0d1a109` (81074a18..81074a26) | |
| 0x84.. | 0 | enterLabel + reserved (struct size 0xa8) |

* `sceImeDialogInit(param)` (81074b7c); 0 -> `box+0xac = 1` (running).

## 3. Poll f81074bc0 (each frame while `box+0xac`)
* `sceImeDialogGetStatus() == 2` (FINISHED) -> `box+0xac = 0`, `sceImeDialogGetResult(&r)` with r = 0x24 bytes zeroed.
* Only `r.result` (offset 0) is read: 0 -> `box+0x70 = 0`, 1 -> 1, 2 -> 2 (81074c0c..81074c34). `r.button` is ignored.
* `sceImeDialogTerm()` (81074c38), then **whatever the result** copies the NUL-terminated UTF-16 `inputTextBuffer` into
  the result u16string `box+0x58` (81074c4a..81074de8) and frees the buffer. Callers check `box+0x70 == 0` before using it,
  so the HLE writes the initial text back on cancel / abort.
* Other box methods: `f81074ba4` = abort (`GetStatus() == 1` -> `sceImeDialogAbort`), `f81074bb4` = "not running"
  (`!box+0xac`). `sceCommonDialogUpdate` has a single caller (813b72f0 in f813b72aa), not specific to the IME.

## 4. HLE (android-port)
* Init records title / initial / max (capped at 2048) / type / option / dialogMode; with a window the dialog stays RUNNING
  until `hle::ime_answer` (event thread, under the dialog lock); headless it finishes on the 2nd status poll with the
  initial text ("Player" when empty), as before.
* On FINISHED, GetStatus writes the text + NUL into inputTextBuffer once; GetResult writes `{result, button}` = OK `{0, 2}`,
  cancel `{1, 1}`, abort `{2, 0}`. Abort sets the initial text and FINISHED; the host sees the request go away.
* Checked on Windows (2026-10-22): Create character -> Greeting message opens the dialog ("Enter your message.", max 30);
  entered text shows in the row and comes back as the initial text on the next open; 30-unit cut; cancel keeps the old
  text; non-ASCII (`Grüße aus Köln`, `日本語 ok été`) reaches the game and is drawn by its font.
