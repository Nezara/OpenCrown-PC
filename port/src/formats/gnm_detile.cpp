#include "gnm_detile.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace dcp::formats {
namespace {

constexpr uint32_t kLinearAligned = 8;  // Display_LinearAligned: plain rows of `pitch` elements
constexpr uint32_t kThin1d = 13;        // Thin_1dThin: 8x8-element micro tiles, row-major, no macro tiling
constexpr uint32_t kThin2d = 14;        // Thin_2dThin: micro tiles spread over 8 pipes x N banks (macro tiling)

constexpr uint32_t kMicro = 8;  // micro tile edge in elements

uint64_t round_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

bool pow2_bpe(uint32_t b) { return b == 1 || b == 2 || b == 4 || b == 8 || b == 16; }

uint32_t log2u(uint32_t v) { uint32_t n = 0; while (v > 1) v >>= 1, ++n; return n; }

// Non-displayable thin micro tiles interleave element bits as Morton order: x0 y0 x1 y1 x2 y2
// (verified for BC7 on a real font; addrlib uses the same order for every element size).
struct MortonTable {
    uint8_t x[64], y[64];
    constexpr MortonTable() : x(), y() {
        for (unsigned i = 0; i < 64; ++i) {
            x[i] = uint8_t((i & 1) | (i >> 1 & 2) | (i >> 2 & 4));
            y[i] = uint8_t((i >> 1 & 1) | (i >> 2 & 2) | (i >> 3 & 4));
        }
    }
};
constexpr MortonTable kMorton;

// Thin_2dThin as the game's own GpuAddress tiler computes it (eboot FUN_0054e4f0 init, FUN_0054e950 address):
// GB_TILE_MODE14 (table 0x760430) = pipe config P8_32x32_16x16, sample split 1; macro mode index =
// log2(min(tileSplit, 64*bpe) / 64) with tileSplit = min(max(256, 2 * 64 * bpe), 1024) into GB_MACROTILE_MODE
// (table 0x7604b0): bpe 1 -> bh4 asp4 16 banks, 2 -> bh2 asp2 16, 4 -> bh1 asp2 16, 8 -> bh1 asp2 16,
// 16 -> bh1 asp1 8; bank width is 1 in all of them. Pipe interleave 256 B.
// PS4 Pro (Neo) "alt" tiling, used by the game's tiler when the surface's minGpuMode is Neo (FUN_0054e4f0 ->
// FUN_0055e510 / FUN_0055e5b0): pipe config = GB_TILE_MODE14 >> 27 = 18 (P16_32x32_16x16) and the ALT_* fields of
// GB_MACROTILE_MODE (bits 8-13): bpe 1 -> bh4 asp2 8 banks, 2 -> bh4 asp2 8, 4 -> bh2 asp1 8, 8 -> bh2 asp1 8,
// 16 -> bh1 asp1 8.
struct Macro { uint32_t bh, asp, banks, pipes; };
Macro macro_mode(uint32_t bpe, bool alt) {
    if (alt) {
        switch (bpe) {
        case 1: case 2: return {4, 2, 8, 16};
        case 4: case 8: return {2, 1, 8, 16};
        default: return {1, 1, 8, 16};
        }
    }
    switch (bpe) {
    case 1: return {4, 4, 16, 8};
    case 2: return {2, 2, 16, 8};
    case 4: return {1, 2, 16, 8};
    case 8: return {1, 2, 16, 8};
    default: return {1, 1, 8, 8};
    }
}
constexpr uint32_t kInterleaveBits = 8;

uint32_t macro_pitch(uint32_t bpe, bool alt) { Macro m = macro_mode(bpe, alt); return kMicro * m.pipes * m.asp; }  // bank width 1
uint32_t macro_height(uint32_t bpe, bool alt) { Macro m = macro_mode(bpe, alt); return kMicro * m.bh * m.banks / m.asp; }

uint32_t bit(uint32_t v, unsigned b) { return v >> b & 1; }

// Byte offset of element (x, y) in a Thin_2dThin slice of `pitch` elements (addrlib macro-tiled address, slice 0,
// one sample, no pipe / bank swizzle).
uint64_t thin2d_offset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bpe, bool alt) {
    const Macro m = macro_mode(bpe, alt);
    const uint32_t mtp = macro_pitch(bpe, alt), mth = macro_height(bpe, alt), bank_bits = log2u(m.banks);
    const uint32_t pipe_bits = log2u(m.pipes);
    // pipe from coordinates as the game's FUN_0054e950 computes it: pipe config 12 (P8_32x32_16x16, base) and
    // 18 (P16_32x32_16x16, Neo alt; bit 3 = x6 ^ y5)
    const uint32_t pipe = m.pipes == 16
        ? (bit(x, 3) ^ bit(y, 3) ^ bit(x, 4)) | (bit(x, 4) ^ bit(y, 4)) << 1 | (bit(x, 5) ^ bit(y, 5)) << 2 | (bit(x, 6) ^ bit(y, 5)) << 3
        : (bit(x, 3) ^ bit(y, 3) ^ bit(x, 4)) | (bit(x, 4) ^ bit(y, 4)) << 1 | (bit(x, 5) ^ bit(y, 5)) << 2;
    const uint32_t tx = x / kMicro / m.pipes, ty = y / kMicro / m.bh;  // bank width 1
    uint32_t bank;
    switch (m.banks) {
    case 16:
        bank = (bit(tx, 0) ^ bit(ty, 3)) | (bit(tx, 1) ^ bit(ty, 2) ^ bit(ty, 3)) << 1 | (bit(tx, 2) ^ bit(ty, 1)) << 2 |
               (bit(tx, 3) ^ bit(ty, 0)) << 3;
        break;
    case 8: bank = (bit(tx, 0) ^ bit(ty, 2)) | (bit(tx, 1) ^ bit(ty, 1) ^ bit(ty, 2)) << 1 | (bit(tx, 2) ^ bit(ty, 0)) << 2; break;
    case 4: bank = (bit(tx, 0) ^ bit(ty, 1)) | (bit(tx, 1) ^ bit(ty, 0)) << 1; break;
    default: bank = bit(tx, 0) ^ bit(ty, 0); break;
    }
    const uint32_t morton = bit(x, 0) | bit(y, 0) << 1 | bit(x, 1) << 2 | bit(y, 1) << 3 | bit(x, 2) << 4 | bit(y, 2) << 5;
    const uint64_t micro_bytes = uint64_t(kMicro) * kMicro * bpe;
    const uint64_t tile_offset = uint64_t((y / kMicro) % m.bh) * micro_bytes;  // tile row within the bank (width 1)
    const uint64_t macro_bytes = uint64_t(mtp) * mth * bpe;
    const uint64_t macro_offset = (uint64_t(y / mth) * (pitch / mtp) + x / mtp) * macro_bytes;
    const uint64_t total = (macro_offset >> (pipe_bits + bank_bits)) + uint64_t(morton) * bpe + tile_offset;
    return (total & ((1u << kInterleaveBits) - 1)) | uint64_t(pipe) << kInterleaveBits |
           uint64_t(bank) << (kInterleaveBits + pipe_bits) | (total >> kInterleaveBits) << (kInterleaveBits + pipe_bits + bank_bits);
}

void check(const GnmSurface& s) {
    if (!gnm_detile_supported(s.tile_index, s.bytes_per_element))
        throw std::runtime_error("gnm: unsupported tile index " + std::to_string(s.tile_index) + " with " +
                                 std::to_string(s.bytes_per_element) + "-byte elements");
    if (s.width == 0 || s.height == 0 || s.block_w == 0 || s.block_h == 0)
        throw std::runtime_error("gnm: empty surface");
    if (s.pitch < s.width) throw std::runtime_error("gnm: pitch smaller than width");
    if (s.tile_index == kThin1d && s.pitch_elems() % kMicro)
        throw std::runtime_error("gnm: 1D-tiled pitch not a multiple of the 8-element micro tile");
    if (s.tile_index == kThin2d && s.pitch_elems() % macro_pitch(s.bytes_per_element, s.alt_tile))
        throw std::runtime_error("gnm: 2D-tiled pitch not a multiple of the macro tile width");
}

}  // namespace

bool gnm_detile_supported(uint32_t tile_index, uint32_t bpe) {
    return (tile_index == kLinearAligned || tile_index == kThin1d || tile_index == kThin2d) && pow2_bpe(bpe);
}

uint64_t gnm_linear_size(const GnmSurface& s) {
    return uint64_t(s.elems_x()) * s.elems_y() * s.bytes_per_element;
}

uint64_t gnm_tiled_size(const GnmSurface& s) {
    check(s);
    uint64_t rows = s.tile_index == kThin1d   ? round_up(s.elems_y(), kMicro)
                    : s.tile_index == kThin2d ? round_up(s.elems_y(), macro_height(s.bytes_per_element, s.alt_tile))
                                              : s.elems_y();
    return uint64_t(s.pitch_elems()) * rows * s.bytes_per_element;
}

std::vector<uint8_t> gnm_detile(std::span<const uint8_t> tiled, const GnmSurface& s) {
    uint64_t need = gnm_tiled_size(s);
    if (tiled.size() < need)
        throw std::runtime_error("gnm: tiled data " + std::to_string(tiled.size()) + " bytes, surface needs " +
                                 std::to_string(need));
    const uint32_t bpe = s.bytes_per_element, ex = s.elems_x(), ey = s.elems_y(), pe = s.pitch_elems();
    std::vector<uint8_t> out(size_t(gnm_linear_size(s)));

    if (s.tile_index == kLinearAligned) {
        for (uint32_t y = 0; y < ey; ++y)
            std::memcpy(&out[size_t(y) * ex * bpe], &tiled[size_t(y) * pe * bpe], size_t(ex) * bpe);
        return out;
    }

    if (s.tile_index == kThin2d) {
        for (uint32_t y = 0; y < ey; ++y)
            for (uint32_t x = 0; x < ex; ++x)
                std::memcpy(&out[(size_t(y) * ex + x) * bpe], &tiled[size_t(thin2d_offset(x, y, pe, bpe, s.alt_tile))], bpe);
        return out;
    }

    const uint32_t tiles_x = pe / kMicro, tiles_y = (ey + kMicro - 1) / kMicro;
    const size_t tile_bytes = size_t(kMicro) * kMicro * bpe;
    for (uint32_t ty = 0; ty < tiles_y; ++ty)
        for (uint32_t tx = 0; tx < tiles_x; ++tx) {
            const uint8_t* src = &tiled[(size_t(ty) * tiles_x + tx) * tile_bytes];
            for (unsigned i = 0; i < 64; ++i) {
                uint32_t x = tx * kMicro + kMorton.x[i], y = ty * kMicro + kMorton.y[i];
                if (x < ex && y < ey) std::memcpy(&out[(size_t(y) * ex + x) * bpe], src + size_t(i) * bpe, bpe);
            }
        }
    return out;
}

}  // namespace dcp::formats
