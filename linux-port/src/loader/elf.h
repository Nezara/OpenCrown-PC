// PS4 (SCE) ELF loading into this process: SELF unwrap, SCE dynamic info, mapping, relocation.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dcp::loader {

constexpr uint32_t kPtLoad = 1, kPtDynamic = 2, kPtSceRelro = 0x61000010, kPtSceDynlibData = 0x61000000;
constexpr uint32_t kPtTls = 7, kPtSceProcParam = 0x61000001, kPtSceModuleParam = 0x61000002;
constexpr uint32_t kRNone = 0, kR64 = 1, kRGlobDat = 6, kRJumpSlot = 7, kRRelative = 8, kRDtpMod64 = 16, kRDtpOff64 = 17;

struct Segment { uint32_t type, flags; uint64_t offset, vaddr, filesz, memsz; };
struct Symbol { std::string name; uint8_t info; uint16_t shndx; uint64_t value; };  // name = "nid#lib#mod" for imports
struct Reloc { uint64_t offset; uint32_t type, sym; int64_t addend; };

struct ElfInfo {
    uint16_t type = 0;          // 0xFE10 = ET_SCE_DYNEXEC
    uint64_t entry = 0, init = 0;
    std::vector<Segment> load;  // PT_LOAD + PT_SCE_RELRO, the segments that get mapped
    std::vector<Symbol> syms;
    std::vector<Reloc> relocs;  // rela + jmprel
    std::vector<std::string> needed;
    uint64_t image_size = 0;    // highest vaddr + memsz, page aligned
    uint64_t proc_param = 0;    // vaddr of PT_SCE_PROCPARAM (eboot) / PT_SCE_MODULEPARAM (prx), 0 if absent
    Segment tls{};              // PT_TLS (memsz 0 = none)
    uint64_t tls_align = 0;
};

// Decrypted (fake-signed) SELF -> flat ELF file bytes (plain ELF is returned unchanged). Throws on encrypted input.
std::vector<uint8_t> unwrap_self(std::vector<uint8_t> file);
// Parses program headers + the SCE dynamic section. Throws on malformed input.
ElfInfo parse_elf(const std::vector<uint8_t>& elf);

struct Image { uint8_t* base = nullptr; uint64_t size = 0; uint32_t tls_modid = 0; };  // tls_modid: for DTPMOD64
// Reserves the image (at `preferred` if free, else anywhere), copies segments, zeroes bss. Pages are RW until protect().
Image map_image(const ElfInfo& info, const std::vector<uint8_t>& elf, uint64_t preferred = 0x400000);
void protect_image(const Image& img, const ElfInfo& info);  // r-x / r-- / rw- by segment flags
void unmap_image(Image& img);

struct RelocStats { std::map<uint32_t, size_t> by_type; std::map<uint32_t, size_t> unsupported; };
// Resolver returns the absolute address for an imported symbol (0 = leave unresolved).
using SymResolver = std::function<uint64_t(const Symbol&)>;
RelocStats relocate(const Image& img, const ElfInfo& info, const SymResolver& resolve);

}  // namespace dcp::loader
