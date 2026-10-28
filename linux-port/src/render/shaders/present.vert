#version 450
// Present: one triangle covering the viewport (the letterbox rect); uv v = 0 at the top (NDC y = +1 = row 0).
layout(location = 0) out vec2 vUv;
void main() {
  vec2 t = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);  // (0,0) (2,0) (0,2)
  gl_Position = vec4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
  vUv = t;
}
