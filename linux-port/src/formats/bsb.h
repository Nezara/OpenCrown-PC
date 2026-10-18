// .bsb stage scene tree: header (string slots, stage bounds, art-name table) + one recursive item record tree.
// See notes/map/bsb.md. Little endian. Payload sizes per item type follow the loader FUN_0005d5f0.
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dcp::formats {

// Decoded head of a type 0x0c ("Mone") art item; indices select Bsb::names (-1 = none).
struct BsbMone {
    int32_t mbs = -1, ftx = -1, mcb = -1;
    int32_t motion = 0;               // dword 3: motion index in the mbs (names match the item names, e.g. doorA_1)
    uint32_t flags = 0;               // dword 4
    float scale_x = 1, scale_y = 1;   // dwords 5, 6 (1.0 in samples; read as floats, role guessed)
    uint32_t color = 0xffffffff;      // dword 7 (0xffffffff in samples; tint guessed)
    int layer = -1;  // u8@0x28 when flags & 0x10
};

struct BsbItem {
    int32_t id = -1, f1 = 0, type = 0;
    int32_t x = 0, y = 0, z = 0;          // as stored (parent-relative positions are made by the loader; see map)
    float scale_x = 1, scale_y = 1;       // block dwords 0, 1 (1.0f in the dump; the map's "[1],[2]" is off by one)
    uint32_t block2 = 0;                  // block dword 2 (unknown)
    std::string name;                     // block bytes 0x0c.. (<= 32)
    std::vector<int32_t> extra;           // extra[c]
    std::span<const uint8_t> payload;     // type-specific bytes, viewing the caller's buffer
    int parent = -1;                      // index into Bsb::items
    int depth = 0;
    std::vector<int> children;            // indices into Bsb::items, file order
    bool has_mone = false;
    BsbMone mone;
};

struct Bsb {
    uint32_t version = 0;
    std::vector<std::string> slots;       // N 0x40-byte string slots (empty in the dump)
    std::array<int32_t, 4> bounds{};      // minX, minY, maxX, maxY; 10000000 / -10000000 = unset
    std::vector<std::string> names;       // K art names (mbs, ftx, mcb triples; may be empty strings)
    std::vector<BsbItem> items;           // pre-order (file order); items[0] = root
    const std::string* name(int32_t i) const {
        return i >= 0 && size_t(i) < names.size() && !names[i].empty() ? &names[i] : nullptr;
    }
};

bool is_bsb(std::span<const uint8_t> bytes);  // cheap plausibility check (version 0x47)

// Byte size of the type-specific payload starting at p (n = bytes available). Throws if unknown / inconsistent.
size_t bsb_payload_size(int32_t type, const uint8_t* p, size_t n);

// Parses the whole file; must consume exactly to EOF. Throws on violation.
Bsb parse_bsb(std::span<const uint8_t> bytes);

}  // namespace dcp::formats
