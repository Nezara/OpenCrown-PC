#include "formats/bsb.h"

#include <cstring>
#include <stdexcept>

namespace dcp::formats {

namespace {

uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
int32_t s32(const uint8_t* p) { return int32_t(u32(p)); }
float f32(const uint8_t* p) { float f; uint32_t v = u32(p); std::memcpy(&f, &v, 4); return f; }

[[noreturn]] void fail(const std::string& m) { throw std::runtime_error("bsb: " + m); }

std::string cstr(const uint8_t* p, size_t n) {
    size_t l = 0;
    while (l < n && p[l]) ++l;
    return std::string(reinterpret_cast<const char*>(p), l);
}

// Payload dword counts of the sub-kinds of types 0x14 / 0x15 (others: 2 / 1).
uint32_t d14(uint32_t sub) {
    switch (sub) {
        case 1: return 4; case 2: return 0x14; case 3: return 0x13; case 4: case 5: return 0x12; case 6: return 0x13;
        case 9: return 0x22; case 0xa: return 0x13; case 0xb: return 0x12; case 0xc: return 0x13; case 0xd: return 0x14;
        case 0xe: case 0xf: case 0x10: case 0x11: case 0x12: return 0x12; case 0x13: return 5; case 0x14: return 0x12;
        default: return 2;
    }
}
uint32_t d15(uint32_t sub) {
    switch (sub) {
        case 1: return 0x22; case 2: case 4: return 0x13; case 5: case 6: return 0x12; case 7: return 4;
        case 8: case 9: return 3; case 0xa: return 5; case 0xb: return 0x21;
        default: return 1;
    }
}

struct Reader {
    std::span<const uint8_t> b;
    Bsb& out;
    size_t item(size_t p, int parent, int depth) {
        const size_t end = b.size();
        if (depth > 64) fail("nesting too deep");
        if (p + 0x14 > end) fail("eof in item head");
        const uint8_t* d = b.data();
        int32_t type = s32(d + p + 8), n = s32(d + p + 16);
        if (type < 0 || type >= 0x28 || n < 0 || n >= 10000) fail("bad type/child count at " + std::to_string(p));
        size_t q = p + 20 + 4 * size_t(n);
        if (q + 12 + 0x50 > end) fail("eof in item block");
        BsbItem it;
        it.id = s32(d + p);
        it.f1 = s32(d + p + 4);
        it.type = type;
        it.x = s32(d + q); it.y = s32(d + q + 4); it.z = s32(d + q + 8);
        const uint8_t* blk = d + q + 12;
        it.scale_x = f32(blk);
        it.scale_y = f32(blk + 4);
        it.block2 = u32(blk + 8);
        it.name = cstr(blk + 0xc, 0x20);
        int32_t c = s32(blk + 0x4c);
        q += 12 + 0x50;
        if (c < 0 || c >= 10000 || q + 4 * size_t(c) > end) fail("bad extra count at " + std::to_string(p));
        for (int32_t i = 0; i < c; ++i) it.extra.push_back(s32(d + q + 4 * i));
        q += 4 * size_t(c);
        size_t ps = bsb_payload_size(type, d + q, end - q);
        if (q + ps > end) fail("eof in payload type " + std::to_string(type));
        it.payload = b.subspan(q, ps);
        if (type == 0x0c) {
            it.has_mone = true;
            const uint8_t* m = d + q;
            it.mone.mbs = s32(m); it.mone.ftx = s32(m + 4); it.mone.mcb = s32(m + 8);
            it.mone.motion = s32(m + 0xc);
            it.mone.flags = u32(m + 0x10);
            it.mone.scale_x = f32(m + 0x14); it.mone.scale_y = f32(m + 0x18);
            it.mone.color = u32(m + 0x1c);
            if (it.mone.flags & 0x10) it.mone.layer = m[0x28];
        }
        q += ps;
        it.parent = parent;
        it.depth = depth;
        int self = int(out.items.size());
        out.items.push_back(std::move(it));
        if (parent >= 0) out.items[parent].children.push_back(self);
        if (type == 0x24) return q;  // tile grid: the loader reads no children
        for (int32_t i = 0; i < n; ++i) q = item(q, self, depth + 1);
        return q;
    }
};

}  // namespace

bool is_bsb(std::span<const uint8_t> b) { return b.size() >= 8 && u32(b.data()) == 0x47; }

size_t bsb_payload_size(int32_t t, const uint8_t* p, size_t n) {
    auto need = [&](size_t k) { if (k > n) fail("eof in payload head type " + std::to_string(t)); };
    switch (t) {
        case 2: return 0x38;
        case 7: return 0x94;
        case 8: return 4;
        case 9: return 12;
        case 0xa: return 16;
        case 0xb: return 4;
        case 0xe: return 0x1c;
        case 0x12: return 8;
        case 0x13: return 4;
        case 0x16: return 0x30;
        case 0x18: return 0x24;
        case 0x19: return 0x40;
        case 0x1b: return 12;
        case 0x1c: return 0x54;
        case 0x1e: return 4;
        case 0x20: return 0x30;
        case 0x21: return 0x14;
        case 0x22: return 0x10;
        case 0x27: return 0x40;
        case 0xc: {
            need(0x14);
            size_t k = 0x28;
            if (u32(p + 0x10) & 0x10) { need(0x2e); k = 0x2e + u16(p + 0x2c); }
            return k + 12;
        }
        case 0x10: case 0x1a: {
            need(4);
            size_t k = 0x68;
            if (p[1] == 2 || p[1] == 4) k += 8;
            if (u16(p + 2) & 0x10) k += 0x24;
            return k;
        }
        case 0x14: need(4); return 4 * size_t(d14(u32(p)));
        case 0x15: need(4); return 4 * size_t(d15(u32(p)));
        case 0x23: need(0x74); return 0x60 + 0x14 + ((u32(p + 0x70) & 4) ? 4 : 0) + 8 + 4;
        case 0x24: {
            need(0x28);
            int32_t w = s32(p + 0x20), h = s32(p + 0x24);
            if (w < 0 || h < 0 || int64_t(w) * h > (1 << 20)) fail("type 0x24 dims");
            return 0x28 + ((size_t(w) * size_t(h) + 3) & ~size_t(3));
        }
        case 0x25: {
            need(0x88);
            if (!(u32(p + 0x70) & 0x40)) return 0x80;
            int32_t a = s32(p + 0x80), c = s32(p + 0x84);
            if (a < 0 || c < 0 || a > 1000 || c > 1000) fail("type 0x25 counts");
            return 0x80 + 8 + 4 * size_t(a) + 8 * size_t(c);
        }
        default: return 0;  // 0,1,3..6,0xd,0xf,0x11,0x17,0x1d,0x1f,0x26: no payload
    }
}

Bsb parse_bsb(std::span<const uint8_t> b) {
    if (b.size() < 8) fail("too small");
    const uint8_t* d = b.data();
    Bsb out;
    out.version = u32(d);
    int32_t N = s32(d + 4);
    if (N < 0 || N > 4096) fail("bad slot count");
    size_t o = 8 + 0x40 * size_t(N);
    if (o + 0x14 > b.size()) fail("eof in header");
    for (int32_t i = 0; i < N; ++i) out.slots.push_back(cstr(d + 8 + 0x40 * i, 0x40));
    for (int i = 0; i < 4; ++i) out.bounds[i] = s32(d + o + 4 * i);
    int32_t K = s32(d + o + 0x10);
    if (K < 0 || K > 100000) fail("bad name count");
    size_t names = o + 0x14 + 4 * size_t(K), s = names + 0x40 * size_t(K);
    if (s > b.size()) fail("eof in name table");
    for (int32_t i = 0; i < K; ++i) out.names.push_back(cstr(d + names + 0x40 * i, 0x40));
    Reader r{b, out};
    size_t e = r.item(s, -1, 0);
    if (e != b.size()) fail("item tree ends at " + std::to_string(e) + ", file size " + std::to_string(b.size()));
    return out;
}

}  // namespace dcp::formats
