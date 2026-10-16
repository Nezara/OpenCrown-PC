#include "eboot_image.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace dcp::formats {
namespace {

template <class T> T rd(const std::vector<uint8_t>& d, size_t off) {
    if (off + sizeof(T) > d.size()) throw std::runtime_error("eboot: truncated");
    T v;
    std::memcpy(&v, d.data() + off, sizeof(T));
    return v;
}

constexpr uint32_t kSelfMagic = 0x1D3D154F;
constexpr uint32_t kPtLoad = 1;

}  // namespace

EbootImage::EbootImage(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("eboot: cannot open " + path.string());
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    // SELF: 0x20 header, nseg 32-byte segment entries, then the ELF header + phdrs; segment data lives at entry
    // offsets (flag 0x800 = has data block, bits 20.. = phdr index, 0x2 encrypted, 0x8 compressed).
    bool self = rd<uint32_t>(d, 0) == kSelfMagic;
    size_t elf = 0;
    uint16_t nseg = 0;
    if (self) {
        nseg = rd<uint16_t>(d, 0x18);
        elf = 0x20 + size_t(nseg) * 32;
    }
    if (std::memcmp(d.data() + elf, "\x7f" "ELF", 4) != 0) throw std::runtime_error("eboot: no ELF header");
    uint64_t phoff = rd<uint64_t>(d, elf + 0x20);
    uint16_t phent = rd<uint16_t>(d, elf + 0x36), phnum = rd<uint16_t>(d, elf + 0x38);

    for (uint16_t i = 0; i < phnum; ++i) {
        size_t p = elf + phoff + size_t(i) * phent;
        if (rd<uint32_t>(d, p) != kPtLoad) continue;
        uint64_t off = rd<uint64_t>(d, p + 8), vaddr = rd<uint64_t>(d, p + 16), filesz = rd<uint64_t>(d, p + 32);
        if (self) {
            off = UINT64_MAX;
            for (uint16_t s = 0; s < nseg; ++s) {
                uint64_t fl = rd<uint64_t>(d, 0x20 + s * 32), so = rd<uint64_t>(d, 0x28 + s * 32);
                if (!(fl & 0x800) || ((fl >> 20) & 0xFFF) != i) continue;
                if (fl & 0xA) throw std::runtime_error("eboot: segment is encrypted or compressed (need a decrypted dump)");
                off = so;
            }
            if (off == UINT64_MAX) continue;
        }
        if (off + filesz > d.size()) throw std::runtime_error("eboot: segment out of file");
        segs_.push_back({vaddr, filesz, std::vector<uint8_t>(d.begin() + ptrdiff_t(off), d.begin() + ptrdiff_t(off + filesz))});
    }
    if (segs_.empty()) throw std::runtime_error("eboot: no loadable segments");
}

std::vector<uint8_t> EbootImage::read(uint64_t vaddr, size_t size) const {
    for (const auto& s : segs_)
        if (vaddr >= s.vaddr && vaddr - s.vaddr + size <= s.filesz)
            return {s.bytes.begin() + ptrdiff_t(vaddr - s.vaddr), s.bytes.begin() + ptrdiff_t(vaddr - s.vaddr + size)};
    throw std::runtime_error("eboot: address range not in a loaded segment");
}

}  // namespace dcp::formats
