// The seam between our GXM layer (src/hle/gxm.cpp) and a host graphics backend.
//
// GXM keeps all API state and hands the backend complete, self-contained records: a scene (render target + colour /
// depth surfaces), each draw (programs, vertex streams, indices, uniform buffers, textures, fixed-function state)
// and each display flip. Guest addresses stay guest addresses; a backend reads guest memory during draw() itself,
// because the game recycles its ring buffers as soon as the call returns.
//
// Backends: Null (records nothing; the game runs headless) now. Vulkan is the primary real backend (desktop + Android),
// with GLES 3.2 as the fallback for devices without usable Vulkan; both go behind this interface and are chosen at
// start-up. The game's GXP shader programs are not translated: each one is identified by a hash of its bytes and
// replaced by our own shader with the same behaviour.
#pragma once
#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace guest {
class Memory;
}

namespace gpu {

struct VertexAttribute {
  uint16_t stream, offset;
  uint8_t format, components;  // SceGxmAttributeFormat, 1..4
  uint16_t reg;                // resource index of the GXP attribute parameter
};
struct VertexStreamLayout {
  uint16_t stride, index_source;
};

// Shader programs as the shader patcher built them. `gxp` is the guest address of the GXP program header.
struct VertexProgram {
  uint32_t gxp = 0;
  std::vector<VertexAttribute> attributes;
  std::vector<VertexStreamLayout> streams;
};
struct FragmentProgram {
  uint32_t gxp = 0;
  uint32_t output_format = 0, multisample = 0;
  bool has_blend = false;
  uint32_t blend = 0;          // SceGxmBlendInfo word
  uint32_t vertex_gxp = 0;     // the vertex program it was linked against
};

struct ColorSurface {
  bool enabled = false;
  uint32_t format = 0, type = 0, scale = 0, output_register_size = 0;
  uint32_t width = 0, height = 0, stride = 0;  // stride in pixels
  uint32_t data = 0;
};
struct DepthStencilSurface {
  bool enabled = false;
  uint32_t format = 0, type = 0, stride = 0;  // stride in samples
  uint32_t depth = 0, stencil = 0;
};

struct Scene {
  uint32_t render_target = 0, width = 0, height = 0, flags = 0;
  uint32_t valid_x_max = 0, valid_y_max = 0;
  ColorSurface color;
  DepthStencilSurface depth;
};

struct StencilState {
  uint32_t func = 0, fail = 0, depth_fail = 0, depth_pass = 0;
  uint8_t compare_mask = 0xff, write_mask = 0xff;
  uint32_t ref = 0;
};
struct RenderState {
  uint32_t cull = 0, front_polygon = 0, back_polygon = 0;
  uint32_t front_depth_func = 0, front_depth_write = 0;
  StencilState front_stencil;
  uint32_t point_line_width = 1;
  bool viewport_enabled = true;
  float viewport[6] = {0, 1, 0, 1, 0, 1};  // x offset, x scale, y offset, y scale, z offset, z scale
};

struct Draw {
  const VertexProgram* vertex = nullptr;
  const FragmentProgram* fragment = nullptr;
  std::array<uint32_t, 16> streams{};      // guest addresses of the vertex streams
  uint32_t primitive = 0, index_format = 0, indices = 0, index_count = 0;
  uint32_t vertex_uniforms = 0, fragment_uniforms = 0;  // default uniform buffers (guest), 0 = none
  std::array<std::array<uint32_t, 4>, 16> textures{};   // SceGxmTexture control words per unit
  RenderState state;
};

struct Frame {  // what sceDisplaySetFrameBuf shows
  uint32_t base = 0, pitch = 0, pixel_format = 0, width = 0, height = 0;
};

class Renderer {
 public:
  virtual ~Renderer() = default;
  virtual const char* name() const = 0;
  virtual void begin_scene(guest::Memory& mem, const Scene& scene) = 0;
  virtual void draw(guest::Memory& mem, const Draw& draw) = 0;
  virtual void end_scene(guest::Memory& mem) = 0;
  // End of a frame's GPU work (sceGxmDisplayQueueAddEntry, on the rendering thread): everything recorded since the
  // last flush is submitted, so a later present() of any of its colour surfaces shows it.
  virtual void flush(guest::Memory&) {}
  // Shows a flipped framebuffer. Called on the GXM display queue thread (the game's display callback) while the
  // rendering thread already records the next frame, so it only reads what flush() published.
  virtual void present(guest::Memory& mem, const Frame& frame) = 0;
  // Android destroys the window surface when the app goes to the background and makes a new one on return.
  virtual void surface_lost() {}
  virtual void surface_restored() {}
  // Writes the backend's on-disk caches now (exit; Android background, where the process may be killed).
  virtual void save_caches() {}
};

// Frame timing (always on, logged by the backend every 300 presents): microseconds the guest spent in
// sceDisplayWaitVblankStart*. The backend adds its own fence / acquire / present waits.
extern std::atomic<uint64_t> vblank_wait_us;
// Extra lines for the timing log (per guest thread: hle::thread_times), given the frame count of the window.
extern std::string (*frame_log_extra)(double frames);

// Identity of a GXP program: FNV-1a 64 over its bytes (size field at +8). Our replacement shaders are keyed by it.
uint64_t gxp_hash(guest::Memory& mem, uint32_t gxp);

// The backend in use (Null until one is chosen with set_renderer).
Renderer& renderer();
void set_renderer(std::unique_ptr<Renderer> r);
std::unique_ptr<Renderer> make_null_renderer();

}  // namespace gpu
