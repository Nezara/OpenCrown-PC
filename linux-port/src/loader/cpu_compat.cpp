// Support for older CPUs without MOVBE / BMI1 / LZCNT. Details are restricted (see the README).
#include "loader/cpu_compat.h"

#include <cpuid.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include <sys/mman.h>

namespace dcp::loader::cpu_compat {
namespace {

constexpr Site kSites[] = {
#include "loader/cpu_compat_sites.inc"
};

struct Mem {
    uint8_t base, index, scale;
    int32_t disp;
};

struct Asm {
    uint8_t* p;
    void b(std::initializer_list<uint8_t> v) { for (uint8_t x : v) *p++ = x; }
    void d32(int32_t v) { std::memcpy(p, &v, 4), p += 4; }
    void prefix(int size, int reg, int index, int base) {
        if (size == 2) *p++ = 0x66;
        uint8_t rex = uint8_t(0x40 | (size == 8 ? 8 : 0) | (reg >> 3 & 1) << 2 | (index >> 3 & 1) << 1 | (base >> 3 & 1));
        if (rex != 0x40) *p++ = rex;
    }
    void rr(int size, std::initializer_list<uint8_t> op, int reg, int rm) {
        prefix(size, reg, 0, rm);
        b(op);
        *p++ = uint8_t(0xc0 | (reg & 7) << 3 | (rm & 7));
    }
    void rm(int size, std::initializer_list<uint8_t> op, int reg, const Mem& m) {
        const bool idx = m.index != kNoReg;
        prefix(size, reg, idx ? m.index : 0, m.base == kNoReg ? 0 : m.base);
        b(op);
        const uint8_t ss = m.scale == 8 ? 3 : m.scale == 4 ? 2 : m.scale == 2 ? 1 : 0;
        if (m.base == kNoReg) {
            *p++ = uint8_t(0x04 | (reg & 7) << 3);
            *p++ = uint8_t(ss << 6 | (idx ? m.index & 7 : 4) << 3 | 5);
        } else if (!idx && (m.base & 7) != 4) {
            *p++ = uint8_t(0x80 | (reg & 7) << 3 | (m.base & 7));
        } else {
            *p++ = uint8_t(0x84 | (reg & 7) << 3);
            *p++ = uint8_t(ss << 6 | (idx ? m.index & 7 : 4) << 3 | (m.base & 7));
        }
        d32(m.disp);
    }
    uint8_t* jcc8(uint8_t op) { b({op, 0}); return p - 1; }
    void land(uint8_t* at) { *at = uint8_t(p - (at + 1)); }
};

constexpr int kRax = 0, kRcx = 1, kRdx = 2;
int32_t slot(int r) { return r == kRax ? 16 : r == kRcx ? 8 : 0; }
Mem slot_mem(int r) { return {4, kNoReg, 1, slot(r)}; }

struct Emitter {
    const Site& s;
    Asm a;

    Mem game_mem() const {
        Mem m{s.base, s.index, s.scale, s.disp};
        if (m.base == 4) m.disp += 128 + 24;
        return m;
    }
    void get(int scratch, int r) {
        if (r <= kRdx) a.rm(s.size, {0x8b}, scratch, slot_mem(r));
        else a.rr(s.size, {0x8b}, scratch, r);
    }
    void source(int scratch) {
        if (s.rm == kMem) a.rm(s.size, {0x8b}, scratch, game_mem());
        else get(scratch, s.rm);
    }
    void put_dst() {
        if (s.dst <= kRdx) a.rm(s.size == 2 ? 2 : 8, {0x89}, kRax, slot_mem(s.dst));
        else a.rr(s.size, {0x89}, kRax, s.dst);
    }
    void swap() {
        if (s.size == 2) a.b({0x86, 0xe0});
        else if (s.size == 4) a.b({0x0f, 0xc8});
        else a.b({0x48, 0x0f, 0xc8});
    }
    void count(bool lz) {
        const int bits = s.size * 8;
        a.rr(s.size, {0x0f, uint8_t(lz ? 0xbd : 0xbc)}, kRax, kRax);
        uint8_t* zero = a.jcc8(0x74);
        if (lz) a.rr(s.size, {0x83}, 6, kRax), a.b({uint8_t(bits - 1)});
        else a.rr(s.size, {0x85}, kRax, kRax);
        uint8_t* done = a.jcc8(0xeb);
        a.land(zero);
        a.b({0xb8}), a.d32(bits);
        a.b({0x83, 0xf8, uint8_t(bits + 1)});
        a.land(done);
    }

    size_t run() {
        uint8_t* start = a.p;
        a.b({0x48, 0x8d, 0x64, 0x24, 0x80, 0x50, 0x51, 0x52});
        switch (s.kind) {
        case kMovbeLoad:
            source(kRax), swap(), put_dst();
            break;
        case kMovbeStore:
            a.rm(8, {0x8d}, kRdx, game_mem());
            get(kRax, s.a), swap();
            a.rm(s.size, {0x89}, kRax, Mem{kRdx, kNoReg, 1, 0});
            break;
        case kAndn:
            source(kRcx), get(kRax, s.a);
            a.rr(s.size, {0xf7}, 2, kRax);
            a.rr(s.size, {0x23}, kRax, kRcx);
            put_dst();
            break;
        case kBextr: {
            const uint8_t bits = uint8_t(s.size * 8);
            source(kRax), get(kRdx, s.a);
            a.b({0x0f, 0xb6, 0xca, 0x83, 0xf9, bits});
            uint8_t* zero = a.jcc8(0x73);
            a.rr(s.size, {0xd3}, 5, kRax);
            a.b({0x0f, 0xb6, 0xce, 0x83, 0xf9, bits});
            uint8_t* all = a.jcc8(0x73);
            a.b({0xba, 1, 0, 0, 0});
            a.rr(8, {0xd3}, 4, kRdx);
            a.rr(8, {0xff}, 1, kRdx);
            a.rr(s.size, {0x23}, kRax, kRdx);
            a.land(all);
            a.rr(s.size, {0x85}, kRax, kRax);
            uint8_t* done = a.jcc8(0xeb);
            a.land(zero);
            a.b({0x31, 0xc0});
            a.land(done);
            put_dst();
            break;
        }
        case kBlsi:
            source(kRax);
            a.rr(8, {0x8b}, kRcx, kRax);
            a.rr(s.size, {0xf7}, 3, kRcx);
            a.rr(s.size, {0x23}, kRax, kRcx);
            a.rr(s.size, {0x85}, kRax, kRax);
            a.b({0x74, 0x01, 0xf9});
            put_dst();
            break;
        case kLzcnt:
        case kTzcnt:
            source(kRax), count(s.kind == kLzcnt), put_dst();
            break;
        default:
            return 0;
        }
        a.b({0x5a, 0x59, 0x58, 0x48, 0x8d, 0xa4, 0x24, 0x80, 0, 0, 0});
        return size_t(a.p - start);
    }
};

bool needed(const Site& s, const Cpu& cpu) {
    switch (s.kind) {
    case kMovbeLoad:
    case kMovbeStore: return !cpu.movbe;
    case kLzcnt: return !cpu.lzcnt;
    default: return !cpu.bmi1;
    }
}

uint8_t* alloc_near(uint64_t at, size_t size) {
    for (uint64_t d = 0; d < (1ull << 30); d += 0x10000)
        for (uint64_t a : {at + d, at - d}) {
            if (a <= 0x10000) continue;
            a &= ~0xffffull;
            void* p = mmap(reinterpret_cast<void*>(a), size, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p == MAP_FAILED) continue;
            if (p == reinterpret_cast<void*>(a)) return static_cast<uint8_t*>(p);
            munmap(p, size);
        }
    return nullptr;
}

void write_code(uint8_t* at, const uint8_t* bytes, size_t n) {
    uint64_t lo = reinterpret_cast<uint64_t>(at) & ~0xfffull, hi = (reinterpret_cast<uint64_t>(at) + n + 0xfff) & ~0xfffull;
    mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC);
    std::memcpy(at, bytes, n);
    mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_EXEC);
}

void put_jmp(uint8_t* out, const uint8_t* at, const uint8_t* to) {
    out[0] = 0xe9;
    int32_t r = int32_t(to - (at + 5));
    std::memcpy(out + 1, &r, 4);
}

}

Cpu detect() {
    Cpu c;
    unsigned a, b, cx, d;
    if (__get_cpuid(1, &a, &b, &cx, &d)) {
        c.movbe = cx >> 22 & 1;
        if ((cx >> 28 & 1) && (cx >> 27 & 1)) {
            uint32_t lo, hi;
            __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
            c.avx = (lo & 6) == 6;
        }
    }
    if (__get_cpuid_count(7, 0, &a, &b, &cx, &d)) c.bmi1 = b >> 3 & 1;
    if (__get_cpuid(0x80000001, &a, &b, &cx, &d)) c.lzcnt = cx >> 5 & 1;
    char brand[49] = {};
    for (unsigned i = 0; i < 3; ++i)
        if (__get_cpuid(0x80000002 + i, &a, &b, &cx, &d))
            std::memcpy(brand + 16 * i, &a, 4), std::memcpy(brand + 16 * i + 4, &b, 4),
                std::memcpy(brand + 16 * i + 8, &cx, 4), std::memcpy(brand + 16 * i + 12, &d, 4);
    c.brand = brand;
    while (!c.brand.empty() && c.brand.front() == ' ') c.brand.erase(0, 1);
    while (!c.brand.empty() && c.brand.back() == ' ') c.brand.pop_back();
    return c;
}

const Site* sites(size_t* count) {
    *count = sizeof kSites / sizeof kSites[0];
    return kSites;
}

size_t emit(const Site& s, uint8_t* out) { return Emitter{s, {out}}.run(); }

uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = c >> 1 ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

size_t install(uint8_t* base, Module m, const Cpu& cpu) {
    const char* env = std::getenv("DCP_CPU_COMPAT");
    if (env && std::strcmp(env, "0") == 0) return 0;
    const bool all = env && std::strcmp(env, "all") == 0;
    Cpu as = cpu;
    if (env && *env && !all) {
        if (std::strstr(env, "movbe")) as.movbe = false;
        if (std::strstr(env, "bmi1")) as.bmi1 = false;
        if (std::strstr(env, "lzcnt")) as.lzcnt = false;
    }
    const char* name = m == kEboot ? "eboot" : "libc";
    constexpr size_t kN = sizeof kSites / sizeof kSites[0];
    size_t mine = 0;
    for (const Site& s : kSites) mine += s.module == m;
    uint8_t* pool = nullptr;
    size_t patched = 0, differ = 0, by_kind[8] = {};
    for (size_t i = 0; i < kN; ++i) {
        const Site& h = kSites[i];
        if (h.module != m || !h.steal) continue;
        size_t end = i + 1;
        while (end < kN && kSites[end].module == m && !kSites[end].steal) ++end;
        bool want = false;
        for (size_t j = i; j < end; ++j) want |= all || needed(kSites[j], as);
        if (!want) continue;
        uint8_t* at = base + h.rva;
        if (crc32(at, h.steal) != h.crc) { ++differ; continue; }
        if (!pool && !(pool = alloc_near(reinterpret_cast<uint64_t>(at), mine * (kMaxEmit + 32) + 0x1000))) {
            std::fprintf(stderr, "cpu_compat: %s: no trampoline memory near the image; not patched\n", name);
            return 0;
        }
        uint8_t* t = pool;
        for (size_t j = i; j < end; ++j) {
            const Site& s = kSites[j];
            t += emit(s, t);
            const uint32_t to = j + 1 < end ? kSites[j + 1].rva : h.rva + h.steal;
            std::memcpy(t, base + s.rva + s.len, to - (s.rva + s.len));
            t += to - (s.rva + s.len);
            ++by_kind[s.kind], ++patched;
        }
        put_jmp(t, t, at + h.steal);
        t += 5;
        int64_t r1 = pool - (at + 5), r2 = (at + h.steal) - (t);
        if (r1 != int32_t(r1) || r2 != int32_t(r2)) {
            std::fprintf(stderr, "cpu_compat: %s: trampoline out of rel32 range; stopped\n", name);
            break;
        }
        uint8_t patch[16];
        std::memset(patch, 0xcc, sizeof patch);
        put_jmp(patch, at, pool);
        write_code(at, patch, h.steal);
        pool = t;
    }
    if (patched || differ)
        std::fprintf(stderr, "cpu_compat: %s: %zu instructions replaced (movbe %zu, andn %zu, bextr %zu, blsi %zu, lzcnt %zu, "
                     "tzcnt %zu)%s, %zu sites skipped (bytes differ)\n", name, patched, by_kind[kMovbeLoad] +
                     by_kind[kMovbeStore], by_kind[kAndn], by_kind[kBextr], by_kind[kBlsi], by_kind[kLzcnt],
                     by_kind[kTzcnt], env && *env ? " [DCP_CPU_COMPAT]" : "", differ);
    return patched;
}

}
