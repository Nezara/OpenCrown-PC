// GL-free .mbs motion player: advances a motion's tracks and emits textured part fans in world space.
// Semantics follow notes/map/mbs.md; everything marked GUESS is not confirmed by the eboot.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "formats/mbs.h"

namespace dcp::viewer {

struct Affine {  // x' = a*x + c*y + tx ; y' = b*x + d*y + ty
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    std::array<float, 2> apply(float x, float y) const { return {a * x + c * y + tx, b * x + d * y + ty}; }
    Affine operator*(const Affine& o) const {  // this after o
        return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d, b * o.c + d * o.d, a * o.tx + c * o.ty + tx,
                b * o.tx + d * o.ty + ty};
    }
};

struct DrawVert { float x, y, u, v; uint8_t rgba[4]; };
struct DrawPart {
    int texture = -1;  // FTX0 child index, -1 = untextured
    uint8_t blend = 0;
    std::array<DrawVert, 6> v;  // fan: 0 centre, 1..4 corners, 5 = 1
};

constexpr double kTicksPerSecond = 60.0;  // GUESS: frame durations are in 1/60 s ticks

class MbsPlayer {
public:
    explicit MbsPlayer(const formats::Mbs& m) : m_(&m) {}
    void set_motion(int motion);
    int motion() const { return motion_; }
    void advance(double ticks);
    // Appends the current pose's parts; tint is multiplied into every vertex (RGBA 0..255).
    void emit(const Affine& world, const std::array<uint8_t, 4>& tint, std::vector<DrawPart>& out) const;

private:
    struct Layer { int track = 0, idx = 0; double t = 0; bool held = false; };
    const formats::Mbs* m_;
    int motion_ = -1;
    std::vector<Layer> layers_;
};

}  // namespace dcp::viewer
