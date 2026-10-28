// Replay of captured draws. Each draw is decoded on the CPU into one canonical vertex format (pos, colour, uv),
// expanded to plain triangles / lines, and handed to the Device (device.h: OpenGL or SDL_gpu) as one Draw record for
// one program whose uniforms stand in for the game's shader permutations. Targets are kept "top row first" so game
// UVs sample render targets unchanged.
#include "backend.h"

#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>

#include "formats/ftx.h"
#include "formats/gnm_detile.h"
#include "device.h"
#include "png.h"
#include "prompts.h"
#include "texwatch.h"

namespace dcp::render {
namespace {

float half_to_float(uint16_t h) {
    uint32_t s = h >> 15, e = h >> 10 & 0x1f, m = h & 0x3ff;
    float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? INFINITY : std::ldexp(float(m | 0x400), int(e) - 25);
    return s ? -v : v;
}

// Decodes one attribute of stream s at vertex i into up to 4 floats; returns the component count (0 = unreadable).
int fetch(const Stream& s, uint32_t i, float out[4]) {
    uint32_t stride = s.v.stride(), dfmt = s.v.dfmt(), nfmt = s.v.nfmt();
    uint64_t off = uint64_t(i) * stride;
    const uint8_t* p = s.bytes.data() + off;
    auto have = [&](uint64_t n) { return off + n <= s.bytes.size(); };
    switch (dfmt) {
    case 4: case 11: case 13: case 14: {  // 32, 32_32, 32_32_32, 32_32_32_32
        int n = dfmt == 4 ? 1 : dfmt == 11 ? 2 : dfmt == 13 ? 3 : 4;
        if (!have(4u * n)) return 0;
        for (int k = 0; k < n; ++k) {
            uint32_t u;
            std::memcpy(&u, p + 4 * k, 4);
            float f;
            std::memcpy(&f, &u, 4);
            out[k] = nfmt == 7 ? f : nfmt == 4 ? float(u) : nfmt == 5 ? float(int32_t(u)) : f;
        }
        return n;
    }
    case 10:  // 8_8_8_8
        if (!have(4)) return 0;
        for (int k = 0; k < 4; ++k) out[k] = nfmt == 1 ? int8_t(p[k]) / 127.0f : p[k] / 255.0f;  // uint (sprite colours) used as unorm
        return 4;
    case 5: case 12: {  // 16_16, 16_16_16_16
        int n = dfmt == 5 ? 2 : 4;
        if (!have(2u * n)) return 0;
        for (int k = 0; k < n; ++k) {
            uint16_t u;
            std::memcpy(&u, p + 2 * k, 2);
            out[k] = nfmt == 7 ? half_to_float(u) : nfmt == 0 ? u / 65535.0f : float(u);
        }
        return n;
    }
    default: return 0;
    }
}

struct Target { TexId tex = 0; int w = 0, h = 0; };

uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
    return h;
}

// Every page of [a, a+n) mapped and readable: process_vm_readv on ourselves reads one byte per page and stops with a
// short count at the first unreadable page instead of faulting (texwatch's read-only pages still read fine).
bool readable(uint64_t a, uint64_t n) {
    if (!a) return false;
    constexpr uint64_t kPage = 4096;
    constexpr int kBatch = 256;
    uint8_t sink[kBatch];
    iovec local[kBatch], remote[kBatch];
    for (uint64_t x = a & ~(kPage - 1); x < a + n;) {
        int k = 0;
        for (; k < kBatch && x < a + n; ++k, x += kPage) {
            local[k] = {&sink[k], 1};
            remote[k] = {reinterpret_cast<void*>(x < a ? a : x), 1};
        }
        ssize_t r = process_vm_readv(getpid(), local, unsigned(k), remote, unsigned(k), 0);
        if (r < 0 && (errno == ENOSYS || errno == EPERM)) return true;  // syscall filtered (some sandboxes): assume mapped
        if (r != k) return false;
    }
    return true;
}

}  // namespace

struct Backend::Impl {
    std::unique_ptr<Device> dev;
    std::unordered_map<uint64_t, Target> targets;  // by colour target base address
    // watched: pages write-protected by texwatch, stale when written_since(seq); else stale when the first 512 B change.
    struct Tex { TexId id = 0; uint64_t last = 0, seq = 0, head = 0; bool watched = false; };
    std::unordered_map<uint64_t, Tex> texs;        // by T# + content hash
    uint64_t reuploads = 0;
    Prompts prompts;                               // button-prompt glyph swap (prompts.h)
    std::set<uint64_t> bad_tex;                    // T# keys we cannot upload (logged once)
    uint64_t shown = 0, frame_no = 0;
    std::vector<Vtx> verts;
    std::set<uint64_t> logged_prog;  // (class, vs, ps, blend) combos already logged (coverage census)
    uint64_t cur_vblank = 0;
    // DCP_RENDER_AB=flagmask,vblank,out.png: at that frame, first replay it with those uFlags bits cleared and write the
    // shown target to out.png, then replay normally (same-frame before/after for a shader feature; debug aid).
    int off_mask = 0, ab_mask = 0;
    long ab_vblank = -1;
    int ab_ps = -1;
    bool ab_on_pending = false;  // optional 4th field: only at a frame that has a draw with this PS index
    std::string ab_path;
    // Game matrices are row-major with the translation in elements 3/7/11 (seen in captured VS cb0: x/640 - 1/960);
    // the shaders want column-major, so upload transposed. DCP_RENDER_NO_TRANSPOSE restores the old behaviour for checks.
    bool transpose = std::getenv("DCP_RENDER_NO_TRANSPOSE") == nullptr;
    long dump_flip = std::getenv("DCP_RENDER_DUMP") ? std::atol(std::getenv("DCP_RENDER_DUMP")) : -1;
    uint32_t sprite_ps_perm[64];                   // PS pool index -> sprite permutation bits (0xff = not a sprite PS)

    Target& target(uint64_t base, int w, int h) {
        Target& t = targets[base];
        if (!t.tex) {
            t.w = w, t.h = h;
            t.tex = dev->create_target(w, h);
            std::fprintf(stderr, "render: target %llx %dx%d\n", (unsigned long long)base, w, h);
        }
        return t;
    }

    // Device texture for a pixel-stage T#: a render target we own, or game texture memory (detiled, uploaded once).
    TexId texture(const uint32_t t[8]) {
        formats::GnfTexture g;
        std::memcpy(g.t.data(), t, 32);
        uint64_t base = (uint64_t(t[0]) | uint64_t(t[1] & 0xff) << 32) << 8;
        if (!base) return 0;
        auto rt = targets.find(base);
        if (rt != targets.end()) return rt->second.tex;
        auto info = formats::gnm_format_info(g.data_format());
        if (!info.bytes_per_element) return 0;
        formats::GnmSurface s = g.surface();
        uint64_t tiled;
        if (!formats::gnm_detile_supported(s.tile_index, s.bytes_per_element)) {
            uint64_t key = fnv(t, 28);
            if (bad_tex.insert(key).second)
                std::fprintf(stderr, "render: texture %llx fmt %u tile %u %ux%u unsupported\n", (unsigned long long)base,
                             g.data_format(), s.tile_index, s.width, s.height);
            return 0;
        }
        tiled = formats::gnm_tiled_size(s);
        uint64_t key = fnv(t, 28);
        auto it = texs.find(key);
        if (it != texs.end()) {  // fast paths: no readable() probe, no hashing (hundreds of lookups per frame)
            Tex& x = it->second;
            if (x.last == frame_no) return x.id;  // already checked this frame
            if (x.watched) {
                uint64_t now = texwatch::seq_now();
                if (!texwatch::written_since(base, tiled, x.seq)) {
                    x.seq = now, x.last = frame_no;
                    return x.id;
                }
            }
        }
        // Protect before reading (validates the range and makes a write racing the detile below mark the texture stale).
        uint64_t seq = 0;
        bool early = it == texs.end() || it->second.watched;
        bool watched = early && texwatch::protect(base, tiled, &seq);
        if (!watched && !readable(base, tiled)) return 0;
        uint64_t head = fnv(reinterpret_cast<const void*>(base), tiled < 512 ? tiled : 512);
        if (it != texs.end()) {
            Tex& x = it->second;
            bool stale = x.watched ? texwatch::written_since(base, tiled, x.seq) : head != x.head;
            x.last = frame_no;
            if (!stale) return x.id;
            if (++reuploads <= 8 || (reuploads & (reuploads - 1)) == 0)
                std::fprintf(stderr, "render: texture %llx %ux%u rewritten by the game, re-uploading (%llu so far, %s)\n",
                             (unsigned long long)base, s.width, s.height, (unsigned long long)reuploads,
                             x.watched ? "write-protect" : "head hash");
        }
        TexFormat fmt;
        switch (g.data_format()) {
        case 41: fmt = TexFormat::BC7; break;
        case 35: fmt = TexFormat::BC1; break;
        case 36: fmt = TexFormat::BC2; break;
        case 37: fmt = TexFormat::BC3; break;
        case 10: fmt = TexFormat::RGBA8; break;
        default:
            if (bad_tex.insert(key).second)
                std::fprintf(stderr, "render: texture format %u (%s) not handled\n", g.data_format(), info.name ? info.name : "?");
            return 0;
        }
        std::vector<uint8_t> lin = formats::gnm_detile({reinterpret_cast<const uint8_t*>(base), size_t(tiled)}, s);
        if (!early) watched = texwatch::protect(base, tiled, &seq);  // head-hashed texture: try write tracking again
        TexId id = dev->upload(it != texs.end() ? it->second.id : 0,  // re-upload in place
                               fmt, int(s.width), int(s.height), lin.data(), lin.size(), false);
        if (!id) {
            if (bad_tex.insert(key).second)
                std::fprintf(stderr, "render: texture %llx fmt %u %ux%u: %s upload failed\n", (unsigned long long)base,
                             g.data_format(), s.width, s.height, dev->name());
            if (it != texs.end()) dev->destroy(it->second.id), texs.erase(it);
            return 0;
        }
        texs[key] = {id, frame_no, seq, head, watched};
        if (prompts.is_atlas(base, s.width, s.height, lin.data(), lin.size())) prompts.atlas_ids.insert(id);
        else prompts.atlas_ids.erase(id);
        // DCP_RENDER_TEXDUMP=hexaddr[,path.png]: write this texture's decoded alpha-over-grey image once (debug aid).
        if (const char* e = std::getenv("DCP_RENDER_TEXDUMP"); e && std::strtoull(e, nullptr, 16) == base) {
            static bool done = false;
            if (!done) {
                done = true;
                std::vector<uint8_t> rgba, rgb(size_t(s.width) * s.height * 3);
                if (!dev->read_texture(id, int(s.width), int(s.height), &rgba)) {
                    std::fprintf(stderr, "render: texdump needs the OpenGL renderer (--renderer gl)\n");
                    return id;
                }
                for (size_t i = 0; i < size_t(s.width) * s.height; ++i)
                    for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = uint8_t(rgba[i * 4 + c] * rgba[i * 4 + 3] / 255);
                const char* comma = std::strchr(e, ',');
                std::string path = comma ? comma + 1 : "texdump.png";
                write_png(path, int(s.width), int(s.height), rgb);
                std::fprintf(stderr, "render: texdump %llx fmt %u %ux%u T# %08x %08x %08x %08x -> %s\n",
                             (unsigned long long)base, g.data_format(), s.width, s.height, t[0], t[1], t[2], t[3], path.c_str());
            }
        }
        return id;
    }

    static Sampler sampler(const uint32_t s[4]) {
        Sampler r;
        r.wrap_u = uint8_t(s[0] & 7), r.wrap_v = uint8_t(s[0] >> 3 & 7);
        r.mag_linear = (s[2] >> 20 & 3) != 0, r.min_linear = (s[2] >> 22 & 3) != 0;
        return r;
    }

    static Blend blend(uint32_t w) {
        Blend b;
        if (!(w >> 30 & 1)) return b;
        b.on = true;
        b.cs = uint8_t(w & 0x1f), b.cc = uint8_t(w >> 5 & 7), b.cd = uint8_t(w >> 8 & 0x1f);
        b.as = b.cs, b.ac = b.cc, b.ad = b.cd;
        if (w >> 29 & 1) b.as = uint8_t(w >> 16 & 0x1f), b.ac = uint8_t(w >> 21 & 7), b.ad = uint8_t(w >> 24 & 0x1f);
        return b;
    }

    // Builds the canonical vertex list for a draw and its primitive; false = nothing to draw. Point lists are dropped:
    // the GL replay returned GL_POINTS (= 0) here and its "!mode" check skipped them, so they never drew.
    bool assemble(const DrawRec& r, bool* raw, Prim* prim) {
        *prim = Prim::Triangles;
        verts.clear();
        *raw = false;
        // Streams: a group of position streams (float), then a group of RGBA8 colour streams, then uv. Sprite morph
        // modes (render_cmds.md s3.1) bind 1, 2 or 4 cells: position / colour = sum of weights from VS cb0 (+0x40).
        std::vector<int> pos, col;
        int iuv = -1;
        for (size_t k = 0; k < r.vb.size(); ++k) {
            const VSharp& v = r.vb[k].v;
            if (r.vb[k].bytes.empty()) continue;
            bool is_col = v.dfmt() == 10 || (v.nfmt() == 7 && v.dfmt() == 14 && !pos.empty());
            if (is_col && iuv < 0) col.push_back(int(k));
            else if (col.empty() && iuv < 0 && !(pos.size() && v.dfmt() != r.vb[pos[0]].v.dfmt())) pos.push_back(int(k));
            else if (iuv < 0) iuv = int(k);
        }
        // VS22 (backdrop copy): stream 0 = clip xy, stream 1 = uv (both 32_32 float). The split above takes the uv stream
        // for a second morph position, leaving uv = 0 (every copy sampled texel 0,0).
        if (r.vs == 22 && r.vb.size() >= 2) pos = {0}, col.clear(), iuv = 1;
        // VS21 (X4 snapshot copy) / VS24 (layer rect) have no fetch shader: vertex-id rect over the whole target (gcn.py
        // vs 21 / 24). Streams still bound are stale state from an earlier draw (drew a stray rect with stale uvs).
        if (r.vs == 21 || r.vs == 24) pos.clear(), col.clear(), iuv = -1;
        if (col.size() > pos.size() && pos.size() == 1) col.resize(1);
        // VS20..24 (group composite / copy / clear rects) read no constant buffers (gcn.py vs 20..24): the cb5 / cb7
        // still bound are stale state from the previous sprite part.
        const bool sprite_vs = r.cls == Cls::Sprite && !(r.vs >= 20 && r.vs <= 24);
        float w[4] = {1, 0, 0, 0};
        auto cbf = [&](size_t i) { float f = 0; if (r.vcb[0].size() >= 4 * (i + 1)) std::memcpy(&f, &r.vcb[0][4 * i], 4); return f; };
        if (pos.size() == 2) w[1] = cbf(16), w[0] = 1 - w[1];  // GUESS: morph 1 = lerp by t at cb0+0x40
        else if (pos.size() == 4) for (int k = 0; k < 4; ++k) w[k] = cbf(16 + k);
        else pos.resize(pos.empty() ? 0 : 1);
        auto make = [&](uint32_t i) {
            Vtx v{{0, 0, 0, 1}, {0, 0, 0, 0}, {0, 0}, {0, 0}};
            float t[4];
            for (size_t c = 0; c < pos.size(); ++c) {
                int n = fetch(r.vb[pos[c]], i, t);
                for (int k = 0; k < n && k < 3; ++k) v.pos[k] += w[c] * t[k];
                if (n == 4) v.pos[3] = t[3];
            }
            if (col.empty()) for (float& c : v.col) c = 1;
            for (size_t c = 0; c < col.size(); ++c) {
                int n = fetch(r.vb[col[c]], i, t);
                for (int k = 0; k < n && k < 4; ++k) v.col[k] += (col.size() == pos.size() ? w[c] : 1.0f) * t[k];
            }
            for (float& c : v.col) c = std::clamp(c, 0.0f, 1.0f);
            int n;
            if (iuv >= 0 && (n = fetch(r.vb[iuv], i, t)) > 0) for (int k = 0; k < n && k < 2; ++k) v.uv[k] = t[k];
            if (sprite_vs && r.vcb[5].size() >= 8) {  // VS cb5 "RATE" (FUN_005a6fb0): VS12 asm uv * rcp(cb5)
                float u[2];
                std::memcpy(u, r.vcb[5].data(), 8);
                if (u[0] != 0 && u[1] != 0) v.uv[0] /= u[0], v.uv[1] /= u[1];
            }
            if (sprite_vs && r.vcb[7].size() >= 16) {  // VS cb7 = colour multiplier (render_cmds.md s3.1 step 5)
                float c[4];
                std::memcpy(c, r.vcb[7].data(), 16);
                for (int k = 0; k < 4; ++k) v.col[k] *= c[k];
            }
            return v;
        };
        int ipos = pos.empty() ? -1 : pos[0];
        std::vector<uint32_t> seq;
        if (r.indexed) seq = r.indices;
        else for (uint32_t i = 0; i < r.count; ++i) seq.push_back(i);
        if (ipos < 0) {  // no vertex buffer: the VS makes a full-target rect (rect-list pass, render_cmds.md s5.2)
            *raw = true;
            const float q[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
            for (auto& c : q) verts.push_back({{c[0], c[1], 0, 1}, {1, 1, 1, 1}, {(c[0] + 1) / 2, (1 - c[1]) / 2}, {0, 0}});
            return true;
        }
        std::vector<Vtx> src;
        src.reserve(seq.size());
        for (uint32_t i : seq) src.push_back(make(i));
        switch (r.prim) {
        case 1: return false;
        case 2: verts = src; *prim = Prim::Lines; return true;
        case 3:
            for (size_t i = 1; i < src.size(); ++i) verts.push_back(src[i - 1]), verts.push_back(src[i]);
            *prim = Prim::Lines;
            return true;
        case 4: verts = src; break;
        case 5: for (size_t i = 2; i < src.size(); ++i) verts.push_back(src[0]), verts.push_back(src[i - 1]), verts.push_back(src[i]); break;
        case 6:
            for (size_t i = 2; i < src.size(); ++i) {
                bool odd = i & 1;
                verts.push_back(src[i - 2]), verts.push_back(src[odd ? i : i - 1]), verts.push_back(src[odd ? i - 1 : i]);
            }
            break;
        case 0x11:  // rect list: 3 corners; the 4th is opposite the right-angle corner (any vertex order)
            for (size_t i = 0; i + 2 < src.size(); i += 3) {
                const Vtx* t[3] = {&src[i], &src[i + 1], &src[i + 2]};
                auto d2 = [](const Vtx& p, const Vtx& q) {
                    float dx = p.pos[0] - q.pos[0], dy = p.pos[1] - q.pos[1];
                    return dx * dx + dy * dy;
                };
                // The diagonal is the longest side; the vertex not on it is the right-angle corner m.
                int m = 0;
                float l0 = d2(*t[1], *t[2]), l1 = d2(*t[0], *t[2]), l2 = d2(*t[0], *t[1]);
                if (l1 > l0 && l1 >= l2) m = 1;
                else if (l2 > l0 && l2 > l1) m = 2;
                if (m != 0) {
                    static bool once = false;
                    if (!once) once = true, std::fprintf(stderr, "render: rect list with corner at v%d (not v0)\n", m);
                }
                const Vtx &a = *t[m], &b = *t[(m + 1) % 3], &c = *t[(m + 2) % 3];
                Vtx d = b;
                for (int k = 0; k < 4; ++k) d.pos[k] = b.pos[k] + c.pos[k] - a.pos[k], d.col[k] = b.col[k];
                for (int k = 0; k < 2; ++k) d.uv[k] = b.uv[k] + c.uv[k] - a.uv[k];
                for (const Vtx& v : {a, b, c, b, d, c}) verts.push_back(v);
            }
            break;
        case 0x13:  // quad list
            for (size_t i = 0; i + 3 < src.size(); i += 4)
                for (int k : {0, 1, 2, 0, 2, 3}) verts.push_back(src[i + k]);
            break;
        default: return false;
        }
        return true;
    }

    void dump(const Frame& f) {
        std::fprintf(stderr, "render dump: flip %llu vblank %llu display %llx, %zu draws\n", (unsigned long long)f.flip,
                     (unsigned long long)f.vblank, (unsigned long long)f.display, f.draws.size());
        int n = 0;
        for (const auto& d : f.draws) {
            const DrawRec& r = *d;
            std::fprintf(stderr, "  #%d %s vs%d ps%d prim%x n%u%s blend %08x rt %llx sc%d[%d,%d,%d,%d] vb", n++, cls_name(r.cls),
                         r.vs, r.ps, r.prim, r.count, r.indexed ? "i" : "", r.blend, (unsigned long long)r.rt,
                         r.scissor_on, r.scissor[0], r.scissor[1], r.scissor[2], r.scissor[3]);
            for (const auto& s : r.vb) std::fprintf(stderr, " %u/%u/s%u/%zuB", s.v.dfmt(), s.v.nfmt(), s.v.stride(), s.bytes.size());
            for (int k = 0; k < 8; ++k)
                if (!r.vcb[k].empty()) {
                    std::fprintf(stderr, " vcb%d[%zu]:", k, r.vcb[k].size());
                    for (size_t j = 0; j + 4 <= r.vcb[k].size() && j < 80; j += 4) {
                        float x; std::memcpy(&x, &r.vcb[k][j], 4); std::fprintf(stderr, " %g", x);
                    }
                }
            for (int k = 0; k < 8; ++k)
                if (!r.pcb[k].empty()) {
                    std::fprintf(stderr, " pcb%d[%zu]:", k, r.pcb[k].size());
                    for (size_t j = 0; j + 4 <= r.pcb[k].size() && j < (k == 4 ? 256u : 48u); j += 4) {
                        float x; std::memcpy(&x, &r.pcb[k][j], 4); std::fprintf(stderr, " %g", x);
                    }
                }
            for (int k = 0; k < 2; ++k)
                if (r.tex[k][0] || r.tex[k][1])
                    std::fprintf(stderr, " t%d:%llx f%u %ux%u tile%u", k,
                                 (unsigned long long)((uint64_t(r.tex[k][0]) | uint64_t(r.tex[k][1] & 0xff) << 32) << 8),
                                 r.tex[k][1] >> 20 & 0x3f, (r.tex[k][2] & 0x3fff) + 1, (r.tex[k][2] >> 14 & 0x3fff) + 1,
                                 r.tex[k][3] >> 20 & 0x1f);
            if (!r.vb.empty() && !r.vb[0].bytes.empty()) {
                float t[4];
                std::fprintf(stderr, " v0:");
                for (uint32_t i = 0; i < 3; ++i) {
                    uint32_t ix = r.indexed && i < r.indices.size() ? r.indices[i] : i;
                    int c = fetch(r.vb[0], ix, t);
                    for (int k = 0; k < c; ++k) std::fprintf(stderr, "%s%g", k ? "," : " ", t[k]);
                }
                for (size_t s = 1; s < r.vb.size(); ++s) {  // first two vertices of the other streams
                    std::fprintf(stderr, " vb%zu:", s);
                    for (uint32_t i = 0; i < 2; ++i) {
                        uint32_t ix = r.indexed && i < r.indices.size() ? r.indices[i] : i;
                        int c = fetch(r.vb[s], ix, t);
                        for (int k = 0; k < c; ++k) std::fprintf(stderr, "%s%g", k ? "," : " ", t[k]);
                    }
                }
            }
            std::fprintf(stderr, "\n");
        }
    }

    // Pixel-shader semantics for this draw, from the disassembled PS / VS binaries (README "Pixel shaders").
    // *mask_uv: 0 none, 1 = the mask uv replaces uv (single texture is the mask), 2 = mask uv for the second texture.
    int flags_for(const DrawRec& r, bool has_tex, bool has_tex1, int* mask_uv) {
        int fl = 0;
        *mask_uv = 0;
        if (r.cls == Cls::Sprite && r.ps >= 0 && r.ps < 64 && sprite_ps_perm[r.ps] != 0xffffffffu) {
            uint32_t perm = sprite_ps_perm[r.ps];
            if (perm & 1) fl |= 2;
            if (perm & 2) fl |= 128;
            if (perm & 4) fl |= 256;
            if (perm & 8) fl |= 512;
            if (perm & 0x30) fl |= 1;  // 0x10: one texture (main, or the mask for untextured+mask VS); 0x20: both
            if (perm & 0x20) fl |= 32, *mask_uv = 2;
            else if (r.vs == 6 || r.vs == 7 || r.vs == 2) *mask_uv = 1;  // VS table row "untextured, mask" (0x79f86c)
        } else if (r.ps == 51) {
            fl = 4;
        } else if (r.ps == 58) {
            fl = 64;  // copy: rgb from the texture, alpha 1
        } else if (r.ps == 50) {
            fl = 16;  // group composite: rgb from the offscreen RT, alpha from the vertex
        } else if (r.cls == Cls::X1) {
            fl = 1;
            if (r.ps == 0) fl |= 32, *mask_uv = 2;  // VS0 + PS0: second texture at the mask uv
        } else if (r.cls == Cls::X2) {
            // X2_Execute (FUN_005a0330): textured VS17 (mask) / VS19 with PS 53/54 (mask) or 55/57, flag 0x20 picks the
            // rgb-only PS (53/55); untextured VS15 (mask) / VS16 with PS57 (mask is the only texture) / PS52.
            if (r.vs == 15) fl = 1, *mask_uv = 1;
            else if (r.vs == 17 || r.vs == 19) {
                fl = (r.ps == 53 || r.ps == 55) ? 16 : 1;
                if (r.vs == 17) fl |= 32, *mask_uv = 2;
            }
        } else if (r.ps == 55 || r.ps == 53) {
            fl = 16;  // overlay quad A/B (FUN_0007e330: VS19 + PS55 / PS57) and other rgb-only users
        } else {
            fl = has_tex ? 1 : 0;
        }
        if (!has_tex) fl &= ~(1 | 16 | 64);
        if (*mask_uv == 2 && !has_tex1) fl &= ~32;
        return fl;
    }

    // DCP_RENDER_PICK=x,y (pixels, top-left origin) with DCP_RENDER_DUMP: lists the dumped draws covering that pixel.
    std::vector<std::pair<int, int>> picks;
    bool pick_now = false;
    int pick_index = 0;
    void pick(const DrawRec& r, bool raw) {
        float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (!raw && r.vcb[0].size() >= 64) std::memcpy(m, r.vcb[0].data(), 64);
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (const Vtx& v : verts) {
            float p[4];
            for (int i = 0; i < 4; ++i) p[i] = m[i * 4] * v.pos[0] + m[i * 4 + 1] * v.pos[1] + m[i * 4 + 2] * v.pos[2] + m[i * 4 + 3] * v.pos[3];
            if (p[3] == 0) continue;
            float x = (p[0] / p[3] + 1) / 2 * r.rt_w, y = (1 - p[1] / p[3]) / 2 * r.rt_h;
            x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
        }
        for (auto [px, py] : picks)
          if (px >= x0 && px <= x1 && py >= y0 && py <= y1)
            std::fprintf(stderr, "render pick %d,%d: #%d %s vs%d ps%d bbox %.0f,%.0f..%.0f,%.0f rt %llx\n", px, py, pick_index, cls_name(r.cls),
                         r.vs, r.ps, x0, y0, x1, y1, (unsigned long long)r.rt);
    }

    void draw_one(const DrawRec& r) {
        ++draw_seq;
        bool raw = false;
        Prim prim;
        if (!assemble(r, &raw, &prim) || verts.empty()) return;
        // VS20 (group composite) / VS22 (backdrop copy): position passes through as clip xy (z 0, w 1), no matrix; the
        // VS cb0 still bound is stale state from the previous sprite part.
        if ((r.vs == 20 || r.vs == 22) && !raw) {
            for (Vtx& v : verts) v.pos[2] = 0, v.pos[3] = 1;
            raw = true;
        }
        if (r.vs == 23 && r.vcb[0].size() >= 8 && !raw) {
            // VS23 (group clear rect, FUN_005a9eb0): pixel position / cb0 (target size) -> (2x-1, 1-2y), both clamped.
            float sz[2];
            std::memcpy(sz, r.vcb[0].data(), 8);
            for (Vtx& v : verts) {
                v.pos[0] = 2 * std::clamp(sz[0] != 0 ? v.pos[0] / sz[0] : 0.0f, 0.0f, 1.0f) - 1;
                v.pos[1] = 1 - 2 * std::clamp(sz[1] != 0 ? v.pos[1] / sz[1] : 0.0f, 0.0f, 1.0f);
                v.pos[2] = 0, v.pos[3] = 1;
            }
            raw = true;
        }
        if (!r.rt) return;
        if (pick_now) pick(r, raw);
        Target& t = target(r.rt, int(r.rt_w), int(r.rt_h));
        Draw& d = cur;
        d.target = t.tex, d.tw = t.w, d.th = t.h;
        d.prim = prim;
        d.scissor = r.scissor_on && r.scissor[2] > r.scissor[0] && r.scissor[3] > r.scissor[1];
        if (d.scissor) d.sc[0] = r.scissor[0], d.sc[1] = r.scissor[1], d.sc[2] = r.scissor[2] - r.scissor[0], d.sc[3] = r.scissor[3] - r.scissor[1];
        d.blend = blend(r.blend);
        TexId tex = (r.tex[0][0] || r.tex[0][1]) ? texture(r.tex[0]) : 0;
        int mask_uv = 0;
        TexId tex1 = 0;
        if ((r.cls == Cls::Sprite || r.cls == Cls::X1 || r.cls == Cls::X2) && (r.tex[1][0] || r.tex[1][1])) tex1 = texture(r.tex[1]);
        int fl = flags_for(r, tex != 0, tex1 != 0, &mask_uv) & ~off_mask;
        if (logged_prog.insert(uint64_t(r.cls) << 56 | uint64_t(r.vs & 0xff) << 48 | uint64_t(r.ps & 0xff) << 40 | r.blend).second)
            std::fprintf(stderr, "render: first %s vs%d ps%d blend %08x flags 0x%x mask%d at vblank %llu\n", cls_name(r.cls), r.vs,
                         r.ps, r.blend, fl, mask_uv, (unsigned long long)cur_vblank);
        if (mask_uv) {
            // Mask uv (VS6/12, VS0, VS17 asm): (clip.xy - rect.xy) / rect.zw from MASKXYWH (sprite VS cb6, X1/X2 VS cb1),
            // clip = matrix * pos before the divide; the sprite VS clamps to [0,1].
            const auto& mc = r.vcb[r.cls == Cls::Sprite ? 6 : 1];
            float rc[4] = {0, 0, 1, 1}, mm[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            if (mc.size() >= 16) std::memcpy(rc, mc.data(), 16);
            if (!raw && r.vcb[0].size() >= 64) std::memcpy(mm, r.vcb[0].data(), 64);
            for (Vtx& v : verts) {
                float cx = mm[0] * v.pos[0] + mm[1] * v.pos[1] + mm[2] * v.pos[2] + mm[3] * v.pos[3];
                float cy = mm[4] * v.pos[0] + mm[5] * v.pos[1] + mm[6] * v.pos[2] + mm[7] * v.pos[3];
                float u = rc[2] != 0 ? (cx - rc[0]) / rc[2] : 0, w = rc[3] != 0 ? (cy - rc[1]) / rc[3] : 0;
                if (r.cls == Cls::Sprite) u = std::clamp(u, 0.0f, 1.0f), w = std::clamp(w, 0.0f, 1.0f);
                if (mask_uv == 1) v.uv[0] = u, v.uv[1] = w;
                else v.muv[0] = u, v.muv[1] = w;
            }
        }
        d.tex[1] = (fl & 32) ? tex1 : 0, d.samp_set[1] = (fl & 32) != 0;
        if (d.samp_set[1]) d.samp[1] = sampler(r.samp[1]);
        d.tex[0] = tex, d.samp_set[0] = tex != 0;
        if (tex) d.samp[0] = sampler(r.samp[0]);
        if (tex && (r.samp[0][0] >> 15 & 1)) {  // S# force_unnormalized (X1 glyph quads: uv in texels, S# 00008000 ...)
            formats::GnfTexture g;
            std::memcpy(g.t.data(), r.tex[0], 32);
            formats::GnmSurface s = g.surface();
            if (s.width && s.height)
                for (Vtx& v : verts) v.uv[0] /= float(s.width), v.uv[1] /= float(s.height);
        }
        float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (r.vcb[0].size() >= 64) std::memcpy(m, r.vcb[0].data(), 64);
        std::memcpy(d.mat, m, sizeof m);
        d.transpose = transpose;
        d.raw = raw;
        static const float kTint[4] = {1, 1, 1, 1}, kCm[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
        std::memcpy(d.tint, r.pcb[1].size() >= 16 ? r.pcb[1].data() : (const void*)kTint, 16);
        std::memset(d.cst, 0, 16);
        if (r.pcb[0].size() >= 16) std::memcpy(d.cst, r.pcb[0].data(), 16);
        // PS cb2: 3 rows (m0 m1 m2 scale), FUN_005a6fb0 slot 2
        std::memcpy(d.cm, (fl & 128) && r.pcb[2].size() >= 48 ? r.pcb[2].data() : (const void*)kCm, 48);
        d.factor = 0;  // PS cb3: factor (byte +0x232 / 255)
        if ((fl & 256) && r.pcb[3].size() >= 4) std::memcpy(&d.factor, r.pcb[3].data(), 4);
        std::memset(d.grad, 0, sizeof d.grad);  // PS cb4: 0x100 B, two 32-float tone-curve stages
        if (fl & 512) {
            if (r.pcb[4].size() >= 256) std::memcpy(d.grad, r.pcb[4].data(), 256);
            else fl &= ~512;
        }
        d.flags = fl;
        if (prompt_overlay.seq == draw_seq && prim == Prim::Triangles && r.cls == Cls::Sprite && prompts.kbm && !prompts.atlas_ids.count(tex)) {
            // Localised lettering ("TOUCHPAD", "OPTIONS") drawn from another atlas right over a button we replaced.
            float b[4];
            screen_bbox(r, raw, m, b);
            const PromptOverlay& o = prompt_overlay;
            float cx = (b[0] + b[2]) / 2, cy = (b[1] + b[3]) / 2, ow = o.x1 - o.x0, oh = o.y1 - o.y0;
            if (cx > o.x0 && cx < o.x1 && cy > o.y0 && cy < o.y1 && b[2] - b[0] <= ow * 1.3f && b[3] - b[1] <= oh * 1.3f) return;
        }
        if (prim == Prim::Triangles && tex && !prompts.atlas_ids.empty() && prompts.atlas_ids.count(tex) && !swap_prompt(r, raw, m)) return;
        d.v = verts.data(), d.n = verts.size();
        dev->draw(d);
    }
    Draw cur;

    // A draw sampling the button atlas (prompts.h, notes/map/button_prompts.md). In KB/M mode a button's parts become
    // one glyph: a ball (button base) is drawn as the glyph and the symbol drawn over it is dropped; a lone symbol
    // becomes the glyph; an L/R plate is dropped and the "1"/"2" digit over it draws the L1/L2/R1/R2 glyph at the
    // plate's size. The glyph is an axis-aligned screen quad (clip space, raw) centred on the original part.
    struct PromptMark { float cx, cy, h; PadBtn btn; bool plate; };
    struct PromptOverlay { float x0, y0, x1, y1; uint64_t seq = 0; } prompt_overlay;
    uint64_t draw_seq = 0;
    void screen_bbox(const DrawRec& r, bool raw, const float* m, float b[4]) const {
        b[0] = b[1] = 1e30f, b[2] = b[3] = -1e30f;
        for (const Vtx& v : verts) {
            float p[4];
            for (int i = 0; i < 4; ++i)
                p[i] = raw ? v.pos[i] : m[i * 4] * v.pos[0] + m[i * 4 + 1] * v.pos[1] + m[i * 4 + 2] * v.pos[2] + m[i * 4 + 3] * v.pos[3];
            if (p[3] == 0) continue;
            float x = (p[0] / p[3] + 1) / 2 * r.rt_w, y = (1 - p[1] / p[3]) / 2 * r.rt_h;
            b[0] = std::min(b[0], x), b[2] = std::max(b[2], x), b[1] = std::min(b[1], y), b[3] = std::max(b[3], y);
        }
    }
    std::vector<PromptMark> prompt_marks;  // this frame's balls / plates (screen px)
    std::set<std::string> prompt_logged;
    bool swap_prompt(const DrawRec& r, bool raw, const float* m) {
        float u0 = 1e30f, v0 = 1e30f, u1 = -1e30f, v1 = -1e30f, x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f, al = 0;
        for (const Vtx& v : verts) {
            u0 = std::min(u0, v.uv[0]), u1 = std::max(u1, v.uv[0]), v0 = std::min(v0, v.uv[1]), v1 = std::max(v1, v.uv[1]);
            float p[4];
            for (int i = 0; i < 4; ++i)
                p[i] = raw ? v.pos[i] : m[i * 4] * v.pos[0] + m[i * 4 + 1] * v.pos[1] + m[i * 4 + 2] * v.pos[2] + m[i * 4 + 3] * v.pos[3];
            if (p[3] == 0) continue;
            float x = (p[0] / p[3] + 1) / 2 * r.rt_w, y = (1 - p[1] / p[3]) / 2 * r.rt_h;
            x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
            al = std::max(al, v.col[3]);
        }
        const PromptCell* c = prompts.find((u0 + u1) / 2 * 1024, (v0 + v1) / 2 * 512);
        if (prompts.log) {
            char b[200];
            std::snprintf(b, sizeof b, "uv %.0f,%.0f..%.0f,%.0f col %.2f,%.2f,%.2f", u0 * 1024, v0 * 512, u1 * 1024, v1 * 512,
                          verts[0].col[0], verts[0].col[1], verts[0].col[2]);
            if (prompt_logged.insert(b).second)
                std::fprintf(stderr, "prompt: v%llu %s scr %.0f,%.0f..%.0f,%.0f -> %s\n", (unsigned long long)cur_vblank, b, x0,
                             y0, x1, y1, c ? pad_btn_name(c->btn) : "UNKNOWN");
        }
        if (!prompts.kbm || !c || x1 <= x0) return true;
        const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, h = y1 - y0;
        auto near_mark = [&](bool plate) -> const PromptMark* {
            for (auto it = prompt_marks.rbegin(); it != prompt_marks.rend(); ++it)
                if (it->plate == plate && std::fabs(it->cx - cx) < it->h * 0.6f && std::fabs(it->cy - cy) < it->h * 0.6f) return &*it;
            return nullptr;
        };
        PadBtn btn = c->btn;
        float gh = h * c->scale, gx = cx, gy = cy;  // glyph height / centre (screen px)
        switch (c->kind) {
        case PromptCell::Ball:
            prompt_marks.push_back({cx, cy, h, btn, false});
            if (btn == PadBtn::Touchpad || btn == PadBtn::Options) prompt_overlay = {x0, y0, x1, y1, draw_seq + 1};
            break;
        case PromptCell::Symbol:
            if (near_mark(false)) return false;  // drawn over a ball that already became the glyph
            break;
        case PromptCell::Plate: prompt_marks.push_back({cx, cy, h, btn, true}); return false;
        case PromptCell::Digit: {
            const PromptMark* pl = near_mark(true);
            if (!pl) return true;  // a digit elsewhere: leave it
            bool two = btn == PadBtn::L2;  // digit cells carry L1 ("1") or L2 ("2")
            btn = pl->btn == PadBtn::R1 ? (two ? PadBtn::R2 : PadBtn::R1) : (two ? PadBtn::L2 : PadBtn::L1);
            gh = pl->h * 0.9f, gx = pl->cx, gy = pl->cy;
            break;
        }
        case PromptCell::Hide: return false;
        }
        float aspect = 1;
        TexId g = prompts.glyph(btn, &aspect);
        if (!g) return true;  // unbound or no font: keep the pad glyph
        const float gw = gh * aspect;
        const float qx0 = (gx - gw / 2) / r.rt_w * 2 - 1, qx1 = (gx + gw / 2) / r.rt_w * 2 - 1;
        const float qy0 = 1 - (gy - gh / 2) / r.rt_h * 2, qy1 = 1 - (gy + gh / 2) / r.rt_h * 2;  // top, bottom
        const float q[6][4] = {{qx0, qy0, 0, 0}, {qx1, qy0, 1, 0}, {qx1, qy1, 1, 1}, {qx0, qy0, 0, 0}, {qx1, qy1, 1, 1}, {qx0, qy1, 0, 1}};
        verts.clear();
        for (auto& k : q) verts.push_back({{k[0], k[1], 0, 1}, {1, 1, 1, al}, {k[2], k[3]}, {0, 0}});
        cur.flags = 1;  // texture x vertex colour only (game alpha kept for fades)
        cur.raw = true;
        std::memset(cur.tint, 0, sizeof cur.tint);
        cur.tex[0] = g, cur.samp_set[0] = false;  // the glyph's own linear / clamp sampling
        return true;
    }
};

Backend::Backend() : m_(new Impl) {}
void Backend::set_prompt_kbm(bool kbm) {
    if (m_->prompts.kbm != kbm && m_->prompts.log)
        std::fprintf(stderr, "prompt: %s glyphs from vblank %llu\n", kbm ? "keyboard/mouse" : "gamepad", (unsigned long long)m_->cur_vblank);
    m_->prompts.kbm = kbm;
}
void Backend::set_prompt_bindings(const std::map<std::string, std::string>& b) { m_->prompts.set_bindings(b); }
Backend::~Backend() = default;

bool Backend::init() { return init(make_gl_device()); }

bool Backend::init(std::unique_ptr<Device> dev) {
    Impl& m = *m_;
    if (!dev || !dev->init()) return false;
    m.dev = std::move(dev);
    m.prompts.dev = m.dev.get();
    std::memset(m.sprite_ps_perm, 0xff, sizeof m.sprite_ps_perm);
    for (uint32_t perm = 0; perm < 48; ++perm) {
        uint32_t ps = sprite_ps_table(perm);
        if (ps < 64 && m.sprite_ps_perm[ps] == 0xffffffffu) m.sprite_ps_perm[ps] = perm;
    }
    return true;
}

void Backend::draw(const Frame& f) {
    Impl& m = *m_;
    ++m.frame_no;
    m.pick_now = false;
    if (m.dump_flip >= 0 && f.vblank >= uint64_t(m.dump_flip)) {  // DCP_RENDER_DUMP=vblank
        m.dump(f), m.dump_flip = -1;
        if (const char* p = std::getenv("DCP_RENDER_PICK")) {  // x,y;x,y;...
            m.picks.clear();
            for (int x, y, n; std::sscanf(p, "%d,%d%n", &x, &y, &n) == 2; p += n + (p[n] == ';')) m.picks.push_back({x, y});
            m.pick_now = !m.picks.empty();
        }
    }
    m.pick_index = 0;
    m.prompt_marks.clear();
    m.cur_vblank = f.vblank;
    if (m.ab_vblank == -1) {
        m.ab_vblank = -2;
        char path[512] = {};
        if (const char* e = std::getenv("DCP_RENDER_AB"); e && std::sscanf(e, "%i,%ld,%511[^,],%d", &m.ab_mask, &m.ab_vblank, path, &m.ab_ps) >= 3)
            m.ab_path = path;
        else
            m.ab_vblank = -2;
    }
    bool ab_hit = m.ab_ps < 0;
    for (const auto& d : f.draws) ab_hit = ab_hit || d->ps == m.ab_ps;
    if (m.ab_vblank >= 0 && f.vblank >= uint64_t(m.ab_vblank) && ab_hit) {
        m.off_mask = m.ab_mask;
        m.dev->begin_frame();
        for (const auto& d : f.draws) m.draw_one(*d);
        m.dev->end_frame();
        uint64_t keep = m.shown;
        if (f.display && m.targets.count(f.display)) m.shown = f.display;
        int w, h;
        std::vector<uint8_t> rgb = read_rgb(&w, &h);
        if (w) {
            write_png(m.ab_path, w, h, rgb);
            std::fprintf(stderr, "render: A/B (flags 0x%x off) wrote %s vblank %llu\n", m.ab_mask, m.ab_path.c_str(),
                         (unsigned long long)f.vblank);
        }
        m.shown = keep;
        m.off_mask = 0;
        m.ab_vblank = -2;
        m.ab_on_pending = true;
    }
    m.dev->begin_frame();
    std::map<uint64_t, int> per_rt;
    for (const auto& d : f.draws) {
        m.draw_one(*d);
        ++m.pick_index;
        ++per_rt[d->rt];
    }
    m.dev->end_frame();
    if (f.display && m.targets.count(f.display)) m.shown = f.display;
    else {
        int best = -1;  // GUESS until the final copy to the display buffer is understood: busiest target
        for (auto [rt, n] : per_rt) if (rt && n > best && m.targets.count(rt)) best = n, m.shown = rt;
    }
    if (m.ab_on_pending) {  // the same frame with every feature on, next to the A/B "off" image
        m.ab_on_pending = false;
        int w, h;
        std::vector<uint8_t> rgb = read_rgb(&w, &h);
        std::string on = m.ab_path.substr(0, m.ab_path.rfind('.')) + "_on.png";
        if (w) write_png(on, w, h, rgb);
    }
    for (auto it = m.texs.begin(); it != m.texs.end();) {  // drop textures unused for ~10 s
        if (m.frame_no - it->second.last > 600) m.dev->destroy(it->second.id), it = m.texs.erase(it);
        else ++it;
    }
}

unsigned Backend::shown_texture(int* w, int* h) const {
    auto it = m_->targets.find(m_->shown);
    if (it == m_->targets.end()) return 0;
    *w = it->second.w, *h = it->second.h;
    return it->second.tex;
}
std::vector<uint8_t> Backend::read_rgb(int* w, int* h) const {
    std::vector<uint8_t> rgb, px;
    TexId t = shown_texture(w, h);
    if (!t || !m_->dev->read_rgba(t, *w, *h, &px)) { *w = *h = 0; return rgb; }
    rgb.resize(size_t(*w) * *h * 3);
    for (size_t i = 0, n = size_t(*w) * *h; i < n; ++i) std::memcpy(&rgb[i * 3], &px[i * 4], 3);
    return rgb;
}

void Backend::present(int w, int h, Device::Overlay* overlay) const {
    int tw = 0, th = 0;
    TexId tex = shown_texture(&tw, &th);
    m_->dev->present(tex, tw, th, w, h, overlay);
}

const char* Backend::device_name() const { return m_->dev ? m_->dev->name() : "none"; }

}  // namespace dcp::render
