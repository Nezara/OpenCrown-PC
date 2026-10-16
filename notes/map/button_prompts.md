# Button prompts (pad glyphs) and the KB/M glyph swap

## 1. How the game draws prompts (captured, DCP_RENDER_DUMP / PICK / TEXDUMP)
- Every prompt seen so far is **sprite parts (Sprite class, VS14/PS49, one quad per part) sampling one 1024x512 BC7
  button atlas** (US_Master dump: FNV-1a 64 of the detiled blocks = `e1d3d9ea867008b4`; uploaded at T# base 22d1c8500
  in our runs). It holds spheres with baked symbols, white face symbols, L/R plates, digits, touchpad / d-pad art,
  OPTIONS / TOUCHPAD / START / SELECT lettering (PS3 leftovers too).
- A face button is a **composite**: the sphere drawn dimmed (vertex colour 0.66,0.69,0.77), then a white symbol tinted
  by vertex colour on top (triangle 0.4,1,0.4; circle 1,0.45,0.22; cross 0.42,0.51,1).
- The touchpad prompt is the touchpad art from the atlas, then the localised "TOUCHPAD" lettering drawn from a
  different 2048x2048 UI atlas (draw immediately after, same centre).
- Inline prompts in text ("Press X button" on the title) are also atlas sprites placed next to X1 font quads; no
  button glyph was found in the 4096x4096 font atlas (rows checked: Latin, symbols, digits).
- Not traced to an eboot function (the swap does not need it): which mbs motion/part the UI uses per button.

## 2. Cells confirmed in game (texels; DCP_PROMPT_LOG)
| cell | role | where seen |
|---|---|---|
| 544,0..640,96 + 632,436..696,496 | triangle sphere + symbol | save select "Delete" |
| 640,0..736,96 + 944,0..1000,56 | cross sphere + symbol | title "Press X button" |
| 572,128..668,224 + 808,56..864,112 | circle sphere + symbol | world map "Skip" |
| 0,0..200,108 | touchpad art | town/inn "Press TOUCHPAD to open Town Map" (KB/M glyph = town_map key, else touchpad) |
Other cells in `port/src/render/prompts.cpp` (square, white-ball variants, L/R plates + digits, OPTIONS lettering,
d-pad) are read off the decoded atlas and **not yet seen in a run** (positions are estimates).

## 3. Hook (renderer level, no eboot patch)
`port/src/render/prompts.{h,cpp}` + `Backend::Impl::swap_prompt` (backend.cpp): texture upload hashes 1024x512
textures to find the atlas; each atlas draw is classified by its uv-rect centre. In KB/M mode: Ball -> glyph quad
(screen-aligned, centred, 0.9 x part height, widened for long labels); Symbol over a Ball -> dropped; Plate dropped,
digit over it -> L1/L2/R1/R2 glyph; the draw right after a touchpad/OPTIONS ball that sits inside it (lettering) ->
dropped. Game alpha kept (fades). Glyphs: rounded key cap + label (FreeType, Windows arialbd/segoeuib/seguisym, or
$DCP_PROMPT_FONT), mouse icon for Mouse1-3; nothing third-party vendored.
Labels: first input of the action in input.ini (`prompt_bindings`, sdl_input.cpp); sticks/d-pad = their 4 keys.

## 4. Device detection (`DeviceTracker`, tools/dcp_run/sdl_input.cpp)
KB/M on key down (not F1/F12), mouse button/wheel, or >= 24 px of mouse motion; pad on gamepad button down or
stick > 16000 / trigger > 8000. Starts on KB/M unless a gamepad is plugged in. Scripted runs stay on pad glyphs;
test hook `DCP_PROMPT_EVENTS=990=key,1020=pad` feeds synthetic SDL events through the same tracker.
