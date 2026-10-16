#include "ftx.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace dcp::formats {
namespace {

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("ftx: " + what); }

std::string hex(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v);
    return b;
}

// Every chunk starts with: magic[4], u32 payloadSize, u32 headerSize, u16 count.
struct Chunk {
    uint64_t off, header, payload;
    uint16_t count;
};

Chunk read_chunk(std::span<const uint8_t> b, uint64_t off, const char* magic) {
    if (off > b.size() || b.size() - off < 16) fail(std::string(magic) + " chunk header past end at " + hex(off));
    const uint8_t* p = &b[size_t(off)];
    if (std::memcmp(p, magic, 4) != 0)
        fail("expected " + std::string(magic) + " at " + hex(off) + ", found '" + std::string((const char*)p, 4) + "'");
    Chunk c{off, le32(p + 8), le32(p + 4), le16(p + 12)};
    if (c.header < 16 || c.header + c.payload > b.size() - off)
        fail(std::string(magic) + " at " + hex(off) + ": header " + hex(c.header) + " + payload " + hex(c.payload) +
             " exceeds file");
    return c;
}

}  // namespace

GnmFormatInfo gnm_format_info(uint32_t f) {
    switch (f) {
        case 1: return {"8", 1, 1, 1, false};
        case 2: return {"16", 1, 1, 2, false};
        case 3: return {"8_8", 1, 1, 2, false};
        case 4: return {"32", 1, 1, 4, false};
        case 5: return {"16_16", 1, 1, 4, false};
        case 6: return {"10_11_11", 1, 1, 4, false};
        case 7: return {"11_11_10", 1, 1, 4, false};
        case 8: return {"10_10_10_2", 1, 1, 4, false};
        case 9: return {"2_10_10_10", 1, 1, 4, false};
        case 10: return {"8_8_8_8", 1, 1, 4, false};
        case 11: return {"32_32", 1, 1, 8, false};
        case 12: return {"16_16_16_16", 1, 1, 8, false};
        case 14: return {"32_32_32_32", 1, 1, 16, false};
        case 16: return {"5_6_5", 1, 1, 2, false};
        case 17: return {"1_5_5_5", 1, 1, 2, false};
        case 18: return {"5_5_5_1", 1, 1, 2, false};
        case 19: return {"4_4_4_4", 1, 1, 2, false};
        case 35: return {"BC1", 4, 4, 8, true};
        case 36: return {"BC2", 4, 4, 16, true};
        case 37: return {"BC3", 4, 4, 16, true};
        case 38: return {"BC4", 4, 4, 8, true};
        case 39: return {"BC5", 4, 4, 16, true};
        case 40: return {"BC6", 4, 4, 16, true};
        case 41: return {"BC7", 4, 4, 16, true};
        default: return {};
    }
}

const char* gnm_num_format_name(uint32_t n) {
    static const char* const names[16] = {"UNORM", "SNORM", "USCALED", "SSCALED", "UINT", "SINT", "SNORM_OGL", "FLOAT",
                                          "nf8",   "SRGB",  "UBNORM",  "UBNORM_OGL", "UBINT", "UBSCALED", "nf14", "nf15"};
    return names[n & 15];
}

uint32_t gnm_to_dxgi(uint32_t d, uint32_t n) {
    const bool srgb = n == 9, snorm = n == 1;
    switch (d) {
        case 35: return srgb ? 72 : n == 0 ? 71 : 0;
        case 36: return srgb ? 75 : n == 0 ? 74 : 0;
        case 37: return srgb ? 78 : n == 0 ? 77 : 0;
        case 38: return snorm ? 81 : n == 0 ? 80 : 0;
        case 39: return snorm ? 84 : n == 0 ? 83 : 0;
        case 40: return n == 1 || n == 5 ? 96 : 95;  // signed vs unsigned float variant
        case 41: return srgb ? 99 : n == 0 ? 98 : 0;
        case 10: return srgb ? 29 : n == 0 ? 28 : 0;
        case 1: return n == 0 ? 61 : 0;
        default: return 0;
    }
}

GnmSurface GnfTexture::surface() const {
    auto fi = gnm_format_info(data_format());
    GnmSurface s;
    s.width = width();
    s.height = height();
    s.pitch = pitch();
    s.block_w = fi.block_w;
    s.block_h = fi.block_h;
    s.bytes_per_element = fi.bytes_per_element;
    s.tile_index = tile_index();
    s.alt_tile = t[6] >> 24 & 1;  // set on the game's Neo-mode CPU-tiled glyph texture, clear on every CPK texture
    return s;
}

bool is_ftx(std::span<const uint8_t> b) { return b.size() >= 16 && std::memcmp(b.data(), "FTEX", 4) == 0; }

std::vector<FtxTexture> parse_ftx(std::span<const uint8_t> b) {
    if (!is_ftx(b)) fail("bad magic");
    Chunk top = read_chunk(b, 0, "FTEX");
    if (top.header < 0x20 + uint64_t(0x30) * top.count)
        fail("header " + hex(top.header) + " too small for " + std::to_string(top.count) + " name entries");

    std::vector<FtxTexture> out;
    uint64_t off = top.header;
    for (uint32_t i = 0; i < top.count; ++i) {
        const uint8_t* ent = &b[0x20 + size_t(0x30) * i];
        std::string name((const char*)ent, strnlen((const char*)ent, 0x20));
        Chunk c = read_chunk(b, off, "FTX0");
        if (c.off + c.header + c.payload > top.header + top.payload)
            fail("FTX0 #" + std::to_string(i) + " runs past the FTEX payload");
        if (le32(ent + 0x24) != c.payload)
            fail("name entry #" + std::to_string(i) + " size " + hex(le32(ent + 0x24)) + " != FTX0 payload " +
                 hex(c.payload));
        const std::string where = "chunk #" + std::to_string(i) + " (" + name + ")";

        // GNF: "GNF ", u32 contentsSize, then contents {u8 version, u8 numTextures, u8 alignLog2, u8 0,
        // u32 gnfFileSize, T#[numTextures], pad}; pixel data follows the contents.
        uint64_t g = c.off + c.header;
        const uint64_t gend = g + c.payload;
        if (c.payload < 16 || std::memcmp(&b[size_t(g)], "GNF ", 4) != 0) fail(where + ": missing GNF header");
        const uint8_t* gp = &b[size_t(g)];
        uint32_t contents = le32(gp + 4);
        uint8_t ver = gp[8], ntex = gp[9], align = gp[10];
        if (ver != 1 && ver != 2) fail(where + ": unknown GNF version " + std::to_string(ver));
        if (align >= 32) fail(where + ": GNF alignment " + std::to_string(align));
        uint64_t table = 8 + uint64_t(ntex) * 32;
        uint64_t expect = ver == 1 ? table : ((table + 8 + (uint64_t(1) << align) - 1) >> align << align) - 8;
        if (contents != expect)
            fail(where + ": GNF contents size " + hex(contents) + ", expected " + hex(expect));
        if (8 + uint64_t(contents) > c.payload) fail(where + ": GNF contents exceed chunk");
        const uint64_t data_base = g + 8 + contents;
        uint32_t gnf_size = le32(gp + 12);  // whole GNF file: header block + pixel data
        if (gnf_size != c.payload)
            fail(where + ": GNF file size " + hex(gnf_size) + " != FTX0 payload " + hex(c.payload));

        for (uint32_t k = 0; k < ntex; ++k) {
            FtxTexture t;
            t.name = name;
            t.chunk = i;
            t.index = k;
            t.gnf_version = ver;
            for (int d = 0; d < 8; ++d) t.tex.t[size_t(d)] = le32(gp + 16 + 32 * k + 4 * d);
            uint64_t po = data_base + t.tex.data_offset();
            if (po > gend || t.tex.data_size() > gend - po)
                fail(where + " tex " + std::to_string(k) + ": pixel range exceeds chunk");
            t.pixels = b.subspan(size_t(po), t.tex.data_size());
            out.push_back(std::move(t));
        }
        off = c.off + c.header + c.payload;
    }
    if (off != top.header + top.payload)
        fail("children end at " + hex(off) + ", FTEX payload ends at " + hex(top.header + top.payload));

    // FEOC footer: "FEOC", 0, 0x10, 0.
    Chunk foot = read_chunk(b, off, "FEOC");
    if (foot.payload != 0 || foot.header != 16) fail("unexpected FEOC footer");
    if (off + 16 != b.size()) fail(hex(b.size() - off - 16) + " trailing bytes after FEOC");
    return out;
}

std::vector<uint8_t> ftx_detile(const FtxTexture& t) {
    const auto& x = t.tex;
    if (x.mips() != 1 || x.slices() != 1 || x.depth() != 1)
        fail("detile: only single-mip single-slice 2D textures are supported");
    auto s = x.surface();
    if (!s.bytes_per_element) fail("detile: unknown data format " + std::to_string(x.data_format()));
    return gnm_detile(t.pixels, s);
}

}  // namespace dcp::formats
