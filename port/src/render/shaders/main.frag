#version 450
// SDL_gpu port of kFs (gl_device.cpp); same flag semantics, keep the two in step. Fragment samplers: set 2 (binding 0
// uTex, 1 uMask), fragment uniforms: set 3 (SDL_gpu SPIR-V convention; HLSL t/s0-1 space2, b0 space3).
// uFlags: 1 texture slot 0 (rgba multiply), 16 texture rgb only (alpha = vertex alpha: PS50/53/55), 32 mask texture
// (second texture at vMuv, rgba multiply), 4 constant colour only (PS51), 64 texture rgb with alpha 1 (PS58),
// sprite ops in the game's order: 256 factor (desaturate), 128 colour matrix, 512 gradient (2 tone-curve stages), 2 tint.
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec2 vMuv;
layout(location = 2) in vec4 vCol;
layout(location = 0) out vec4 o;
layout(set = 2, binding = 0) uniform sampler2D uTex;
layout(set = 2, binding = 1) uniform sampler2D uMask;
layout(set = 3, binding = 0, std140) uniform Fs {
  vec4 uTint;
  vec4 uConst;
  vec4 uCm[3];
  vec4 uGrad[16];
  float uFactor;
  int uFlags;
};
float g(int i) { return uGrad[i >> 2][i & 3]; }
// GCN computes clamp(x * rcp(s) - lo * rcp(s)); a NaN clamps to 0. With s = 0 (unused stages carry 0 scales) that is
// 0 unless x and lo have opposite signs, so an unused stage stays an exact no-op instead of producing NaN.
float ramp(float x, float lo, float s) {
  if (s != 0.0) return clamp((x - lo) / s, 0.0, 1.0);
  return (x > 0.0 && lo < 0.0) ? 1.0 : 0.0;
}
vec3 curve(vec3 x, int b) {  // one 32-float stage: lerp(x, base + w1*t1 + w2*t2, mix * amount)
  vec3 r;
  for (int c = 0; c < 3; ++c) {
    float t1 = ramp(x[c], g(b + c), g(b + 8 + c)), t2 = ramp(x[c], g(b + 4 + c), g(b + 12 + c));
    float k = g(b + 16 + c) + g(b + 20 + c) * t1 + g(b + 24 + c) * t2;
    r[c] = x[c] + (k - x[c]) * (g(b + 28 + c) * g(b + 31));
  }
  return r;
}
void main() {
  if ((uFlags & 4) != 0) { o = uConst; return; }
  if ((uFlags & 64) != 0) { o = vec4(texture(uTex, vUv).rgb, 1.0); return; }
  vec4 c = vCol;
  if ((uFlags & 1) != 0) c *= texture(uTex, vUv);
  if ((uFlags & 16) != 0) c.rgb *= texture(uTex, vUv).rgb;
  if ((uFlags & 32) != 0) c *= texture(uMask, vMuv);
  if ((uFlags & 256) != 0) c.rgb += uFactor * (vec3((c.r + c.g + c.b) * (1.0 / 3.0)) - c.rgb);
  if ((uFlags & 128) != 0) c.rgb = vec3(dot(uCm[0].xyz, c.rgb) * uCm[0].w, dot(uCm[1].xyz, c.rgb) * uCm[1].w,
                                        dot(uCm[2].xyz, c.rgb) * uCm[2].w);
  if ((uFlags & 512) != 0) c.rgb = curve(curve(c.rgb, 0), 32);
  if ((uFlags & 2) != 0) c.rgb += (uTint.rgb - c.rgb) * uTint.a;
  o = c;
}
