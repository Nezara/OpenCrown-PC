#version 450
#extension GL_GOOGLE_include_directive : require
layout(location = 0) out vec4 o;
layout(set = 2, binding = 0) uniform sampler2D uTex;
layout(set = 3, binding = 0) uniform Con { uvec4 con; ivec4 origin; };
#define A_GPU 1
#define A_GLSL 1
#include "fsr/ffx_a.h"
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(uTex, clamp(p, ivec2(0), textureSize(uTex, 0) - 1), 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "fsr/ffx_fsr1.h"
void main() {
  AF1 r, g, b;
  FsrRcasF(r, g, b, AU2(ivec2(gl_FragCoord.xy) - origin.xy), con);
  o = vec4(r, g, b, 1.0);
}
