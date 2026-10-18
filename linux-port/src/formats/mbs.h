// .mbs "FMBS" 2D sprite-mesh animation bank: parts (textured 6-vertex fans) -> cells -> frames -> tracks -> motions.
// No pixels; parts reference textures of the paired .ftx by FTX0 child index. See notes/map/mbs.md. Little endian
// (a big-endian variant flagged by header byte 0x17 bit0 exists in the loader; not seen in the dump, rejected here).
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dcp::formats {

// The 14 section slots (header offsets 0xb0..0x118), in slot order.
enum class MbsSection : int {
    Color, Uv, Pos, Quad3D /*0xc8*/, Part, Aux /*0xd8*/, Cell, Transform, Frame, Motion, Track, Ev108, S110, S118, Count
};
constexpr int kMbsSections = int(MbsSection::Count);
extern const std::array<uint32_t, kMbsSections> kMbsStride;
const char* mbs_section_name(MbsSection s);

struct MbsColorSet { std::array<std::array<uint8_t, 4>, 6> rgba; };  // one per fan vertex
struct MbsVec2Set { std::array<std::array<float, 2>, 6> v; };        // UVs (0..1) or positions; v[0] centre, 1..4 corners

struct MbsPart {
    uint16_t id = 0;
    uint8_t b4 = 0;
    uint8_t flags = 0;       // bit1 hidden, bit2 untextured
    uint8_t blend = 0;
    uint8_t texture = 0;     // FTX0 child index
    uint16_t vis_mask = 0;
    uint16_t uv = 0, color = 0, pos = 0;
    bool untextured() const { return flags & 4; }
};

struct MbsCell {
    std::array<float, 4> rect{};
    uint32_t first_part = 0;
    uint16_t u14 = 0;
    uint16_t part_count = 0;
    uint8_t helper = 0, flags = 0;
};

struct MbsTransform {
    std::array<float, 3> pos{}, rot{};
    std::array<float, 2> scale{};
    uint32_t tint = 0;
};

struct MbsFrame {
    uint16_t cell = 0, transform = 0, duration = 0;
    uint32_t flags = 0;
    uint16_t jump = 0;
    uint8_t helper = 0, time_curve = 0, interp = 0, morph = 0;
};

struct MbsTrack {
    uint16_t first_frame = 0, frame_count = 0;
    uint32_t duration = 0;
    int8_t start = 0;
    uint8_t channel = 0;
};

struct MbsMotion {
    std::array<float, 4> rect{};
    std::string name;
    uint16_t first_track = 0;
    uint8_t track_count = 0, main_track = 0, flags = 0;
};

struct MbsRaw {  // a section whose records are not decoded
    uint32_t stride = 0, count = 0;
    std::span<const uint8_t> bytes;
    std::span<const uint8_t> record(size_t i) const { return bytes.subspan(i * stride, stride); }
};

struct Mbs {
    uint16_t version = 0;
    uint8_t flags = 0;
    uint8_t child_slots = 0;  // header byte 0x69
    std::string stem;         // header 0x80
    std::array<uint64_t, kMbsSections> offset{};
    std::array<uint32_t, kMbsSections> count{};

    std::vector<MbsColorSet> colors;
    std::vector<MbsVec2Set> uvs, positions;
    std::vector<MbsPart> parts;
    std::vector<MbsCell> cells;
    std::vector<MbsTransform> transforms;
    std::vector<MbsFrame> frames;
    std::vector<MbsTrack> tracks;
    std::vector<MbsMotion> motions;
    MbsRaw quad3d, aux, ev108, s110, s118;
};

bool is_mbs(std::span<const uint8_t> bytes);

// Parses and validates framing and all cross-table indices; throws on violation.
Mbs parse_mbs(std::span<const uint8_t> bytes);

// Parts whose texture index is >= texture_count (untextured parts excluded).
size_t mbs_texture_violations(const Mbs& m, size_t texture_count);

}  // namespace dcp::formats
