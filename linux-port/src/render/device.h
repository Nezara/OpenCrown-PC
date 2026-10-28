// Graphics-API seam under Backend (internal to dcp_render). Backend decodes the captured GNM draws into Draw records;
// a Device turns them into API calls. GlDevice (gl_device.cpp) executes each draw at once; GpuDevice (gpu_device.cpp,
// SDL_gpu: Vulkan / D3D12) records the frame and submits it at end_frame (one copy pass, then one render pass per run of
// draws on the same target). Both keep render targets top row first: row 0 = the game's top, uv v = 0 = row 0,
// scissor y counts rows from row 0.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dcp::render {

struct Vtx { float pos[4]; float col[4]; float uv[2]; float muv[2]; };

using TexId = uint32_t;  // 0 = none (samples as the 1x1 white texture)

enum class TexFormat : uint8_t { RGBA8, BC1, BC2, BC3, BC7 };
enum class Prim : uint8_t { Points, Lines, Triangles };

// CB_BLEND_CONTROL fields as the game wrote them. Factor codes 0..10 = ZERO, ONE, SRC_COLOR, ONE_MINUS_SRC_COLOR,
// SRC_ALPHA, ONE_MINUS_SRC_ALPHA, DST_ALPHA, ONE_MINUS_DST_ALPHA, DST_COLOR, ONE_MINUS_DST_COLOR, SRC_ALPHA_SATURATE
// (others = ONE); combine codes 0..4 = ADD, SUBTRACT, MIN, MAX, REVERSE_SUBTRACT (others = ADD).
struct Blend { bool on = false; uint8_t cs = 1, cd = 0, cc = 0, as = 1, ad = 0, ac = 0; };

// S# fields: wrap codes (clamp_x / clamp_y, 0..7: 0 wrap, 1 mirror, 2 clamp, 3 mirror-once, 4.. border -> clamp).
struct Sampler { uint8_t wrap_u = 2, wrap_v = 2; bool mag_linear = true, min_linear = true; };

struct Draw {
    TexId target = 0;
    int tw = 0, th = 0;                  // target size (viewport = whole target)
    bool scissor = false;
    int sc[4] = {};                      // x, y (row from row 0), w, h
    Blend blend;
    TexId tex[2] = {};                   // slot 0 uTex, slot 1 uMask
    bool samp_set[2] = {};               // false: the texture's own sampling (GL: last params set; GPU: linear clamp)
    Sampler samp[2];
    Prim prim = Prim::Triangles;
    const Vtx* v = nullptr;
    size_t n = 0;
    // Uniforms (shader semantics in backend.cpp kFs / shaders/main.frag).
    float mat[16];                       // VS cb0 as the game wrote it (row-major unless !transpose)
    bool transpose = true;
    bool raw = false;                    // positions are clip space already
    int flags = 0;
    float tint[4], cst[4], cm[12], factor, grad[64];
};

class Device {
public:
    virtual ~Device() = default;
    virtual const char* name() const = 0;
    virtual bool init() = 0;
    // RGBA8 render target, cleared to 0 (before any later draw that renders to or samples it).
    virtual TexId create_target(int w, int h) = 0;
    // Uploads level 0 (BC data as blocks). reuse != 0: replace that texture's content in place (same size / format).
    // linear_clamp: give the texture its own linear / clamp sampling (glyph textures). Returns 0 on failure.
    virtual TexId upload(TexId reuse, TexFormat f, int w, int h, const void* data, size_t n, bool linear_clamp) = 0;
    virtual void destroy(TexId t) = 0;
    virtual void begin_frame() {}
    virtual void draw(const Draw& d) = 0;
    // Everything drawn since begin_frame reaches the GPU (GpuDevice submits here).
    virtual void end_frame() {}
    // RGBA8 rows of a target, row 0 first.
    virtual bool read_rgba(TexId t, int w, int h, std::vector<uint8_t>* out) = 0;
    // Decoded RGBA8 of an uploaded texture (DCP_RENDER_TEXDUMP); false if this device cannot read textures back.
    virtual bool read_texture(TexId, int, int, std::vector<uint8_t>*) { return false; }
    // Shows target t (tw x th, 0 = black) letterboxed in the window (w x h pixels; GpuDevice uses the swapchain size).
    // overlay (GpuDevice only) draws into the same swapchain pass after the picture.
    struct Overlay {
        virtual ~Overlay() = default;
        virtual void prepare(void* cmd) = 0;              // SDL_GPUCommandBuffer*, outside any pass
        virtual void render(void* cmd, void* pass) = 0;   // inside the swapchain render pass
    };
    virtual void present(TexId t, int tw, int th, int w, int h, Overlay* overlay) = 0;
};

std::unique_ptr<Device> make_gl_device();
#ifdef DCP_HAVE_SDL_GPU
// window may be null (hidden runs: no present). On failure returns null and sets *err.
std::unique_ptr<Device> make_gpu_device(void* sdl_gpu_device, void* sdl_window, std::string* err);
#endif

}  // namespace dcp::render
