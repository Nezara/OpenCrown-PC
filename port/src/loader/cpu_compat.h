// Support for older CPUs without MOVBE / BMI1 / LZCNT. Details are restricted (see the README).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace dcp::loader::cpu_compat {

struct Cpu {
    std::string brand;
    bool avx = false;
    bool movbe = false, bmi1 = false, lzcnt = false;
};
Cpu detect();

enum Module : uint8_t { kEboot = 0, kLibc = 1 };
enum Kind : uint8_t { kMovbeLoad = 1, kMovbeStore, kAndn, kBextr, kBlsi, kLzcnt, kTzcnt };
constexpr uint8_t kNoReg = 0xff, kMem = 0xfe;

struct Site {
    uint8_t module, kind, size, len, steal, dst, a, rm, base, index, scale;
    int32_t disp;
    uint32_t rva, crc;
};
const Site* sites(size_t* count);

constexpr size_t kMaxEmit = 128;
size_t emit(const Site& s, uint8_t* out);

uint32_t crc32(const uint8_t* p, size_t n);

size_t install(uint8_t* base, Module m, const Cpu& cpu);

}
