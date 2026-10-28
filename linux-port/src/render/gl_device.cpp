// OpenGL 3.3 Device (device.h): each draw is executed at once. Targets are kept top row first by negating clip y in
// the VS (GL's row 0 is the bottom of the viewport), so game UVs sample render targets unchanged.
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "device.h"
#include "gl.h"

using namespace dcp::gl;

namespace dcp::render {
namespace {

const char* kVs = R"(#version 330 core
layout(location=0) in vec4 aPos; layout(location=1) in vec4 aCol; layout(location=2) in vec2 aUv; layout(location=3) in vec2 aMuv;
uniform mat4 uMat; uniform int uRaw; out vec2 vUv; out vec2 vMuv; out vec4 vCol;
void main(){ vec4 p = uRaw == 1 ? aPos : uMat * aPos; p.y = -p.y; gl_Position = p; vUv = aUv; vMuv = aMuv; vCol = aCol; })";
// Re-authored from the game's own PS binaries (GCN disassembly, Work/tools/gcn.py; see README "Pixel shaders").
// uFlags: 1 texture slot 0 (rgba multiply), 16 texture rgb only (alpha = vertex alpha: PS50/53/55), 32 mask texture
// (second texture at vMuv, rgba multiply), 4 constant colour only (PS51), 64 texture rgb with alpha 1 (PS58),
// sprite ops in the game's order: 256 factor (desaturate), 128 colour matrix, 512 gradient (2 tone-curve stages), 2 tint.
// shaders/main.frag is the same program for SDL_gpu; keep the two in step.
const char* kFs = R"(#version 330 core
in vec2 vUv; in vec2 vMuv; in vec4 vCol; uniform sampler2D uTex; uniform sampler2D uMask; uniform int uFlags;
uniform vec4 uTint; uniform vec4 uConst; uniform vec4 uCm[3]; uniform float uFactor; uniform vec4 uGrad[16];
out vec4 o;
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
void main(){
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
})";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "render: shader: %s\n", log);
        return 0;
    }
    return s;
}

GLenum gl_factor(uint32_t f) {
    static const GLenum t[] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                               GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
                               GL_SRC_ALPHA_SATURATE};
    return f < 11 ? t[f] : GL_ONE;
}
GLenum gl_comb(uint32_t c) {
    static const GLenum t[] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_MIN, GL_MAX, GL_FUNC_REVERSE_SUBTRACT};
    return c < 5 ? t[c] : GL_FUNC_ADD;
}

class GlDevice final : public Device {
public:
    const char* name() const override { return "OpenGL"; }

    bool init() override {
        GLuint vs = compile(GL_VERTEX_SHADER, kVs), fs = compile(GL_FRAGMENT_SHADER, kFs);
        if (!vs || !fs) return false;
        prog_ = glCreateProgram();
        glAttachShader(prog_, vs);
        glAttachShader(prog_, fs);
        glLinkProgram(prog_);
        GLint ok = 0;
        glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
        if (!ok) { std::fprintf(stderr, "render: program link failed\n"); return false; }
        u_mat_ = glGetUniformLocation(prog_, "uMat");
        u_raw_ = glGetUniformLocation(prog_, "uRaw");
        u_flags_ = glGetUniformLocation(prog_, "uFlags");
        u_tint_ = glGetUniformLocation(prog_, "uTint");
        u_const_ = glGetUniformLocation(prog_, "uConst");
        u_tex_ = glGetUniformLocation(prog_, "uTex");
        u_mask_ = glGetUniformLocation(prog_, "uMask");
        u_cm_ = glGetUniformLocation(prog_, "uCm");
        u_factor_ = glGetUniformLocation(prog_, "uFactor");
        u_grad_ = glGetUniformLocation(prog_, "uGrad");
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glVertexAttribPointer(0, 4, GL_FLOAT, 0, sizeof(Vtx), (void*)offsetof(Vtx, pos));
        glVertexAttribPointer(1, 4, GL_FLOAT, 0, sizeof(Vtx), (void*)offsetof(Vtx, col));
        glVertexAttribPointer(2, 2, GL_FLOAT, 0, sizeof(Vtx), (void*)offsetof(Vtx, uv));
        glVertexAttribPointer(3, 2, GL_FLOAT, 0, sizeof(Vtx), (void*)offsetof(Vtx, muv));
        for (int i = 0; i < 4; ++i) glEnableVertexAttribArray(i);
        uint8_t px[4] = {255, 255, 255, 255};
        glGenTextures(1, &white_);
        glBindTexture(GL_TEXTURE_2D, white_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return glGetError() == 0;
    }

    TexId create_target(int w, int h) override {
        GLuint tex, fbo;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        fbos_[tex] = fbo;
        return tex;
    }

    TexId upload(TexId reuse, TexFormat f, int w, int h, const void* data, size_t n, bool linear_clamp) override {
        GLuint id = reuse;
        if (!id) glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        GLenum fmt = 0;
        switch (f) {
        case TexFormat::BC7: fmt = GL_COMPRESSED_RGBA_BPTC_UNORM; break;
        case TexFormat::BC1: fmt = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; break;
        case TexFormat::BC2: fmt = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; break;
        case TexFormat::BC3: fmt = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; break;
        case TexFormat::RGBA8: break;
        }
        if (fmt) glCompressedTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, GLsizei(n), data);
        else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        if (linear_clamp) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        return id;
    }

    void destroy(TexId t) override {
        GLuint id = t;
        glDeleteTextures(1, &id);
    }

    void begin_frame() override {
        glUseProgram(prog_);
        glUniform1i(u_tex_, 0);
        glUniform1i(u_mask_, 1);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glColorMask(1, 1, 1, 1);
    }

    void end_frame() override { glDisable(GL_SCISSOR_TEST); }

    void draw(const Draw& d) override {
        auto it = fbos_.find(d.target);
        if (it == fbos_.end()) return;
        glBindFramebuffer(GL_FRAMEBUFFER, it->second);
        glViewport(0, 0, d.tw, d.th);
        if (d.scissor) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(d.sc[0], d.sc[1], d.sc[2], d.sc[3]);  // GL counts from the bottom = row 0 of our top-first targets
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
        if (!d.blend.on) {
            glDisable(GL_BLEND);
        } else {
            glEnable(GL_BLEND);
            glBlendEquationSeparate(gl_comb(d.blend.cc), gl_comb(d.blend.ac));
            glBlendFuncSeparate(gl_factor(d.blend.cs), gl_factor(d.blend.cd), gl_factor(d.blend.as), gl_factor(d.blend.ad));
        }
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, d.tex[1] ? d.tex[1] : white_);
        if (d.tex[1] && d.samp_set[1]) sampler(d.samp[1]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, d.tex[0] ? d.tex[0] : white_);
        if (d.tex[0] && d.samp_set[0]) sampler(d.samp[0]);
        glUniformMatrix4fv(u_mat_, 1, d.transpose, d.mat);
        glUniform1i(u_raw_, d.raw ? 1 : 0);
        glUniform1i(u_flags_, d.flags);
        glUniform4fv(u_tint_, 1, d.tint);
        glUniform4fv(u_const_, 1, d.cst);
        if (d.flags & 128) glUniform4fv(u_cm_, 3, d.cm);
        if (d.flags & 256) glUniform1f(u_factor_, d.factor);
        if (d.flags & 512) glUniform4fv(u_grad_, 16, d.grad);
        static const GLenum kMode[] = {GL_POINTS, GL_LINES, GL_TRIANGLES};
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(d.n * sizeof(Vtx)), d.v, GL_STREAM_DRAW);
        glDrawArrays(kMode[int(d.prim)], 0, GLsizei(d.n));
    }

    bool read_rgba(TexId t, int w, int h, std::vector<uint8_t>* out) override {
        auto it = fbos_.find(t);
        if (it == fbos_.end()) return false;
        out->resize(size_t(w) * h * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, it->second);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out->data());
        return true;
    }

    bool read_texture(TexId t, int w, int h, std::vector<uint8_t>* out) override {
        out->resize(size_t(w) * h * 4);
        glBindTexture(GL_TEXTURE_2D, t);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out->data());
        return true;
    }

    // Drawn as a textured quad, not glBlitFramebuffer: a blit into a multisampled window framebuffer (antialiasing
    // forced in the GPU driver's control panel) is GL_INVALID_OPERATION and left the window black while the game ran
    // (bug reports 28/29, GTX 1660). The first present logs the window framebuffer's samples and any GL error.
    void present(TexId tex, int tw, int th, int w, int h, Overlay*) override {
        static bool logged = false;
        while (!logged && glGetError()) {}  // errors from the replay are not the present's
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glColorMask(1, 1, 1, 1);
        glViewport(0, 0, w, h);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!tex) return;
        float s = std::min(float(w) / tw, float(h) / th);
        int dw = int(tw * s), dh = int(th * s), x = (w - dw) / 2, y = (h - dh) / 2;
        glViewport(x, y, dw, dh);
        glUseProgram(prog_);
        glUniform1i(u_raw_, 1);
        glUniform1i(u_flags_, 64);  // texture rgb, alpha 1
        glUniform1i(u_tex_, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        // The VS negates clip y (targets are top row first): pos y -1 lands at the top of the window, where uv v = 0 is.
        const Vtx q[6] = {{{-1, -1, 0, 1}, {1, 1, 1, 1}, {0, 0}, {}}, {{1, -1, 0, 1}, {1, 1, 1, 1}, {1, 0}, {}},
                          {{-1, 1, 0, 1}, {1, 1, 1, 1}, {0, 1}, {}},  {{1, -1, 0, 1}, {1, 1, 1, 1}, {1, 0}, {}},
                          {{1, 1, 0, 1}, {1, 1, 1, 1}, {1, 1}, {}},   {{-1, 1, 0, 1}, {1, 1, 1, 1}, {0, 1}, {}}};
        glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glViewport(0, 0, w, h);
        if (!logged) {
            logged = true;
            GLint sb = 0, ns = 0;
            glGetIntegerv(GL_SAMPLE_BUFFERS, &sb);
            glGetIntegerv(GL_SAMPLES, &ns);
            GLenum err = glGetError();
            std::fprintf(stderr, "render: present %dx%d -> window %dx%d (%dx%d at %d,%d), window samples %d/%d, GL error 0x%x\n",
                         tw, th, w, h, dw, dh, x, y, sb, ns, err);
        }
    }

private:
    static void sampler(const Sampler& s) {
        static const GLint wrap[] = {GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE, GL_MIRRORED_REPEAT,
                                     GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE};
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap[s.wrap_u & 7]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap[s.wrap_v & 7]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s.mag_linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s.min_linear ? GL_LINEAR : GL_NEAREST);
    }

    GLuint prog_ = 0, vao_ = 0, vbo_ = 0, white_ = 0;
    GLint u_mat_ = -1, u_raw_ = -1, u_flags_ = -1, u_tint_ = -1, u_const_ = -1, u_tex_ = -1, u_mask_ = -1, u_cm_ = -1,
          u_factor_ = -1, u_grad_ = -1;
    std::unordered_map<GLuint, GLuint> fbos_;  // target texture -> its FBO
};

}  // namespace

std::unique_ptr<Device> make_gl_device() { return std::make_unique<GlDevice>(); }

}  // namespace dcp::render
