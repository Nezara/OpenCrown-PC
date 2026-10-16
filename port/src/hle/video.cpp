// libSceVideoOut: no display yet, but the game loop is paced. sceVideoOutWaitVblank sleeps to the next 1/60 s
// boundary (QPC) and is the frame clock (vblank_count) used by the input script and --frames. Flips are counted in
// gnm.cpp (they arrive through sceGnmSubmitAndFlipCommandBuffers).
#include <atomic>
#include <cstring>

#include <windows.h>

#include "common.h"
#include "render/capture.h"

namespace dcp::hle {
namespace {

std::atomic<uint64_t> g_vblanks{0};
LARGE_INTEGER g_freq;
int64_t g_next = 0;      // QPC of the next vblank (main thread only)

constexpr int32_t kVideoHandle = 1;

DCP_SYSV int32_t sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void*) {
    std::fprintf(stderr, "HLE sceVideoOutOpen: user %d bus %d index %d -> handle %d\n", user, bus, index, kVideoHandle);
    return kVideoHandle;
}
DCP_SYSV int32_t sceVideoOutClose(int32_t) { return kOk; }

// SceVideoOutResolutionStatus: fullWidth, fullHeight, paneWidth, paneHeight (u32), refreshRate u64, screenSizeInInch f32.
DCP_SYSV int32_t sceVideoOutGetResolutionStatus(int32_t, uint8_t* st) {
    if (!st) return int32_t(0x80290002);
    const uint32_t w = config().neo ? 3840 : 1920, h = config().neo ? 2160 : 1080;  // Pro mode: 4K output
    uint32_t dims[4] = {w, h, w, h};
    uint64_t rate = 3;  // SCE_VIDEO_OUT_REFRESH_RATE_59_94HZ
    float inch = 50.0f;
    std::memset(st, 0, 0x28);
    std::memcpy(st, dims, 16);
    std::memcpy(st + 0x10, &rate, 8);
    std::memcpy(st + 0x18, &inch, 4);
    return kOk;
}

// attr: +0 pixelFormat, +4 tilingMode, +8 aspectRatio, +0xc width, +0x10 height, +0x14 pitchInPixel.
DCP_SYSV int32_t sceVideoOutSetBufferAttribute(uint32_t* attr, uint32_t fmt, uint32_t tiling, uint32_t aspect, uint32_t w,
                                               uint32_t h, uint32_t pitch) {
    if (!attr) return int32_t(0x80290002);
    std::memset(attr, 0, 0x28);
    attr[0] = fmt, attr[1] = tiling, attr[2] = aspect, attr[3] = w, attr[4] = h, attr[5] = pitch;
    return kOk;
}
DCP_SYSV int32_t sceVideoOutRegisterBuffers(int32_t, int32_t start, void* const* addrs, int32_t n, const uint32_t* attr) {
    std::fprintf(stderr, "HLE sceVideoOutRegisterBuffers: start %d, %d buffers, %ux%u fmt 0x%x, [0] %p\n", start, n,
                 attr ? attr[3] : 0, attr ? attr[4] : 0, attr ? attr[0] : 0, addrs ? addrs[0] : nullptr);
    if (addrs && n > 0) render::set_display_buffers(addrs, start, n);
    return kOk;
}
DCP_SYSV int32_t sceVideoOutAddFlipEvent(void*, int32_t, void*) { return kOk; }
DCP_SYSV int32_t sceVideoOutAddVblankEvent(void*, int32_t, void*) { return kOk; }
DCP_SYSV int32_t sceVideoOutSetFlipRate(int32_t, int32_t) { return kOk; }

DCP_SYSV int32_t sceVideoOutWaitVblank(int32_t) {
    const int64_t period = g_freq.QuadPart / 60;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_next == 0 || now.QuadPart - g_next > 4 * period) g_next = now.QuadPart + period;  // first call / long stall
    int64_t ms_left = (g_next - now.QuadPart) * 1000 / g_freq.QuadPart;
    if (ms_left > 1) Sleep(DWORD(ms_left - 1));
    for (;;) {
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= g_next) break;
        SwitchToThread();
    }
    g_next += period;
    uint64_t v = ++g_vblanks;
    if (config().max_frames && v >= config().max_frames) {
        std::fprintf(stderr, "reached --frames %llu (flips %llu)\n", (unsigned long long)v,
                     (unsigned long long)flip_count());
        config().on_exit(0);
    }
    return kOk;
}

}  // namespace

uint64_t vblank_count() { return g_vblanks.load(); }
uint64_t vblank_count_public() { return g_vblanks.load(); }

void register_video() {
    QueryPerformanceFrequency(&g_freq);
    const char* v = "libSceVideoOut";
    REG(v, sceVideoOutOpen); REG(v, sceVideoOutClose); REG(v, sceVideoOutGetResolutionStatus);
    REG(v, sceVideoOutSetBufferAttribute); REG(v, sceVideoOutRegisterBuffers); REG(v, sceVideoOutAddFlipEvent);
    REG(v, sceVideoOutAddVblankEvent); REG(v, sceVideoOutSetFlipRate); REG(v, sceVideoOutWaitVblank);
    register_gnm();
}

}  // namespace dcp::hle
