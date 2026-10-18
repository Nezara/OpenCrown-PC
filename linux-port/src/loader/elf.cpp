#include "elf.h"

#include <cstring>
#include <stdexcept>

#include <sys/mman.h>

namespace dcp::loader {
namespace {

template <class T> T rd(const std::vector<uint8_t>& d, uint64_t off) {
    if (off > d.size() || sizeof(T) > d.size() - off) throw std::runtime_error("elf: truncated");
    T v;
    std::memcpy(&v, d.data() + off, sizeof(T));
    return v;
}

constexpr uint32_t kSelfMagic = 0x1D3D154F;
constexpr int64_t kDtNeeded = 1, kDtInit = 12, kDtSceJmprel = 0x61000029, kDtScePltRelSz = 0x6100002D,
                  kDtSceRela = 0x6100002F, kDtSceRelaSz = 0x61000031, kDtSceStrtab = 0x61000035,
                  kDtSceStrSz = 0x61000037, kDtSceSymtab = 0x61000039, kDtSceSymtabSz = 0x6100003F;

}  // namespace

std::vector<uint8_t> unwrap_self(std::vector<uint8_t> d) {
    if (d.size() >= 4 && std::memcmp(d.data(), "\x7f" "ELF", 4) == 0) return d;
    if (rd<uint32_t>(d, 0) != kSelfMagic) throw std::runtime_error("eboot: neither SELF nor ELF");
    uint16_t nseg = rd<uint16_t>(d, 0x18);
    uint64_t e = 0x20 + uint64_t(nseg) * 32;
    if (rd<uint32_t>(d, e) != 0x464C457F) throw std::runtime_error("eboot: no ELF header in SELF");
    uint64_t phoff = rd<uint64_t>(d, e + 0x20);
    uint16_t phent = rd<uint16_t>(d, e + 0x36), phnum = rd<uint16_t>(d, e + 0x38);
    uint64_t size = phoff + uint64_t(phent) * phnum;
    for (uint16_t i = 0; i < phnum; ++i) {
        uint64_t p = e + phoff + uint64_t(i) * phent;
        size = std::max(size, rd<uint64_t>(d, p + 8) + rd<uint64_t>(d, p + 32));
    }
    std::vector<uint8_t> out(size);
    std::memcpy(out.data(), d.data() + e, phoff + uint64_t(phent) * phnum);
    // SELF segment entry: flags (0x800 = has data, bits 20.. = phdr index, 0x2 encrypted, 0x8 compressed), offset, size.
    for (uint16_t s = 0; s < nseg; ++s) {
        uint64_t fl = rd<uint64_t>(d, 0x20 + s * 32), so = rd<uint64_t>(d, 0x28 + s * 32), sz = rd<uint64_t>(d, 0x30 + s * 32);
        if (!(fl & 0x800)) continue;
        if (fl & 0xA) throw std::runtime_error("eboot: segment is encrypted or compressed (need a decrypted dump)");
        uint16_t idx = uint16_t((fl >> 20) & 0xFFF);
        if (idx >= phnum) continue;
        uint64_t poff = rd<uint64_t>(d, e + phoff + uint64_t(idx) * phent + 8);
        if (so + sz > d.size() || poff + sz > out.size()) throw std::runtime_error("eboot: SELF segment out of range");
        std::memcpy(out.data() + poff, d.data() + so, sz);
    }
    return out;
}

ElfInfo parse_elf(const std::vector<uint8_t>& d) {
    if (rd<uint32_t>(d, 0) != 0x464C457F) throw std::runtime_error("elf: bad magic");
    ElfInfo info;
    info.type = rd<uint16_t>(d, 0x10);
    info.entry = rd<uint64_t>(d, 0x18);
    uint64_t phoff = rd<uint64_t>(d, 0x20);
    uint16_t phent = rd<uint16_t>(d, 0x36), phnum = rd<uint16_t>(d, 0x38);
    const Segment* dyn = nullptr;
    const Segment* dlib = nullptr;
    std::vector<Segment> all;
    for (uint16_t i = 0; i < phnum; ++i) {
        uint64_t p = phoff + uint64_t(i) * phent;
        if (rd<uint32_t>(d, p) == kPtTls) info.tls_align = rd<uint64_t>(d, p + 48);
        all.push_back({rd<uint32_t>(d, p), rd<uint32_t>(d, p + 4), rd<uint64_t>(d, p + 8), rd<uint64_t>(d, p + 16),
                       rd<uint64_t>(d, p + 32), rd<uint64_t>(d, p + 40)});
    }
    for (const auto& s : all) {
        if (s.type == kPtLoad || s.type == kPtSceRelro) {
            if (s.filesz > s.memsz || s.offset + s.filesz > d.size()) throw std::runtime_error("elf: segment out of file");
            info.load.push_back(s);
            info.image_size = std::max(info.image_size, (s.vaddr + s.memsz + 0x3FFF) & ~uint64_t(0x3FFF));
        }
        if (s.type == kPtSceProcParam || s.type == kPtSceModuleParam) info.proc_param = s.vaddr;
        if (s.type == kPtTls) info.tls = s;
        if (s.type == kPtDynamic) dyn = &s;
        if (s.type == kPtSceDynlibData) dlib = &s;
    }
    if (info.load.empty()) throw std::runtime_error("elf: no loadable segments");
    if (!dyn || !dlib) return info;  // static image: nothing to link

    std::map<int64_t, std::vector<uint64_t>> tags;
    for (uint64_t o = 0; o + 16 <= dyn->filesz; o += 16) {
        int64_t t = rd<int64_t>(d, dyn->offset + o);
        if (t == 0) break;
        tags[t].push_back(rd<uint64_t>(d, dyn->offset + o + 8));
    }
    auto tag = [&](int64_t t) -> uint64_t {
        auto it = tags.find(t);
        return it == tags.end() ? 0 : it->second[0];
    };
    uint64_t base = dlib->offset, strtab = base + tag(kDtSceStrtab), strsz = tag(kDtSceStrSz);
    auto str = [&](uint64_t o) {
        if (o >= strsz) throw std::runtime_error("elf: string offset out of strtab");
        const char* s = reinterpret_cast<const char*>(d.data() + strtab + o);
        return std::string(s, strnlen(s, strsz - o));
    };
    if (strtab + strsz > d.size()) throw std::runtime_error("elf: strtab out of file");
    if (tags.count(kDtInit)) info.init = tag(kDtInit);
    for (uint64_t v : tags[kDtNeeded]) info.needed.push_back(str(v));

    uint64_t symtab = base + tag(kDtSceSymtab), symsz = tag(kDtSceSymtabSz);
    for (uint64_t o = 0; o + 24 <= symsz; o += 24)
        info.syms.push_back({str(rd<uint32_t>(d, symtab + o)), rd<uint8_t>(d, symtab + o + 4),
                             rd<uint16_t>(d, symtab + o + 6), rd<uint64_t>(d, symtab + o + 8)});
    auto relas = [&](int64_t at, int64_t sz) {
        if (!tags.count(at)) return;
        uint64_t r = base + tag(at), n = tag(sz);
        for (uint64_t o = 0; o + 24 <= n; o += 24) {
            uint64_t inf = rd<uint64_t>(d, r + o + 8);
            info.relocs.push_back({rd<uint64_t>(d, r + o), uint32_t(inf), uint32_t(inf >> 32), rd<int64_t>(d, r + o + 16)});
        }
    };
    relas(kDtSceRela, kDtSceRelaSz);
    relas(kDtSceJmprel, kDtScePltRelSz);
    return info;
}

Image map_image(const ElfInfo& info, const std::vector<uint8_t>& elf, uint64_t preferred) {
    Image img;
    img.size = info.image_size;
    // The host is PIE (loaded high), so 0x400000 is normally free. MAP_FIXED_NOREPLACE never clobbers a mapping;
    // pre-4.17 kernels ignore the flag and treat the address as a hint, so check where it landed.
    void* p = MAP_FAILED;
    if (preferred) {
        p = mmap(reinterpret_cast<void*>(preferred), img.size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED && reinterpret_cast<uint64_t>(p) != preferred) munmap(p, img.size), p = MAP_FAILED;
    }
    if (p == MAP_FAILED) p = mmap(nullptr, img.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);  // PIE: any base works
    if (p == MAP_FAILED) throw std::runtime_error("loader: mmap failed for the image");
    img.base = static_cast<uint8_t*>(p);  // fresh anonymous pages are zero, so bss needs no extra work
    for (const auto& s : info.load) {
        if (s.vaddr + s.memsz > img.size) throw std::runtime_error("loader: segment outside image");
        std::memcpy(img.base + s.vaddr, elf.data() + s.offset, s.filesz);
    }
    return img;
}

void protect_image(const Image& img, const ElfInfo& info) {
    for (const auto& s : info.load) {
        int prot = (s.flags & 1) ? PROT_READ | PROT_EXEC : (s.flags & 2) ? PROT_READ | PROT_WRITE : PROT_READ;
        uint64_t lo = s.vaddr & ~uint64_t(0xFFF), hi = (s.vaddr + s.memsz + 0xFFF) & ~uint64_t(0xFFF);
        if (mprotect(img.base + lo, hi - lo, prot) != 0) throw std::runtime_error("loader: mprotect failed");
    }
    // x86 keeps the instruction cache coherent: no flush needed.
}

void unmap_image(Image& img) {
    if (img.base) munmap(img.base, img.size);
    img = {};
}

RelocStats relocate(const Image& img, const ElfInfo& info, const SymResolver& resolve) {
    RelocStats st;
    uint64_t base = reinterpret_cast<uint64_t>(img.base);
    for (const auto& r : info.relocs) {
        if (r.offset + 8 > img.size) throw std::runtime_error("loader: relocation outside image");
        uint64_t* slot = reinterpret_cast<uint64_t*>(img.base + r.offset);
        auto sym_addr = [&]() -> uint64_t {
            if (r.sym >= info.syms.size()) throw std::runtime_error("loader: relocation symbol index out of range");
            const Symbol& s = info.syms[r.sym];
            return s.shndx ? base + s.value : resolve(s);  // defined locally vs imported
        };
        switch (r.type) {
        case kRRelative: *slot = base + uint64_t(r.addend); break;
        case kR64: *slot = sym_addr() + uint64_t(r.addend); break;
        case kRGlobDat:
        case kRJumpSlot: *slot = sym_addr(); break;
        case kRDtpMod64: *slot = img.tls_modid; break;  // module's TLS block index for __tls_get_addr
        case kRDtpOff64: *slot = (r.sym ? info.syms[r.sym].value : 0) + uint64_t(r.addend); break;
        case kRNone: break;
        default: ++st.unsupported[r.type]; continue;
        }
        ++st.by_type[r.type];
    }
    return st;
}

}  // namespace dcp::loader
