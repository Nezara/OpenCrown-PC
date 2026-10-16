// HLE pure-helper tests (no game data): aligned host allocation, direct-memory offset allocator, /app0 path mapping.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>

#include "hle/kernel/pure.h"

using namespace dcp::hle;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main() {
    // 2 MiB alignment as requested by the game's MapDirectMemory calls; memory is writable.
    for (uint64_t len : {uint64_t(0x4000), uint64_t(0x200000), uint64_t(0x1234000)}) {
        auto* p = static_cast<uint8_t*>(alloc_aligned(len, 0x200000));
        CHECK(p && (reinterpret_cast<uint64_t>(p) & 0x1FFFFF) == 0);
        if (p) p[0] = 1, p[len - 1] = 2, VirtualFree(p, 0, MEM_RELEASE);
    }
    CHECK(alloc_aligned(0, 0x200000) == nullptr);
    for (uint64_t len : {uint64_t(0x4000), uint64_t(0x4200000)}) {  // direct memory: 40-bit GPU-visible addresses
        auto* p = static_cast<uint8_t*>(alloc_low(len, 0x200000));
        uint64_t a = reinterpret_cast<uint64_t>(p);
        CHECK(p && (a & 0x1FFFFF) == 0 && a + len <= (1ull << 40) && a >= 0x200000000ull);
        if (p) p[len - 1] = 1, VirtualFree(p, 0, MEM_RELEASE);
    }

    uint64_t cur = 0;
    CHECK(dmem_alloc(&cur, 0, 0x1000000, 0x4000, 0x200000) == 0);
    CHECK(dmem_alloc(&cur, 0, 0x1000000, 0x4000, 0x200000) == 0x200000);
    CHECK(dmem_alloc(&cur, 0, 0x1000000, 0x10000, 0x4000) == 0x204000);
    CHECK(dmem_alloc(&cur, 0, 0x1000000, 0x1000000, 0x200000) == -1);  // does not fit
    CHECK(dmem_alloc(&cur, 0, 0x1000000, 0x4000, 0x3000) == -1);        // align not a power of two

    CHECK(map_app0("C:/dump", "/app0/eboot.bin") == "C:/dump/eboot.bin");
    CHECK(map_app0("C:/dump", "/app0//sce_sys/./param.sfo") == "C:/dump/sce_sys/param.sfo");
    CHECK(map_app0("C:/dump", "/app0") == "C:/dump");
    CHECK(!map_app0("C:/dump", "/app0/../secret"));
    CHECK(!map_app0("C:/dump", "/app0/a/..\\b"));
    CHECK(!map_app0("C:/dump", "/app0/c:/x"));
    CHECK(!map_app0("C:/dump", "/app0x/eboot.bin"));
    CHECK(!map_app0("C:/dump", "/savedata0/x"));

    // Save-data mount table: /savedataN -> host dir, writable paths only under a live mount.
    SaveMountTable mt;
    CHECK(!mt.resolve("/savedata0/SAVE0.DAT"));
    CHECK(mt.mount("SAVEDATA00", "R:/saves/SAVEDATA00") == "/savedata0");
    CHECK(mt.mount("SAVEDATA00", "R:/saves/SAVEDATA00").empty());  // already mounted
    CHECK(mt.mount("OTHER", "R:/saves/OTHER") == "/savedata1");
    CHECK(mt.resolve("/savedata0/SAVE0.DAT") == "R:/saves/SAVEDATA00/SAVE0.DAT");
    CHECK(mt.resolve("/savedata1/sce_sys/param.bin") == "R:/saves/OTHER/sce_sys/param.bin");
    CHECK(!mt.resolve("/savedata0/../SAVEDATA01/x"));
    CHECK(!mt.resolve("/savedata00/x"));
    CHECK(!mt.resolve("/savedata2/x"));
    CHECK(!mt.resolve("/app0/eboot.bin"));
    CHECK(mt.dir_name_of("/savedata1") && *mt.dir_name_of("/savedata1") == "OTHER");
    CHECK(mt.umount("/savedata0"));
    CHECK(!mt.umount("/savedata0"));
    CHECK(!mt.resolve("/savedata0/SAVE0.DAT"));
    CHECK(mt.mount("SAVEDATA00", "R:/x") == "/savedata0");  // freed slot is reused
    for (int i = 2; i < SaveMountTable::kMax; ++i) CHECK(!mt.mount("D" + std::to_string(i), "R:/d").empty());
    CHECK(mt.mount("FULL", "R:/d").empty());
    CHECK(mt.resolve("/savedata15/a") == "R:/d/a");

    CHECK(valid_savedata_dirname("SAVEDATA00") && valid_savedata_dirname("a-b_c"));
    CHECK(!valid_savedata_dirname("") && !valid_savedata_dirname("..") && !valid_savedata_dirname("a/b") &&
          !valid_savedata_dirname("C:") && !valid_savedata_dirname("0123456789012345678901234567890123"));
    CHECK(savedata_name_match(nullptr, "SAVEDATA00") && savedata_name_match("", "X"));
    CHECK(savedata_name_match("SAVEDATA00", "SAVEDATA00") && !savedata_name_match("SAVEDATA00", "SAVEDATA01"));
    CHECK(savedata_name_match("SAVE%", "SAVEDATA00") && savedata_name_match("%00", "SAVEDATA00"));
    CHECK(savedata_name_match("SAVEDATA0_", "SAVEDATA07") && !savedata_name_match("SAVEDATA0_", "SAVEDATA0"));

    // Input script.
    std::vector<InputEvent> ev;
    int bad = 0;
    CHECK(parse_input_script("# title\n300 CROSS\r\n120 OPTIONS  # comment\n130 0\n\n400 up+Circle\n500 0x4008\n", &ev, &bad));
    CHECK(ev.size() == 5 && ev[0].frame == 120 && ev[0].buttons == 0x8 && ev[2].buttons == 0x4000);
    CHECK(script_buttons_at(ev, 0) == 0 && script_buttons_at(ev, 120) == 0x8 && script_buttons_at(ev, 129) == 0x8);
    CHECK(script_buttons_at(ev, 130) == 0 && script_buttons_at(ev, 350) == 0x4000);
    CHECK(script_buttons_at(ev, 400) == 0x2010 && script_buttons_at(ev, 99999) == 0x4008);
    CHECK(!parse_input_script("10 CROSS\nabc CROSS\n", &ev, &bad) && bad == 2);
    CHECK(!parse_input_script("10 JUMP\n", &ev, &bad) && bad == 1);
    CHECK(!parse_input_script("10\n", &ev, &bad) && bad == 1);
    CHECK(parse_input_script("", &ev, &bad) && ev.empty());

    std::printf("hle: %d failures\n", g_fail);
    return g_fail ? 1 : 0;
}
