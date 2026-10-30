# Confirm / cancel buttons ("Japanese layout", issue #7)

Mapper notes. PS4 addresses are eboot-relative (q.py); Vita addresses are 1.09 Thumb VAs (`Work/vita/update109.elf`,
`notes/raw/vita_net_109/v9.py`). "(unverified)" = inferred, not read in the asm. Raw dumps: `notes/raw/confirm_cancel/`.
This corrects `input_save.md` section 4: the game does not hard-wire Cross = confirm. It has its own JP/Western switch
and forces it to Western at startup.

## 1. Summary
- **The game already has a Japanese layout.** All menu input goes through a per-player **action table** (43 actions,
  each a list of up to 4 pad-bit indices). The builder (PS4 **FUN_0007ee00**, Vita **f81073ed0**) picks the buttons
  for action 0 (**decide**), action 1 (**cancel**) and action 5 (a second decide action) from one global flag:
  **DcCore+0x134** (PS4) / **DcCore+0xc4** (Vita). Flag == 0 -> decide = Circle, cancel = Cross (JP).
  Flag != 0 -> decide = Cross, cancel = Circle (Western).
- The DcCore constructor zeroes the flag. The startup step that reads the system language then sets it to **1** on
  every path, whatever the language or the return value (PS4 0x176c0 in FUN_00016da0, and 0x177da in FUN_00017750;
  Vita 0x8101841c in f810182ce). Neither build reads the system enter-button setting: PS4 never calls
  `sceSystemServiceParamGetInt(1000)`, and Vita calls `sceAppUtilSystemParamGetInt` once (0x810749e0), with id 1 =
  LANG, inside the IME opener. Our HLE return values for those ids do nothing.
- **Gameplay is separate.** Combat uses its own actions with fixed buttons that the flag never touches: 0x18/0x19 =
  Square, 0x1a = Cross (+R1), 0x1b = Triangle, 0x1c = R2, 0x1d = Circle, 0x16/0x17 = L1/R1. So the flag is a
  menus-only switch, with **one gameplay leak** (section 4).
- **Prompts stay correct.** The same flag picks the prompt icons (PS4 FUN_0006b1a0: prompt 0 and 0x15 = decide icon,
  prompt 1 = cancel icon, icon ids 0x12 / 0x18 swapped) and the UI panels with baked-in button art (PS4 FUN_004f3580,
  FUN_004f41c0, FUN_00508100: motion `base + 2*i + (flag != 0)`; Vita readers listed in section 2).
- **Rating: routine.** One global int, set at startup, plus one 1-byte fix for the gameplay leak. Recommended hooks are
  in section 4.

## 2. Function table
PS4:
| addr | proposed name | role | conf |
|---|---|---|---|
| 0x0007ee00 | InputMap_Build | Fills the action->button table; reads DcCore+0x134 at 0x7ee4b (`cmp [rax+0x134],0`). If 0: act0/act5 = btn 5 (Circle), act1 = btn 6 (Cross); else swapped. | high (asm) |
| 0x0007ed90 | InputMap_RebuildIfDirty | If input+0x8 != 0: clear it, clear the table (0x43cc0), rebuild (0x7ee00). Called every tick from 0x44190. | high |
| 0x0007edd0 | InputMap_Rebuild | Forced rebuild. Callers 0x44fc0, 0x45810, 0x8c2f0 (save-option change, unverified), 0x503fc0 (options menu, unverified). | high / role med |
| 0x00043cc0 | InputMap_Clear | All 43 actions -> sentinel 0x18 (no button), count 0 | high |
| 0x00043ac0 | Input_Init | Per player: pad index at +0xc, dirty +0x8 = -1, autorepeat config in the pad struct | high |
| 0x00043f30 | Input_CopyPad | 24 buttons x 5 state bytes from the game pad struct (held / pressed / released / decay / repeat) | high |
| 0x00044190 | Input_Update | Per tick: copy pad, rebuild if dirty, per-action edge bookkeeping | high |
| 0x00044930 | Action_Held | | high |
| 0x00044a80 | Action_Pressed | | high |
| 0x00044f20 | Action_PressedOnce | Pressed, consumed via +0xb2 per action | high |
| 0x00044dc0 | Action_Repeat | Pressed or autorepeat | high |
| 0x00044020 / 0x00044ba0 | Action_Released / Action_Decay | | med |
| 0x00044ef0 / 0x00044d40 | Action_HeldNotConsumed / Action_ReleasedAfterHold(min,max) | Used by attack charge | med |
| 0x00045040 | InputSet_ctor | 4 x 0x920 per-player input objects (vtable 0x8c3520) | high |
| 0x00045a40 | DcInputMsg_ctor | Task 'DcInputMsg', created by DcCore startup and stored at DcCore+0xa0 | med |
| 0x0007d950 | Owner_GetInput(owner, player) | `owner+0x110` -> base + player*0x920 (85 callers) | high |
| 0x00016b10 | DcCore_Get | Returns the pointer stored at 0xc17758 (179 callers) | high |
| 0x00016da0 / 0x00017750 | DcCore_Startup / DcCore_ReadLanguage | Write DcCore+0x130 (language) and **+0x134 = 1** (0x176c0 / 0x177da). 0x17750 has no code or reloc refs (dead?). | high |
| 0x0006b1a0 | Prompt_IconForAction | Prompt index -> icon id; decide/cancel icons swap on +0x134; other icons swap on a save option bit | high |
| 0x004f3580 / 0x004f41c0 / 0x00508100 | UI panel setup | Pick the JP or Western art motion by +0x134 | med |
| 0x00484880 / 0x00484900 | UiWidget::Decide / UiWidget::Cancel | Virtual slot +0x118 / +0x120 in about 39 UI vtables: act 0 / act 1 (pressed-once, or repeat if widget flag +0x967&1) | high |
| 0x002f25e0 | PlayerCtrl_ReadActions | Gameplay controller (acts 0x16..0x22). **Also queries act 1** at 0x2f394b -> sets the character command bit 0x40 in +0xb14 | high |
| 0x002f2020 | Char_OnCancelCmd | Virtual in 7 character vtables: if cmd bit 0x40 is set and the character has a linked object at +0x1e0 (whose +0x2a10 & 0x800 is clear), sends event 0x1427 to itself | high (code) / meaning unverified |
| 0x005a6550 / 0x005a65b0 | FnDbMenu any-pad Circle / Cross | Debug menu only (vtable 0xbc0d88, RTTI 'FnDbMenu'). The only raw 0x20/0x40 pad tests in the eboot. | high |

Vita 1.09 (same design: action stride 0x28, sentinel 0x1a, button index = SceCtrl bit, Circle = 13, Cross = 14):
| addr | role | conf |
|---|---|---|
| f81073ed0 | InputMap_Build. 0x81073ee6 loads DcCore+0xc4. If 0: act0 (+0xa8) = 13 Circle, act1 (+0xd0) = 14 Cross, act5 (+0x170) = 13; else swapped. act 0x1a (+0x4b8) = 14 Cross, act 0x1d (+0x530) = 13 Circle (unconditional). | high (asm) |
| f81074850 | InputMap_RebuildIfDirty (dirty flag at input+0x4) | high |
| f8103dad2 | Input_Update | high |
| f8103d3a2 / f8103d512 / f8103e40a / f8103e3f0 / f8103d726 | Action_Held / Pressed / PressedOnce / HeldNotConsumed / ReleasedAfterHold | high |
| f81017cf4 | DcCore_Get (`*(u32*)0x8177e01c`) | high |
| f81018106 | DcCore_ctor: +0xc4 = 0 (0x81018186) | high |
| f810182ce | DcCore startup: +0xc0 = 1 (language), **+0xc4 = 1** (0x8101841c, same r1 register as the language store) | high |
| 0x810180fa | Tiny setter: +0xc0 = +0xc4 = 1. No call or pointer refs found (dead?). | med |
| f8105d194 | Prompt icon chooser (reads +0xc4; PS4 0x6b1a0 analogue) | med |
| f81329054, f81329074, f813296be, f8133742c | Other +0xc4 readers (UI art variants, unverified) | med |
| f811ef750 | PlayerCtrl_ReadActions. Act 1 query at 0x811f087a (`movs r1,#1`) -> f811ef6ca(.., 0x26) = the same cancel command bit (word 2, bit 6) | high |

## 3. Data structures
**Per-player input object** (PS4 0x920 bytes, 4 in a row; Vita: action table at +0xa8, stride 0x28):
| off (PS4) | meaning |
|---|---|
| +0x08 | dirty / rebuild request (int; -1 after init). While non-zero the focus check below is skipped. |
| +0x0c | pad slot index |
| +0x28 + b*5 | button b (0..23): held, pressed, released, decay, repeat (bytes) |
| +0xa0 + a*0x30 | action a (0..0x2a): int btn[4] (0x18 = none), +0x11 disabled, +0x12 consumed (pressed-once), +0x13 blocked, +0x14 "all buttons required", +0x15 count, +0x28 pointer to an optional virtual source (non-null: its bytes +0x20/+0x21/+0x22 replace held/released/pressed) |
| +0x8b0.. | stick axis selectors |
| +0x8dd | "inside update" guard |
| +0x8de | when set, the decide button is also appended to act 0x29 (set via 0x44fc0) |
| +0x900 | owner back-pointer |

**Focus gate** (in every Action_* query, PS4): if `owner->+0x110 (input set) ->+0x2608` (an "exclusive input owner"
task) is set, every action except 0x29/0x2a reads false unless one of the engine's 5 current-task slots
(engine+0x50 -> +0x20..+0x40) is that owner. It is cleared when the owner's state +0x54 == 2 or a countdown at
+0x2610 runs out (0x45530). Not needed for the hook, but this is the "a UI layer has input focus" state if one is
ever wanted (role unverified).

**Action -> button (PS4 bit index; Western = flag != 0)**: 0 decide = 6 Cross (JP: 5 Circle). 1 cancel = 5 (JP: 6).
2 = 4 Triangle. 3 = 8 Options. 4 = 4. 5 = like act 0. 6/7 = 7 Square. 8 = 8. 9 = 9 Touchpad. 10..17 = d-pad / left
stick (order swapped by a save option bit). 0x18/0x19 = Square. 0x1a = Cross + R1. 0x1b = Triangle. 0x1c = R2.
0x1d = Circle. 0x22..0x28 = stick / L1 / R1 variants. 0x29 = L1 + R3 (+ decide when +0x8de). 0x2a = Touchpad.
Decide/cancel callers: act 0 = 28 call sites in 24 functions, act 1 = 33 sites in 26 functions (PS4). Vita: 28 / 29.
Act 5 has no call site with an immediate id in either build.

**Flag**: PS4 `DcCore+0x134` (int), DcCore = `*(u64*)0xc17758`. Vita `DcCore+0xc4`, DcCore = `*(u32*)0x8177e01c`.
0 = JP (Circle decides), non-zero = Western.

## 4. Recommended hooks
**PS4 (port/):**
1. Set the flag to 0 when the option is on. Either change the imm32 of the two startup stores to 0 (imm at
   **0x176c8**, instruction 0x176c0, and at **0x177e0**, instruction 0x177da), or have the host write
   `*(int*)(DcCore+0x134) = 0` once DcCore startup step 0 has run. The first keymap build is lazy (dirty = -1, first
   Input_Update after DcInputMsg exists), so either way it lands before any build. The UI art panels read the flag
   when they are built, so a startup-time setting keeps them consistent.
   Changing it while running: write the flag, then set +0x8 = 1 on each of the 4 input objects (InputMap_RebuildIfDirty
   rebuilds on the next tick). Panels already built keep their old art until rebuilt (unverified how long they live).
2. Plug the gameplay leak. In PlayerCtrl_ReadActions, change the 5-byte `mov esi, 1` at **0x2f394b** to
   `mov esi, 0x1d` (imm byte at 0x2f394c: 0x01 -> 0x1d). This makes the cancel command bit follow Circle (act 0x1d,
   always btn 5), exactly as in the Western layout. Without it, Cross (jump) would raise that bit in JP mode.
   Caveat (unverified): act 0x1d can be gated (+0x11 / +0x13) where act 1 is not, e.g. during an attack lockout.
   Apply this patch only when the option is on.

**Vita (android-port/):**
1. Flag at DcCore+0xc4. The startup store at 0x8101841c shares r1 = 1 with the language store, so a 4-byte in-place
   patch has no zero register to use. Best: an HLE-side write `*(u32*)(*(u32*)0x8177e01c + 0xc4) = 0` after the startup
   step, before the first Input_Update; or a trampoline at 0x8101841c. Live toggle: set input+0x4 = 1 on each input
   object.
2. Same leak: at **0x811f087a**, `movs r1,#1` (2 bytes) -> `movs r1,#0x1d` (low byte 0x01 -> 0x1d).
3. The `sceAppUtilSystemParamGetInt(2)` value in apputil.cpp:59 is never read. Leave it, or tie it to the setting
   for honesty only.

**What changes**: in every menu, dialog, shop, save/load, spoils, revive/continue prompt (PS4 0x2f1300),
bones/burial events, quest menu and text boxes using acts 0 / 1 / 5 / 0x29 (title "Press X" assumed to be among them, unverified): Circle confirms and
Cross cancels. Prompts and baked-in button art show the JP glyphs. Combat (attack / jump / skills / items) stays as
it is.

**Risks / edge cases**
- Text boxes during stages and act 0x29 (dialogue advance) follow decide: Circle advances in JP mode. That is
  intended, but Circle is also a gameplay action (0x1d) in the same frame, which mirrors how Cross/jump already
  overlaps today.
- "Skip on any of decide / cancel / attack held" (0x47ec10, 0x47ee90) is symmetric, so no change.
- KB/M: the flag swaps the game's meaning of the Cross/Circle bits. With the default input.ini (Space = cross,
  Mouse2 = circle), JP mode makes Mouse2 confirm and Space cancel in menus. Either gate the option to pad users,
  or document it. The KB/M glyph swap (prompts.cpp, classified by glyph) still labels the right key.
- Local co-op: the flag is global, so all 4 players get it.
- Online: purely local. The command bit stays the same if the 0x2f394b / 0x811f087a patch is applied (whether the
  command word is sent over the net is unverified).
- Touch / fairy pointer: separate path (touch points + act 9 touchpad), no change.

## 5. Open questions
- What event 0x1427 (character "cancel" command with a linked object at +0x1e0) does in play: maybe dismount or drop
  a carried object (unverified). The 0x2f394b / 0x811f087a patch keeps it on Circle either way.
- Whether any UI art panel outlives a runtime toggle. Recommend restart-required, or apply at startup only.
- PS4 FUN_00017750 and Vita 0x810180fa (the other +0x134 / +0xc4 = 1 writers) have no refs found. Patch them anyway
  if the code-patch route is used.
- Other access paths to +0x134 / +0xc4 that do not go through DcCore_Get were not searched (only the getter pattern
  plus in-class stores).
- Not play-tested. First check: title "Press (Circle) button", save select, shop buy/cancel, spoils screen,
  jump in a stage still on Cross.
