// CRI AFS2 (.awb) wave bank: a header with id/offset tables followed by aligned streams (usually HCA).
#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace dcp::formats {

struct Afs2Stream {
    uint32_t id = 0;
    uint64_t offset = 0;  // start of the stream (already aligned), relative to the start of the AWB
    uint64_t size = 0;
};

struct Afs2Header {
    uint8_t version = 0, offset_size = 0, id_size = 0;
    uint16_t align = 0, subkey = 0;  // subkey feeds the HCA key derivation when ciphered (type 56)
    uint64_t header_size = 0;        // bytes covered by the fixed header + id/offset tables
    std::vector<Afs2Stream> streams;

    const Afs2Stream* find(uint32_t id) const;
};

bool is_afs2(std::span<const uint8_t> bytes);

// Bytes needed to parse the header, from the first 16 bytes; lets callers range-read only the tables.
uint64_t afs2_header_size(std::span<const uint8_t> first16);

// `bytes` holds at least afs2_header_size() bytes from the start of the AWB; `total_size` is the full AWB size,
// used to bound stream ranges. Throws on malformed input.
Afs2Header parse_afs2(std::span<const uint8_t> bytes, uint64_t total_size);

// One stream's bytes from a fully loaded AWB (e.g. the memory AWB embedded in an ACB).
std::span<const uint8_t> afs2_stream(std::span<const uint8_t> awb, const Afs2Stream& s);

}  // namespace dcp::formats
