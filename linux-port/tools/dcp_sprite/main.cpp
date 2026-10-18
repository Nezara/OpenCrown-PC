// dcp_sprite: inspect / verify .mbs sprite banks (paired with .ftx) and .mcb collision files inside a CPK.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "formats/bsb.h"
#include "formats/cpk.h"
#include "formats/ftx.h"
#include "formats/mbs.h"
#include "formats/mcb.h"

namespace fs = std::filesystem;
using namespace dcp::formats;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: dcp_sprite info   <archive.cpk> <mbs-path>\n"
                 "       dcp_sprite verify <archive.cpk>   (every .mbs (+ same-stem .ftx) and .mcb)\n"
                 "       dcp_sprite bsb-info <archive.cpk> <bsb-path>\n"
                 "       dcp_sprite bsb-verify <archive.cpk>   (every .bsb; art names resolve in the archive)\n");
    return 2;
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}
bool has_ext(const std::string& n, const char* e) { return n.size() > 4 && lower(n.substr(n.size() - 4)) == e; }

// FTX0 child count of the same-stem .ftx, or -1 when there is none.
long ftx_children(const CpkArchive& a, const std::string& mbs_path) {
    std::string want = lower(mbs_path.substr(0, mbs_path.size() - 4) + ".ftx");
    for (const auto& e : a.entries())
        if (lower(e.path()) == want) {
            auto tx = parse_ftx(a.read(e));
            uint32_t n = 0;
            for (const auto& t : tx) n = std::max(n, t.chunk + 1);
            return long(n);
        }
    return -1;
}

int info(const fs::path& cpk, const std::string& path) {
    CpkArchive a{cpk};
    const CpkEntry* e = a.find(path);
    if (!e) { std::fprintf(stderr, "not found: %s\n", path.c_str()); return 1; }
    auto bytes = a.read(*e);
    Mbs m = parse_mbs(bytes);
    std::printf("%s: stem \"%s\" version 0x%x child_slots %u\n", path.c_str(), m.stem.c_str(), m.version, m.child_slots);
    for (int s = 0; s < kMbsSections; ++s)
        std::printf("  %-9s @0x%06llx  %6u x %3u\n", mbs_section_name(MbsSection(s)), (unsigned long long)m.offset[s],
                    m.count[s], kMbsStride[s]);
    for (size_t i = 0; i < m.motions.size(); ++i) {
        const auto& mo = m.motions[i];
        size_t frames = 0;
        for (size_t t = 0; t < mo.track_count; ++t) frames += m.tracks[mo.first_track + t].frame_count;
        std::printf("  motion %3zu %-24s tracks %2u frames %4zu%s\n", i, mo.name.c_str(), mo.track_count, frames,
                    mo.flags & 1 ? " (disabled)" : "");
    }
    std::set<int> used;
    for (const auto& p : m.parts)
        if (!p.untextured()) used.insert(p.texture);
    std::string u;
    for (int t : used) u += " " + std::to_string(t);
    long tc = ftx_children(a, path);
    std::printf("textures used:%s; paired ftx children: %ld; violations: %zu\n", u.c_str(), tc,
                tc < 0 ? 0 : mbs_texture_violations(m, size_t(tc)));
    return 0;
}

int verify(const fs::path& cpk) {
    CpkArchive a{cpk};
    size_t mbs = 0, mcb = 0, motions = 0, parts = 0, paired = 0, viol = 0, viol_files = 0, bad = 0, mcb_recs = 0, mcb_short = 0;
    int max_tex = -1;
    size_t nz[kMbsSections] = {};
    for (const auto& e : a.entries()) {
        bool is_b = has_ext(e.name, ".mbs"), is_c = has_ext(e.name, ".mcb");
        if (!is_b && !is_c) continue;
        std::string path = e.path();
        try {
            auto bytes = a.read(e);
            if (is_c) {
                ++mcb;
                auto c = parse_mcb(bytes);
                mcb_recs += c.records.size();
                if (c.missing) ++mcb_short;
                continue;
            }
            ++mbs;
            Mbs m = parse_mbs(bytes);
            motions += m.motions.size();
            parts += m.parts.size();
            for (int s = 0; s < kMbsSections; ++s)
                if (m.count[s]) ++nz[s];
            for (const auto& p : m.parts)
                if (!p.untextured()) max_tex = std::max(max_tex, int(p.texture));
            long tc = ftx_children(a, path);
            if (tc >= 0) {
                ++paired;
                if (size_t v = mbs_texture_violations(m, size_t(tc))) {
                    viol += v;
                    ++viol_files;
                    std::fprintf(stderr, "TEX %s: %zu parts index >= %ld children\n", path.c_str(), v, tc);
                }
            }
        } catch (const std::exception& ex) {
            ++bad;
            std::fprintf(stderr, "FAIL %s: %s\n", path.c_str(), ex.what());
        }
    }
    std::printf("%zu mbs (%zu with ftx), %zu motions, %zu parts, max texture index %d, %zu texture violations in %zu files; "
                "non-empty quad3d=%zu aux=%zu s108=%zu s110=%zu s118=%zu; %zu mcb, %zu records, %zu with count > records; %zu failures\n",
                mbs, paired, motions, parts, max_tex, viol, viol_files, nz[int(MbsSection::Quad3D)],
                nz[int(MbsSection::Aux)], nz[int(MbsSection::Ev108)], nz[int(MbsSection::S110)],
                nz[int(MbsSection::S118)], mcb, mcb_recs, mcb_short, bad);
    return bad ? 1 : 0;
}

int bsb_info(const fs::path& cpk, const std::string& path) {
    CpkArchive a{cpk};
    const CpkEntry* e = a.find(path);
    if (!e) { std::fprintf(stderr, "not found: %s\n", path.c_str()); return 1; }
    auto bytes = a.read(*e);
    Bsb s = parse_bsb(bytes);
    std::printf("%s: version 0x%x bounds %d,%d..%d,%d names %zu items %zu\n", path.c_str(), s.version, s.bounds[0],
                s.bounds[1], s.bounds[2], s.bounds[3], s.names.size(), s.items.size());
    for (size_t i = 0; i < s.names.size(); ++i) std::printf("  name %3zu %s\n", i, s.names[i].c_str());
    for (const auto& it : s.items)
        if (it.has_mone)
            std::printf("  %*smone %-20s pos %6d %6d %6d scale %.2f %.2f layer %d motion %d mbs %s\n", it.depth, "", it.name.c_str(),
                        it.x, it.y, it.z, it.scale_x, it.scale_y, it.mone.layer, it.mone.motion,
                        s.name(it.mone.mbs) ? s.name(it.mone.mbs)->c_str() : "-");
    return 0;
}

int bsb_verify(const fs::path& cpk) {
    CpkArchive a{cpk};
    std::set<std::string> paths;
    for (const auto& e : a.entries()) paths.insert(lower(e.path()));
    size_t files = 0, items = 0, mones = 0, art = 0, missing = 0, bad = 0;
    for (const auto& e : a.entries()) {
        if (!has_ext(e.name, ".bsb")) continue;
        ++files;
        try {
            Bsb s = parse_bsb(a.read(e));
            items += s.items.size();
            for (const auto& it : s.items) {
                if (!it.has_mone) continue;
                ++mones;
                if (!s.name(it.mone.mbs)) continue;
                ++art;
                for (int32_t k : {it.mone.mbs, it.mone.ftx})
                    if (s.name(k) && !paths.count(lower(*s.name(k)))) {
                        if (++missing <= 5) std::fprintf(stderr, "MISSING %s: %s\n", e.path().c_str(), s.name(k)->c_str());
                    }
            }
        } catch (const std::exception& ex) {
            ++bad;
            std::fprintf(stderr, "FAIL %s: %s\n", e.path().c_str(), ex.what());
        }
    }
    std::printf("%zu bsb, %zu items, %zu art items (%zu with names), %zu unresolved art names; %zu failures\n", files, items,
                mones, art, missing, bad);
    return bad ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string cmd = argv[1];
    try {
        if (cmd == "bsb-verify") return bsb_verify(fs::path(argv[2]));
        if (cmd == "bsb-info" && argc >= 4) return bsb_info(fs::path(argv[2]), argv[3]);
        if (cmd == "verify") return verify(fs::path(argv[2]));
        if (cmd == "info" && argc >= 4) return info(fs::path(argv[2]), argv[3]);
        return usage();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
