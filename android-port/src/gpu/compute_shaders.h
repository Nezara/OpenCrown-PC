// SPIR-V for the Vulkan backend's own compute shaders, built with our builder (Vulkan 1.1 / SPIR-V 1.3).
#pragma once
#include <cstdint>
#include <vector>

namespace gpu {

// BC1 / BC2 / BC3 -> RGBA8 for GPUs without BC sampling, one invocation per texel (8x8 groups), reading the game's
// blocks as they are stored (Morton-swizzled or linear) and writing one mip level of an RGBA8 storage image.
// Results are bit-identical to tex::decompress_bc (same integer palette arithmetic).
//   set 0 binding 0: storage buffer, uint words of the texture data
//   set 0 binding 1: storage image rgba8 (the level)
//   push constants (uint each): src (word offset of the level's first block), width, height (level texels),
//   grid_w (blocks per stored row: the level's power-of-two grid), n (Morton: log2 of the grid's shorter side),
//   wide (grid wider than tall), mode (1 = BC1, 2 = BC2, 3 = BC3), swizzled
struct BcDecodePush {
  uint32_t src, width, height, grid_w, n, wide, mode, swizzled;
};
std::vector<uint32_t> bc_decode_spirv();

}  // namespace gpu
