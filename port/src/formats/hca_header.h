// CRI HCA stream header (chunks only; decoding is separate). Chunk magics may have their top bit set as
// obfuscation, so every magic byte is compared with 0x7F masked off.
#pragma once
#include <cstdint>
#include <span>
#include <string>

namespace dcp::formats {

struct HcaHeader {
    uint16_t version = 0, header_size = 0;  // header_size = offset of the first block
    // fmt
    uint8_t channels = 0;
    uint32_t sample_rate = 0, block_count = 0;
    uint16_t encoder_delay = 0, encoder_padding = 0;  // "mute" samples before / after
    // comp or dec
    uint16_t block_size = 0;
    uint8_t min_resolution = 0, max_resolution = 0, track_count = 0, channel_config = 0;
    uint8_t total_band_count = 0, base_band_count = 0, stereo_band_count = 0, bands_per_hfr_group = 0;
    bool has_comp = false, has_dec = false;
    // optional chunks
    bool has_vbr = false, has_ath = false, has_loop = false, has_cipher = false, has_rva = false;
    uint16_t vbr_max_block_size = 0, vbr_noise_level = 0;
    uint16_t ath_type = 0;
    uint32_t loop_start = 0, loop_end = 0;  // in blocks
    uint16_t loop_start_delay = 0, loop_end_padding = 0;
    uint16_t cipher_type = 0;  // 0 none, 1 fixed table, 56 keyed (key + AWB subkey)
    float rva_volume = 1.0f;
    std::string comment;
    bool masked = false;  // some chunk magic had its top bit set
    bool crc_ok = false;  // CRC-16 over the stored header bytes
};

bool is_hca(std::span<const uint8_t> bytes);  // "HCA\0" with masking

// CRC-16 (poly 0x8005, MSB first, init 0). Headers and frames end with their CRC, so a valid one sums to 0.
uint16_t hca_crc16(std::span<const uint8_t> bytes);

// header_size field from the first 8 bytes (so range readers can fetch exactly the header). Throws if not HCA.
uint16_t hca_header_size(std::span<const uint8_t> first8);

// Parses the header chunks; `bytes` must hold at least header_size bytes. Throws on malformed / unknown chunks.
HcaHeader parse_hca_header(std::span<const uint8_t> bytes);

}  // namespace dcp::formats
