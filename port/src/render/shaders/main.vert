#version 450
// SDL_gpu port of kVs (gl_device.cpp). SDL_gpu clip space puts NDC y = +1 at row 0 on every backend, so unlike the GL
// VS there is no y negation: targets still come out top row first. z is remapped from GL's [-w, w] clip range to
// SDL_gpu's [0, w] so exactly the primitives GL keeps are kept. Vertex uniforms: set 1 (SDL_gpu SPIR-V convention).
layout(location = 0) in vec4 aPos;
layout(location = 1) in vec4 aCol;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec2 aMuv;
layout(location = 0) out vec2 vUv;
layout(location = 1) out vec2 vMuv;
layout(location = 2) out vec4 vCol;
layout(set = 1, binding = 0, std140) uniform Vs {
  mat4 uMat;  // column-major (the device transposes the game's row-major cb0)
  int uRaw;
};
void main() {
  vec4 p = uRaw == 1 ? aPos : uMat * aPos;
  p.z = (p.z + p.w) * 0.5;
  gl_Position = p;
  vUv = aUv;
  vMuv = aMuv;
  vCol = aCol;
}
