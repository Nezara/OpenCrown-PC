// Checks the save files in <save root>/SAVEDATA00 before the game starts, so an unreadable one (most often a Vita or
// PS4 save copied off the console still encrypted) gets a message instead of a crash at the title screen.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace dcp::save_check {

constexpr size_t kSize = 0x20eb18;

enum class Verdict { Ok, Missing, Encrypted, WrongSize, Damaged };

struct Result {
    std::string name;  // SAVE0.DAT ..
    std::filesystem::path path;
    Verdict verdict = Verdict::Missing;
    std::string detail;  // short reason, for the log / tooltip
};

inline uint32_t crc32(const uint8_t* p, size_t n) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

inline uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

inline Result check_file(const std::filesystem::path& path) {
    Result r;
    r.path = path;
    r.name = path.filename().string();  // ASCII (SAVEn.DAT)
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return r;
    std::ifstream in(path, std::ios::binary);  // path, not a narrow string: non-ASCII folders on Windows
    if (!in) {
        r.verdict = Verdict::Damaged, r.detail = "cannot be read";
        return r;
    }
    std::vector<uint8_t> b(kSize + 1);
    in.read(reinterpret_cast<char*>(b.data()), std::streamsize(b.size()));
    b.resize(size_t(in.gcount()));
    const bool header = b.size() >= 8 && b[4] == 'D' && b[5] == 'C' && b[6] == 'S' && b[7] == 'D';
    if (!header) {
        // No plain header: an encrypted console copy (the usual case) or not a save at all.
        r.verdict = Verdict::Encrypted, r.detail = "no DCSD header (" + std::to_string(b.size()) + " bytes)";
    } else if (b.size() != kSize) {
        r.verdict = Verdict::WrongSize, r.detail = "wrong size (" + std::to_string(b.size()) + " bytes)";
    } else if (le32(b.data()) != crc32(b.data() + 4, kSize - 4) || b[0x20eb15] > 3) {
        r.verdict = Verdict::Damaged, r.detail = "checksum mismatch";
    } else {
        r.verdict = Verdict::Ok;
    }
    return r;
}

// Every SAVEn.DAT that exists and is not usable (empty = all fine or no saves yet).
inline std::vector<Result> bad_saves(const std::filesystem::path& save_root) {
    std::vector<Result> out;
    for (int i = 0; i < 4; ++i) {
        Result r = check_file(save_root / "SAVEDATA00" / ("SAVE" + std::to_string(i) + ".DAT"));
        if (r.verdict != Verdict::Ok && r.verdict != Verdict::Missing) out.push_back(r);
    }
    return out;
}

// Renames a bad save to SAVEn.DAT.unreadable (or .unreadable2, ...) so the game starts without it. Keeps the bytes.
inline bool move_aside(const Result& r, std::string* new_name = nullptr) {
    std::error_code ec;
    for (int n = 1; n < 100; ++n) {
        std::filesystem::path to = r.path;
        to += n == 1 ? ".unreadable" : ".unreadable" + std::to_string(n);
        if (std::filesystem::exists(to, ec)) continue;
        std::filesystem::rename(r.path, to, ec);
        if (!ec && new_name) *new_name = to.filename().string();
        return !ec;
    }
    return false;
}

inline const char* explain(Verdict v) {
    switch (v) {
        case Verdict::Encrypted:
            return "This file is not a readable Dragon's Crown save. If you copied it from a PS Vita or PS4, it is "
                   "most likely still encrypted: consoles encrypt saves on their storage. You will need to decrypt it "
                   "before OpenCrown can use it. Saves from OpenCrown, shadPS4 and Vita3K already work.";
        case Verdict::WrongSize:
            return "This save has the wrong size, so it is incomplete or comes from a save tool that adds its own "
                   "header. Export it again as the plain SAVE0.DAT file.";
        case Verdict::Damaged:
            return "This save is damaged (its checksum does not match), for example from an interrupted copy or an "
                   "edit. Copy it again, or use a backup.";
        default: return "";
    }
}

}  // namespace dcp::save_check
