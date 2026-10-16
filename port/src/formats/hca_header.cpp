#include "hca_header.h"

#include <array>
#include <cstring>
#include <stdexcept>

namespace dcp::formats {
namespace {

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be24(const uint8_t* p) { return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]; }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | be24(p + 1); }

bool magic_is(const uint8_t* p, const char (&m)[5], bool& masked) {
    for (int i = 0; i < 4; ++i)
        if ((p[i] & 0x7F) != uint8_t(m[i])) return false;
    for (int i = 0; i < 4; ++i) masked |= (p[i] & 0x80) != 0;
    return true;
}

}  // namespace

uint16_t hca_crc16(std::span<const uint8_t> b) {
    static const auto table = [] {
        std::array<uint16_t, 256> t{};
        for (unsigned i = 0; i < 256; ++i) {
            uint16_t crc = uint16_t(i << 8);
            for (int k = 0; k < 8; ++k) crc = uint16_t(crc & 0x8000 ? crc << 1 ^ 0x8005 : crc << 1);
            t[i] = crc;
        }
        return t;
    }();
    uint16_t crc = 0;
    for (uint8_t c : b) crc = uint16_t(crc << 8 ^ table[(crc >> 8 ^ c) & 0xFF]);
    return crc;
}

bool is_hca(std::span<const uint8_t> b) {
    bool masked = false;
    return b.size() >= 8 && magic_is(b.data(), "HCA\0", masked);
}

uint16_t hca_header_size(std::span<const uint8_t> b) {
    if (!is_hca(b)) throw std::runtime_error("hca: bad magic");
    return be16(&b[6]);
}

HcaHeader parse_hca_header(std::span<const uint8_t> b) {
    HcaHeader h;
    h.header_size = hca_header_size(b);
    h.version = be16(&b[4]);
    magic_is(b.data(), "HCA\0", h.masked);
    if (h.header_size < 8 + 2 || b.size() < h.header_size) throw std::runtime_error("hca: truncated header");
    h.crc_ok = hca_crc16(b.first(h.header_size)) == 0;

    const uint8_t* p = b.data();
    size_t pos = 8, end = h.header_size - 2;  // last two bytes are the CRC
    auto need = [&](size_t n) {
        if (pos + n > end) throw std::runtime_error("hca: chunk overruns header");
        return p + pos;
    };
    bool has_fmt = false;
    while (pos + 4 <= end) {
        const uint8_t* c = p + pos;
        if (magic_is(c, "fmt\0", h.masked)) {
            c = need(16);
            h.channels = c[4];
            h.sample_rate = be24(c + 5);
            h.block_count = be32(c + 8);
            h.encoder_delay = be16(c + 12);
            h.encoder_padding = be16(c + 14);
            has_fmt = true;
            pos += 16;
        } else if (magic_is(c, "comp", h.masked)) {
            c = need(16);
            h.block_size = be16(c + 4);
            h.min_resolution = c[6];
            h.max_resolution = c[7];
            h.track_count = c[8];
            h.channel_config = c[9];
            h.total_band_count = c[10];
            h.base_band_count = c[11];
            h.stereo_band_count = c[12];
            h.bands_per_hfr_group = c[13];
            h.has_comp = true;
            pos += 16;
        } else if (magic_is(c, "dec\0", h.masked)) {
            // Older layout: band counts stored minus one, track/channel config packed in one byte.
            c = need(12);
            h.block_size = be16(c + 4);
            h.min_resolution = c[6];
            h.max_resolution = c[7];
            h.total_band_count = uint8_t(c[8] + 1);
            h.base_band_count = uint8_t(c[9] + 1);
            h.track_count = c[10] >> 4;
            h.channel_config = c[10] & 0x0F;
            if (c[11] == 0) h.base_band_count = h.total_band_count;  // stereo_type 0: no stereo bands
            h.stereo_band_count = uint8_t(h.total_band_count - h.base_band_count);
            h.has_dec = true;
            pos += 12;
        } else if (magic_is(c, "vbr\0", h.masked)) {
            c = need(8);
            h.vbr_max_block_size = be16(c + 4);
            h.vbr_noise_level = be16(c + 6);
            h.has_vbr = true;
            pos += 8;
        } else if (magic_is(c, "ath\0", h.masked)) {
            h.ath_type = be16(need(6) + 4);
            h.has_ath = true;
            pos += 6;
        } else if (magic_is(c, "loop", h.masked)) {
            c = need(16);
            h.loop_start = be32(c + 4);
            h.loop_end = be32(c + 8);
            h.loop_start_delay = be16(c + 12);
            h.loop_end_padding = be16(c + 14);
            h.has_loop = true;
            pos += 16;
        } else if (magic_is(c, "ciph", h.masked)) {
            h.cipher_type = be16(need(6) + 4);
            h.has_cipher = true;
            pos += 6;
        } else if (magic_is(c, "rva\0", h.masked)) {
            uint32_t u = be32(need(8) + 4);
            std::memcpy(&h.rva_volume, &u, 4);
            h.has_rva = true;
            pos += 8;
        } else if (magic_is(c, "comm", h.masked)) {
            size_t len = need(5)[4];
            c = need(5 + len);
            h.comment.assign(reinterpret_cast<const char*>(c + 5), strnlen(reinterpret_cast<const char*>(c + 5), len));
            pos += 5 + len;
        } else if (magic_is(c, "pad\0", h.masked)) {
            break;  // filler up to the CRC
        } else {
            throw std::runtime_error("hca: unknown chunk");
        }
    }
    if (!has_fmt || !(h.has_comp || h.has_dec)) throw std::runtime_error("hca: missing fmt/comp chunk");
    if (h.channels == 0 || h.sample_rate == 0 || (h.block_size == 0 && !h.has_vbr)) throw std::runtime_error("hca: bad fmt values");
    return h;
}

}  // namespace dcp::formats
