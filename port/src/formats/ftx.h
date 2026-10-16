// .ftx texture bank: "FTEX" container of "FTX0" chunks, each wrapping a PS4 GNF file (GCN T# descriptors +
// GPU-tiled pixel data). See notes/map/ftx.md. Little endian.
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "gnm_detile.h"

namespace dcp::formats {

// GCN texture data format (T# dword1 bits 20..25) properties.
struct GnmFormatInfo {
    const char* name = nullptr;  // nullptr = unknown format
    uint32_t block_w = 1, block_h = 1, bytes_per_element = 0;
    bool compressed = false;
};
GnmFormatInfo gnm_format_info(uint32_t data_format);
const char* gnm_num_format_name(uint32_t num_format);

// One GCN T# (8 dwords) as stored in a GNF; dword7 holds the pixel data size in GNF files.
struct GnfTexture {
    std::array<uint32_t, 8> t{};
    uint32_t data_offset() const { return t[0]; }  // relative to the end of the GNF contents block
    uint32_t align_log2() const { return t[1] & 0xff; }
    uint32_t data_format() const { return t[1] >> 20 & 0x3f; }
    uint32_t num_format() const { return t[1] >> 26 & 0xf; }
    uint32_t width() const { return (t[2] & 0x3fff) + 1; }
    uint32_t height() const { return (t[2] >> 14 & 0x3fff) + 1; }
    uint32_t dst_sel() const { return t[3] & 0xfff; }  // 4x 3-bit channel selectors; 0xfac = identity RGBA
    uint32_t base_level() const { return t[3] >> 12 & 0xf; }
    uint32_t last_level() const { return t[3] >> 16 & 0xf; }
    uint32_t tile_index() const { return t[3] >> 20 & 0x1f; }
    uint32_t type() const { return t[3] >> 28; }  // 8 1D, 9 2D, 10 3D, 11 cube, 12 1D array, 13 2D array, ...
    uint32_t depth() const { return (t[4] & 0x1fff) + 1; }
    uint32_t pitch() const { return (t[4] >> 13 & 0x3fff) + 1; }
    uint32_t base_array() const { return t[5] & 0x1fff; }
    uint32_t last_array() const { return t[5] >> 13 & 0x1fff; }
    uint32_t data_size() const { return t[7]; }
    uint32_t mips() const { return last_level() >= base_level() ? last_level() - base_level() + 1 : 1; }
    uint32_t slices() const { return last_array() >= base_array() ? last_array() - base_array() + 1 : 1; }

    GnmSurface surface() const;  // mip 0, slice 0 geometry for gnm_detile (bytes_per_element 0 if format unknown)
};

struct FtxTexture {
    std::string name;   // from the FTEX name table (e.g. "Foo_arranged000.gnf"); untrusted
    uint32_t chunk = 0; // FTX0 child index
    uint32_t index = 0; // texture index within that chunk's GNF
    uint8_t gnf_version = 0;
    GnfTexture tex;
    std::span<const uint8_t> pixels;  // raw tiled bytes (data_size()), viewing the caller's buffer
};

bool is_ftx(std::span<const uint8_t> bytes);

// Parses the container and all GNF headers; pixel spans point into `bytes`, which must outlive the result.
// Throws on malformed input or unknown container variants.
std::vector<FtxTexture> parse_ftx(std::span<const uint8_t> bytes);

// Linear (detiled) mip-0 slice-0 data of one texture. Throws if the format / tile mode is unsupported.
std::vector<uint8_t> ftx_detile(const FtxTexture& t);

// DXGI_FORMAT for a GCN (data, num) format pair, or 0 when there is no direct mapping.
uint32_t gnm_to_dxgi(uint32_t data_format, uint32_t num_format);

}  // namespace dcp::formats
