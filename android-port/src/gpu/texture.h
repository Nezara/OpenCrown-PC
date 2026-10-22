// GXM texture decode: the 4 SceGxmTexture control words plus guest memory in, a Vulkan-uploadable image out.
// No Vulkan headers here: the backend maps HostFormat / Comp to VkFormat / VkComponentSwizzle itself.
// Native formats are kept where Vulkan has one (BC1-3, R8, RG8, RGBA8, RGBA16F); the rest is converted to RGBA8.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gpu::tex {

using Words = uint32_t[4];

enum class HostFormat : uint8_t { RGBA8, R8, RG8, BC1, BC2, BC3, RGBA16F };
enum class Comp : uint8_t { R, G, B, A, Zero, One };  // sampler component mapping (identity = R,G,B,A)
struct Swizzle {
  Comp r = Comp::R, g = Comp::G, b = Comp::B, a = Comp::A;
};
enum class Type : uint8_t { Swizzled, Cube, Linear, Tiled, SwizzledArbitrary, LinearStrided, CubeArbitrary };

struct Desc {
  bool ok = false;
  std::string reason;  // set when !ok
  HostFormat format = HostFormat::RGBA8;
  Swizzle swizzle;
  Type type = Type::Swizzled;
  uint32_t width = 0, height = 0, mips = 1;
  uint32_t data = 0, palette = 0;  // guest addresses
  uint32_t source_bytes = 0;       // guest bytes read for the texels (palette excluded)
};

struct Level {
  uint32_t offset, width, height;  // offset into Decoded::data; rows are tight (BC: width/height in blocks)
};
struct Decoded {
  Desc desc;
  std::vector<uint8_t> data;
  std::vector<Level> levels;
};

// Returns a host pointer to n readable bytes at guest address va, or nullptr.
using ReadFn = std::function<const uint8_t*(uint32_t va, uint32_t n)>;

Desc describe(const Words& w);
bool decode(const Words& w, const ReadFn& read, Decoded& out);  // false: out.desc.reason says why
// Texture-cache key over the words and the guest bytes they point at (0 if unsupported or unreadable).
uint64_t cache_key(const Words& w, const ReadFn& read);
// Cheap change check: the words and 64 chunks of 32 bytes spread evenly over the texel data (all of it when that is
// small). Equal sample keys do not prove equal data; cache_key does (as far as a 64-bit hash goes).
uint64_t sample_key(const Words& w, const ReadFn& read);
// For GPUs without BC sampling (most Android devices): BC1-3 levels decompressed to RGBA8 in place (format and
// levels updated, swizzle kept). Other formats are left alone.
void decompress_bc(Decoded& dec);

}  // namespace gpu::tex
