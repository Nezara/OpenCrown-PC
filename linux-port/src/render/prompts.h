// Button-prompt glyph swap (notes/map/button_prompts.md). The game draws every pad-button prompt (HUD, menus,
// dialogs) as sprite parts sampling one 1024x512 BC7 button atlas. While keyboard/mouse is the last-used device, the
// backend replaces those parts with key-cap / mouse glyphs we render ourselves (FreeType + a Windows system font), labelled
// with the current input.ini binding. GL thread only.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace dcp::render {

// Pad buttons a prompt can show. Names match input.ini actions where one exists.
enum class PadBtn : uint8_t {
    None, Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3, Options, Touchpad, Share,
    DpadUp, DpadDown, DpadLeft, DpadRight, Dpad, LStick, RStick, Count
};

// One atlas cell (texels, [x0,x1) x [y0,y1)), matched by the centre of a draw's uv rect. Kinds (KB/M mode):
//   Ball   = a button base (sphere / touchpad / d-pad art); becomes the glyph
//   Symbol = a face symbol; becomes the glyph, unless drawn over a Ball (then dropped)
//   Plate  = L / R shoulder plate (btn L1 or R1); dropped, the Digit over it draws the glyph
//   Digit  = "1" (btn L1) / "2" (btn L2) over a plate -> L1/L2/R1/R2 glyph; elsewhere left alone
//   Hide   = dropped
struct PromptCell {
    int x0, y0, x1, y1;
    PadBtn btn;
    enum Kind : uint8_t { Ball, Symbol, Plate, Digit, Hide } kind;
    float scale;  // glyph height / this part's on-screen height
};

class Prompts {
public:
    std::set<unsigned> atlas_ids;  // GL textures holding the button atlas
    bool kbm = false;              // keyboard/mouse prompts on
    bool log = false;              // DCP_PROMPT_LOG: print atlas uploads and every new atlas part drawn

    Prompts();
    // Called on each texture upload; true for the button atlas (content hash of the detiled BC7 blocks).
    bool is_atlas(uint64_t base, uint32_t w, uint32_t h, const uint8_t* lin, size_t n);
    // Cell containing texel (u, v) of the atlas, or null.
    const PromptCell* find(float u, float v) const;
    // action (input.ini name) -> first bound input ("E", "Left Shift", "Mouse1"); call before draws (any thread
    // that owns the backend). Drops cached glyphs.
    void set_bindings(const std::map<std::string, std::string>& b);
    // GL texture for the KB/M glyph of b (lazily rendered); *aspect = width / height. 0 if unbound / no font.
    unsigned glyph(PadBtn b, float* aspect);

private:
    struct Glyph { unsigned tex = 0; float aspect = 1; bool tried = false; };
    std::map<std::string, std::string> binds_;
    Glyph cache_[size_t(PadBtn::Count)];
    std::string label_for(PadBtn b) const;
};

const char* pad_btn_name(PadBtn b);

}  // namespace dcp::render
