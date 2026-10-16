#include "dds.h"

#include <stdexcept>
#include <string>

namespace dcp::formats {
namespace {

void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int s = 0; s < 32; s += 8) b.push_back(uint8_t(v >> s));
}

uint32_t uncompressed_bpp(uint32_t f) {
    switch (f) {
        case 28: case 29: case 87: case 91: return 4;  // R8G8B8A8, B8G8R8A8 (+SRGB)
        case 61: return 1;                             // R8_UNORM
        default: return 0;
    }
}

}  // namespace

uint32_t dxgi_bc_block_bytes(uint32_t f) {
    switch (f) {
        case 71: case 72: case 80: case 81: return 8;                        // BC1, BC4
        case 74: case 75: case 77: case 78: case 83: case 84:                // BC2, BC3, BC5
        case 95: case 96: case 98: case 99: return 16;                       // BC6H, BC7
        default: return 0;
    }
}

std::vector<uint8_t> make_dds(uint32_t w, uint32_t h, uint32_t fmt, std::span<const uint8_t> data) {
    uint32_t bc = dxgi_bc_block_bytes(fmt), bpp = uncompressed_bpp(fmt);
    if (!bc && !bpp) throw std::runtime_error("dds: unsupported DXGI format " + std::to_string(fmt));
    uint64_t pitch = bc ? uint64_t((w + 3) / 4) * bc : uint64_t(w) * bpp;
    uint64_t size = bc ? pitch * ((h + 3) / 4) : pitch * h;
    if (data.size() != size)
        throw std::runtime_error("dds: data is " + std::to_string(data.size()) + " bytes, expected " +
                                 std::to_string(size));

    std::vector<uint8_t> b = {'D', 'D', 'S', ' '};
    put32(b, 124);
    put32(b, 0x1 | 0x2 | 0x4 | 0x1000 | (bc ? 0x80000u : 0x8u));  // CAPS|HEIGHT|WIDTH|PIXELFORMAT|LINEARSIZE/PITCH
    put32(b, h);
    put32(b, w);
    put32(b, uint32_t(bc ? size : pitch));
    put32(b, 0);  // depth
    put32(b, 1);  // mip count
    for (int i = 0; i < 11; ++i) put32(b, 0);
    put32(b, 32);  // DDS_PIXELFORMAT size
    put32(b, 0x4); // DDPF_FOURCC
    b.insert(b.end(), {'D', 'X', '1', '0'});
    for (int i = 0; i < 5; ++i) put32(b, 0);
    put32(b, 0x1000);  // DDSCAPS_TEXTURE
    for (int i = 0; i < 4; ++i) put32(b, 0);
    put32(b, fmt);  // DDS_HEADER_DXT10
    put32(b, 3);    // D3D10_RESOURCE_DIMENSION_TEXTURE2D
    put32(b, 0);
    put32(b, 1);    // array size
    put32(b, 0);
    b.insert(b.end(), data.begin(), data.end());
    return b;
}

}  // namespace dcp::formats
