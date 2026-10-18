// Input backends for the libScePad shim. The pad shim asks the active source for the state of a local pad once per
// scePadReadState; a source is headless (scripted, deterministic: keyed by the vblank frame clock) or, later, an SDL3
// gamepad/keyboard backend. Sources are called from game threads, so they must be thread-safe.
#pragma once
#include <cstdint>
#include <memory>

namespace dcp::hle {

struct PadState {
    bool connected = true;
    uint32_t buttons = 0;                                   // ScePadData.buttons bits (SDK layout)
    uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;     // sticks, centre 0x80
    uint8_t l2 = 0, r2 = 0;                                 // analog triggers
    uint8_t touch_num = 0;                                  // 0..2 points, touchpad coords (1920 x 943)
    uint16_t tx[2] = {}, ty[2] = {};
    uint8_t tid[2] = {};
};

class InputSource {
public:
    virtual ~InputSource() = default;
    virtual PadState read(int pad, uint64_t frame) = 0;    // pad = local slot 0..3
    virtual bool present(int pad) { return pad == 0; }     // slot has a device (drives user login/logout; 0 always)
    virtual void set_vibration(int, uint8_t /*large*/, uint8_t /*small*/) {}
    virtual void set_light_bar(int, uint8_t, uint8_t, uint8_t) {}
};

// Replace the active source (default: scripted from Config::input_script, or idle).
void set_input_source(std::unique_ptr<InputSource> src);
// Removes and returns the active source (null if it is the default idle one), e.g. to wrap the scripted one.
std::unique_ptr<InputSource> take_input_source();

// Local player slot i is user kUserId + i (1000..1003). True when slot i should be logged in.
bool pad_present(int slot);

}  // namespace dcp::hle
