#include "formats/mcb.h"

#include <stdexcept>
#include <string>

namespace dcp::formats {

Mcb parse_mcb(std::span<const uint8_t> b) {
    auto u16 = [&](size_t o) { return uint16_t(b[o] | b[o + 1] << 8); };
    if (b.size() < 4) throw std::runtime_error("mcb: too small");
    Mcb m;
    m.max_id = u16(0);
    m.declared = u16(2);
    size_t n = m.declared;
    // u32 index slots at +8, one per id (filled at load time, zero on disk); +4 unknown.
    size_t pos = 8 + 4 * size_t(m.max_id);
    if (pos > b.size()) throw std::runtime_error("mcb: index table past EOF");
    for (size_t i = 0; i < n; ++i) {
        if (pos == b.size()) { m.missing = n - i; break; }  // count overstates (3 stage files); the game reads past
        if (pos + 8 > b.size()) throw std::runtime_error("mcb: record " + std::to_string(i) + " head past EOF");
        McbRecord r;
        r.id = u16(pos);
        r.n1 = b[pos + 2];
        r.n3 = b[pos + 3];
        r.n2 = b[pos + 4];
        r.offset = pos;
        if (r.id >= m.max_id) throw std::runtime_error("mcb: record id " + std::to_string(r.id) + " > max");
        size_t a = size_t(r.n1) * 0x20, c = (size_t(r.n3) + r.n2) * 0x28;
        if (pos + 8 + a + c > b.size()) throw std::runtime_error("mcb: record " + std::to_string(i) + " past EOF");
        r.head = b.subspan(pos, 8);
        r.entries20 = b.subspan(pos + 8, a);
        r.entries28 = b.subspan(pos + 8 + a, c);
        pos += 8 + a + c;
        m.records.push_back(r);
    }
    if (pos != b.size())
        throw std::runtime_error("mcb: chain ends at " + std::to_string(pos) + ", size " + std::to_string(b.size()));
    return m;
}

}  // namespace dcp::formats
