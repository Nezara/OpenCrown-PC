// Host UI overlay drawn over the game picture after it is scaled to the window (touch controls). Each vertex is in
// window pixels; (u, v) is the position inside a round shape's bounding square (-1..1) and `inner` its hole radius:
// coverage = inside the unit circle and outside `inner` (anti-aliased). Solid shapes use u = v = 0, inner = -1.
#pragma once
#include <vector>

namespace gpu {

struct OverlayVertex {
  float x, y, u, v;
  float r, g, b, a;
  float inner;
};

// Replaces the overlay (triangle list); thread-safe, the backend picks it up at the next present.
void set_overlay(std::vector<OverlayVertex> tris);
std::vector<OverlayVertex> overlay();  // a copy of the current one

}  // namespace gpu
