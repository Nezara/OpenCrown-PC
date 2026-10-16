#include "afs2.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace dcp::formats {
namespace {

uint64_t le(const uint8_t* p, unsigned n) {
    uint64_t v = 0;
    for (unsigned i = n; i-- > 0;) v = v << 8 | p[i];
    return v;
}

}  // namespace

const Afs2Stream* Afs2Header::find(uint32_t id) const {
    for (const auto& s : streams)
        if (s.id == id) return &s;
    return nullptr;
}

bool is_afs2(std::span<const uint8_t> b) { return b.size() >= 16 && std::memcmp(b.data(), "AFS2", 4) == 0; }

uint64_t afs2_header_size(std::span<const uint8_t> b) {
    if (!is_afs2(b)) throw std::runtime_error("afs2: bad magic");
    uint64_t count = le(&b[8], 4);
    return 16 + count * b[6] + (count + 1) * b[5];
}

Afs2Header parse_afs2(std::span<const uint8_t> b, uint64_t total_size) {
    Afs2Header h;
    uint64_t need = afs2_header_size(b);
    h.version = b[4];
    h.offset_size = b[5];
    h.id_size = b[6];
    h.align = uint16_t(le(&b[12], 2));
    h.subkey = uint16_t(le(&b[14], 2));
    h.header_size = need;
    if ((h.offset_size != 2 && h.offset_size != 4) || (h.id_size != 2 && h.id_size != 4))
        throw std::runtime_error("afs2: unsupported field sizes");
    if (h.align == 0) throw std::runtime_error("afs2: zero alignment");
    if (b.size() < need || need > total_size) throw std::runtime_error("afs2: truncated header");

    uint32_t count = uint32_t(le(&b[8], 4));
    const uint8_t* ids = b.data() + 16;
    const uint8_t* offs = ids + uint64_t(count) * h.id_size;
    h.streams.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t start = le(offs + uint64_t(i) * h.offset_size, h.offset_size);
        uint64_t end = le(offs + uint64_t(i + 1) * h.offset_size, h.offset_size);
        if (end < start || end > total_size) throw std::runtime_error("afs2: stream out of bounds");
        // The table holds unaligned ends; streams begin at the next aligned offset (empty streams may not reach it).
        start = std::min((start + h.align - 1) / h.align * h.align, end);
        h.streams[i] = {uint32_t(le(ids + uint64_t(i) * h.id_size, h.id_size)), start, end - start};
    }
    return h;
}

std::span<const uint8_t> afs2_stream(std::span<const uint8_t> awb, const Afs2Stream& s) {
    if (s.offset + s.size > awb.size()) throw std::runtime_error("afs2: stream out of bounds");
    return awb.subspan(size_t(s.offset), size_t(s.size));
}

}  // namespace dcp::formats
