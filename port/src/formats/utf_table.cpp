#include "utf_table.h"

#include <cstring>
#include <stdexcept>

namespace dcp::formats {
namespace {

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

enum : uint8_t { kStorageMask = 0xF0, kZero = 0x10, kConst = 0x30, kPerRow = 0x50, kConst2 = 0x70 };
enum : uint8_t { kU8, kS8, kU16, kS16, kU32, kS32, kU64, kS64, kF32, kF64, kString, kData };

size_t type_size(uint8_t type) {
    static constexpr uint8_t sizes[] = {1, 1, 2, 2, 4, 4, 8, 8, 4, 8, 4, 8};
    if (type > kData) throw std::runtime_error("utf: bad column type");
    return sizes[type];
}

// Some CPKs obfuscate their tables with a multiplicative XOR stream.
void deobfuscate(std::vector<uint8_t>& b) {
    uint8_t m = 0x5F;
    for (auto& c : b) { c ^= m; m = uint8_t(m * 0x15); }
}

}  // namespace

UtfTable::UtfTable(std::span<const uint8_t> bytes) {
    if (bytes.size() < 8) throw std::runtime_error("utf: truncated header");
    std::vector<uint8_t> hdr(bytes.begin(), bytes.begin() + 8);
    bool obfuscated = std::memcmp(hdr.data(), "@UTF", 4) != 0;
    if (obfuscated) {
        deobfuscate(hdr);
        if (std::memcmp(hdr.data(), "@UTF", 4) != 0) throw std::runtime_error("utf: bad magic");
    }
    uint32_t size = be32(hdr.data() + 4);
    if (size < 24 || bytes.size() - 8 < size) throw std::runtime_error("utf: truncated table");
    if (obfuscated) {
        std::vector<uint8_t> all(bytes.begin(), bytes.begin() + 8 + size);
        deobfuscate(all);
        t_.assign(all.begin() + 8, all.end());
    } else {
        t_.assign(bytes.begin() + 8, bytes.begin() + 8 + size);
    }

    const uint8_t* p = t_.data();
    rows_off_ = be16(p + 2);
    strings_off_ = be32(p + 4);
    data_off_ = be32(p + 8);
    uint32_t name_off = be32(p + 12);
    uint16_t ncols = be16(p + 16);
    row_len_ = be16(p + 18);
    rows_ = be32(p + 20);
    if (strings_off_ > size || data_off_ > size || rows_off_ + uint64_t(row_len_) * rows_ > size)
        throw std::runtime_error("utf: bad layout");

    auto string_at = [&](uint32_t off) {
        size_t start = size_t(strings_off_) + off;
        if (start >= t_.size()) throw std::runtime_error("utf: bad string offset");
        const char* s = reinterpret_cast<const char*>(t_.data() + start);
        return std::string(s, strnlen(s, t_.size() - start));
    };
    name_ = string_at(name_off);

    uint32_t pos = 24, row_pos = 0;
    for (uint16_t i = 0; i < ncols; ++i) {
        if (pos + 5 > rows_off_) throw std::runtime_error("utf: schema overruns rows");
        Column c{string_at(be32(p + pos + 1)), p[pos], 0, 0};
        pos += 5;
        size_t sz = type_size(c.flags & 0x0F);
        switch (c.flags & kStorageMask) {
            case kZero: break;
            case kConst:
            case kConst2: c.const_pos = pos; pos += uint32_t(sz); break;
            case kPerRow: c.row_pos = row_pos; row_pos += uint32_t(sz); break;
            default: throw std::runtime_error("utf: bad column storage");
        }
        cols_.push_back(std::move(c));
    }
    if (row_pos > row_len_) throw std::runtime_error("utf: row length mismatch");
}

std::optional<size_t> UtfTable::find_column(std::string_view name) const {
    for (size_t i = 0; i < cols_.size(); ++i)
        if (cols_[i].name == name) return i;
    return std::nullopt;
}

UtfValue UtfTable::decode(uint8_t type, uint32_t pos) const {
    const uint8_t* p = t_.data() + pos;
    switch (type) {
        case kU8: return uint64_t(p[0]);
        case kS8: return int64_t(int8_t(p[0]));
        case kU16: return uint64_t(be16(p));
        case kS16: return int64_t(int16_t(be16(p)));
        case kU32: return uint64_t(be32(p));
        case kS32: return int64_t(int32_t(be32(p)));
        case kU64: return be64(p);
        case kS64: return int64_t(be64(p));
        case kF32: { uint32_t u = be32(p); float f; std::memcpy(&f, &u, 4); return double(f); }
        case kF64: { uint64_t u = be64(p); double d; std::memcpy(&d, &u, 8); return d; }
        case kString: {
            size_t start = size_t(strings_off_) + be32(p);
            if (start >= t_.size()) throw std::runtime_error("utf: bad string offset");
            const char* s = reinterpret_cast<const char*>(t_.data() + start);
            return std::string(s, strnlen(s, t_.size() - start));
        }
        case kData: {
            UtfData d{be32(p), be32(p + 4)};
            if (uint64_t(data_off_) + d.offset + d.size > t_.size()) throw std::runtime_error("utf: bad data range");
            return d;
        }
    }
    throw std::runtime_error("utf: bad column type");
}

UtfValue UtfTable::get(uint32_t row, size_t col) const {
    if (row >= rows_ || col >= cols_.size()) throw std::out_of_range("utf: row/column out of range");
    const Column& c = cols_[col];
    uint8_t type = c.flags & 0x0F;
    switch (c.flags & kStorageMask) {
        case kConst:
        case kConst2: return decode(type, c.const_pos);
        case kPerRow: return decode(type, rows_off_ + row * row_len_ + c.row_pos);
        default: return std::monostate{};
    }
}

UtfValue UtfTable::get(uint32_t row, std::string_view col) const {
    auto i = find_column(col);
    return i ? get(row, *i) : UtfValue{};
}

uint64_t UtfTable::u64(uint32_t row, std::string_view col, uint64_t fallback) const {
    UtfValue v = get(row, col);
    if (auto u = std::get_if<uint64_t>(&v)) return *u;
    if (auto s = std::get_if<int64_t>(&v)) return uint64_t(*s);
    return fallback;
}

std::string UtfTable::str(uint32_t row, std::string_view col) const {
    UtfValue v = get(row, col);
    if (auto s = std::get_if<std::string>(&v); s && *s != "<NULL>") return *s;
    return {};
}

std::span<const uint8_t> UtfTable::data(uint32_t row, std::string_view col) const {
    UtfValue v = get(row, col);
    if (auto d = std::get_if<UtfData>(&v)) return {t_.data() + data_off_ + d->offset, d->size};
    return {};
}

}  // namespace dcp::formats
