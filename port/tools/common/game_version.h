// Shared by the Windows and Linux launchers: is the dump's eboot.bin the supported build (latest patch, 1.03)?
// param.sfo can't tell (a merged 1.03 dump still reads APP_VER 01.00), so this looks for code that only exists in 1.03:
// the "Damage Value" option (display controlled players only / all / hide), notes/map/version_check.md sites A-C.
// A plain byte search, so it works on a raw ELF and on a SELF-wrapped eboot alike (the wrapper only shifts offsets).
// Header-only so both launchers stay in lockstep without a library.
#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace dcp {

enum class GameVersion { Supported, Unsupported, Unreadable };

struct VersionSite {
    const char* what;
    std::vector<uint8_t> bytes;
};

// eboot vaddrs 0x8c500, 0x8c520, 0x503972, 0x503997 of the supported build.
inline const std::vector<VersionSite>& version_sites() {
    static const std::vector<VersionSite> sites = {
        {"damage value getter (3-state)",
         {0x8a, 0x07, 0x31, 0xc9, 0x84, 0xc0, 0x0f, 0x99, 0xc1, 0x01, 0xc9, 0xa8, 0x02, 0xb8, 0x01, 0x00}},
        {"damage value setter (3-way)",
         {0x83, 0xfe, 0x02, 0x74, 0x12, 0x83, 0xfe, 0x01, 0x74, 0x11, 0x85, 0xf6, 0x75, 0x0c, 0x8a, 0x07}},
        {"options row label 0x1a7", {0x49, 0x8d, 0xbc, 0x24, 0x80, 0x03, 0x00, 0x00, 0xbe, 0xa7, 0x01, 0x00, 0x00}},
        {"options value labels 0x1e3", {0x8d, 0xb0, 0xe3, 0x01, 0x00, 0x00}},
    };
    return sites;
}

inline bool version_matches(const std::vector<uint8_t>& image) {
    for (const VersionSite& s : version_sites())
        if (std::search(image.begin(), image.end(),
                        std::boyer_moore_horspool_searcher(s.bytes.begin(), s.bytes.end())) == image.end())
            return false;
    return true;
}

// Cached per path + size + modification time: the Linux launcher validates every folder it lists.
inline GameVersion check_game_version(const std::filesystem::path& eboot) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(eboot, ec);
    if (ec) return GameVersion::Unreadable;
    const auto mtime = std::filesystem::last_write_time(eboot, ec);
    if (ec) return GameVersion::Unreadable;
    struct Entry { uintmax_t size; std::filesystem::file_time_type mtime; GameVersion v; };
    static std::map<std::filesystem::path, Entry> cache;
    if (auto it = cache.find(eboot); it != cache.end() && it->second.size == size && it->second.mtime == mtime)
        return it->second.v;
    std::vector<uint8_t> image(size);
    std::ifstream f(eboot, std::ios::binary);
    GameVersion v = GameVersion::Unreadable;
    if (f && f.read(reinterpret_cast<char*>(image.data()), std::streamsize(size)))
        v = version_matches(image) ? GameVersion::Supported : GameVersion::Unsupported;
    cache[eboot] = {size, mtime, v};
    return v;
}

}  // namespace dcp
