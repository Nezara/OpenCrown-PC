// Minimal .dds writer (export / debugging): DX10 extended header, one mip, one 2D slice.
#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace dcp::formats {

// Bytes per 4x4 block for BC DXGI formats, 0 for anything else.
uint32_t dxgi_bc_block_bytes(uint32_t dxgi_format);

// `data` must be linear mip 0 in `dxgi_format` layout (BC: rows of 4x4 blocks). Throws if the size does not match.
std::vector<uint8_t> make_dds(uint32_t width, uint32_t height, uint32_t dxgi_format, std::span<const uint8_t> data);

}  // namespace dcp::formats
