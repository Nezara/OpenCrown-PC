// Shared by the Windows and Linux launchers: is a dump file usable as it is, and if not, why (still encrypted, cut
// short, not the right kind of file)? Header-only like game_version.h, so both launchers say the same thing.
// Executables (eboot.bin, sce_module/*.prx) use the loader's rule (src/loader/elf.cpp unwrap_self): a plain ELF, or a
// fake-signed SELF whose data segments carry neither the encrypted (0x2) nor the compressed (0x8) flag.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "formats/utf_table.h"

namespace dcp {

enum class FileState { Ok, Encrypted, Truncated, WrongKind, Unreadable };

inline FileState exe_state(const std::filesystem::path& p) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(p, ec);
    if (ec) return FileState::Unreadable;
    std::ifstream f(p, std::ios::binary);
    std::vector<uint8_t> h(size_t(std::min<uintmax_t>(size, 0x10000)));
    if (!f || !f.read(reinterpret_cast<char*>(h.data()), std::streamsize(h.size()))) return FileState::Unreadable;
    auto u16 = [&](size_t o) { uint16_t v = 0; if (o + 2 <= h.size()) std::memcpy(&v, &h[o], 2); return v; };
    auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= h.size()) std::memcpy(&v, &h[o], 4); return v; };
    auto u64 = [&](size_t o) { uint64_t v = 0; if (o + 8 <= h.size()) std::memcpy(&v, &h[o], 8); return v; };
    if (u32(0) == 0x464C457F) return FileState::Ok;     // "\x7fELF": already unwrapped
    if (u32(0) != 0x1D3D154F) return FileState::WrongKind;  // not a SELF either (e.g. a PKG or a renamed file)
    const uint16_t nseg = u16(0x18);
    if (0x20 + size_t(nseg) * 32 > h.size()) return FileState::Truncated;
    for (uint16_t s = 0; s < nseg; ++s) {
        const uint64_t fl = u64(0x20 + s * 32), off = u64(0x28 + s * 32), sz = u64(0x30 + s * 32);
        if (!(fl & 0x800)) continue;
        if (fl & 0xA) return FileState::Encrypted;
        if (off + sz > size) return FileState::Truncated;
    }
    return FileState::Ok;
}

// A CRI archive ("CPK ") is whole when every file its TOC lists lies inside it: the loader's own rule
// (src/formats/cpk.cpp). No fixed size, so a re-packed archive (a fan translation) passes. `need` = the size the TOC
// asks for (0 when the TOC could not be read).
inline FileState cpk_state(const std::filesystem::path& p, uintmax_t* need = nullptr) {
    if (need) *need = 0;
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(p, ec);
    if (ec) return FileState::Unreadable;
    std::ifstream f(p, std::ios::binary);
    if (!f) return FileState::Unreadable;
    // Packets: "CPK " / "TOC " magic, flags, LE u32 size, pad, then an @UTF table.
    auto packet = [&](uint64_t off, const char* magic, std::vector<uint8_t>& out) {
        uint8_t h[16];
        if (off + 16 > size || !f.seekg(std::streamoff(off)) || !f.read(reinterpret_cast<char*>(h), 16))
            return FileState::Truncated;
        if (std::memcmp(h, magic, 4) != 0) return FileState::WrongKind;
        const uint32_t n = uint32_t(h[8]) | uint32_t(h[9]) << 8 | uint32_t(h[10]) << 16 | uint32_t(h[11]) << 24;
        if (off + 16 + n > size) return FileState::Truncated;
        out.resize(n);
        return f.read(reinterpret_cast<char*>(out.data()), n) ? FileState::Ok : FileState::Unreadable;
    };
    std::vector<uint8_t> b;
    if (FileState s = packet(0, "CPK ", b); s != FileState::Ok) return s;
    try {
        const formats::UtfTable hdr(b);
        const uint64_t toc = hdr.u64(0, "TocOffset"), content = hdr.u64(0, "ContentOffset");
        if (hdr.name() != "CpkHeader" || !toc) return FileState::WrongKind;  // ITOC-only archives: loader can't read
        if (FileState s = packet(toc, "TOC ", b); s != FileState::Ok) return s;
        const formats::UtfTable t(b);
        const uint64_t base = content ? std::min(toc, content) : toc;  // TOC offsets are relative to the earlier one
        uint64_t end = toc + 16 + b.size();
        for (uint32_t r = 0; r < t.rows(); ++r)
            end = std::max(end, base + t.u64(r, "FileOffset") + t.u64(r, "FileSize"));
        if (need) *need = end;
        return end > size ? FileState::Truncated : FileState::Ok;
    } catch (const std::exception&) {
        return FileState::WrongKind;  // malformed @UTF table
    }
}

// The data files of the supported dump (US, latest patch 1.03) and their sizes there. A different size is fine
// (cpk_state decides); the launcher only notes it as modified data.
struct DumpCpk {
    const char* name;
    uintmax_t size;
};
inline const DumpCpk kDumpCpks[] = {{"DRACRO.CPK", 5264082928ull}, {"DRACRO_US.CPK", 871756328ull}};

// One line for the launcher's check list (what is wrong and what to do), for a file that exists.
inline std::string exe_problem(const char* name, FileState s) {
    switch (s) {
    case FileState::Ok: return "";
    case FileState::Encrypted: return std::string(name) + " is still encrypted: dump it again, decrypted";
    case FileState::Truncated: return std::string(name) + " is incomplete: copy it again";
    case FileState::WrongKind: return std::string(name) + " is not a PS4 executable";
    default: return std::string(name) + " could not be read";
    }
}
// `want` = cpk_state's `need` (0: unknown, the archive is cut short before its TOC).
inline std::string cpk_problem(const char* name, FileState s, uintmax_t have, uintmax_t want) {
    char gb[64] = "";
    switch (s) {
    case FileState::Ok: return "";
    case FileState::Truncated:
        if (want) std::snprintf(gb, sizeof gb, " (%.2f of %.2f GB)", double(have) / 1e9, double(want) / 1e9);
        return std::string(name) + " is incomplete" + gb + ": copy it again";
    case FileState::WrongKind: return std::string(name) + " is not valid game data: dump it again";
    default: return std::string(name) + " could not be read";
    }
}

}  // namespace dcp
