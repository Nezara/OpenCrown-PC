// SPIR-V for the host UI overlay (gpu/overlay.h): our own builder, Vulkan 1.1 / SPIR-V 1.3.
#pragma once
#include <cstdint>
#include <vector>

namespace gpu {

std::vector<uint32_t> overlay_vertex_spirv();
std::vector<uint32_t> overlay_fragment_spirv();

}  // namespace gpu
