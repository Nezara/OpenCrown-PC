#include "nid.h"

#include <cstring>
#include <vector>

namespace dcp::loader {
namespace {

uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

}  // namespace

std::array<uint8_t, 20> sha1(const void* data, size_t size) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::vector<uint8_t> m(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    uint64_t bits = uint64_t(size) * 8;
    for (int i = 7; i >= 0; --i) m.push_back(uint8_t(bits >> (i * 8)));

    for (size_t blk = 0; blk < m.size(); blk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(m[blk + i * 4]) << 24 | uint32_t(m[blk + i * 4 + 1]) << 16 | uint32_t(m[blk + i * 4 + 2]) << 8 |
                   m[blk + i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    std::array<uint8_t, 20> out;
    for (int i = 0; i < 20; ++i) out[i] = uint8_t(h[i / 4] >> (24 - (i % 4) * 8));
    return out;
}

std::string nid_from_name(std::string_view name) {
    static const uint8_t kSuffix[16] = {0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1,
                                        0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30};
    static const char kAlpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::vector<uint8_t> in(name.begin(), name.end());
    in.insert(in.end(), kSuffix, kSuffix + 16);
    auto h = sha1(in.data(), in.size());
    // The first 8 digest bytes read as a little-endian u64, then base64'd big-endian = digest bytes 7..0.
    uint8_t be[9] = {};
    for (int i = 0; i < 8; ++i) be[i] = h[7 - i];
    std::string s;
    for (int bit = 0; bit < 66; bit += 6) {  // 11 chars; the last one carries 4 data bits + 2 zero bits
        int byte = bit / 8, sh = bit % 8;
        unsigned v = (unsigned(be[byte]) << 8 | be[byte + 1]) >> (10 - sh);
        s += kAlpha[v & 63];
    }
    return s;
}

}  // namespace dcp::loader
