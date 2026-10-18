// libScePad: up to 4 local pads, slot i = user kUserId + i = handle i + 1 (handle 1 for the first, as shadPS4), fed by
// an InputSource. The user-service shim logs users in/out from pad_present().
// Default source = Config::input_script (headless, deterministic: state keyed by vblank_count()); no script = a
// connected pad with nothing pressed. Layouts: ScePadData 0x78 B, ScePadControllerInformation 0x1c B (SDK).
#include <cstring>
#include <mutex>
#include <vector>


#include "common.h"
#include "input.h"
#include "kernel/pure.h"

namespace dcp::hle {
namespace {

constexpr int32_t kPadErrInvalidArg = int32_t(0x80920001);
constexpr int32_t kPadErrInvalidHandle = int32_t(0x80920003);
constexpr int kSlots = 4;

class IdleSource final : public InputSource {
public:
    PadState read(int, uint64_t) override { return {}; }
};

class ScriptSource final : public InputSource {
public:
    explicit ScriptSource(std::vector<InputEvent> ev) : ev_(std::move(ev)) {}
    PadState read(int pad, uint64_t frame) override {
        PadState s;
        if (pad == 0) s.buttons = script_buttons_at(ev_, frame);
        if (s.buttons != last_) {
            std::fprintf(stderr, "INPUT frame %llu: buttons 0x%x\n", (unsigned long long)frame, s.buttons);
            last_ = s.buttons;
        }
        if (s.buttons & kScriptStickLeft) s.lx = 0x00;
        if (s.buttons & kScriptStickRight) s.lx = 0xff;
        if (s.buttons & kScriptStickUp) s.ly = 0x00;
        if (s.buttons & kScriptStickDown) s.ly = 0xff;
        s.buttons &= ~kScriptStickMask;
        return s;
    }

private:
    std::vector<InputEvent> ev_;
    uint32_t last_ = 0;
};

std::mutex g_mu;
std::unique_ptr<InputSource> g_src;
bool g_open[kSlots] = {};

int slot_of(int32_t h) { return h >= 1 && h <= kSlots && g_open[h - 1] ? h - 1 : -1; }

InputSource& source() {
    if (!g_src) g_src = std::make_unique<IdleSource>();
    return *g_src;
}

DCP_SYSV int32_t scePadInit() { DCP_HLE_LOG("scePadInit", "ok"); return kOk; }

DCP_SYSV int32_t scePadOpen(int32_t user, int32_t type, int32_t index, const void*) {
    std::lock_guard<std::mutex> lk(g_mu);
    int slot = user - kUserId;
    if (slot < 0 || slot >= kSlots || type != 0 || index != 0) {
        std::fprintf(stderr, "HLE scePadOpen: user %d type %d index %d -> INVALID_ARG\n", user, type, index);
        return kPadErrInvalidArg;
    }
    g_open[slot] = true;
    std::fprintf(stderr, "HLE scePadOpen: user %d -> handle %d\n", user, slot + 1);
    return slot + 1;
}

DCP_SYSV int32_t scePadClose(int32_t h) {
    std::lock_guard<std::mutex> lk(g_mu);
    int slot = slot_of(h);
    if (slot < 0) return kPadErrInvalidHandle;
    g_open[slot] = false;
    return kOk;
}

DCP_SYSV int32_t scePadGetHandle(int32_t user, int32_t, int32_t) {
    int slot = user - kUserId;
    return slot >= 0 && slot < kSlots && g_open[slot] ? slot + 1 : kPadErrInvalidArg;
}

DCP_SYSV int32_t scePadReadState(int32_t h, uint8_t* d) {
    int slot = slot_of(h);
    if (slot < 0 || !d) return kPadErrInvalidHandle;
    PadState s;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        s = source().read(slot, vblank_count());
    }
    std::memset(d, 0, 0x78);
    std::memcpy(d, &s.buttons, 4);
    d[4] = s.lx, d[5] = s.ly, d[6] = s.rx, d[7] = s.ry, d[8] = s.l2, d[9] = s.r2;
    float w = 1.0f;
    std::memcpy(d + 0x18, &w, 4);  // orientation quaternion (x, y, z, w) at +0xc: identity
    d[0x34] = s.touch_num;  // touchData: touchNum +0x34, touch[i] {u16 x, u16 y, u8 id} at +0x3c + 8i
    for (int i = 0; i < s.touch_num && i < 2; ++i) {
        std::memcpy(d + 0x3c + 8 * i, &s.tx[i], 2);
        std::memcpy(d + 0x3e + 8 * i, &s.ty[i], 2);
        d[0x40 + 8 * i] = s.tid[i];
    }
    d[0x4c] = s.connected;
    uint64_t us = host_ns() / 1000;
    std::memcpy(d + 0x50, &us, 8);
    d[0x68] = 1;  // connectedCount
    return kOk;
}

DCP_SYSV int32_t scePadGetControllerInformation(int32_t h, uint8_t* info) {
    if (slot_of(h) < 0 || !info) return kPadErrInvalidHandle;
    std::memset(info, 0, 0x1c);
    float density = 44.86f;  // DualShock 4 touchpad (pixels per mm)
    uint16_t rx = 1920, ry = 943;
    std::memcpy(info, &density, 4);
    std::memcpy(info + 4, &rx, 2);
    std::memcpy(info + 6, &ry, 2);
    info[8] = 0x0d, info[9] = 0x0d;  // stick dead zones (left, right); typical DS4 value
    info[0xa] = 0;                   // connection type: local
    info[0xb] = 1;                   // connected count
    info[0xc] = 1;                   // connected
    return kOk;
}

// +0 large motor, +1 small motor.
DCP_SYSV int32_t scePadSetVibration(int32_t h, const uint8_t* v) {
    int slot = slot_of(h);
    if (slot < 0 || !v) return kPadErrInvalidHandle;
    std::lock_guard<std::mutex> lk(g_mu);
    source().set_vibration(slot, v[0], v[1]);
    return kOk;
}
DCP_SYSV int32_t scePadSetLightBar(int32_t h, const uint8_t* c) {
    int slot = slot_of(h);
    if (slot < 0 || !c) return kPadErrInvalidHandle;
    DCP_HLE_LOG("scePadSetLightBar", "rgb %02x%02x%02x", c[0], c[1], c[2]);
    std::lock_guard<std::mutex> lk(g_mu);
    source().set_light_bar(slot, c[0], c[1], c[2]);
    return kOk;
}
DCP_SYSV int32_t scePadResetLightBar(int32_t h) { return slot_of(h) >= 0 ? kOk : kPadErrInvalidHandle; }
DCP_SYSV int32_t scePadResetOrientation(int32_t h) { return slot_of(h) >= 0 ? kOk : kPadErrInvalidHandle; }

}  // namespace

void set_input_source(std::unique_ptr<InputSource> src) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_src = std::move(src);
}

std::unique_ptr<InputSource> take_input_source() {
    std::lock_guard<std::mutex> lk(g_mu);
    return std::move(g_src);
}

bool pad_present(int slot) {
    if (slot == 0) return true;
    std::lock_guard<std::mutex> lk(g_mu);
    return slot > 0 && slot < kSlots && source().present(slot);
}

void register_pad() {
    if (!config().input_script.empty()) {
        std::vector<InputEvent> ev;
        int bad = 0;
        if (parse_input_script(config().input_script, &ev, &bad)) {
            std::fprintf(stderr, "HLE pad: input script, %zu events\n", ev.size());
            set_input_source(std::make_unique<ScriptSource>(std::move(ev)));
        } else {
            std::fprintf(stderr, "HLE pad: input script line %d malformed; using idle pad\n", bad);
        }
    }
    const char* l = "libScePad";
    REG(l, scePadInit); REG(l, scePadOpen); REG(l, scePadClose); REG(l, scePadGetHandle); REG(l, scePadReadState);
    REG(l, scePadGetControllerInformation); REG(l, scePadSetVibration); REG(l, scePadSetLightBar);
    REG(l, scePadResetLightBar); REG(l, scePadResetOrientation);
}

}  // namespace dcp::hle
