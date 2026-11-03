// cpu_compat tests.
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <windows.h>

#include "loader/cpu_compat.h"

using namespace dcp::loader::cpu_compat;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace {

struct Enc {
    std::vector<uint8_t> b;
    void modrm(int reg, const Site& s) {
        if (s.rm != kMem) { b.push_back(uint8_t(0xc0 | (reg & 7) << 3 | (s.rm & 7))); return; }
        const bool idx = s.index != kNoReg;
        const uint8_t ss = s.scale == 8 ? 3 : s.scale == 4 ? 2 : s.scale == 2 ? 1 : 0;
        if (s.base == kNoReg) {
            b.push_back(uint8_t(0x04 | (reg & 7) << 3));
            b.push_back(uint8_t(ss << 6 | (idx ? s.index & 7 : 4) << 3 | 5));
            for (int i = 0; i < 4; ++i) b.push_back(uint8_t(uint32_t(s.disp) >> (8 * i)));
            return;
        }
        const int mod = s.disp == 0 && (s.base & 7) != 5 ? 0 : s.disp == int8_t(s.disp) ? 1 : 2;
        const bool sib = idx || (s.base & 7) == 4;
        b.push_back(uint8_t(mod << 6 | (reg & 7) << 3 | (sib ? 4 : s.base & 7)));
        if (sib) b.push_back(uint8_t(ss << 6 | (idx ? s.index & 7 : 4) << 3 | (s.base & 7)));
        for (int i = 0; i < (mod == 1 ? 1 : mod == 2 ? 4 : 0); ++i) b.push_back(uint8_t(uint32_t(s.disp) >> (8 * i)));
    }
    int rx(const Site& s) const { return s.rm == kMem ? (s.index != kNoReg ? s.index : 0) : 0; }
    int rb(const Site& s) const { return s.rm == kMem ? (s.base != kNoReg ? s.base : 0) : s.rm; }
    void rex(int w, int reg, const Site& s) {
        uint8_t r = uint8_t(0x40 | w << 3 | (reg >> 3 & 1) << 2 | (rx(s) >> 3 & 1) << 1 | (rb(s) >> 3 & 1));
        if (r != 0x40) b.push_back(r);
    }
    void vex(int reg, int vvvv, int op, const Site& s) {
        b.push_back(0xc4);
        b.push_back(uint8_t((~reg >> 3 & 1) << 7 | (~rx(s) >> 3 & 1) << 6 | (~rb(s) >> 3 & 1) << 5 | 2));
        b.push_back(uint8_t((s.size == 8 ? 0x80 : 0) | (~vvvv & 15) << 3));
        b.push_back(uint8_t(op));
        modrm(reg, s);
    }
};

std::vector<uint8_t> original(const Site& s) {
    Enc e;
    switch (s.kind) {
    case kMovbeLoad:
    case kMovbeStore: {
        const int reg = s.kind == kMovbeLoad ? s.dst : s.a;
        if (s.size == 2) e.b.push_back(0x66);
        e.rex(s.size == 8, reg, s);
        e.b.insert(e.b.end(), {0x0f, 0x38, uint8_t(s.kind == kMovbeLoad ? 0xf0 : 0xf1)});
        e.modrm(reg, s);
        break;
    }
    case kAndn: e.vex(s.dst, s.a, 0xf2, s); break;
    case kBextr: e.vex(s.dst, s.a, 0xf7, s); break;
    case kBlsi: e.vex(3, s.dst, 0xf3, s); break;
    case kLzcnt:
    case kTzcnt:
        if (s.size == 2) e.b.push_back(0x66);
        e.b.push_back(0xf3);
        e.rex(s.size == 8, s.dst, s);
        e.b.insert(e.b.end(), {0x0f, uint8_t(s.kind == kLzcnt ? 0xbd : 0xbc)});
        e.modrm(s.dst, s);
        break;
    }
    return e.b;
}

struct Ctx {
    uint64_t in[16], in_flags, out[16], out_flags, host_rsp;
};
constexpr int kIn = 0, kInFlags = 128, kOut = 136, kOutFlags = 264, kHost = 272;

struct Runner {
    uint8_t* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    void run(Ctx* c, const uint8_t* body, size_t n) {
        uint8_t* p = code;
        auto b = [&](std::initializer_list<uint8_t> v) { for (uint8_t x : v) *p++ = x; };
        auto d32 = [&](int32_t v) { std::memcpy(p, &v, 4), p += 4; };
        b({0x53, 0x55, 0x57, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});
        b({0x48, 0x89, 0xa1}), d32(kHost);
        b({0x49, 0x89, 0xcf});
        b({0x41, 0xff, 0xb7}), d32(kInFlags), b({0x9d});
        for (int r = 0; r < 15; ++r)
            b({uint8_t(0x49 | (r >= 8 ? 4 : 0)), 0x8b, uint8_t(0x87 | (r & 7) << 3)}), d32(kIn + 8 * r);
        b({0x4d, 0x8b, 0xbf}), d32(kIn + 8 * 15);
        std::memcpy(p, body, n), p += n;
        b({0x41, 0x57, 0x49, 0xbf});
        uint64_t cp = reinterpret_cast<uint64_t>(c);
        std::memcpy(p, &cp, 8), p += 8;
        b({0x41, 0x8f, 0x87}), d32(kOut + 8 * 15);
        b({0x9c, 0x41, 0x8f, 0x87}), d32(kOutFlags);
        for (int r = 0; r < 15; ++r)
            b({uint8_t(0x49 | (r >= 8 ? 4 : 0)), 0x89, uint8_t(0x87 | (r & 7) << 3)}), d32(kOut + 8 * r);
        b({0x49, 0x8b, 0xa7}), d32(kHost);
        b({0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5e, 0x5f, 0x5d, 0x5b, 0xc3});
        FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
        reinterpret_cast<void (*)(Ctx*)>(code)(c);
    }
};

constexpr uint64_t CF = 1, PF = 4, AF = 0x10, ZF = 0x40, SF = 0x80, OF = 0x800;
uint64_t defined_flags(uint8_t kind) {
    switch (kind) {
    case kAndn:
    case kBlsi: return CF | ZF | SF | OF;
    case kBextr: return CF | ZF | OF;
    case kLzcnt:
    case kTzcnt: return CF | ZF;
    default: return CF | PF | AF | ZF | SF | OF;
    }
}

constexpr size_t kBuf = 0x10000;

bool compare(const Site& s, Runner& run, std::mt19937_64& rng, int trials, bool* skipped) {
    static uint8_t* data = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4 * kBuf, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    uint8_t *data2 = data + kBuf, *stack = data + 2 * kBuf, *stack2 = data + 3 * kBuf;
    const uint64_t rsp0 = reinterpret_cast<uint64_t>(stack) + kBuf / 2;
    const std::vector<uint8_t> orig = original(s);
    uint8_t emu[kMaxEmit];
    const size_t n = emit(s, emu);
    CHECK(n > 0 && n <= kMaxEmit);
    if (s.rm == kMem && (s.base == kNoReg || (s.base == 4 && (s.disp < -0x6000 || s.disp > 0x6000)))) {
        *skipped = true;
        return true;
    }
    for (int t = 0; t < trials; ++t) {
        Ctx a{}, b{};
        for (auto& r : a.in) r = rng();
        auto interesting = [&](uint64_t& r) {
            switch (rng() % 4) {
            case 0: r = 0; break;
            case 1: r = rng() & ((1ull << (rng() % 64)) - 1); break;
            case 2: r = 1ull << (rng() % 64); break;
            }
        };
        if (s.rm != kMem && s.rm != kNoReg) interesting(a.in[s.rm]);
        if (s.a != kNoReg) interesting(a.in[s.a]);
        if (s.kind == kBextr && rng() % 2) a.in[s.a] = (a.in[s.a] & ~0xffffull) | (rng() % 72) | (rng() % 72) << 8;
        a.in[4] = rsp0;
        uint64_t target = 0;
        if (s.rm == kMem) {
            uint64_t idx = s.index != kNoReg ? rng() % 16 : 0;
            if (s.index != kNoReg) a.in[s.index] = idx;
            if (s.base == 4) target = rsp0 + s.disp + idx * s.scale;
            else target = reinterpret_cast<uint64_t>(data) + kBuf / 2 + rng() % 512,
                 a.in[s.base] = target - int64_t(s.disp) - idx * s.scale;
        }
        a.in_flags = 0x202 | (rng() & (CF | PF | AF | ZF | SF | OF));
        for (size_t i = 0; i < kBuf; i += 8) {
            uint64_t v = rng();
            std::memcpy(data + i, &v, 8), std::memcpy(stack + i, &v, 8);
        }
        if (target && rng() % 4 == 0) std::memset(reinterpret_cast<uint8_t*>(target), 0, 8);
        std::memcpy(data2, data, kBuf), std::memcpy(stack2, stack, kBuf);
        b = a;
        run.run(&a, orig.data(), orig.size());
        std::swap_ranges(data, data + kBuf, data2), std::swap_ranges(stack, stack + kBuf, stack2);
        run.run(&b, emu, n);
        const size_t top = kBuf / 2;
        bool ok = std::memcmp(data, data2, kBuf) == 0 &&
                  std::memcmp(stack + top - 128, stack2 + top - 128, 112) == 0 &&
                  std::memcmp(stack + top, stack2 + top, kBuf - top) == 0;
        for (int r = 0; r < 16; ++r) ok &= a.out[r] == b.out[r];
        const uint64_t fm = defined_flags(s.kind);
        ok &= (a.out_flags & fm) == (b.out_flags & fm);
        if (!ok) {
            std::printf("FAIL site m%d rva 0x%x kind %d size %d trial %d:", s.module, s.rva, s.kind, s.size, t);
            for (int r = 0; r < 16; ++r)
                if (a.out[r] != b.out[r])
                    std::printf(" r%d %llx/%llx", r, (unsigned long long)a.out[r], (unsigned long long)b.out[r]);
            std::printf(" flags %llx/%llx\n", (unsigned long long)(a.out_flags & fm), (unsigned long long)(b.out_flags & fm));
            ++g_fail;
            return false;
        }
    }
    return true;
}

void test_known_encodings() {
    struct K { Site s; const char* hex; };
    const K ks[] = {
        {{0, kMovbeStore, 8, 5, 5, kNoReg, 2, kMem, 1, kNoReg, 1, 0, 0, 0}, "480f38f111"},
        {{0, kMovbeStore, 2, 6, 6, kNoReg, 0, kMem, 7, kNoReg, 1, 0x12, 0, 0}, "660f38f14712"},
        {{1, kMovbeStore, 2, 7, 7, kNoReg, 3, kMem, 14, kNoReg, 1, 0x24, 0, 0}, "66410f38f15e24"},
        {{0, kBextr, 4, 6, 6, 1, 0, kMem, 7, kNoReg, 1, 0x40, 0, 0}, "c4e278f74f40"},
        {{0, kBextr, 4, 5, 5, 12, 0, 1, kNoReg, kNoReg, 0, 0, 0, 0}, "c46278f7e1"},
        {{0, kAndn, 4, 5, 5, 0, 12, 0, kNoReg, kNoReg, 0, 0, 0, 0}, "c4e218f2c0"},
        {{1, kAndn, 4, 6, 6, 0, 6, kMem, 7, kNoReg, 1, 0x18, 0, 0}, "c4e248f24718"},
        {{1, kBlsi, 4, 5, 5, 2, kNoReg, 8, kNoReg, kNoReg, 0, 0, 0, 0}, "c4c268f3d8"},
        {{0, kLzcnt, 4, 5, 5, 10, kNoReg, 6, kNoReg, kNoReg, 0, 0, 0, 0}, "f3440fbdd6"},
        {{1, kTzcnt, 8, 5, 5, 1, kNoReg, 2, kNoReg, kNoReg, 0, 0, 0, 0}, "f3480fbcca"},
    };
    for (const K& k : ks) {
        std::string h;
        char x[3];
        for (uint8_t v : original(k.s)) std::snprintf(x, 3, "%02x", v), h += x;
        if (h != k.hex) std::printf("  encoded %s, expected %s\n", h.c_str(), k.hex);
        CHECK(h == k.hex);
    }
}

void test_install() {
    _putenv("DCP_CPU_COMPAT=all");
    size_t count;
    const Site* all = sites(&count);
    for (uint8_t mod : {kEboot, kLibc}) {
        uint32_t top = 0;
        for (size_t i = 0; i < count; ++i)
            if (all[i].module == mod && all[i].rva + 16 > top) top = all[i].rva + 16;
        auto* img = static_cast<uint8_t*>(VirtualAlloc(nullptr, top, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        std::memset(img, 0x90, top);
        size_t expect = 0;
        for (size_t i = 0; i < count; ++i)
            if (all[i].module == mod) {
                std::vector<uint8_t> o = original(all[i]);
                std::memcpy(img + all[i].rva, o.data(), o.size());
                expect += all[i].steal == all[i].len;
            }
        CHECK(install(img, Module(mod), Cpu{}) >= expect);
        size_t checked = 0;
        for (size_t i = 0; i < count; ++i) {
            const Site& s = all[i];
            if (s.module != mod || s.steal != s.len) continue;
            const uint8_t* at = img + s.rva;
            int32_t r;
            std::memcpy(&r, at + 1, 4);
            const uint8_t* t = at + 5 + r;
            uint8_t e[kMaxEmit];
            const size_t n = emit(s, e);
            bool ok = at[0] == 0xe9 && std::memcmp(t, e, n) == 0 && t[n] == 0xe9;
            if (ok) std::memcpy(&r, t + n + 1, 4), ok = t + n + 5 + r == at + s.steal;
            if (!ok) std::printf("  install: module %d rva 0x%x not patched as expected\n", mod, s.rva);
            CHECK(ok);
            checked += ok;
        }
        std::printf("install module %d: %zu sites patched and followed\n", mod, checked);
        VirtualFree(img, 0, MEM_RELEASE);
    }
    _putenv("DCP_CPU_COMPAT=");
}

}

int main() {
    const Cpu cpu = detect();
    std::printf("cpu: %s (avx %d movbe %d bmi1 %d lzcnt %d)\n", cpu.brand.c_str(), cpu.avx, cpu.movbe, cpu.bmi1, cpu.lzcnt);
    CHECK(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xcbf43926u);
    test_known_encodings();
    test_install();
    if (!cpu.movbe || !cpu.bmi1 || !cpu.lzcnt) {
        std::printf("this CPU lacks a feature: native comparison skipped\n");
        return g_fail ? 1 : 0;
    }
    size_t count;
    const Site* all = sites(&count);
    Runner run;
    std::mt19937_64 rng(33);
    size_t crc_ok = 0, crc_n = 0, skipped = 0, failed = 0;
    for (size_t i = 0; i < count; ++i) {
        const Site& s = all[i];
        std::vector<uint8_t> o = original(s);
        CHECK(o.size() == s.len);
        if (s.steal == s.len) ++crc_n, crc_ok += crc32(o.data(), o.size()) == s.crc;
        bool sk = false;
        failed += !compare(s, run, rng, 200, &sk);
        skipped += sk;
    }
    std::printf("%zu sites: %zu/%zu re-encoded byte-exact (CRC), %zu not placeable, %zu failed\n", count, crc_ok, crc_n,
                skipped, failed);
    CHECK(crc_ok == crc_n);
    CHECK(failed == 0);
    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
