#version 450
layout(location = 0) out vec4 o;
layout(set = 2, binding = 0) uniform sampler2D uTex;
layout(set = 3, binding = 0) uniform Con { vec4 scale; ivec4 origin; };
void main() {
  vec2 p = (vec2(ivec2(gl_FragCoord.xy) - origin.xy) + 0.5) * scale.xy;  // footprint centre, in source texels
  ivec2 n = clamp(ivec2(ceil(scale.xy)), ivec2(1), ivec2(4));
  vec3 c = vec3(0.0);
  for (int y = 0; y < n.y; ++y)
    for (int x = 0; x < n.x; ++x) {
      vec2 q = p + ((vec2(x, y) + 0.5) / vec2(n) - 0.5) * scale.xy;
      c += texture(uTex, q * scale.zw).rgb;
    }
  o = vec4(c / float(n.x * n.y), 1.0);
}
