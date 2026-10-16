#include "formats/mbs.h"

#include <cstring>
#include <stdexcept>

namespace dcp::formats {

const std::array<uint32_t, kMbsSections> kMbsStride = {24, 48, 48, 80, 16, 8, 28, 36, 32, 48, 24, 20, 96, 120};

namespace {

constexpr size_t kHeader = 0x120, kFooter = 0x10;
// Header offset of each section's count (0x50 = u32, others u16), in slot order.
constexpr std::array<uint32_t, kMbsSections> kCountAt = {0x54, 0x56, 0x58, 0x5a, 0x50, 0x5c, 0x62,
                                                         0x5e, 0x60, 0x64, 0x66, 0x6a, 0x6c, 0x6e};

uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t u64(const uint8_t* p) { return uint64_t(u32(p)) | uint64_t(u32(p + 4)) << 32; }
float f32(const uint8_t* p) { float f; uint32_t v = u32(p); std::memcpy(&f, &v, 4); return f; }

[[noreturn]] void fail(const std::string& m) { throw std::runtime_error("mbs: " + m); }

std::string cstr(const uint8_t* p, size_t n) {
    size_t l = 0;
    while (l < n && p[l]) ++l;
    return std::string(reinterpret_cast<const char*>(p), l);
}

void check_range(const char* what, size_t i, uint64_t first, uint64_t n, size_t size) {
    if (first + n > size)
        fail(std::string(what) + " " + std::to_string(i) + " range " + std::to_string(first) + "+" + std::to_string(n) +
             " > " + std::to_string(size));
}

}  // namespace

const char* mbs_section_name(MbsSection s) {
    static const char* n[] = {"color", "uv", "pos", "quad3d", "part", "aux", "cell",
                              "transform", "frame", "motion", "track", "s108", "s110", "s118"};
    return n[int(s)];
}

bool is_mbs(std::span<const uint8_t> b) { return b.size() >= 4 && std::memcmp(b.data(), "FMBS", 4) == 0; }

Mbs parse_mbs(std::span<const uint8_t> b) {
    if (b.size() < kHeader + kFooter || !is_mbs(b)) fail("not an FMBS file");
    const uint8_t* h = b.data();
    if (u32(h + 8) != kHeader) fail("header size");
    if (u32(h + 4) != b.size() - kHeader - kFooter) fail("payload size mismatch");
    if (std::memcmp(h + b.size() - kFooter, "FEOC", 4) != 0) fail("missing FEOC footer");
    Mbs m;
    m.version = u16(h + 0x14);
    m.flags = h[0x17];
    m.child_slots = h[0x69];
    m.stem = cstr(h + 0x80, 0x20);
    if (m.version != 0x72) fail("version " + std::to_string(m.version));
    if (m.flags & 1) fail("big-endian variant not supported");

    const size_t end = b.size() - kFooter;
    for (int s = 0; s < kMbsSections; ++s) {
        m.offset[s] = u64(h + 0xb0 + 8 * s);
        m.count[s] = s == int(MbsSection::Part) ? u32(h + kCountAt[s]) : u16(h + kCountAt[s]);
        if (!m.offset[s]) {
            if (m.count[s]) fail(std::string(mbs_section_name(MbsSection(s))) + " absent but count nonzero");
            continue;
        }
        if (m.offset[s] < kHeader || m.offset[s] + uint64_t(m.count[s]) * kMbsStride[s] > end)
            fail(std::string(mbs_section_name(MbsSection(s))) + " out of file");
    }
    auto rec = [&](MbsSection s, size_t i) { return h + m.offset[int(s)] + i * kMbsStride[int(s)]; };
    auto n = [&](MbsSection s) { return size_t(m.count[int(s)]); };
    auto raw = [&](MbsSection s) {
        return MbsRaw{kMbsStride[int(s)], m.count[int(s)],
                      m.offset[int(s)] ? b.subspan(m.offset[int(s)], n(s) * kMbsStride[int(s)]) : std::span<const uint8_t>{}};
    };

    for (size_t i = 0; i < n(MbsSection::Color); ++i) {
        MbsColorSet c;
        std::memcpy(c.rgba.data(), rec(MbsSection::Color, i), 24);
        m.colors.push_back(c);
    }
    auto vec2 = [&](MbsSection s, std::vector<MbsVec2Set>& out) {
        for (size_t i = 0; i < n(s); ++i) {
            MbsVec2Set v;
            const uint8_t* p = rec(s, i);
            for (int k = 0; k < 6; ++k) v.v[k] = {f32(p + 8 * k), f32(p + 8 * k + 4)};
            out.push_back(v);
        }
    };
    vec2(MbsSection::Uv, m.uvs);
    vec2(MbsSection::Pos, m.positions);
    for (size_t i = 0; i < n(MbsSection::Part); ++i) {
        const uint8_t* p = rec(MbsSection::Part, i);
        MbsPart t{u16(p), p[4], p[5], p[6], p[7], u16(p + 8), u16(p + 0xa), u16(p + 0xc), u16(p + 0xe)};
        if (t.pos >= m.positions.size()) fail("part " + std::to_string(i) + " pos index");
        if (t.color >= m.colors.size()) fail("part " + std::to_string(i) + " color index");
        if (!t.untextured() && t.uv >= m.uvs.size()) fail("part " + std::to_string(i) + " uv index");
        m.parts.push_back(t);
    }
    for (size_t i = 0; i < n(MbsSection::Cell); ++i) {
        const uint8_t* p = rec(MbsSection::Cell, i);
        MbsCell c{{f32(p), f32(p + 4), f32(p + 8), f32(p + 12)}, u32(p + 0x10), u16(p + 0x14), u16(p + 0x16), p[0x18], p[0x19]};
        check_range("cell", i, c.first_part, c.part_count, m.parts.size());
        m.cells.push_back(c);
    }
    for (size_t i = 0; i < n(MbsSection::Transform); ++i) {
        const uint8_t* p = rec(MbsSection::Transform, i);
        m.transforms.push_back({{f32(p), f32(p + 4), f32(p + 8)}, {f32(p + 12), f32(p + 16), f32(p + 20)},
                                {f32(p + 24), f32(p + 28)}, u32(p + 0x20)});
    }
    for (size_t i = 0; i < n(MbsSection::Frame); ++i) {
        const uint8_t* p = rec(MbsSection::Frame, i);
        MbsFrame f{u16(p), u16(p + 4), u16(p + 6), u32(p + 8), u16(p + 0xc), p[0xe], p[0xf], p[0x10], p[0x11]};
        if (f.cell >= m.cells.size()) fail("frame " + std::to_string(i) + " cell index " + std::to_string(f.cell));
        if (f.transform >= m.transforms.size()) fail("frame " + std::to_string(i) + " transform index");
        m.frames.push_back(f);
    }
    for (size_t i = 0; i < n(MbsSection::Track); ++i) {
        const uint8_t* p = rec(MbsSection::Track, i);
        MbsTrack t{u16(p), u16(p + 2), u32(p + 4), int8_t(p[0x13]), p[0x14]};
        check_range("track", i, t.first_frame, t.frame_count, m.frames.size());
        m.tracks.push_back(t);
    }
    for (size_t i = 0; i < n(MbsSection::Motion); ++i) {
        const uint8_t* p = rec(MbsSection::Motion, i);
        MbsMotion mo{{f32(p), f32(p + 4), f32(p + 8), f32(p + 12)}, cstr(p + 0x10, 24), u16(p + 0x28), p[0x2a], p[0x2b], p[0x2f]};
        check_range("motion", i, mo.first_track, mo.track_count, m.tracks.size());
        m.motions.push_back(mo);
    }
    m.quad3d = raw(MbsSection::Quad3D);
    m.aux = raw(MbsSection::Aux);
    m.ev108 = raw(MbsSection::Ev108);
    m.s110 = raw(MbsSection::S110);
    m.s118 = raw(MbsSection::S118);
    return m;
}

size_t mbs_texture_violations(const Mbs& m, size_t texture_count) {
    size_t v = 0;
    for (const auto& p : m.parts)
        if (!p.untextured() && p.texture >= texture_count) ++v;
    return v;
}

}  // namespace dcp::formats
