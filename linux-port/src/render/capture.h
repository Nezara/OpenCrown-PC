// Render capture: observes the game's own render-command layer (notes/map/render_cmds.md) and turns every draw into
// a self-contained DrawRec (state + copied vertex / index / constant bytes). See src/render/README.md.
// Built into dcp_hle (-fno-exceptions); no GL here. Everything is inert until install_hooks() succeeds.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace dcp::render {

// GCN V# (buffer resource, 4 dwords).
struct VSharp {
    uint32_t w[4]{};
    uint64_t base() const { return w[0] | uint64_t(w[1] & 0xfff) << 32; }
    uint32_t stride() const { return w[1] >> 16 & 0x3fff; }
    uint32_t records() const { return w[2]; }
    uint32_t nfmt() const { return w[3] >> 12 & 7; }
    uint32_t dfmt() const { return w[3] >> 15 & 0xf; }
};

// Which Execute (command class) was running on the thread when the draw was issued.
enum class Cls : uint8_t { None, Layer, Sprite, X1, X2, X3, X4, A, B, Count };
const char* cls_name(Cls c);

struct Stream {
    VSharp v;
    std::vector<uint8_t> bytes;  // copy of v.base() .. +needed (empty if unreadable)
};

struct DrawRec {
    Cls cls = Cls::None;
    int vs = -1, ps = -1;          // indices into the game's VS / PS pools (-1 = unknown program)
    uint32_t blend = 0;            // CB_BLEND0_CONTROL word (render target 0)
    uint32_t prim = 0;             // VGT primitive type (GCN enum)
    uint32_t count = 0;            // index / vertex count
    bool indexed = false;
    bool scissor_on = false;
    int32_t scissor[4]{};          // x0, y0, x1, y1 (pixels, top-left origin)
    uint64_t rt = 0;               // colour target 0 base address (0 = never set)
    uint32_t rt_w = 1920, rt_h = 1080;
    std::vector<Stream> vb;        // vertex stage streams in slot order
    std::vector<uint8_t> vcb[8];   // vertex-stage constant buffers by slot
    std::vector<uint8_t> pcb[8];   // pixel-stage constant buffers by slot
    uint32_t tex[2][8]{};          // pixel-stage T# slots 0/1 (all zero = unbound)
    uint32_t samp[2][4]{};         // pixel-stage S# slots 0/1
    std::vector<uint32_t> indices; // expanded to 32 bit (indexed draws)
};

struct Frame {
    uint64_t vblank = 0, flip = 0;
    uint64_t display = 0;          // address of the video-out buffer this flip shows (0 = unknown)
    std::vector<std::unique_ptr<DrawRec>> draws;  // GPU (submit) order
};

// Patches the eboot (base = load address) so its render wrappers report to us. Returns false (and patches
// nothing further) if an expected prologue is not found, i.e. a different eboot build.
bool install_hooks(uint8_t* eboot_base);
bool capture_enabled();
// Player 0's fairy cursor as last sampled at Cursor_Update entry (centred 1920x1080 px, y down). Returns the sample
// counter (0 = never sampled: hook off or no cursor yet).
uint32_t fairy_cursor(float* x, float* y);

// --- called from the GNM / VideoOut shims (game threads) ---
// A draw packet of `size` dwords at `cmd`: records the current state and leaves a marker NOP in its place.
void on_draw(uint32_t* cmd, uint32_t size, uint32_t count, uint64_t index_addr, bool indexed);
// A submitted marker NOP (payload after the header); keeps submit order.
void on_marker(const uint32_t* payload, uint32_t ndw);
void on_flip(uint32_t display_index);
void set_display_buffers(void* const* addrs, int start, int n);

// --- consumer (render thread) ---
// Next completed frame; waits up to timeout_ms (0 = poll). nullptr if none.
std::unique_ptr<Frame> take_frame(uint32_t timeout_ms);
// Sprite PS pool index for a 6-bit permutation (table at eboot 0x79f890, render_cmds.md s3.1); 0xffffffff if unknown.
uint32_t sprite_ps_table(uint32_t perm);
constexpr uint32_t kMarkerMagic = 0x52504344;  // 'DCPR'

}  // namespace dcp::render
