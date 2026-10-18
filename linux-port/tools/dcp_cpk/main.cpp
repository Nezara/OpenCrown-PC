// dcp_cpk: list / extract / verify a user-supplied CRI CPK archive.
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "formats/cpk.h"

namespace fs = std::filesystem;
using dcp::formats::CpkArchive;
using dcp::formats::CpkEntry;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: dcp_cpk list    <archive.cpk>\n"
                 "       dcp_cpk stats   <archive.cpk>\n"
                 "       dcp_cpk extract <archive.cpk> <out_dir> [path-substring]\n"
                 "       dcp_cpk verify  <archive.cpk>      (decompress every entry, write nothing)\n");
    return 2;
}

// Entry paths come from the archive; refuse anything that could escape the output directory.
bool safe_relative(const std::string& p) {
    fs::path rel(p);
    if (rel.empty() || rel.is_absolute() || rel.has_root_name()) return false;
    for (const auto& part : rel)
        if (part == "..") return false;
    return true;
}

int list(const CpkArchive& a) {
    for (const auto& e : a.entries())
        std::printf("%08llx %10u %10u %c %s\n", (unsigned long long)e.offset, e.stored_size, e.extract_size,
                    e.compressed() ? 'C' : '-', e.path().c_str());
    std::printf("%zu entries\n", a.entries().size());
    return 0;
}

int stats(const CpkArchive& a) {
    struct Agg { size_t n = 0, packed = 0; uint64_t stored = 0, extracted = 0; };
    std::map<std::string, Agg> by_ext;
    for (const auto& e : a.entries()) {
        std::string ext = fs::path(e.name).extension().string();
        auto& g = by_ext[ext.empty() ? "(none)" : ext];
        ++g.n;
        g.packed += e.compressed();
        g.stored += e.stored_size;
        g.extracted += e.extract_size;
    }
    std::printf("%-12s %7s %7s %14s %14s\n", "ext", "files", "packed", "stored", "extracted");
    for (const auto& [ext, g] : by_ext)
        std::printf("%-12s %7zu %7zu %14llu %14llu\n", ext.c_str(), g.n, g.packed, (unsigned long long)g.stored,
                    (unsigned long long)g.extracted);
    return 0;
}

int extract(const CpkArchive& a, const fs::path& out, const std::string& filter) {
    size_t n = 0;
    for (const auto& e : a.entries()) {
        std::string p = e.path();
        if (!filter.empty() && p.find(filter) == std::string::npos) continue;
        if (!safe_relative(p)) { std::fprintf(stderr, "skip unsafe path: %s\n", p.c_str()); continue; }
        auto data = a.read(e);
        fs::path dst = out / fs::path(p);
        fs::create_directories(dst.parent_path());
        std::ofstream(dst, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        ++n;
    }
    std::printf("extracted %zu files to %s\n", n, out.string().c_str());
    return 0;
}

int verify(const CpkArchive& a) {
    size_t bad = 0, packed = 0;
    for (const auto& e : a.entries()) {
        try {
            auto d = a.read(e);
            packed += e.compressed();
            if (d.size() != e.extract_size) throw std::runtime_error("size mismatch");
        } catch (const std::exception& ex) {
            ++bad;
            std::fprintf(stderr, "FAIL %s: %s\n", e.path().c_str(), ex.what());
        }
    }
    std::printf("%zu entries, %zu compressed, %zu failures\n", a.entries().size(), packed, bad);
    return bad ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string cmd = argv[1];
    try {
        CpkArchive a{fs::path(argv[2])};
        if (cmd == "list") return list(a);
        if (cmd == "stats") return stats(a);
        if (cmd == "verify") return verify(a);
        if (cmd == "extract" && argc >= 4) return extract(a, fs::path(argv[3]), argc >= 5 ? argv[4] : "");
        return usage();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
