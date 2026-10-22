// Our USSE (Vita GPU shader ISA) -> SPIR-V translator, for the subset Dragon's Crown's 252 GXP programs use
// (spec: notes/map/usse_gxp.md). Every program is straight-line code, so it is executed symbolically at translation
// time: each register holds a SPIR-V value id, and the emitted shader is plain SSA with no register file.
//
// Interface the backend binds (Vulkan 1.1, descriptor set 0):
//   binding 0 vertex / 1 fragment: default uniform buffer, std140 `uvec4 w[kUniformVec4s]` (the game's words in order)
//   binding kTextureBinding + unit: combined image sampler of texture unit `unit`
//   vertex inputs: one vec4 per GXP attribute parameter at `VertexInput::location`
//   varyings: colour0 vec4 at location 0, TEXCOORDn vec4 at location 1 + n
//   vertex push constants: vec4 scale at 0, vec4 offset at 16; clip position = pos * scale + pos.w * offset (the GXM
//   viewport, applied in the shader against one fixed full-target Vulkan viewport)
//   fragment output: vec4 at location 0. Programs that blend in the shader (SOP2 against the framebuffer colour) are
//   translated to "output the source colour" plus `blend` = one of six fixed-function blend modes the backend sets.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gpu::usse {

constexpr uint32_t kUniformVec4s = 16;  // 64 words; the game's largest default buffer + literal is 51
constexpr uint32_t kTextureBinding = 2;

enum class Stage : uint8_t { Vertex, Fragment };

// Fixed-function equivalents of the game's in-shader blends (S = shader colour, D = framebuffer colour).
enum class Blend : int8_t {
  None = -1,
  Alpha = 0,             // D*(1-S.a) + S*S.a
  Add = 1,               // D + S*S.a           (alpha as Alpha)
  Subtract = 2,          // D - S*S.a           (alpha: D.a*(1-S.a) - S.a*S.a)
  DestAlpha = 3,         // D*(1-D.a) + S*D.a
  DestAlphaAdd = 4,      // D + S*D.a           (alpha as DestAlpha)
  DestAlphaSubtract = 5  // D - S*D.a           (alpha: D.a*(1-D.a) - S.a*D.a)
};

struct VertexInput {
  uint32_t location, pa_reg, components;  // pa_reg = the parameter's resource index (SceGxmVertexAttribute::regIndex)
};

enum Varying : uint32_t { kColor0 = 1, kTexcoord0 = 2 };  // bit kTexcoord0 << n = TEXCOORDn

struct Shader {
  bool ok = false;
  std::string error;     // why translation failed (first problem found)
  std::string listing;   // disassembly, one line per instruction (also filled on failure)
  Stage stage = Stage::Vertex;
  std::vector<uint32_t> spirv;
  uint32_t uniform_words = 0;        // default uniform buffer words the program declares
  std::vector<VertexInput> inputs;   // vertex
  uint32_t varyings = 0;             // vertex: written; fragment: read (Varying bits)
  uint32_t texture_units = 0;        // fragment: bit n = unit n sampled
  Blend blend = Blend::None;         // fragment
  bool native_color = true;          // fragment: u8 RGBA colour output (else f16 RGBA)
};

// How a vertex input is fetched when the GPU lacks the attribute's own vertex format (the backend substitutes one it
// has): bits per input location, 0 = a float vec4 as declared.
enum Fetch : uint8_t {
  kFetchUint = 1,  // the input is a uvec4 (USCALED -> UINT), converted to float
  kFetchSint = 2,  // ivec4 (SSCALED -> SINT), converted to float
  kFetchWOne = 4   // a 3-component format widened to 4: w = 1 as the 3-component fetch gives
};

// `gxp` = the whole GXP program (size field at +8). `fetch[location]` = Fetch bits (vertex programs; missing = 0).
// Never throws.
Shader translate(std::span<const uint8_t> gxp, std::span<const uint8_t> fetch = {});

}  // namespace gpu::usse
