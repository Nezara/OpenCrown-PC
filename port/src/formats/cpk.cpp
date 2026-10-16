#include "cpk.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "crilayla.h"
#include "utf_table.h"

namespace dcp::formats {
namespace {

int seek64(std::FILE* f, uint64_t off) {
#ifdef _WIN32
    return _fseeki64(f, int64_t(off), SEEK_SET);
#else
    return fseeko(f, off_t(off), SEEK_SET);
#endif
}

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

}  // namespace

CpkArchive::CpkArchive(const std::filesystem::path& path) {
#ifdef _WIN32
    f_ = _wfopen(path.c_str(), L"rb");
#else
    f_ = std::fopen(path.c_str(), "rb");
#endif
    if (!f_) throw std::runtime_error("cpk: cannot open " + path.string());
    file_size_ = std::filesystem::file_size(path);

    auto hdr_bytes = read_packet(0, "CPK ");
    UtfTable hdr(hdr_bytes);
    if (hdr.name() != "CpkHeader") throw std::runtime_error("cpk: unexpected header table " + hdr.name());
    align_ = uint32_t(hdr.u64(0, "Align"));
    uint64_t toc = hdr.u64(0, "TocOffset"), content = hdr.u64(0, "ContentOffset");
    if (!toc) throw std::runtime_error("cpk: archive has no filename TOC (ITOC-only archives not supported yet)");

    // TOC file offsets are relative to whichever of the TOC or content area comes first.
    uint64_t base = content ? std::min(toc, content) : toc;
    auto toc_bytes = read_packet(toc, "TOC ");
    UtfTable t(toc_bytes);
    entries_.reserve(t.rows());
    for (uint32_t r = 0; r < t.rows(); ++r) {
        CpkEntry e;
        e.dir = t.str(r, "DirName");
        e.name = t.str(r, "FileName");
        e.offset = base + t.u64(r, "FileOffset");
        e.stored_size = uint32_t(t.u64(r, "FileSize"));
        e.extract_size = uint32_t(t.u64(r, "ExtractSize", e.stored_size));
        e.id = uint32_t(t.u64(r, "ID"));
        if (e.offset + e.stored_size > file_size_) throw std::runtime_error("cpk: entry out of bounds: " + e.path());
        entries_.push_back(std::move(e));
    }
}

CpkArchive::~CpkArchive() {
    if (f_) std::fclose(f_);
}

const CpkEntry* CpkArchive::find(const std::string& path) const {
    for (const auto& e : entries_)
        if (e.path() == path) return &e;
    return nullptr;
}

std::vector<uint8_t> CpkArchive::read_at(uint64_t off, size_t size) const {
    std::vector<uint8_t> buf(size);
    if (seek64(f_, off) != 0 || std::fread(buf.data(), 1, size, f_) != size)
        throw std::runtime_error("cpk: read failed");
    return buf;
}

// Packets are a 16-byte header (magic, flags, LE size, pad) followed by an @UTF table.
std::vector<uint8_t> CpkArchive::read_packet(uint64_t off, const char magic[4]) const {
    auto head = read_at(off, 16);
    if (std::memcmp(head.data(), magic, 4) != 0) throw std::runtime_error(std::string("cpk: missing packet ") + magic);
    uint32_t size = le32(head.data() + 8);
    return read_at(off + 16, size);
}

std::vector<uint8_t> CpkArchive::read_raw(const CpkEntry& e) const {
    return read_at(e.offset, e.stored_size);
}

std::vector<uint8_t> CpkArchive::read_range(const CpkEntry& e, uint64_t off, size_t size) const {
    if (e.compressed()) throw std::runtime_error("cpk: range read of compressed entry: " + e.path());
    if (off > e.stored_size || size > e.stored_size - off) throw std::runtime_error("cpk: range out of entry: " + e.path());
    return read_at(e.offset + off, size);
}

std::vector<uint8_t> CpkArchive::read(const CpkEntry& e) const {
    auto raw = read_raw(e);
    if (!e.compressed() || !is_crilayla(raw)) return raw;
    auto out = crilayla_decompress(raw);
    if (out.size() != e.extract_size) throw std::runtime_error("cpk: size mismatch after decompress: " + e.path());
    return out;
}

}  // namespace dcp::formats
