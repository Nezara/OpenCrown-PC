// libSceVideoOut: no display yet, but the game loop is paced. sceVideoOutWaitVblank sleeps to the next 1/60 s
// boundary (CLOCK_MONOTONIC) and is the frame clock (vblank_count) used by the input script and --frames. Flips are counted in
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

#include <sched.h>

#include "common.h"
#include "render/capture.h"

namespace dcp::hle {
namespace {

std::atomic<uint64_t> g_vblanks{0};
int64_t g_next = 0;      // host_ns of the next vblank (main thread only)
std::atomic<int64_t> g_period{0};
std::atomic<bool> g_lock{false};
std::atomic<uint64_t> g_presents{0};
std::atomic<int64_t> g_last_present{0};
std::mutex g_present_mu;
std::condition_variable g_present_cv;

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

bool wait_present(int64_t period) {
    int64_t now = int64_t(host_ns());
    if (now - g_last_present.load() > 4 * period) return false;
    const uint64_t seen = g_presents.load();
    const int64_t until = now + period * 3 / 2;
    {
        std::unique_lock<std::mutex> lk(g_present_mu);
        while (g_presents.load() == seen) {
            const int64_t left = until - int64_t(host_ns());
            if (left <= 0) return false;
            g_present_cv.wait_for(lk, std::chrono::nanoseconds(left));
        }
    }
    g_next = int64_t(host_ns()) + period;  // the timer continues from here if presents stop
    return true;
}

DCP_SYSV int32_t sceVideoOutWaitVblank(int32_t) {
    const int64_t period = g_period.load() ? g_period.load() : 1000000000ll / 60;
    if (!g_lock.load() || !wait_present(period)) {
        int64_t now = int64_t(host_ns());
        if (g_next == 0 || now - g_next > 4 * period) g_next = now + period;  // first call / long stall
        if (g_next - now > 1000000) host_sleep_ns(uint64_t(g_next - now - 1000000));  // sleep to 1 ms before, then spin
        while (int64_t(host_ns()) < g_next) sched_yield();
        g_next += period;
    }
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

void set_display_sync(double hz, bool lock) {
    g_period = hz > 0 ? int64_t(1e9 / hz) : 0;
    g_lock = lock;
}

void display_vblank() {
    {
        std::lock_guard<std::mutex> lk(g_present_mu);
        g_last_present = int64_t(host_ns());
        ++g_presents;
    }
    g_present_cv.notify_all();
}

void register_video() {
    const char* v = "libSceVideoOut";
    REG(v, sceVideoOutOpen); REG(v, sceVideoOutClose); REG(v, sceVideoOutGetResolutionStatus);
    REG(v, sceVideoOutSetBufferAttribute); REG(v, sceVideoOutRegisterBuffers); REG(v, sceVideoOutAddFlipEvent);
    REG(v, sceVideoOutAddVblankEvent); REG(v, sceVideoOutSetFlipRate); REG(v, sceVideoOutWaitVblank);
    register_gnm();
}

}  // namespace dcp::hle
