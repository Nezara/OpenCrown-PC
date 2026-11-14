// Launcher save check (tools/common/save_check.h) on synthetic files: good, encrypted-looking, cut, damaged.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

#include "../tools/common/save_check.h"

using namespace dcp::save_check;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static std::vector<uint8_t> good_save() {
    std::vector<uint8_t> b(kSize, 0);
    b[4] = 'D', b[5] = 'C', b[6] = 'S', b[7] = 'D', b[8] = 0x26;
    const uint32_t c = crc32(b.data() + 4, kSize - 4);
    for (int i = 0; i < 4; ++i) b[i] = uint8_t(c >> (8 * i));
    return b;
}

static void write(const fs::path& p, const std::vector<uint8_t>& b) {
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

int main() {
    CHECK(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xcbf43926u);  // standard CRC-32 check value

    const fs::path root = fs::temp_directory_path() / "dcp_test_save_check";
    fs::remove_all(root);
    fs::create_directories(root / "SAVEDATA00");
    const fs::path dir = root / "SAVEDATA00";

    CHECK(bad_saves(root).empty());  // no saves yet
    write(dir / "SAVE0.DAT", good_save());
    CHECK(check_file(dir / "SAVE0.DAT").verdict == Verdict::Ok);
    CHECK(bad_saves(root).empty());

    std::vector<uint8_t> enc(kSize);  // a console copy still encrypted: noise, no header
    std::mt19937 rng(1);
    for (auto& x : enc) x = uint8_t(rng());
    enc[4] = 'x';
    write(dir / "SAVE1.DAT", enc);
    CHECK(check_file(dir / "SAVE1.DAT").verdict == Verdict::Encrypted);

    std::vector<uint8_t> cut = good_save();
    cut.resize(kSize - 100);
    write(dir / "SAVE2.DAT", cut);
    CHECK(check_file(dir / "SAVE2.DAT").verdict == Verdict::WrongSize);

    std::vector<uint8_t> dmg = good_save();
    dmg[0x1000] ^= 1;
    write(dir / "SAVE3.DAT", dmg);
    CHECK(check_file(dir / "SAVE3.DAT").verdict == Verdict::Damaged);

    std::vector<uint8_t> slot = good_save();  // valid checksum but a slot index the game can't use (the crash)
    slot[0x20eb15] = 14;
    const uint32_t c = crc32(slot.data() + 4, kSize - 4);
    for (int i = 0; i < 4; ++i) slot[i] = uint8_t(c >> (8 * i));
    write(dir / "SAVE3.DAT", slot);
    CHECK(check_file(dir / "SAVE3.DAT").verdict == Verdict::Damaged);

    auto bad = bad_saves(root);
    CHECK(bad.size() == 3);
    std::string to;
    CHECK(move_aside(bad[0], &to) && to == "SAVE1.DAT.unreadable");
    CHECK(!fs::exists(dir / "SAVE1.DAT") && fs::exists(dir / "SAVE1.DAT.unreadable"));
    write(dir / "SAVE1.DAT", enc);
    CHECK(move_aside(check_file(dir / "SAVE1.DAT"), &to) && to == "SAVE1.DAT.unreadable2");  // never overwrites
    CHECK(bad_saves(root).size() == 2);

    fs::remove_all(root);
    std::printf(g_fail ? "%d failure(s)\n" : "save_check: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
