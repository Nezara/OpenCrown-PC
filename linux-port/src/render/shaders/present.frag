#version 450
// Present: the shown target's rgb, alpha 1 (as the GL present's flag 64).
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 o;
layout(set = 2, binding = 0) uniform sampler2D uTex;
void main() { o = vec4(texture(uTex, vUv).rgb, 1.0); }
