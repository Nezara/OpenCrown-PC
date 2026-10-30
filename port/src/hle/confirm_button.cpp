// See confirm_button.h; asm evidence in notes/map/confirm_cancel.md.
//   0x176c0  mov dword [r12+0x134], 1   (DcCore_Startup)        imm32 -> 0
//   0x177da  mov dword [rbx+0x134], 1   (DcCore_ReadLanguage)   imm32 -> 0
//   0x2f394b mov esi, 1                 (PlayerCtrl_ReadActions: act 1 -> command bit 0x40)   -> mov esi, 0x1d
#include "confirm_button.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <windows.h>

namespace dcp::confirm_button {
namespace {

struct Patch {
    uint32_t site;
    uint8_t len;
    uint8_t expect[12];
    uint8_t repl[12];
};

constexpr Patch kPatches[] = {
    {0x176c0, 12, {0x41, 0xc7, 0x84, 0x24, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00},
                  {0x41, 0xc7, 0x84, 0x24, 0x34, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0x177da, 10, {0xc7, 0x83, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00},
                  {0xc7, 0x83, 0x34, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0x2f394b, 5, {0xbe, 0x01, 0x00, 0x00, 0x00}, {0xbe, 0x1d, 0x00, 0x00, 0x00}},
};

void write_code(uint8_t* at, const uint8_t* bytes, size_t n) {
    DWORD old;
    VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}

}  // namespace

void install(uint8_t* base) {
    const char* v = std::getenv("DCP_CONFIRM");
    if (!v || std::strcmp(v, "circle") != 0) return;
    for (const Patch& p : kPatches)  // all or nothing: half the layout would be worse than none
        if (std::memcmp(base + p.site, p.expect, p.len) != 0) {
            std::fprintf(stderr, "confirm_button: unexpected bytes at eboot+0x%x; Japanese layout not installed\n", p.site);
            return;
        }
    for (const Patch& p : kPatches) write_code(base + p.site, p.repl, p.len);
    std::fprintf(stderr, "confirm_button: Japanese layout (Circle confirms, Cross cancels)\n");
}

}  // namespace dcp::confirm_button
