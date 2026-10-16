// CRI HCA frame decoder: frames -> interleaved float PCM. Written from a description of the format (frame layout,
// scalefactor / resolution / intensity / HFR scheme, MDCT); numeric constants are checked against the tables of the
// CRI runtime linked into the game. Supports versions up to 2.0 (all DCP streams), cipher 0 and 1 (1 has no DCP
// sample to test against), ATH type 0. Output gain / polarity match ffmpeg's hca decoder (~1e-6); intensity-stereo
// bands follow the game's runtime, which differs from ffmpeg (see synthesize()). Loop chunks are metadata only.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "eboot_image.h"
#include "hca_header.h"

namespace dcp::formats {

inline constexpr unsigned kHcaSubframes = 8, kHcaSubframeSamples = 128;
inline constexpr unsigned kHcaFrameSamples = kHcaSubframes * kHcaSubframeSamples;  // per channel

// MSB-first bit reader over one frame. Reads past the end yield zero bits and set overrun().
class HcaBitReader {
public:
    explicit HcaBitReader(std::span<const uint8_t> bytes, size_t limit_bits = SIZE_MAX);
    uint32_t peek(unsigned n) const;  // n <= 24
    uint32_t read(unsigned n) { uint32_t v = peek(n); pos_ += n; return v; }
    void skip(int n) { pos_ = size_t(ptrdiff_t(pos_) + n); }
    size_t position() const { return pos_; }
    bool overrun() const { return pos_ > limit_; }

private:
    std::span<const uint8_t> b_;
    size_t pos_ = 0, limit_;
};

// Unnormalised DCT-IV of 128 points: out[k] = sum_n in[n] cos(pi/128 (n + 1/2)(k + 1/2)). Fast (FFT-based).
void hca_dct4(const float* in, float* out);

enum class HcaFrameStatus { ok, bad_crc, bad_sync, unpack_error };
const char* hca_frame_status_name(HcaFrameStatus s);

class HcaDecoder {
public:
    explicit HcaDecoder(const HcaHeader& h);  // throws on versions / ciphers / ATH / layouts it cannot decode

    const HcaHeader& header() const { return h_; }
    // Decodes one block_size-byte frame to kHcaFrameSamples * channels interleaved floats. On failure the output is
    // silence and the overlap state is cleared.
    HcaFrameStatus decode_frame(std::span<const uint8_t> frame, std::span<float> out);
    void reset();  // clear the overlap state (e.g. before seeking)

private:
    enum ChannelType : uint8_t { kDiscrete = 0, kStereoPrimary = 1, kStereoSecondary = 2 };
    struct Channel {
        ChannelType type = kDiscrete;
        unsigned coded_count = 0;
        std::array<uint8_t, 128> scalefactors{}, resolution{};
        std::array<uint8_t, kHcaSubframes> intensity{};
        std::array<float, 128> gain{}, prev{};
        std::array<std::array<float, 128>, kHcaSubframes> spectra{};
    };
    bool unpack(HcaBitReader& br);
    void synthesize(std::span<float> out);

    HcaHeader h_;
    unsigned hfr_group_count_ = 0;
    std::array<uint8_t, 256> cipher_{};
    std::vector<Channel> ch_;
    std::vector<uint8_t> frame_;  // deciphered copy
};

// The 64-point rising half of the MDCT window is a CRI constant with no known formula, so it is not shipped.
// Install it once before constructing decoders: from the user's eboot (checked by hash), or any window in tests.
void hca_install_window(std::span<const float, 64> rising_half);
void hca_load_window_from_eboot(const EbootImage& eboot);  // throws if this eboot does not carry the expected table
bool hca_window_installed();

// Whole-stream convenience (header + frames in memory).
struct HcaPcm {
    HcaHeader header;
    std::vector<float> samples;  // every frame, interleaved; encoder delay / padding not removed (see below)
    size_t frames = 0, crc_failures = 0, decode_failures = 0;  // decode_failures: bad sync or unpack error
    // Playable range in per-channel samples: [encoder_delay, encoder_delay + playable_samples()).
    uint64_t playable_samples() const;
};
// Throws on a bad header or an unsupported stream; frames missing at the end of the stream count as decode failures.
HcaPcm decode_hca(std::span<const uint8_t> stream, bool keep_samples = true);

// Loop points of a stream with a loop chunk, in playable-sample space (encoder delay removed); end is exclusive.
struct HcaLoopSamples { uint64_t start = 0, end = 0; };
HcaLoopSamples hca_loop_samples(const HcaHeader& h);

}  // namespace dcp::formats
