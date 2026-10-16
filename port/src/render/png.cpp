#include "png.h"

#include <algorithm>
#include <cstdio>

namespace dcp::render {
namespace {
uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0) {
    static uint32_t tab[256];
    if (!tab[1])
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            tab[i] = v;
        }
    c = ~c;
    for (size_t i = 0; i < n; ++i) c = tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return ~c;
}

}  // namespace

bool write_png(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb) {
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + size_t(y) * w * 3, rgb.begin() + size_t(y + 1) * w * 3);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    for (size_t o = 0; o < raw.size() || o == 0;) {
        size_t n = std::min<size_t>(65535, raw.size() - o);
        z.push_back(o + n == raw.size());
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8)); z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + o, raw.begin() + o + n);
        o += n;
        if (!n) break;
    }
    uint32_t adler = b << 16 | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back(uint8_t(adler >> s));
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(data.size() >> s));
        size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        uint32_t c = crc32(out.data() + start, out.size() - start);
        for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(c >> s));
    };
    std::vector<uint8_t> ihdr;
    for (uint32_t v : {uint32_t(w), uint32_t(h)})
        for (int s = 24; s >= 0; s -= 8) ihdr.push_back(uint8_t(v >> s));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

}  // namespace dcp::render
