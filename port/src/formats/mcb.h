// .mcb per-motion collision data paired by stem with an .mbs. See notes/map/formats_misc.md sec. 3. Little endian.
// Box/entry fields are not interpreted yet; entries are exposed raw.
#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace dcp::formats {

struct McbRecord {
    uint16_t id = 0;  // motion index in the paired .mbs
    uint8_t n1 = 0, n3 = 0, n2 = 0;
    uint64_t offset = 0;                // file offset of the record head
    std::span<const uint8_t> head;      // 8 bytes
    std::span<const uint8_t> entries20; // n1 x 0x20
    std::span<const uint8_t> entries28; // (n3 + n2) x 0x28
};

struct Mcb {
    uint16_t max_id = 0;    // u16 @0: number of index slots; ids are < max_id
    uint16_t declared = 0;  // u16 @2: record count as stored
    size_t missing = 0;     // declared records absent because EOF was reached exactly at a record boundary
    std::vector<McbRecord> records;
};

// Throws unless the record chain ends exactly at EOF and every id < max_id.
Mcb parse_mcb(std::span<const uint8_t> bytes);

}  // namespace dcp::formats
