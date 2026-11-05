#version 450
#extension GL_GOOGLE_include_directive : require
layout(location = 0) out vec4 o;
layout(set = 2, binding = 0) uniform sampler2D uTex;
layout(set = 3, binding = 0) uniform Con { uvec4 con0, con1, con2, con3; };
#define A_GPU 1
#define A_GLSL 1
#include "fsr/ffx_a.h"
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(uTex, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(uTex, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(uTex, p, 2); }
#include "fsr/ffx_fsr1.h"
void main() {
  AF3 c;
  FsrEasuF(c, AU2(gl_FragCoord.xy), con0, con1, con2, con3);
  o = vec4(c, 1.0);
}
