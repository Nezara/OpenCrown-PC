#include "crilayla.h"

#include <cstring>
#include <stdexcept>

namespace dcp::formats {
namespace {

constexpr size_t kPrefix = 0x100;  // stored uncompressed after the compressed stream

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// The compressed stream is read from its last byte towards its first, MSB-first within each byte.
class BackwardBits {
public:
    BackwardBits(const uint8_t* base, size_t last) : base_(base), pos_(ptrdiff_t(last)) {}
    uint32_t take(int n) {
        uint32_t out = 0;
        while (n > 0) {
            if (left_ == 0) {
                if (pos_ < 0) throw std::runtime_error("crilayla: stream underrun");
                pool_ = base_[pos_--];
                left_ = 8;
            }
            int k = n < left_ ? n : left_;
            out = out << k | ((pool_ >> (left_ - k)) & ((1u << k) - 1));
            left_ -= k;
            n -= k;
        }
        return out;
    }
private:
    const uint8_t* base_;
    ptrdiff_t pos_;
    uint8_t pool_ = 0;
    int left_ = 0;
};

}  // namespace

bool is_crilayla(std::span<const uint8_t> in) {
    return in.size() >= 16 && std::memcmp(in.data(), "CRILAYLA", 8) == 0;
}

std::vector<uint8_t> crilayla_decompress(std::span<const uint8_t> in) {
    if (!is_crilayla(in)) throw std::runtime_error("crilayla: bad magic");
    uint32_t body_size = le32(in.data() + 8);
    uint32_t prefix_off = le32(in.data() + 12);
    if (uint64_t(prefix_off) + 0x10 + kPrefix > in.size()) throw std::runtime_error("crilayla: bad prefix offset");

    std::vector<uint8_t> out(kPrefix + body_size);
    std::memcpy(out.data(), in.data() + prefix_off + 0x10, kPrefix);

    // Compressed stream occupies in[0x10, 0x10 + prefix_off); output is produced from the end backwards.
    BackwardBits bits(in.data() + 0x10, prefix_off - 1);
    static constexpr int kVle[] = {2, 3, 5, 8};
    size_t end = kPrefix + body_size - 1;
    size_t done = 0;
    while (done < body_size) {
        if (bits.take(1)) {
            size_t src = end - done + bits.take(13) + 3;
            size_t len = 3;
            int level = 0;
            for (; level < 4; ++level) {
                uint32_t v = bits.take(kVle[level]);
                len += v;
                if (v != (1u << kVle[level]) - 1) break;
            }
            if (level == 4) {
                uint32_t v;
                do { v = bits.take(8); len += v; } while (v == 0xFF);
            }
            if (len > body_size - done || src > end) throw std::runtime_error("crilayla: bad backreference");
            for (size_t i = 0; i < len; ++i) out[end - done++] = out[src--];
        } else {
            out[end - done++] = uint8_t(bits.take(8));
        }
    }
    return out;
}

}  // namespace dcp::formats
