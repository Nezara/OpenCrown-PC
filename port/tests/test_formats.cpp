// Format reader tests using synthetic data only (no game files needed).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

#include "formats/acb.h"
#include "formats/bsb.h"
#include "formats/afs2.h"
#include "formats/crilayla.h"
#include "formats/dds.h"
#include "formats/ftx.h"
#include "formats/gnm_detile.h"
#include "formats/hca_decoder.h"
#include "formats/hca_header.h"
#include "formats/mbs.h"
#include "formats/mcb.h"
#include "formats/utf_table.h"

using namespace dcp::formats;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { ++failures; std::fprintf(stderr, "%s:%d CHECK(%s)\n", __FILE__, __LINE__, #c); } } while (0)

namespace {

void put32(std::vector<uint8_t>& b, uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s)); }
void put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }

// Table "T": col A u32 per-row, col B string constant, col C u16 zero, col D data per-row. 2 rows.
std::vector<uint8_t> make_table() {
    std::string strings = std::string("<NULL>\0T\0A\0B\0C\0D\0hello\0row1\0", 29);
    auto soff = [&](const char* s) { return uint32_t(strings.find(std::string(s) + '\0')); };
    std::vector<uint8_t> schema;
    schema.push_back(0x54); put32(schema, soff("A"));
    schema.push_back(0x3A); put32(schema, soff("B")); put32(schema, soff("hello"));
    schema.push_back(0x12); put32(schema, soff("C"));
    schema.push_back(0x5B); put32(schema, soff("D"));
    std::vector<uint8_t> rows;
    put32(rows, 7); put32(rows, 0); put32(rows, 3);
    put32(rows, 0xDEADBEEF); put32(rows, 3); put32(rows, 2);
    std::vector<uint8_t> data = {1, 2, 3, 4, 5};

    uint32_t rows_off = 24 + uint32_t(schema.size());
    uint32_t str_off = rows_off + uint32_t(rows.size());
    uint32_t data_off = str_off + uint32_t(strings.size());
    std::vector<uint8_t> body;
    put16(body, 0); put16(body, uint16_t(rows_off));
    put32(body, str_off); put32(body, data_off); put32(body, soff("T"));
    put16(body, 4); put16(body, 12); put32(body, 2);
    body.insert(body.end(), schema.begin(), schema.end());
    body.insert(body.end(), rows.begin(), rows.end());
    body.insert(body.end(), strings.begin(), strings.end());
    body.insert(body.end(), data.begin(), data.end());

    std::vector<uint8_t> t = {'@', 'U', 'T', 'F'};
    put32(t, uint32_t(body.size()));
    t.insert(t.end(), body.begin(), body.end());
    return t;
}

void test_utf(const std::vector<uint8_t>& bytes) {
    UtfTable t(bytes);
    CHECK(t.name() == "T");
    CHECK(t.rows() == 2 && t.columns() == 4);
    CHECK(t.u64(0, "A") == 7 && t.u64(1, "A") == 0xDEADBEEF);
    CHECK(t.str(1, "B") == "hello");
    CHECK(t.u64(0, "C", 99) == 99);
    auto d0 = t.data(0, "D"), d1 = t.data(1, "D");
    CHECK(d0.size() == 3 && d0[0] == 1 && d0[2] == 3);
    CHECK(d1.size() == 2 && d1[0] == 4 && d1[1] == 5);
    CHECK(!t.find_column("missing"));
}

// Writes bits in decoder consumption order, then reverses bytes into CRILAYLA's backwards layout.
struct BitWriter {
    std::vector<uint8_t> bytes;
    int used = 8;
    void put(uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            if (used == 8) { bytes.push_back(0); used = 0; }
            bytes.back() |= uint8_t(((v >> i) & 1) << (7 - used++));
        }
    }
};

void test_crilayla() {
    // Body (in output order) "ABCABCABCX": decoder emits from the end, so X, then C,B,A literals, then a 6-byte copy.
    const std::string body = "ABCABCABCX";
    BitWriter w;
    for (char c : std::string("XCBA")) { w.put(0, 1); w.put(uint8_t(c), 8); }
    // After 4 bytes out, copy 6 bytes from distance 3 back: src = end - done + off + 3 => off = 0 points to 'C' of "ABC".
    w.put(1, 1); w.put(0, 13); w.put(3, 2); w.put(0, 3);  // len = 3 + 3 + 0 = 6
    std::vector<uint8_t> stream(w.bytes.rbegin(), w.bytes.rend());

    std::vector<uint8_t> in(16);
    std::memcpy(in.data(), "CRILAYLA", 8);
    uint32_t sz = uint32_t(body.size()), off = uint32_t(stream.size());
    std::memcpy(in.data() + 8, &sz, 4);
    std::memcpy(in.data() + 12, &off, 4);
    in.insert(in.end(), stream.begin(), stream.end());
    for (int i = 0; i < 0x100; ++i) in.push_back(uint8_t(i));

    auto out = crilayla_decompress(in);
    CHECK(out.size() == 0x100 + body.size());
    CHECK(out[0] == 0 && out[0xFF] == 0xFF);
    CHECK(std::string(out.begin() + 0x100, out.end()) == body);

    in.resize(20);
    bool threw = false;
    try { crilayla_decompress(in); } catch (...) { threw = true; }
    CHECK(threw);
}

// Generic per-row @UTF builder for the nested ACB tables.
using Cell = std::variant<uint64_t, std::string, std::vector<uint8_t>>;
struct Col { std::string name; uint8_t type; std::vector<Cell> vals; };  // type: 0 u8, 2 u16, 4 u32, 10 str, 11 data

std::vector<uint8_t> build_utf(const std::string& name, const std::vector<Col>& cols) {
    uint32_t nrows = cols.empty() ? 0 : uint32_t(cols[0].vals.size());
    std::string strings("<NULL>\0", 7);
    auto intern = [&](const std::string& v) {
        uint32_t off = uint32_t(strings.size());
        strings += v;
        strings.push_back('\0');
        return off;
    };
    uint32_t name_off = intern(name);
    std::vector<uint8_t> schema, rows, data;
    uint16_t row_len = 0;
    for (const auto& c : cols) {
        schema.push_back(uint8_t(0x50 | c.type));
        put32(schema, intern(c.name));
        row_len += c.type == 0 ? 1 : c.type == 2 ? 2 : c.type == 11 ? 8 : 4;
    }
    for (uint32_t r = 0; r < nrows; ++r)
        for (const auto& c : cols) {
            const Cell& v = c.vals[r];
            switch (c.type) {
                case 0: rows.push_back(uint8_t(std::get<uint64_t>(v))); break;
                case 2: put16(rows, uint16_t(std::get<uint64_t>(v))); break;
                case 4: put32(rows, uint32_t(std::get<uint64_t>(v))); break;
                case 10: put32(rows, intern(std::get<std::string>(v))); break;
                case 11: {
                    const auto& d = std::get<std::vector<uint8_t>>(v);
                    put32(rows, uint32_t(data.size()));
                    put32(rows, uint32_t(d.size()));
                    data.insert(data.end(), d.begin(), d.end());
                    break;
                }
            }
        }
    uint32_t rows_off = 24 + uint32_t(schema.size());
    uint32_t str_off = rows_off + uint32_t(rows.size());
    uint32_t data_off = str_off + uint32_t(strings.size());
    std::vector<uint8_t> body;
    put16(body, 0); put16(body, uint16_t(rows_off));
    put32(body, str_off); put32(body, data_off); put32(body, name_off);
    put16(body, uint16_t(cols.size())); put16(body, row_len); put32(body, nrows);
    body.insert(body.end(), schema.begin(), schema.end());
    body.insert(body.end(), rows.begin(), rows.end());
    body.insert(body.end(), strings.begin(), strings.end());
    body.insert(body.end(), data.begin(), data.end());
    std::vector<uint8_t> t = {'@', 'U', 'T', 'F'};
    put32(t, uint32_t(body.size()));
    t.insert(t.end(), body.begin(), body.end());
    return t;
}

void putle(std::vector<uint8_t>& b, uint64_t v, int n) { for (int i = 0; i < n; ++i) b.push_back(uint8_t(v >> 8 * i)); }

// AFS2 with the given ids/payloads; the offset table holds unaligned ends, streams start aligned.
std::vector<uint8_t> make_afs2(const std::vector<uint16_t>& ids, const std::vector<std::string>& payloads, int offsz,
                               uint16_t align) {
    size_t n = ids.size();
    std::vector<uint8_t> b = {'A', 'F', 'S', '2', 1, uint8_t(offsz), 2, 0};
    putle(b, n, 4); putle(b, align, 2); putle(b, 0x1234, 2);
    for (auto id : ids) putle(b, id, 2);
    uint64_t pos = b.size() + (n + 1) * offsz;
    std::vector<uint64_t> offs;
    for (const auto& p : payloads) {
        offs.push_back(pos);
        pos = (pos + align - 1) / align * align + p.size();
    }
    offs.push_back(pos);
    for (auto o : offs) putle(b, o, offsz);
    for (size_t i = 0; i < n; ++i) {
        b.resize((b.size() + align - 1) / align * align);
        b.insert(b.end(), payloads[i].begin(), payloads[i].end());
    }
    return b;
}

void test_afs2() {
    for (int offsz : {2, 4}) {
        auto b = make_afs2({5, 7, 9}, {"abc", "", "defgh"}, offsz, 32);
        CHECK(is_afs2(b));
        CHECK(afs2_header_size(b) == 16 + 3 * 2 + 4 * uint64_t(offsz));
        Afs2Header h = parse_afs2(b, b.size());
        CHECK(h.version == 1 && h.offset_size == offsz && h.id_size == 2 && h.align == 32 && h.subkey == 0x1234);
        CHECK(h.streams.size() == 3);
        CHECK(h.streams[0].id == 5 && h.streams[0].offset == (h.header_size + 31) / 32 * 32 && h.streams[0].size == 3);
        CHECK(h.streams[1].size == 0);
        const Afs2Stream* s = h.find(9);
        CHECK(s && s->offset % 32 == 0 && s->size == 5);
        auto d = afs2_stream(b, *s);
        CHECK(std::string(d.begin(), d.end()) == "defgh");
        CHECK(!h.find(6));
        bool threw = false;
        try { parse_afs2(b, b.size() - 1); } catch (...) { threw = true; }  // last stream past the end
        CHECK(threw);
    }
}

uint16_t crc16(const std::vector<uint8_t>& b) {
    uint16_t crc = 0;
    for (uint8_t c : b) {
        crc ^= uint16_t(c << 8);
        for (int i = 0; i < 8; ++i) crc = uint16_t(crc & 0x8000 ? crc << 1 ^ 0x8005 : crc << 1);
    }
    return crc;
}

void put_magic(std::vector<uint8_t>& b, const char* m, bool mask) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(m[i] | (mask && m[i] ? 0x80 : 0)));
}

std::vector<uint8_t> make_hca(bool use_dec, bool mask) {
    std::vector<uint8_t> b;
    put_magic(b, "HCA\0", mask);
    put16(b, 0x0200); put16(b, 0x60);
    put_magic(b, "fmt\0", mask);
    b.push_back(2); b.push_back(0x00); b.push_back(0xAC); b.push_back(0x44);  // 2 ch, 44100 Hz
    put32(b, 1234); put16(b, 0x80); put16(b, 0x20);
    if (use_dec) {
        put_magic(b, "dec\0", mask);
        put16(b, 0x200); b.push_back(1); b.push_back(15); b.push_back(0x7F); b.push_back(0x3F); b.push_back(0x12);
        b.push_back(1);
    } else {
        put_magic(b, "comp", mask);
        put16(b, 0x155); b.push_back(1); b.push_back(15); b.push_back(1); b.push_back(2);
        b.push_back(128); b.push_back(100); b.push_back(28); b.push_back(4); put16(b, 0);
    }
    put_magic(b, "loop", mask);
    put32(b, 10); put32(b, 900); put16(b, 0x80); put16(b, 0x40);
    put_magic(b, "ciph", mask);
    put16(b, 56);
    put_magic(b, "rva\0", mask);
    put32(b, 0x3F000000);  // 0.5f
    put_magic(b, "pad\0", mask);
    b.resize(0x60 - 2);
    put16(b, crc16(b));
    return b;
}

void test_hca() {
    auto b = make_hca(false, true);
    CHECK(is_hca(b) && hca_header_size(b) == 0x60);
    HcaHeader h = parse_hca_header(b);
    CHECK(h.masked && h.crc_ok);
    CHECK(h.version == 0x0200 && h.channels == 2 && h.sample_rate == 44100 && h.block_count == 1234);
    CHECK(h.encoder_delay == 0x80 && h.encoder_padding == 0x20);
    CHECK(h.has_comp && h.block_size == 0x155 && h.track_count == 1 && h.channel_config == 2);
    CHECK(h.total_band_count == 128 && h.base_band_count == 100 && h.stereo_band_count == 28);
    CHECK(h.has_loop && h.loop_start == 10 && h.loop_end == 900);
    CHECK(h.has_cipher && h.cipher_type == 56);
    CHECK(h.has_rva && h.rva_volume == 0.5f);

    auto d = make_hca(true, false);
    HcaHeader g = parse_hca_header(d);
    CHECK(!g.masked && g.crc_ok && g.has_dec && g.block_size == 0x200);
    CHECK(g.total_band_count == 128 && g.base_band_count == 64 && g.stereo_band_count == 64);
    CHECK(g.track_count == 1 && g.channel_config == 2);

    d[20] ^= 1;  // corrupt fmt: the CRC must catch it
    CHECK(!parse_hca_header(d).crc_ok);
    bool threw = false;
    try { parse_hca_header(std::span<const uint8_t>(b).first(0x40)); } catch (...) { threw = true; }
    CHECK(threw);
}

void test_hca_bitreader() {
    const uint8_t b[] = {0xA5, 0x3C, 0xFF};
    HcaBitReader br(b);
    CHECK(br.peek(0) == 0 && br.peek(4) == 0xA);
    CHECK(br.read(3) == 0x5 && br.read(9) == 0x053);  // 101 | 0 0101 0011
    CHECK(br.read(4) == 0xC && br.position() == 16);
    br.skip(-2);
    CHECK(br.read(2) == 0 && !br.overrun());
    CHECK(br.read(8) == 0xFF && !br.overrun());
    CHECK(br.read(5) == 0 && br.overrun());  // past the end: zeros, flagged
    HcaBitReader lim(b, 12);                // limit below the buffer size
    lim.read(12);
    CHECK(!lim.overrun());
    lim.read(1);
    CHECK(lim.overrun());
}

void test_hca_dct4() {
    const double pi = std::acos(-1.0);
    for (int k0 : {0, 1, 37, 127}) {  // impulses, compared with the defining sum
        float in[128] = {}, out[128];
        in[k0] = 1.0f;
        hca_dct4(in, out);
        double err = 0;
        for (int n = 0; n < 128; ++n) err = std::max(err, std::abs(out[n] - std::cos(pi / 128 * (k0 + 0.5) * (n + 0.5))));
        CHECK(err < 1e-5);
    }
    float in[128], out[128];
    for (int i = 0; i < 128; ++i) in[i] = float(std::sin(i * 0.7) + 0.25 * (i % 5));
    hca_dct4(in, out);
    double err = 0;
    for (int n = 0; n < 128; ++n) {
        double y = 0;
        for (int k = 0; k < 128; ++k) y += in[k] * std::cos(pi / 128 * (k + 0.5) * (n + 0.5));
        err = std::max(err, std::abs(out[n] - y));
    }
    CHECK(err < 1e-4);
}

// Minimal v2.0 stream: header (fmt, comp) + `frames` frames whose scalefactors are all zero (no coefficients coded).
std::vector<uint8_t> make_silent_hca(uint8_t channels, uint8_t stereo_bands, uint32_t frames, uint16_t block_size) {
    std::vector<uint8_t> b;
    put_magic(b, "HCA\0", false);
    put16(b, 0x0200); put16(b, 0x30);
    put_magic(b, "fmt\0", false);
    b.push_back(channels); b.push_back(0x00); b.push_back(0xBB); b.push_back(0x80);  // 48000 Hz
    put32(b, frames); put16(b, 0x80); put16(b, 0x20);
    put_magic(b, "comp", false);
    put16(b, block_size); b.push_back(1); b.push_back(15); b.push_back(1); b.push_back(channels == 1);
    b.push_back(128); b.push_back(uint8_t(64 - stereo_bands / 2)); b.push_back(stereo_bands); b.push_back(4); put16(b, 0);
    put_magic(b, "pad\0", false);
    b.resize(0x30 - 2);
    put16(b, crc16(b));
    for (uint32_t f = 0; f < frames; ++f) {
        // sync, noise level 0x10, boundary 0, then per channel delta_bits = 0 and either 8 HFR scales (6 bits)
        // or, on a stereo secondary, a 4-bit intensity of 15 that is not consumed. Zero bits all the way.
        std::vector<uint8_t> fr(block_size, 0);
        fr[0] = fr[1] = 0xFF;
        fr[2] = 0x10 >> 1; fr[3] = uint8_t(0x10 << 7);
        if (stereo_bands) fr[10] = 0x1E;  // ch0 = bits 32..82 (3 + 8 * 6), so ch1's intensity is bits 83..86
        fr.resize(block_size - 2);
        put16(fr, crc16(fr));
        b.insert(b.end(), fr.begin(), fr.end());
    }
    return b;
}

void test_hca_decoder() {
    {   // Refuses to decode before a window is installed (the real one comes from the user's eboot).
        bool threw = false;
        try { decode_hca(make_silent_hca(1, 0, 1, 0x40)); } catch (const std::exception&) { threw = true; }
        CHECK(threw && !hca_window_installed());
    }
    {   // Stand-in window for tests: sine, which satisfies the same Princen-Bradley condition.
        std::array<float, 64> w;
        for (unsigned i = 0; i < 64; ++i) w[i] = float(std::sin(3.14159265358979 / 256 * (2 * i + 1)));
        hca_install_window(w);
    }
    for (uint8_t stereo : {0, 1}) {
        auto s = make_silent_hca(uint8_t(1 + stereo), stereo ? 64 : 0, 3, 0x40);
        HcaPcm pcm = decode_hca(s);
        CHECK(pcm.frames == 3 && pcm.crc_failures == 0 && pcm.decode_failures == 0);
        CHECK(pcm.samples.size() == 3u * 1024 * (1 + stereo));
        CHECK(std::all_of(pcm.samples.begin(), pcm.samples.end(), [](float v) { return v == 0.0f; }));
        CHECK(pcm.playable_samples() == 3 * 1024 - 0x80 - 0x20);

        HcaDecoder dec(pcm.header);
        std::vector<float> out(2048, 1.0f);
        auto frame = std::span<const uint8_t>(s).subspan(0x30, 0x40);
        CHECK(dec.decode_frame(frame, out) == HcaFrameStatus::ok);
        auto bad = std::vector<uint8_t>(frame.begin(), frame.end());
        bad[20] ^= 0x01;
        CHECK(dec.decode_frame(bad, out) == HcaFrameStatus::bad_crc);
        bad = std::vector<uint8_t>(frame.begin(), frame.end());
        bad[0] = 0x7F;  // valid CRC, broken sync
        bad.resize(0x3E);
        put16(bad, crc16(bad));
        CHECK(dec.decode_frame(bad, out) == HcaFrameStatus::bad_sync);
        CHECK(out[0] == 0.0f && out[1023] == 0.0f);
    }
    // A stream cut short: the missing frames count as decode failures, not crashes.
    auto s = make_silent_hca(1, 0, 4, 0x40);
    s.resize(s.size() - 0x50);
    HcaPcm pcm = decode_hca(s);
    CHECK(pcm.frames == 4 && pcm.decode_failures == 2);
    // Unsupported variants are refused up front.
    HcaHeader h = pcm.header;
    h.cipher_type = 56;
    bool threw = false;
    try { HcaDecoder d(h); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

std::vector<uint8_t> bytes(std::initializer_list<int> v) {
    std::vector<uint8_t> b;
    for (int x : v) b.push_back(uint8_t(x));
    return b;
}

void test_acb() {
    using V = std::vector<Cell>;
    auto awb = make_afs2({0}, {"HCA"}, 4, 32);
    auto stream_hdr = make_afs2({3}, {"xxxx"}, 4, 32);
    stream_hdr.resize(size_t(afs2_header_size(stream_hdr)));

    // Cue 100 -> sequence 0 -> 2 tracks -> synth 0 -> waveforms 0, 1; cue 200 -> waveform 1; cue 300 -> synth 1 (type 7).
    auto cue = build_utf("Cue", {{"CueId", 4, V{100ull, 200ull, 300ull}},
                                 {"ReferenceType", 0, V{3ull, 1ull, 2ull}},
                                 {"ReferenceIndex", 2, V{0ull, 1ull, 1ull}},
                                 {"Length", 4, V{1000ull, 2000ull, 0ull}}});
    auto names = build_utf("CueName", {{"CueName", 10, V{std::string("C"), std::string("A"), std::string("B")}},
                                       {"CueIndex", 2, V{2ull, 0ull, 1ull}}});
    auto wave = build_utf("Waveform", {{"MemoryAwbId", 2, V{0ull, 0xFFFFull}},
                                       {"EncodeType", 0, V{2ull, 6ull}},
                                       {"Streaming", 0, V{0ull, 1ull}},
                                       {"NumChannels", 0, V{1ull, 2ull}},
                                       {"SamplingRate", 2, V{48000ull, 44100ull}},
                                       {"NumSamples", 4, V{10ull, 20ull}},
                                       {"StreamAwbPortNo", 2, V{0xFFFFull, 0ull}},
                                       {"StreamAwbId", 2, V{0xFFFFull, 3ull}}});
    auto synth = build_utf("Synth", {{"ReferenceItems", 11, V{bytes({0, 1, 0, 0, 0, 1, 0, 1}), bytes({0, 7, 0, 0})}}});
    auto seq = build_utf("Sequence", {{"NumTracks", 2, V{2ull}}, {"TrackIndex", 11, V{bytes({0, 0, 0, 1})}}});
    auto track = build_utf("Track", {{"EventIndex", 2, V{0ull, 1ull}}});
    // Event 0: an unrelated command, note-on synth 0, end marker. Event 1: note-on (2003) synth 0 again, deduplicated.
    auto events = build_utf("TrackEvent", {{"Command", 11, V{bytes({0x04, 0xB1, 4, 0, 0, 0xFF, 0xFF, 0x07, 0xD0, 4, 0, 2,
                                                                     0, 0, 0, 0, 0}),
                                                              bytes({0x07, 0xD3, 4, 0, 2, 0, 0})}}});
    auto streams = build_utf("StreamAwb", {{"Name", 10, V{std::string("bank")}}});
    auto shdr = build_utf("StreamAwbHeader", {{"Header", 11, V{stream_hdr}}});
    auto header = build_utf("Header", {{"Name", 10, V{std::string("test")}},
                                       {"CueTable", 11, V{cue}},
                                       {"CueNameTable", 11, V{names}},
                                       {"WaveformTable", 11, V{wave}},
                                       {"SynthTable", 11, V{synth}},
                                       {"SequenceTable", 11, V{seq}},
                                       {"TrackTable", 11, V{track}},
                                       {"TrackEventTable", 11, V{events}},
                                       {"AwbFile", 11, V{awb}},
                                       {"StreamAwbHash", 11, V{streams}},
                                       {"StreamAwbAfs2Header", 11, V{shdr}}});
    Acb a(header);
    CHECK(a.name() == "test");
    CHECK(a.cues().size() == 3 && a.waveforms().size() == 2);
    const auto& c0 = a.cues()[0];
    CHECK(c0.id == 100 && c0.name == "A" && c0.length_ms == 1000);
    CHECK(c0.waveforms.size() == 2 && c0.waveforms[0] == 0 && c0.waveforms[1] == 1);
    CHECK(a.cues()[1].name == "B" && a.cues()[1].waveforms.size() == 1 && a.cues()[1].waveforms[0] == 1);
    CHECK(a.cues()[2].name == "C" && a.cues()[2].waveforms.empty());
    CHECK(a.skipped().size() == 1 && a.skipped().begin()->first == "synth item type 7");
    const auto& w0 = a.waveforms()[0];
    const auto& w1 = a.waveforms()[1];
    CHECK(w0.in_memory() && !w0.in_stream() && w0.memory_awb_id == 0 && w0.channels == 1 && w0.sample_rate == 48000);
    CHECK(!w1.in_memory() && w1.in_stream() && w1.stream_awb_id == 3 && w1.encode_type == 6);
    CHECK(std::string(Acb::codec_name(w1.encode_type)) == "HCA-MX");
    CHECK(a.memory_awb().size() == awb.size() && is_afs2(a.memory_awb()));
    CHECK(a.stream_awb_names().size() == 1 && a.stream_awb_names()[0] == "bank");
    CHECK(a.stream_awb_header().size() == stream_hdr.size());
    auto sh = parse_afs2(a.stream_awb_header(), 1 << 20);
    CHECK(sh.streams.size() == 1 && sh.streams[0].id == 3);
}

// Reference GNM tiler, written independently of gnm_detile: walks linear elements and interleaves the in-tile
// coordinate bits (x0 y0 x1 y1 x2 y2) for Thin_1dThin; LinearAligned just pads rows to the pitch.
std::vector<uint8_t> gnm_tile_ref(const std::vector<uint8_t>& lin, const GnmSurface& s) {
    uint32_t ex = s.elems_x(), ey = s.elems_y(), pe = s.pitch_elems(), bpe = s.bytes_per_element;
    uint32_t rows = s.tile_index == 13 ? (ey + 7) / 8 * 8 : ey;
    std::vector<uint8_t> out(size_t(pe) * rows * bpe, 0xEE);
    for (uint32_t y = 0; y < ey; ++y)
        for (uint32_t x = 0; x < ex; ++x) {
            size_t dst;
            if (s.tile_index == 13) {
                uint32_t in = 0;
                for (int b = 0; b < 3; ++b) in |= (x >> b & 1) << (2 * b) | (y >> b & 1) << (2 * b + 1);
                dst = ((size_t(y / 8) * (pe / 8) + x / 8) * 64 + in) * bpe;
            } else {
                dst = (size_t(y) * pe + x) * bpe;
            }
            std::memcpy(&out[dst], &lin[(size_t(y) * ex + x) * bpe], bpe);
        }
    return out;
}

std::vector<uint8_t> pattern(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    for (auto& c : v) { seed = seed * 1103515245u + 12345u; c = uint8_t(seed >> 16); }
    return v;
}

void test_detile() {
    struct Case { uint32_t w, h, pitch, bw, bpe, tile; };
    const Case cases[] = {{64, 32, 64, 4, 16, 13},   // BC7, exact tiles
                          {100, 40, 128, 4, 16, 13}, // partial tiles in x and y (height pads to 32 px)
                          {40, 12, 64, 4, 8, 13},    // BC1-sized blocks
                          {24, 9, 32, 1, 4, 13},     // uncompressed RGBA8
                          {30, 5, 32, 1, 2, 8}};     // linear aligned
    for (const auto& c : cases) {
        GnmSurface s;
        s.width = c.w; s.height = c.h; s.pitch = c.pitch;
        s.block_w = s.block_h = c.bw; s.bytes_per_element = c.bpe; s.tile_index = c.tile;
        auto lin = pattern(size_t(gnm_linear_size(s)), c.w * 31 + c.h);
        auto tiled = gnm_tile_ref(lin, s);
        CHECK(gnm_tiled_size(s) == tiled.size());
        CHECK(gnm_detile(tiled, s) == lin);
    }
    // Known placement: element (1,2) of a 16-byte-element 8x8 tile is in-tile index 0b001001 = 9.
    GnmSurface s;
    s.width = s.height = s.pitch = 32; s.block_w = s.block_h = 4; s.bytes_per_element = 16; s.tile_index = 13;
    std::vector<uint8_t> t(1024, 0);
    t[9 * 16] = 0x77;
    CHECK(gnm_detile(t, s)[(2 * 8 + 1) * 16] == 0x77);
    bool threw = false;
    try { gnm_detile(std::vector<uint8_t>(1023), s); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    CHECK(!gnm_detile_supported(15, 16));

    // Thin_2dThin (tile 14), RGBA8 128x64 = one macro tile per 64 rows: placements from the eboot tiler's pipe/bank
    // bits (FUN_0054e950): (1,0) -> byte 4, (8,0) -> 0x100 (pipe 1), (0,8) -> 0x4100 (pipe 1, bank 8).
    GnmSurface m;
    m.width = m.pitch = 128; m.height = 64; m.bytes_per_element = 4; m.tile_index = 14;
    CHECK(gnm_detile_supported(14, 4));
    CHECK(gnm_tiled_size(m) == 128 * 64 * 4);
    std::vector<uint8_t> mt(128 * 64 * 4, 0);
    mt[4] = 0x11, mt[0x100] = 0x22, mt[0x4100] = 0x33;
    auto ml = gnm_detile(mt, m);
    CHECK(ml[(0 * 128 + 1) * 4] == 0x11);
    CHECK(ml[(0 * 128 + 8) * 4] == 0x22);
    CHECK(ml[(8 * 128 + 0) * 4] == 0x33);
}

void putle(std::vector<uint8_t>& b, uint32_t v) { for (int s = 0; s < 32; s += 8) b.push_back(uint8_t(v >> s)); }

// One GNF v2 (single T#, alignment 256) wrapping `pixels`.
std::vector<uint8_t> make_gnf(uint32_t w, uint32_t h, uint32_t pitch, uint32_t dfmt, const std::vector<uint8_t>& px) {
    std::vector<uint8_t> g = {'G', 'N', 'F', ' '};
    putle(g, 0xF8);
    g.insert(g.end(), {2, 1, 8, 0});
    putle(g, uint32_t(0x100 + px.size()));                        // whole GNF size
    putle(g, 0);                                                  // d0 data offset
    putle(g, 8u | dfmt << 20);                                    // d1 align, data format, UNORM
    putle(g, (w - 1) | (h - 1) << 14);                            // d2
    putle(g, 0xFACu | 13u << 20 | 9u << 28);                      // d3 identity swizzle, tile 13, 2D
    putle(g, (pitch - 1) << 13);                                  // d4
    putle(g, 0); putle(g, 0);
    putle(g, uint32_t(px.size()));                                // d7 size
    g.resize(8 + 0xF8, 0);
    g.insert(g.end(), px.begin(), px.end());
    return g;
}

void test_ftx() {
    auto px0 = pattern(64 * 32, 1);  // 64x32 BC7: 16x8 blocks * 16 B
    auto px1 = pattern(64 * 32 / 2, 2);  // 64x32 BC1 (8-byte blocks); width 40 sits inside pitch 64
    std::vector<std::vector<uint8_t>> gnfs = {make_gnf(64, 32, 64, 41, px0), make_gnf(40, 32, 64, 35, px1)};
    const char* names[] = {"A_arranged000.gnf", "B_arranged001.gnf"};

    std::vector<uint8_t> body;
    for (auto& g : gnfs) {
        std::vector<uint8_t> c = {'F', 'T', 'X', '0'};
        putle(c, uint32_t(g.size()));
        putle(c, 0x40);
        c.resize(0x40, 0);
        c.insert(c.end(), g.begin(), g.end());
        body.insert(body.end(), c.begin(), c.end());
    }
    std::vector<uint8_t> f = {'F', 'T', 'E', 'X'};
    putle(f, uint32_t(body.size()));
    putle(f, 0x80);
    f.push_back(2); f.push_back(0);
    f.resize(0x20, 0);
    for (int i = 0; i < 2; ++i) {
        size_t at = f.size();
        f.resize(at + 0x30, 0);
        std::memcpy(&f[at], names[i], std::strlen(names[i]));
        uint32_t sz = uint32_t(gnfs[size_t(i)].size());
        std::memcpy(&f[at + 0x24], &sz, 4);
    }
    f.resize(0x80, 0);
    f.insert(f.end(), body.begin(), body.end());
    f.insert(f.end(), {'F', 'E', 'O', 'C', 0, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0});

    CHECK(is_ftx(f));
    auto tx = parse_ftx(f);
    CHECK(tx.size() == 2);
    if (tx.size() == 2) {
        const auto& a = tx[0].tex;
        CHECK(tx[0].name == "A_arranged000.gnf" && tx[1].name == "B_arranged001.gnf");
        CHECK(a.width() == 64 && a.height() == 32 && a.pitch() == 64 && a.data_format() == 41 && a.num_format() == 0);
        CHECK(a.tile_index() == 13 && a.type() == 9 && a.mips() == 1 && a.slices() == 1 && a.dst_sel() == 0xFAC);
        CHECK(tx[0].pixels.size() == px0.size() && std::equal(px0.begin(), px0.end(), tx[0].pixels.begin()));
        CHECK(tx[1].tex.width() == 40 && tx[1].tex.data_format() == 35 && tx[1].pixels.size() == px1.size());
        CHECK(gnm_to_dxgi(41, 0) == 98 && gnm_to_dxgi(35, 0) == 71);
        auto lin = ftx_detile(tx[1]);
        CHECK(lin.size() == 10 * 8 * 8);
        auto dds = make_dds(40, 32, 71, lin);
        CHECK(dds.size() == 148 + lin.size() && std::memcmp(dds.data() + 84, "DX10", 4) == 0 && dds[128] == 71);
    }
    // Truncation and a bad child magic must be reported, not read past.
    auto bad = f;
    bad.resize(bad.size() - 100);
    bool threw = false;
    try { parse_ftx(bad); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    bad = f;
    bad[0x80] = 'Z';
    threw = false;
    try { parse_ftx(bad); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

void le16(std::vector<uint8_t>& b, size_t o, uint16_t v) { b[o] = uint8_t(v); b[o + 1] = uint8_t(v >> 8); }
void le32(std::vector<uint8_t>& b, size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) b[o + i] = uint8_t(v >> 8 * i); }

// One record in each decoded section, laid out in slot order (the game uses another order; offsets decide).
std::vector<uint8_t> make_mbs() {
    size_t off[kMbsSections] = {}, pos = 0x120;
    for (int s = 0; s < kMbsSections; ++s) {
        bool present = s != int(MbsSection::Quad3D) && s != int(MbsSection::Aux) && s < int(MbsSection::Ev108);
        if (present) { off[s] = pos; pos += (kMbsStride[s] + 15) & ~15u; }
    }
    std::vector<uint8_t> b(pos + 0x10);
    std::memcpy(b.data(), "FMBS", 4);
    le32(b, 4, uint32_t(pos - 0x120));
    le32(b, 8, 0x120);
    le16(b, 0x14, 0x72);
    std::memcpy(b.data() + 0x80, "Test.mbs", 8);
    const uint32_t count_at[kMbsSections] = {0x54, 0x56, 0x58, 0x5a, 0x50, 0x5c, 0x62, 0x5e, 0x60, 0x64, 0x66, 0x6a, 0x6c, 0x6e};
    for (int s = 0; s < kMbsSections; ++s) {
        le32(b, 0xb0 + 8 * s, uint32_t(off[s]));
        if (off[s]) le16(b, count_at[s], 1);
    }
    auto at = [&](MbsSection s) { return off[int(s)]; };
    float uv[12] = {0.5f, 0.5f, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0};
    std::memcpy(b.data() + at(MbsSection::Uv), uv, 48);
    b[at(MbsSection::Part) + 7] = 2;  // texture index
    le16(b, at(MbsSection::Cell) + 0x16, 1);
    le16(b, at(MbsSection::Frame) + 6, 5);
    le16(b, at(MbsSection::Track) + 2, 1);
    std::memcpy(b.data() + at(MbsSection::Motion) + 0x10, "IDLE", 4);
    b[at(MbsSection::Motion) + 0x2a] = 1;
    std::memcpy(b.data() + pos, "FEOC", 4);
    b[pos + 8] = 0x10;
    return b;
}

void test_mbs() {
    auto b = make_mbs();
    Mbs m = parse_mbs(b);
    CHECK(m.stem == "Test.mbs" && m.parts.size() == 1 && m.cells.size() == 1 && m.motions.size() == 1);
    CHECK(m.motions[0].name == "IDLE" && m.motions[0].track_count == 1 && m.tracks[0].frame_count == 1);
    CHECK(m.frames[0].duration == 5 && m.uvs[0].v[2][1] == 1.0f && m.parts[0].texture == 2);
    CHECK(m.quad3d.count == 0 && m.ev108.bytes.empty());
    CHECK(mbs_texture_violations(m, 3) == 0 && mbs_texture_violations(m, 2) == 1);
    auto expect_throw = [](std::vector<uint8_t> x) {
        bool threw = false;
        try { parse_mbs(x); } catch (const std::exception&) { threw = true; }
        return threw;
    };
    auto bad = b;
    le16(bad, m.offset[int(MbsSection::Frame)], 1);  // cell index out of range
    CHECK(expect_throw(bad));
    bad = b;
    le16(bad, m.offset[int(MbsSection::Track)] + 2, 2);  // frame range past table
    CHECK(expect_throw(bad));
    bad = b;
    le16(bad, 0x54, 200);  // colour count overruns the file
    CHECK(expect_throw(bad));
    bad = b;
    bad.resize(bad.size() - 1);
    CHECK(expect_throw(bad));
}

void test_mcb() {
    // 3 index slots; records: id 0 with one 0x20 entry, id 2 with one 0x28 entry.
    std::vector<uint8_t> b(8 + 12 + 8 + 0x20 + 8 + 0x28);
    le16(b, 0, 3);
    le16(b, 2, 2);
    b[20 + 2] = 1;
    le16(b, 20 + 8 + 0x20, 2);
    b[20 + 8 + 0x20 + 4] = 1;
    Mcb m = parse_mcb(b);
    CHECK(m.records.size() == 2 && m.records[1].id == 2 && m.records[1].entries28.size() == 0x28 && m.missing == 0);
    auto bad = b;
    bad.push_back(0);  // chain must end exactly at EOF
    bool threw = false;
    try { parse_mcb(bad); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    bad = b;
    le16(bad, 2, 3);  // overstated count tolerated when EOF lands on a record boundary
    CHECK(parse_mcb(bad).missing == 1);
}

// Root (type 2) with two children: a Mone (type 0xc, flags 0x10 + 3-byte string) and a Lane (type 9) with one extra.
void bsb_item(std::vector<uint8_t>& b, int32_t type, int32_t n, int32_t x, const char* name, std::vector<int32_t> extra,
              std::vector<uint8_t> payload) {
    size_t p = b.size();
    b.resize(p + 20 + 4 * n + 12 + 0x50);
    le32(b, p, uint32_t(-1)); le32(b, p + 8, uint32_t(type)); le32(b, p + 16, uint32_t(n));
    size_t q = p + 20 + 4 * n;
    le32(b, q, uint32_t(x));
    le32(b, q + 12, 0x3f800000); le32(b, q + 12 + 4, 0x40000000);
    std::memcpy(&b[q + 12 + 0xc], name, std::strlen(name));
    le32(b, q + 12 + 0x4c, uint32_t(extra.size()));
    for (auto e : extra) { size_t o = b.size(); b.resize(o + 4); le32(b, o, uint32_t(e)); }
    b.insert(b.end(), payload.begin(), payload.end());
}

void test_bsb() {
    std::vector<uint8_t> b(8 + 0x14 + 3 * 4 + 3 * 0x40);
    le32(b, 0, 0x47);
    le32(b, 8, uint32_t(-500)); le32(b, 0x10, 500);
    le32(b, 0x18, 3);
    std::memcpy(&b[0x1c + 12], "stage/a.mbs", 11);
    std::memcpy(&b[0x1c + 12 + 0x40], "stage/a.ftx", 11);
    bsb_item(b, 2, 2, 0, "root", {}, std::vector<uint8_t>(0x38));
    std::vector<uint8_t> mone(0x2e + 3 + 12);
    le32(mone, 0, 0); le32(mone, 4, 1); le32(mone, 8, 2); le32(mone, 0xc, 7); le32(mone, 0x10, 0x10);
    mone[0x28] = 5; le16(mone, 0x2c, 3);
    bsb_item(b, 0xc, 0, 123, "art", {}, mone);
    bsb_item(b, 9, 0, -7, "lane", {42}, std::vector<uint8_t>(12));
    Bsb s = parse_bsb(b);
    CHECK(s.version == 0x47 && s.bounds[0] == -500 && s.bounds[2] == 500 && s.names.size() == 3);
    CHECK(s.items.size() == 3 && s.items[0].children.size() == 2 && s.items[0].type == 2);
    CHECK(s.items[1].has_mone && s.items[1].mone.ftx == 1 && s.items[1].mone.layer == 5 && s.items[1].mone.motion == 7 && s.items[1].x == 123);
    CHECK(s.items[1].scale_y == 2.0f && s.items[1].name == "art" && s.items[1].parent == 0);
    CHECK(s.name(0) && *s.name(0) == "stage/a.mbs" && !s.name(2));
    CHECK(s.items[2].extra.size() == 1 && s.items[2].extra[0] == 42 && s.items[2].payload.size() == 12);
    auto bad = b;
    bad.push_back(0);  // must end exactly at EOF
    bool threw = false;
    try { parse_bsb(bad); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

}  // namespace

int main() {
    auto t = make_table();
    test_utf(t);
    // Same table under CRI's XOR obfuscation.
    uint8_t m = 0x5F;
    for (auto& c : t) { c ^= m; m = uint8_t(m * 0x15); }
    test_utf(t);
    test_crilayla();
    test_afs2();
    test_hca();
    test_hca_bitreader();
    test_hca_dct4();
    test_hca_decoder();
    test_acb();
    test_detile();
    test_ftx();
    test_mbs();
    test_mcb();
    test_bsb();
    std::printf(failures ? "FAILED (%d)\n" : "all tests passed\n", failures);
    return failures ? 1 : 0;
}
