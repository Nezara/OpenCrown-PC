#include "hca_decoder.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <stdexcept>

namespace dcp::formats {
namespace {

constexpr unsigned kN = kHcaSubframeSamples, kHalf = kN / 2;

// Rising half of the 256-point MDCT window. Only 64 points are free: the window satisfies Princen-Bradley,
// w[i]^2 + w[127 - i]^2 = 1, so the rest is derived. No formula reproduces them, so they are not shipped: the
// port installs them from the user's own eboot (hca_load_window_from_eboot) before decoding.
std::array<float, kHcaSubframeSamples> g_window{};
bool g_window_installed = false;

// Resolution (quantiser size) for a "curve position" = noise level - 2.5 * scalefactor; positions beyond the table
// mean 0 (then clamped to the stream's minimum).
constexpr uint8_t kResolutionCurve[57] = {
    14, 14, 14, 14, 14, 14, 13, 13, 13, 13, 13, 13, 12, 12, 12, 12, 12, 12, 11, 11, 11, 11, 11, 11, 10, 10, 10, 10, 10,
    10, 10, 9,  9,  9,  9,  9,  9,  8,  8,  8,  8,  8,  8,  7,  6,  6,  5,  4,  4,  4,  3,  3,  3,  2,  2,  2,  2,
};

// Coefficient code size per resolution: resolutions 1-7 use a prefix code of max_bits, 8-15 sign + magnitude.
constexpr uint8_t kMaxBits[16] = {0, 2, 3, 3, 4, 4, 4, 4, 5, 6, 7, 8, 9, 10, 11, 12};
// Prefix codes for resolution r < 8 list values 0, +1, -1, +2, -2, ... in code order; the first kShortCodes[r]
// values use max_bits - 1 bits, the rest max_bits.
constexpr uint8_t kShortCodes[8] = {0, 1, 3, 1, 7, 5, 3, 1};

struct Tables {
    std::array<float, 64> scaling;     // scalefactor -> amplitude
    std::array<float, 16> step;        // resolution -> quantiser step
    std::array<float, 128> hfr;        // HFR scale difference (+63) -> ratio
    std::array<float, 16> intensity;   // intensity index -> left ratio
    std::array<uint8_t, 128> code_bits;
    std::array<int8_t, 128> code_value;
    std::array<std::complex<float>, 64> pre, post;  // DCT-IV twiddles
    std::array<std::complex<float>, 32> fft_tw;
    std::array<uint8_t, 64> bitrev;
};

const Tables& tables() {
    static const Tables t = [] {
        Tables t{};
        // Scalefactors are a log scale of 53/128 octave steps (~2.5 dB), 63 -> sqrt(128).
        const double step_db = 53.0 / 128.0;
        for (int i = 0; i < 64; ++i) t.scaling[size_t(i)] = float(std::sqrt(128.0) * std::exp2((i - 63) * step_db));
        for (int i = 1; i < 128; ++i) t.hfr[size_t(i)] = float(std::exp2((i - 63) * step_db));
        // Resolution r quantises to 2r+1 levels (r < 8) or 2^(r-3)-1 levels; step = 2 / levels.
        for (int r = 1; r < 16; ++r) t.step[size_t(r)] = float(2.0 / (r < 8 ? 2 * r + 1 : (1 << (r - 3)) - 1));
        for (int i = 0; i < 15; ++i) t.intensity[size_t(i)] = float((14 - i) / 7.0);
        for (unsigned r = 1; r < 8; ++r) {
            unsigned code = 0;
            for (unsigned j = 0; j < 2 * r + 1; ++j) {
                int v = j == 0 ? 0 : (j & 1 ? int(j + 1) / 2 : -int(j / 2));
                unsigned len = j < kShortCodes[r] ? kMaxBits[r] - 1u : kMaxBits[r];
                for (unsigned c = 0; c < 1u << (kMaxBits[r] - len); ++c, ++code) {
                    t.code_bits[r << 4 | code] = uint8_t(len);
                    t.code_value[r << 4 | code] = int8_t(v);
                }
            }
        }
        const double pi = std::acos(-1.0);
        for (unsigned i = 0; i < 64; ++i) {
            t.pre[i] = std::polar(1.0f, float(pi * i / kN));
            t.post[i] = std::polar(1.0f, float(pi * (i + 0.25) / kN));
            unsigned r = 0;
            for (unsigned b = 0; b < 6; ++b) r |= (i >> b & 1) << (5 - b);
            t.bitrev[i] = uint8_t(r);
        }
        for (unsigned i = 0; i < 32; ++i) t.fft_tw[i] = std::polar(1.0f, float(2 * pi * i / 64));
        return t;
    }();
    return t;
}

}  // namespace

HcaBitReader::HcaBitReader(std::span<const uint8_t> bytes, size_t limit_bits)
    : b_(bytes), limit_(std::min(limit_bits, bytes.size() * 8)) {}

uint32_t HcaBitReader::peek(unsigned n) const {
    uint32_t v = 0;
    size_t byte = pos_ >> 3;
    for (unsigned i = 0; i < 4; ++i) v = v << 8 | (byte + i < b_.size() ? b_[byte + i] : 0);
    return n == 0 ? 0 : (v << (pos_ & 7)) >> (32 - n);
}

void hca_dct4(const float* in, float* out) {
    // Even/odd input pairs fold into a 64-point complex inverse FFT with pre/post rotation:
    // z[m] = (x[2m] - i x[127-2m]) e^(i pi m/128); U = IFFT64(z) e^(i pi (p+1/4)/128); y[2p] = Re U, y[127-2p] = Im U.
    const Tables& t = tables();
    std::array<std::complex<float>, 64> z;
    for (unsigned m = 0; m < 64; ++m) z[t.bitrev[m]] = std::complex<float>(in[2 * m], -in[kN - 1 - 2 * m]) * t.pre[m];
    for (unsigned len = 2; len <= 64; len <<= 1) {
        unsigned stride = 64 / len;
        for (unsigned s = 0; s < 64; s += len)
            for (unsigned k = 0; k < len / 2; ++k) {
                std::complex<float> a = z[s + k], b = z[s + k + len / 2] * t.fft_tw[k * stride];
                z[s + k] = a + b;
                z[s + k + len / 2] = a - b;
            }
    }
    for (unsigned p = 0; p < 64; ++p) {
        std::complex<float> u = z[p] * t.post[p];
        out[2 * p] = u.real();
        out[kN - 1 - 2 * p] = u.imag();
    }
}

const char* hca_frame_status_name(HcaFrameStatus s) {
    switch (s) {
        case HcaFrameStatus::ok: return "ok";
        case HcaFrameStatus::bad_crc: return "CRC mismatch";
        case HcaFrameStatus::bad_sync: return "bad sync";
        case HcaFrameStatus::unpack_error: return "unpack error";
    }
    return "?";
}

void hca_install_window(std::span<const float, 64> rising_half) {
    for (unsigned i = 0; i < 64; ++i) {
        g_window[i] = rising_half[i];
        g_window[kN - 1 - i] = float(std::sqrt(1.0 - double(rising_half[i]) * rising_half[i]));
    }
    g_window_installed = true;
}

bool hca_window_installed() { return g_window_installed; }

void hca_load_window_from_eboot(const EbootImage& eboot) {
    // Location in DCP's US eboot (CRI runtime table, 64 LE floats) + FNV-1a-64 of those bytes. Only the address and
    // hash ship; the values come from the user's own copy.
    constexpr uint64_t kVaddr = 0x7b6de0, kFnv = 0x96356e36aeb81569ull;
    auto b = eboot.read(kVaddr, 64 * 4);
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t c : b) h = (h ^ c) * 0x100000001b3ull;
    if (h != kFnv) throw std::runtime_error("hca: MDCT window not found in this eboot (unsupported build?)");
    std::array<float, 64> w;
    std::memcpy(w.data(), b.data(), b.size());
    hca_install_window(w);
}

HcaDecoder::HcaDecoder(const HcaHeader& h) : h_(h) {
    if (!g_window_installed) throw std::runtime_error("hca: MDCT window not installed (load it from the user's eboot)");
    if (h.version > 0x200) throw std::runtime_error("hca: version > 2.0 not supported");
    // Without an ath chunk v1.3 streams default to type 1 (a sample-rate curve); v2.0 to type 0.
    if (h.has_ath ? h.ath_type != 0 : h.version < 0x200) throw std::runtime_error("hca: ATH type 1 not supported");
    if (h.cipher_type != 0 && h.cipher_type != 1) throw std::runtime_error("hca: keyed cipher (56) not supported");
    if (h.block_size < 8 || h.has_vbr) throw std::runtime_error("hca: VBR / tiny blocks not supported");
    if (h.min_resolution > h.max_resolution || h.max_resolution > 15) throw std::runtime_error("hca: bad resolution range");
    if (h.total_band_count > 128 || h.base_band_count + h.stereo_band_count > h.total_band_count)
        throw std::runtime_error("hca: bad band counts");
    if (h.track_count == 0 || h.channels % h.track_count) throw std::runtime_error("hca: bad track count");

    unsigned hfr_bands = h.total_band_count - h.base_band_count - h.stereo_band_count;
    if (h.bands_per_hfr_group) hfr_group_count_ = (hfr_bands + h.bands_per_hfr_group - 1) / h.bands_per_hfr_group;

    // Cipher 1: fixed byte substitution from an LCG (x*13+11), skipping 0 and 0xFF which map to themselves.
    for (unsigned i = 0; i < 256; ++i) cipher_[i] = uint8_t(i);
    if (h.cipher_type == 1) {
        unsigned v = 0;
        for (unsigned i = 1; i < 255; ++i) {
            v = (v * 13 + 11) & 0xFF;
            if (v == 0 || v == 0xFF) v = (v * 13 + 11) & 0xFF;
            cipher_[i] = uint8_t(v);
        }
    }

    // Stereo pairing per track: which channels carry intensity-coded bands.
    ch_.resize(h.channels);
    unsigned per_track = h.channels / h.track_count;
    std::vector<ChannelType> layout(per_track, kDiscrete);
    if (h.stereo_band_count && per_track > 1) {
        const ChannelType P = kStereoPrimary, S = kStereoSecondary, D = kDiscrete;
        switch (per_track) {
            case 2: layout = {P, S}; break;
            case 3: layout = {P, S, D}; break;
            case 4: layout = h.channel_config ? std::vector{P, S, D, D} : std::vector{P, S, P, S}; break;
            case 5: layout = h.channel_config > 2 ? std::vector{P, S, D, P, S} : std::vector{P, S, D, D, D}; break;
            case 6: layout = {P, S, D, D, P, S}; break;
            case 7: layout = {P, S, D, D, P, S, D}; break;
            case 8: layout = {P, S, D, D, P, S, P, S}; break;
            default: break;
        }
    }
    for (unsigned c = 0; c < h.channels; ++c) {
        ch_[c].type = layout[c % per_track];
        ch_[c].coded_count = h.base_band_count + (ch_[c].type == kStereoSecondary ? 0 : h.stereo_band_count);
    }
    frame_.resize(h.block_size);
}

void HcaDecoder::reset() {
    for (auto& c : ch_) c.prev.fill(0.0f);
}

bool HcaDecoder::unpack(HcaBitReader& br) {
    const Tables& t = tables();
    if (br.read(16) != 0xFFFF) return false;
    int noise_level = int(br.read(9)) << 8;
    noise_level -= int(br.read(7));

    for (auto& c : ch_) {
        // Scalefactors: 6-bit absolute, or deltas of `delta_bits` with the all-ones code escaping to absolute.
        unsigned delta_bits = br.read(3);
        c.scalefactors.fill(0);
        if (delta_bits >= 6) {
            for (unsigned i = 0; i < c.coded_count; ++i) c.scalefactors[i] = uint8_t(br.read(6));
        } else if (delta_bits > 0) {
            int escape = (1 << delta_bits) - 1, value = int(br.read(6));
            if (c.coded_count) c.scalefactors[0] = uint8_t(value);
            for (unsigned i = 1; i < c.coded_count; ++i) {
                int d = int(br.read(delta_bits));
                value = d == escape ? int(br.read(6)) : value + d - (escape >> 1);
                if (value < 0 || value > 63) return false;
                c.scalefactors[i] = uint8_t(value);
            }
        }
        if (c.type == kStereoSecondary) {
            // A leading 15 is not consumed and only replaces subframe 0's index (ratio 0); the rest carry over.
            if (br.peek(4) < 15)
                for (auto& v : c.intensity) v = uint8_t(br.read(4));
            else
                c.intensity[0] = 15;
        } else {
            for (unsigned g = 0; g < hfr_group_count_; ++g) c.scalefactors[128 - hfr_group_count_ + g] = uint8_t(br.read(6));
        }

        for (unsigned i = 0; i < c.coded_count; ++i) {
            int res = 0, sf = c.scalefactors[i];
            if (sf > 0) {
                int pos = ((noise_level + int(i)) >> 8) + 1 - ((5 * sf) >> 1);  // ATH type 0: no curve offset
                res = pos < 0 ? 15 : pos < int(sizeof kResolutionCurve) ? kResolutionCurve[pos] : 0;
                res = std::clamp(res, int(h_.min_resolution), int(h_.max_resolution));
            }
            c.resolution[i] = uint8_t(res);
            c.gain[i] = t.scaling[size_t(sf)] * t.step[size_t(res)];
        }
    }

    for (unsigned sub = 0; sub < kHcaSubframes; ++sub)
        for (auto& c : ch_) {
            auto& s = c.spectra[sub];
            for (unsigned i = 0; i < c.coded_count; ++i) {
                unsigned res = c.resolution[i];
                int q = 0;
                if (res >= 8) {
                    uint32_t code = br.read(kMaxBits[res]);
                    q = int(code >> 1);
                    if (q == 0) br.skip(-1);  // zero has no sign bit
                    else if (code & 1) q = -q;
                } else if (res > 0) {
                    unsigned idx = res << 4 | br.peek(kMaxBits[res]);
                    br.skip(t.code_bits[idx]);
                    q = t.code_value[idx];
                }
                s[i] = float(q) * c.gain[i];
            }
            std::fill(s.begin() + c.coded_count, s.end(), 0.0f);
        }
    return !br.overrun();
}

void HcaDecoder::synthesize(std::span<float> out) {
    const Tables& t = tables();
    const unsigned total = h_.total_band_count, base = h_.base_band_count, nch = h_.channels;
    const unsigned hfr_start = base + h_.stereo_band_count;
    std::array<float, kN> y;
    for (unsigned sub = 0; sub < kHcaSubframes; ++sub) {
        // High-frequency reconstruction: mirror bands below the cutoff upwards, rescaled per group.
        if (hfr_group_count_)
            for (auto& c : ch_) {
                if (c.type == kStereoSecondary) continue;
                auto& s = c.spectra[sub];
                int high = int(hfr_start), low = int(hfr_start) - 1;
                for (unsigned g = 0; g < hfr_group_count_; ++g)
                    for (unsigned i = 0; i < h_.bands_per_hfr_group && high < int(total) && low >= 0; ++i, ++high, --low) {
                        int idx = int(c.scalefactors[128 - hfr_group_count_ + g]) - int(c.scalefactors[size_t(low)]) + 63;
                        s[size_t(high)] = t.hfr[size_t(std::clamp(idx, 0, 127))] * s[size_t(low)];
                    }
                s[kN - 1] = 0.0f;
            }
        // Intensity stereo: the secondary channel's upper bands are the primary's, split by a ratio. The CRI runtime
        // in the eboot computes 2 - l (FUN_00632780); ffmpeg uses l - 2, i.e. inverts those bands on the right.
        for (unsigned c = 0; c + 1 < nch; ++c) {
            if (ch_[c].type != kStereoPrimary) continue;
            float l = t.intensity[ch_[c + 1].intensity[sub] & 15], r = 2.0f - l;
            auto &sl = ch_[c].spectra[sub], &sr = ch_[c + 1].spectra[sub];
            for (unsigned b = base; b < total; ++b) {
                sr[b] = sl[b] * r;
                sl[b] *= l;
            }
        }
        // IMDCT: DCT-IV, unfold to 256 points (first half odd-, second half even-symmetric), window, overlap-add.
        for (unsigned c = 0; c < nch; ++c) {
            Channel& ch = ch_[c];
            hca_dct4(ch.spectra[sub].data(), y.data());
            for (float& v : y) v *= -0.125f;  // codec's output gain and polarity (exact: power of two)
            float* o = out.data() + size_t(sub) * kN * nch + c;
            for (unsigned i = 0; i < kHalf; ++i) {
                o[size_t(i) * nch] = g_window[i] * y[kHalf + i] + ch.prev[i];
                o[size_t(kHalf + i) * nch] = ch.prev[kHalf + i] - g_window[kHalf + i] * y[kN - 1 - i];
            }
            for (unsigned i = 0; i < kHalf; ++i) {
                ch.prev[i] = -g_window[kN - 1 - i] * y[kHalf - 1 - i];
                ch.prev[kHalf + i] = -g_window[kHalf - 1 - i] * y[i];
            }
        }
    }
}

HcaFrameStatus HcaDecoder::decode_frame(std::span<const uint8_t> frame, std::span<float> out) {
    if (out.size() < size_t(kHcaFrameSamples) * h_.channels) throw std::invalid_argument("hca: output too small");
    auto fail = [&](HcaFrameStatus s) {
        std::fill(out.begin(), out.begin() + ptrdiff_t(kHcaFrameSamples * h_.channels), 0.0f);
        reset();
        return s;
    };
    if (frame.size() < h_.block_size) return fail(HcaFrameStatus::unpack_error);
    frame = frame.first(h_.block_size);
    if (hca_crc16(frame) != 0) return fail(HcaFrameStatus::bad_crc);
    for (size_t i = 0; i < frame.size(); ++i) frame_[i] = cipher_[frame[i]];
    // The sync word is outside the cipher's reach (0xFF maps to itself), so check it before unpacking.
    HcaBitReader br(frame_, (size_t(h_.block_size) - 2) * 8);
    if (br.peek(16) != 0xFFFF) return fail(HcaFrameStatus::bad_sync);
    if (!unpack(br)) return fail(HcaFrameStatus::unpack_error);
    synthesize(out);
    return HcaFrameStatus::ok;
}

uint64_t HcaPcm::playable_samples() const {
    uint64_t all = uint64_t(header.block_count) * kHcaFrameSamples, trim = header.encoder_delay + header.encoder_padding;
    return all > trim ? all - trim : 0;
}

HcaPcm decode_hca(std::span<const uint8_t> stream, bool keep_samples) {
    HcaPcm pcm;
    if (stream.size() < 8) throw std::runtime_error("hca: stream too small");
    uint16_t hs = hca_header_size(stream.first(8));
    if (hs > stream.size()) throw std::runtime_error("hca: truncated header");
    pcm.header = parse_hca_header(stream.first(hs));
    if (!pcm.header.crc_ok) throw std::runtime_error("hca: header CRC mismatch");
    HcaDecoder dec(pcm.header);
    const size_t per_frame = size_t(kHcaFrameSamples) * pcm.header.channels;
    std::vector<float> scratch(keep_samples ? 0 : per_frame);
    if (keep_samples) pcm.samples.resize(per_frame * pcm.header.block_count);
    for (uint32_t f = 0; f < pcm.header.block_count; ++f) {
        std::span<float> out = keep_samples ? std::span<float>(pcm.samples).subspan(f * per_frame, per_frame)
                                            : std::span<float>(scratch);
        size_t off = hs + size_t(f) * pcm.header.block_size;
        auto frame = off < stream.size() ? stream.subspan(off) : std::span<const uint8_t>{};
        HcaFrameStatus s = dec.decode_frame(frame, out);
        ++pcm.frames;
        if (s == HcaFrameStatus::bad_crc) ++pcm.crc_failures;
        else if (s != HcaFrameStatus::ok) ++pcm.decode_failures;
    }
    return pcm;
}

HcaLoopSamples hca_loop_samples(const HcaHeader& h) {
    if (!h.has_loop) return {};
    auto at = [&](uint64_t s) { return s > h.encoder_delay ? s - h.encoder_delay : 0; };
    return {at(uint64_t(h.loop_start) * kHcaFrameSamples + h.loop_start_delay),
            at((uint64_t(h.loop_end) + 1) * kHcaFrameSamples - h.loop_end_padding)};
}

}  // namespace dcp::formats
