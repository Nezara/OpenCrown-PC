// PS4 GNM (GCN) surface detiling: GPU-tiled texture bytes -> linear rows of elements (pixels or 4x4 blocks).
// Block-compressed data stays compressed; the PC GPU samples BC formats natively.
#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace dcp::formats {

struct GnmSurface {
    uint32_t width = 0, height = 0;   // pixels
    uint32_t pitch = 0;               // pixels (T# dword4); >= width, padded to the tile width
    uint32_t block_w = 1, block_h = 1; // pixels per element (4x4 for BC formats)
    uint32_t bytes_per_element = 0;    // 8 / 16 for BC, texel size otherwise
    uint32_t tile_index = 0;           // GB_TILE_MODE index from T# dword3
    bool alt_tile = false;             // PS4 Pro (Neo) alt tiling (T# dword6 bit 24): Thin_2dThin uses 16 pipes

    uint32_t elems_x() const { return (width + block_w - 1) / block_w; }
    uint32_t elems_y() const { return (height + block_h - 1) / block_h; }
    uint32_t pitch_elems() const { return (pitch + block_w - 1) / block_w; }
};

// Tile indices handled by detile(): 8 (Display_LinearAligned), 13 (Thin_1dThin) and 14 (Thin_2dThin; only 4-byte
// elements seen in game data), any power-of-two element size.
bool gnm_detile_supported(uint32_t tile_index, uint32_t bytes_per_element);

// Bytes one tiled mip-0 slice occupies (pitch and tile-height padding included). Throws if unsupported.
uint64_t gnm_tiled_size(const GnmSurface& s);

// Tightly packed linear size: elems_x * elems_y * bytes_per_element.
uint64_t gnm_linear_size(const GnmSurface& s);

// Detiles one mip-0 slice. `tiled` must hold at least gnm_tiled_size(s) bytes. Throws on unsupported / short input.
std::vector<uint8_t> gnm_detile(std::span<const uint8_t> tiled, const GnmSurface& s);

}  // namespace dcp::formats
