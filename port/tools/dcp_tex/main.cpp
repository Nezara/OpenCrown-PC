// dcp_tex: inspect / export .ftx texture banks (FTEX -> FTX0 -> GNF) from a loose file or a CPK.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "formats/cpk.h"
#include "formats/dds.h"
#include "formats/ftx.h"

namespace fs = std::filesystem;
using namespace dcp::formats;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: dcp_tex list   <file.ftx | archive.cpk> [path-substring]\n"
                 "       dcp_tex export <archive.cpk | file.ftx> <out_dir> [path-substring]  (each texture -> .dds)\n"
                 "       dcp_tex verify <archive.cpk>   (parse + detile every .ftx)\n");
    return 2;
}

std::vector<uint8_t> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    return {std::istreambuf_iterator<char>(f), {}};
}

bool is_ftx_name(const std::string& n) {
    if (n.size() < 4) return false;
    std::string e = n.substr(n.size() - 4);
    for (auto& c : e) c = char(std::tolower((unsigned char)c));
    return e == ".ftx";
}

bool loose_ftx(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    char m[4] = {};
    f.read(m, 4);
    return f && std::string(m, 4) == "FTEX";
}

// Calls fn(path, bytes) for each .ftx in a CPK (or the single loose file) whose path contains `filter`.
void for_each_ftx(const fs::path& src, const std::string& filter,
                  const std::function<void(const std::string&, const std::vector<uint8_t>&)>& fn) {
    if (loose_ftx(src)) {
        fn(src.filename().string(), read_file(src));
        return;
    }
    CpkArchive a{src};
    for (const auto& e : a.entries()) {
        std::string p = e.path();
        if (!is_ftx_name(e.name) || (!filter.empty() && p.find(filter) == std::string::npos)) continue;
        fn(p, a.read(e));
    }
}

std::string fmt_name(const GnfTexture& t) {
    auto fi = gnm_format_info(t.data_format());
    std::string d = fi.name ? fi.name : "fmt" + std::to_string(t.data_format());
    return d + "/" + gnm_num_format_name(t.num_format());
}

// Why a texture cannot be exported as-is, or empty if it can.
std::string unsupported_reason(const GnfTexture& t) {
    auto s = t.surface();
    if (!s.bytes_per_element) return "unknown data format";
    if (!gnm_to_dxgi(t.data_format(), t.num_format())) return "no DXGI mapping";
    if (!gnm_detile_supported(s.tile_index, s.bytes_per_element)) return "tile mode";
    if (t.mips() != 1 || t.slices() != 1 || t.depth() != 1) return "mips/slices";
    return {};
}

int list(const fs::path& src, const std::string& filter) {
    size_t files = 0, texs = 0;
    for_each_ftx(src, filter, [&](const std::string& path, const std::vector<uint8_t>& bytes) {
        auto tx = parse_ftx(bytes);
        ++files;
        std::printf("%s: %zu textures\n", path.c_str(), tx.size());
        for (const auto& t : tx) {
            const auto& x = t.tex;
            std::printf("  [%u.%u] %-36s %5ux%-5u pitch %5u %-10s tile %2u type %2u mips %u slices %u sel %03x %u B\n",
                        t.chunk, t.index, t.name.c_str(), x.width(), x.height(), x.pitch(), fmt_name(x).c_str(),
                        x.tile_index(), x.type(), x.mips(), x.slices(), x.dst_sel(), x.data_size());
            ++texs;
        }
    });
    std::printf("%zu files, %zu textures\n", files, texs);
    return 0;
}

// Names come from the archive: keep only harmless characters so they cannot form paths.
std::string sanitize(std::string s) {
    if (s.size() > 4 && s.substr(s.size() - 4) == ".gnf") s.resize(s.size() - 4);
    for (auto& c : s)
        if (!std::isalnum((unsigned char)c) && c != '_' && c != '-' && c != '.') c = '_';
    if (s.empty() || s[0] == '.') s = "_" + s;
    return s;
}

bool safe_relative(const std::string& p) {
    fs::path rel(p);
    if (rel.empty() || rel.is_absolute() || rel.has_root_name()) return false;
    for (const auto& part : rel)
        if (part == "..") return false;
    return true;
}

int export_dds(const fs::path& src, const fs::path& out, const std::string& filter) {
    size_t written = 0, skipped = 0, bad = 0;
    for_each_ftx(src, filter, [&](const std::string& path, const std::vector<uint8_t>& bytes) {
        if (!safe_relative(path)) { std::fprintf(stderr, "skip unsafe path: %s\n", path.c_str()); return; }
        try {
            auto tx = parse_ftx(bytes);
            fs::path dir = out / fs::path(path).replace_extension();
            fs::create_directories(dir);
            for (const auto& t : tx) {
                if (auto why = unsupported_reason(t.tex); !why.empty()) {
                    std::fprintf(stderr, "skip %s [%u.%u]: %s\n", path.c_str(), t.chunk, t.index, why.c_str());
                    ++skipped;
                    continue;
                }
                auto dds = make_dds(t.tex.width(), t.tex.height(), gnm_to_dxgi(t.tex.data_format(), t.tex.num_format()),
                                    ftx_detile(t));
                char pre[16];
                std::snprintf(pre, sizeof pre, "%02u_", t.chunk);
                std::string fn = pre + sanitize(t.name) + (t.index ? "_" + std::to_string(t.index) : "") + ".dds";
                std::ofstream(dir / fn, std::ios::binary).write((const char*)dds.data(), std::streamsize(dds.size()));
                ++written;
            }
        } catch (const std::exception& ex) {
            ++bad;
            std::fprintf(stderr, "FAIL %s: %s\n", path.c_str(), ex.what());
        }
    });
    std::printf("%zu textures written to %s, %zu skipped, %zu failures\n", written, out.string().c_str(), skipped, bad);
    return bad ? 1 : 0;
}

int verify(const fs::path& src) {
    size_t files = 0, texs = 0, bad = 0, padded = 0;
    std::map<std::string, size_t> hist, unsupported;
    for_each_ftx(src, "", [&](const std::string& path, const std::vector<uint8_t>& bytes) {
        ++files;
        try {
            auto tx = parse_ftx(bytes);
            for (const auto& t : tx) {
                const auto& x = t.tex;
                std::string key = fmt_name(x) + " tile" + std::to_string(x.tile_index()) +
                                  (x.mips() > 1 ? " mips>1" : "") + (x.slices() > 1 ? " slices>1" : "") +
                                  (x.type() != 9 ? " type" + std::to_string(x.type()) : "");
                ++hist[key];
                ++texs;
                if (auto why = unsupported_reason(x); !why.empty()) {
                    ++unsupported[key + " (" + why + ")"];
                    continue;
                }
                if (gnm_tiled_size(x.surface()) != x.data_size()) ++padded;  // data_size >= is enforced by detile
                ftx_detile(t);
            }
        } catch (const std::exception& ex) {
            ++bad;
            std::fprintf(stderr, "FAIL %s: %s\n", path.c_str(), ex.what());
        }
    });
    size_t nu = 0;
    for (const auto& [k, n] : unsupported) nu += n;
    std::string h;
    for (const auto& [k, n] : hist) h += (h.empty() ? "" : ", ") + k + "=" + std::to_string(n);
    std::printf("%zu files, %zu textures [%s], %zu size!=tiled, %zu unsupported, %zu failures\n", files, texs, h.c_str(),
                padded, nu, bad);
    for (const auto& [k, n] : unsupported) std::printf("  unsupported: %s x%zu\n", k.c_str(), n);
    return bad ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string cmd = argv[1];
    try {
        if (cmd == "list") return list(fs::path(argv[2]), argc >= 4 ? argv[3] : "");
        if (cmd == "verify") return verify(fs::path(argv[2]));
        if (cmd == "export" && argc >= 4) return export_dds(fs::path(argv[2]), fs::path(argv[3]), argc >= 5 ? argv[4] : "");
        return usage();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
