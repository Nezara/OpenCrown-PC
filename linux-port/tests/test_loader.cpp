// Loader tests on synthetic bytes only: SHA-1 vectors, NIDs of SDK names, and a tiny fake SCE ELF loaded + relocated.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "loader/elf.h"
#include "loader/hle.h"
#include "loader/nid.h"

using namespace dcp::loader;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static std::string hex(const std::array<uint8_t, 20>& h) {
    std::string s;
    char b[3];
    for (auto x : h) std::snprintf(b, 3, "%02x", x), s += b;
    return s;
}

template <class T> static void put(std::vector<uint8_t>& d, size_t off, T v) {
    if (d.size() < off + sizeof(T)) d.resize(off + sizeof(T));
    std::memcpy(d.data() + off, &v, sizeof(T));
}

static void test_sha1_nid() {
    CHECK(hex(sha1("abc", 3)) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex(sha1("", 0)) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    std::string m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";  // 2-block message
    CHECK(hex(sha1(m.data(), m.size())) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK(nid_from_name("memcpy") == "Q3VBxCXhUHs");
    CHECK(nid_from_name("__stack_chk_fail") == "Ou3iL1abvng");
    CHECK(nid_from_name("sceKernelUsleep") == "1jfXLRVzisc");
    CHECK(nid_from_name("exit") == "uMei1W9uyNo");
    const NidName* n = lookup_nid("bzQExy189ZI");
    CHECK(n && n->name == "_init_env" && n->lib == "libc");
}

// ELF: phdrs for PT_LOAD (rw, vaddr 0, 0x2000 mem, 0x100 file), PT_DYNAMIC, PT_SCE_DYNLIBDATA.
static void test_fake_elf() {
    std::vector<uint8_t> d(0x1000);
    std::memcpy(d.data(), "\x7f" "ELF", 4);
    put<uint16_t>(d, 0x10, 0xFE10);
    put<uint64_t>(d, 0x18, 0x40);       // entry
    put<uint64_t>(d, 0x20, 0x40);       // phoff
    put<uint16_t>(d, 0x36, 56);
    put<uint16_t>(d, 0x38, 3);
    auto ph = [&](int i, uint32_t type, uint32_t flags, uint64_t off, uint64_t va, uint64_t fs, uint64_t ms) {
        size_t p = 0x40 + i * 56;
        put(d, p, type), put(d, p + 4, flags), put(d, p + 8, off), put(d, p + 16, va), put(d, p + 32, fs), put(d, p + 40, ms);
    };
    ph(0, kPtLoad, 6, 0x400, 0, 0x100, 0x2000);
    ph(1, kPtDynamic, 4, 0x600, 0, 0x100, 0x100);
    ph(2, kPtSceDynlibData, 4, 0x800, 0, 0x400, 0);
    put<uint64_t>(d, 0x400 + 0x10, 0x1234);  // a byte pattern that must survive the copy
    // dynlib blob @0x800: strtab +0, symtab +0x40, rela +0x100, jmprel +0x200
    const char str[] = "\0Q3VBxCXhUHs#B#C\0zzzzzzzzzzz#B#C\0";
    std::memcpy(d.data() + 0x800, str, sizeof str);
    // sym0 null, sym1 memcpy (func, import), sym2 unknown data object
    put<uint32_t>(d, 0x840 + 24, 1), put<uint8_t>(d, 0x840 + 24 + 4, 0x12);
    put<uint32_t>(d, 0x840 + 48, 18), put<uint8_t>(d, 0x840 + 48 + 4, 0x11);
    auto rela = [&](size_t at, uint64_t off, uint32_t type, uint32_t sym, int64_t add) {
        put(d, at, off), put<uint64_t>(d, at + 8, uint64_t(sym) << 32 | type), put(d, at + 16, add);
    };
    rela(0x900, 0x20, kRRelative, 0, 0x1000);
    rela(0x918, 0x28, kR64, 2, 16);
    rela(0x930, 0x38, 99, 0, 0);  // unsupported type must be counted, not applied
    rela(0xA00, 0x30, kRJumpSlot, 1, 0);
    int64_t tags[][2] = {{0x61000035, 0}, {0x61000037, sizeof str}, {0x61000039, 0x40}, {0x6100003F, 72},
                         {0x6100002F, 0x100}, {0x61000031, 72}, {0x61000029, 0x200}, {0x6100002D, 24}, {0, 0}};
    for (size_t i = 0; i < sizeof tags / sizeof tags[0]; ++i) put(d, 0x600 + i * 16, tags[i][0]), put(d, 0x608 + i * 16, tags[i][1]);

    ElfInfo info = parse_elf(d);
    CHECK(info.type == 0xFE10 && info.load.size() == 1 && info.image_size == 0x4000);
    CHECK(info.syms.size() == 3 && info.syms[1].name == "Q3VBxCXhUHs#B#C" && info.relocs.size() == 4);
    Image img = map_image(info, d, 0);
    uint64_t base = reinterpret_cast<uint64_t>(img.base);
    Binder binder;
    RelocStats st = relocate(img, info, [&](const Symbol& s) { return binder.resolve(s); });
    auto at = [&](size_t o) { uint64_t v; std::memcpy(&v, img.base + o, 8); return v; };
    CHECK(at(0x10) == 0x1234);
    CHECK(at(0x20) == base + 0x1000);
    CHECK(at(0x28) != 16 && at(0x38) == 0);
    CHECK(at(0x1100) == 0);  // bss zero
    CHECK(st.by_type[kRRelative] == 1 && st.by_type[kR64] == 1 && st.by_type[kRJumpSlot] == 1 && st.unsupported[99] == 1);
    CHECK(binder.stats().stubs == 1 && binder.stats().data == 1 && binder.stats().unknown == 1);
    // Call the stub through the SysV ABI: logs UNIMPL once, returns 0.
    binder.finalize();
    using Fn = DCP_SYSV uint64_t (*)(uint64_t, uint64_t, uint64_t);
    CHECK(reinterpret_cast<Fn>(at(0x30))(1, 2, 3) == 0);
    auto calls = recent_unimpl_calls();
    CHECK(!calls.empty() && calls.back().find("libc::memcpy(1, 2, 3") == 0);
    unmap_image(img);
}

int main() {
    test_sha1_nid();
    test_fake_elf();
    std::printf("loader tests: %d failures\n", g_fail);
    return g_fail ? 1 : 0;
}
