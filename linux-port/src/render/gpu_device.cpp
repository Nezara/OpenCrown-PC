// SDL_gpu Device (device.h): Vulkan on Linux (SPIR-V shaders only). Record then submit: draw() only appends a record
// (vertices into one frame array, uniforms by value); end_frame() fills one transfer buffer (texture uploads + all
// vertices), runs one copy pass, then the render passes in draw order: a new pass whenever the target changes (load
// LOAD; a target's first pass after creation clears it to 0). SDL_gpu's clip space has NDC y = +1 at row 0 on every
// backend, so the VS does not negate y (the GL VS does) and targets stay top row first as in the GL path.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "device.h"
// SPIR-V of shaders/*.vert / *.frag, prebuilt (src/render/README.md): no shader compiler in the build.
#include "shaders/generated/main_frag.spv.h"
#include "shaders/generated/main_vert.spv.h"
#include "shaders/generated/present_frag.spv.h"
#include "shaders/generated/present_vert.spv.h"

namespace dcp::render {
namespace {

// std140 blocks of shaders/main.vert / main.frag.
struct VsBlock { float mat[16]; int32_t raw; int32_t pad[3]; };
struct FsBlock { float tint[4], cst[4], cm[12], grad[64]; float factor; int32_t flags; int32_t pad[2]; };
static_assert(sizeof(VsBlock) == 80 && sizeof(FsBlock) == 352, "std140 layout");
static_assert(offsetof(FsBlock, factor) == 336 && offsetof(FsBlock, flags) == 340, "std140 layout");

SDL_GPUBlendFactor factor(uint32_t f) {
    static const SDL_GPUBlendFactor t[] = {
        SDL_GPU_BLENDFACTOR_ZERO, SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_SRC_COLOR,
        SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR, SDL_GPU_BLENDFACTOR_SRC_ALPHA, SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        SDL_GPU_BLENDFACTOR_DST_ALPHA, SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA, SDL_GPU_BLENDFACTOR_DST_COLOR,
        SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR, SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE};
    return f < 11 ? t[f] : SDL_GPU_BLENDFACTOR_ONE;
}
SDL_GPUBlendOp op(uint32_t c) {
    static const SDL_GPUBlendOp t[] = {SDL_GPU_BLENDOP_ADD, SDL_GPU_BLENDOP_SUBTRACT, SDL_GPU_BLENDOP_MIN,
                                       SDL_GPU_BLENDOP_MAX, SDL_GPU_BLENDOP_REVERSE_SUBTRACT};
    return c < 5 ? t[c] : SDL_GPU_BLENDOP_ADD;
}
// Same table as the GL device (GCN clamp modes 3..7 approximated).
SDL_GPUSamplerAddressMode wrap(uint32_t w) {
    static const SDL_GPUSamplerAddressMode t[] = {
        SDL_GPU_SAMPLERADDRESSMODE_REPEAT, SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT,
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE, SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT,
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE, SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE, SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE};
    return t[w & 7];
}

// Pipeline key: blend (normalised so equal GL states share one pipeline) + primitive.
uint32_t pipe_key(const Blend& b, Prim p) {
    uint32_t k = uint32_t(p) << 30;
    if (!b.on) return k;
    auto f = [](uint32_t x) { return x < 11 ? x : 1u; };
    auto c = [](uint32_t x) { return x < 5 ? x : 0u; };
    return k | 1u << 29 | f(b.cs) << 24 | f(b.cd) << 19 | c(b.cc) << 16 | f(b.as) << 11 | f(b.ad) << 6 | c(b.ac) << 3;
}
uint32_t samp_key(const Sampler& s) {
    return uint32_t(wrap(s.wrap_u)) | uint32_t(wrap(s.wrap_v)) << 4 | uint32_t(s.mag_linear) << 8 | uint32_t(s.min_linear) << 9;
}
const uint32_t kLinearClamp = samp_key(Sampler{});

SDL_GPUTextureFormat sdl_format(TexFormat f) {
    switch (f) {
    case TexFormat::BC1: return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
    case TexFormat::BC2: return SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM;
    case TexFormat::BC3: return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
    case TexFormat::BC7: return SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM;
    case TexFormat::RGBA8: break;
    }
    return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
}
size_t level_size(TexFormat f, int w, int h) {
    if (f == TexFormat::RGBA8) return size_t(w) * h * 4;
    size_t blocks = size_t((w + 3) / 4) * size_t((h + 3) / 4);
    return blocks * (f == TexFormat::BC1 ? 8 : 16);
}

class GpuDevice final : public Device {
public:
    GpuDevice(SDL_GPUDevice* d, SDL_Window* w) : dev_(d), win_(w) {}
    ~GpuDevice() override {
        if (!dev_) return;
        SDL_WaitForGPUIdle(dev_);
        for (SDL_GPUFence* f : fences_) SDL_ReleaseGPUFence(dev_, f);
        for (auto& [id, t] : texs_) SDL_ReleaseGPUTexture(dev_, t.t);
        if (scratch_) SDL_ReleaseGPUTexture(dev_, scratch_);
        for (auto& [k, p] : pipes_) SDL_ReleaseGPUGraphicsPipeline(dev_, p);
        if (present_pipe_) SDL_ReleaseGPUGraphicsPipeline(dev_, present_pipe_);
        for (auto& [k, s] : samplers_) SDL_ReleaseGPUSampler(dev_, s);
        for (SDL_GPUShader* s : {vs_, fs_, pvs_, pfs_}) if (s) SDL_ReleaseGPUShader(dev_, s);
        if (vbuf_) SDL_ReleaseGPUBuffer(dev_, vbuf_);
        if (up_) SDL_ReleaseGPUTransferBuffer(dev_, up_);
        if (down_) SDL_ReleaseGPUTransferBuffer(dev_, down_);
    }

    const char* name() const override { return "SDL GPU"; }

    // Shaders, the present pipeline and the white texture; err says what failed.
    bool create(std::string* err) {
        SDL_GPUShaderFormat have = SDL_GetGPUShaderFormats(dev_);
        if (!(have & SDL_GPU_SHADERFORMAT_SPIRV)) return fail(err, "no SPIR-V shader support");
        fmt_ = SDL_GPU_SHADERFORMAT_SPIRV;
        vs_ = shader(kMainVertSpv, sizeof kMainVertSpv, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
        fs_ = shader(kMainFragSpv, sizeof kMainFragSpv, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
        pvs_ = shader(kPresentVertSpv, sizeof kPresentVertSpv, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
        pfs_ = shader(kPresentFragSpv, sizeof kPresentFragSpv, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
        if (!vs_ || !fs_ || !pvs_ || !pfs_) return fail(err, std::string("shader: ") + SDL_GetError());
        for (TexFormat f : {TexFormat::RGBA8, TexFormat::BC1, TexFormat::BC2, TexFormat::BC3, TexFormat::BC7})
            if (!SDL_GPUTextureSupportsFormat(dev_, sdl_format(f), SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER))
                return fail(err, "texture format " + std::to_string(int(sdl_format(f))) + " not supported");
        if (!SDL_GPUTextureSupportsFormat(dev_, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_2D,
                                          SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER))
            return fail(err, "RGBA8 render targets not supported");
        // A test pipeline now, so a driver that rejects our shaders fails here (GL fallback) and not mid-game.
        if (!pipeline(pipe_key(Blend{}, Prim::Triangles))) return fail(err, std::string("pipeline: ") + SDL_GetError());
        if (win_) {
            SDL_GPUColorTargetDescription ct{};
            ct.format = SDL_GetGPUSwapchainTextureFormat(dev_, win_);
            SDL_GPUGraphicsPipelineCreateInfo pi{};
            pi.vertex_shader = pvs_, pi.fragment_shader = pfs_;
            pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            pi.rasterizer_state.enable_depth_clip = true;
            pi.target_info.color_target_descriptions = &ct;
            pi.target_info.num_color_targets = 1;
            present_pipe_ = SDL_CreateGPUGraphicsPipeline(dev_, &pi);
            if (!present_pipe_) return fail(err, std::string("present pipeline: ") + SDL_GetError());
        }
        const uint8_t px[4] = {255, 255, 255, 255};
        white_ = upload(0, TexFormat::RGBA8, 1, 1, px, 4, false);
        if (!white_) return fail(err, std::string("texture: ") + SDL_GetError());
        return true;
    }

    bool init() override { return true; }  // done in create()

    TexId create_target(int w, int h) override {
        SDL_GPUTexture* t = texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, w, h,
                                    SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        if (!t) return 0;
        TexId id = next_id_++;
        texs_[id] = {t, w, h, TexFormat::RGBA8, true};
        Rec r{};
        r.clear = true, r.target = id;  // cleared to 0 at this point of the frame
        recs_.push_back(r);
        return id;
    }

    TexId upload(TexId reuse, TexFormat f, int w, int h, const void* data, size_t n, bool) override {
        if (w <= 0 || h <= 0 || n < level_size(f, w, h)) return 0;
        TexId id = reuse;
        if (!id || !texs_.count(id)) {
            SDL_GPUTexture* t = texture(sdl_format(f), w, h, SDL_GPU_TEXTUREUSAGE_SAMPLER);
            if (!t) return 0;
            id = next_id_++;
            texs_[id] = {t, w, h, f, false};
        }
        size_t off = (staging_.size() + 511) & ~size_t(511);  // D3D12 texture placement alignment
        staging_.resize(off + level_size(f, w, h));
        std::memcpy(staging_.data() + off, data, level_size(f, w, h));
        ups_.push_back({id, off});
        return id;
    }

    void destroy(TexId id) override {
        auto it = texs_.find(id);
        if (it == texs_.end()) return;
        SDL_ReleaseGPUTexture(dev_, it->second.t);  // SDL defers the release past work in flight
        texs_.erase(it);
    }

    void draw(const Draw& d) override {
        if (!texs_.count(d.target) || !d.n) return;
        Rec r{};
        r.target = d.target, r.tw = d.tw, r.th = d.th;
        if (d.scissor) {  // clamped to the target (SDL_gpu rejects off-target rects; GL clips them the same way)
            int x0 = std::max(d.sc[0], 0), y0 = std::max(d.sc[1], 0);
            int x1 = std::min(d.sc[0] + d.sc[2], d.tw), y1 = std::min(d.sc[1] + d.sc[3], d.th);
            if (x1 <= x0 || y1 <= y0) return;  // nothing passes the scissor
            r.sc = {x0, y0, x1 - x0, y1 - y0};
        } else {
            r.sc = {0, 0, d.tw, d.th};
        }
        r.pipe = pipe_key(d.blend, d.prim);
        // Slots the shader does not read get the white texture: a stale binding of the target being drawn (seen:
        // PS51 constant-colour rects) would otherwise look like a feedback loop.
        const bool reads[2] = {!(d.flags & 4) && (d.flags & (1 | 16 | 64)) != 0,
                               !(d.flags & (4 | 64)) && (d.flags & 32) != 0};
        for (int k = 0; k < 2; ++k) {
            r.tex[k] = reads[k] && d.tex[k] && texs_.count(d.tex[k]) ? d.tex[k] : white_;
            // Unset = the texture's own state: glyphs (and GL targets as created) sample linear / clamp.
            r.samp[k] = d.samp_set[k] ? samp_key(d.samp[k]) : kLinearClamp;
        }
        // GL keeps sampling state on the texture: one texture in both slots samples with slot 0's state.
        if (d.tex[0] && d.tex[0] == d.tex[1] && d.samp_set[0]) r.samp[1] = r.samp[0];
        r.first = uint32_t(verts_.size()), r.count = uint32_t(d.n);
        verts_.insert(verts_.end(), d.v, d.v + d.n);
        VsBlock& v = r.vs;
        if (d.transpose) {  // game cb0 row-major -> std140 column-major
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j) v.mat[j * 4 + i] = d.mat[i * 4 + j];
        } else {
            std::memcpy(v.mat, d.mat, sizeof v.mat);
        }
        v.raw = d.raw ? 1 : 0;
        FsBlock& f = r.fs;
        std::memcpy(f.tint, d.tint, sizeof f.tint);
        std::memcpy(f.cst, d.cst, sizeof f.cst);
        std::memcpy(f.cm, d.cm, sizeof f.cm);
        std::memcpy(f.grad, d.grad, sizeof f.grad);
        f.factor = d.factor, f.flags = d.flags;
        recs_.push_back(r);
    }

    void end_frame() override { submit(); }

    bool read_rgba(TexId id, int w, int h, std::vector<uint8_t>* out) override {
        submit();
        auto it = texs_.find(id);
        if (it == texs_.end()) return false;
        const uint32_t size = uint32_t(w) * uint32_t(h) * 4;
        if (!down_ || down_size_ < size) {
            if (down_) SDL_ReleaseGPUTransferBuffer(dev_, down_);
            SDL_GPUTransferBufferCreateInfo ti{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, size, 0};
            down_ = SDL_CreateGPUTransferBuffer(dev_, &ti);
            down_size_ = down_ ? size : 0;
            if (!down_) return false;
        }
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
        if (!cmd) return false;
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion src{};
        src.texture = it->second.t, src.w = uint32_t(w), src.h = uint32_t(h), src.d = 1;
        SDL_GPUTextureTransferInfo dst{down_, 0, uint32_t(w), uint32_t(h)};
        SDL_DownloadFromGPUTexture(cp, &src, &dst);
        SDL_EndGPUCopyPass(cp);
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if (!fence) return false;
        SDL_WaitForGPUFences(dev_, true, &fence, 1);
        SDL_ReleaseGPUFence(dev_, fence);
        const void* p = SDL_MapGPUTransferBuffer(dev_, down_, false);
        if (!p) return false;
        out->assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + size);  // row 0 = top
        SDL_UnmapGPUTransferBuffer(dev_, down_);
        return true;
    }

    void present(TexId id, int tw, int th, int, int, Overlay* overlay) override {
        submit();
        if (!win_) return;
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
        if (!cmd) return;
        SDL_GPUTexture* sw = nullptr;
        Uint32 w = 0, h = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, win_, &sw, &w, &h) || !sw) {  // minimised / occluded
            SDL_SubmitGPUCommandBuffer(cmd);
            return;
        }
        if (overlay) overlay->prepare(cmd);
        SDL_GPUColorTargetInfo ct{};
        ct.texture = sw, ct.clear_color = {0, 0, 0, 1}, ct.load_op = SDL_GPU_LOADOP_CLEAR, ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
        auto it = texs_.find(id);
        int dw = 0, dh = 0, x = 0, y = 0;
        if (it != texs_.end() && tw > 0 && th > 0) {
            float s = std::min(float(w) / tw, float(h) / th);
            dw = int(tw * s), dh = int(th * s), x = (int(w) - dw) / 2, y = (int(h) - dh) / 2;
            SDL_GPUViewport vp{float(x), float(y), float(dw), float(dh), 0, 1};
            SDL_SetGPUViewport(pass, &vp);
            SDL_BindGPUGraphicsPipeline(pass, present_pipe_);
            SDL_GPUTextureSamplerBinding b{it->second.t, sampler(kLinearClamp)};
            SDL_BindGPUFragmentSamplers(pass, 0, &b, 1);
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
            SDL_GPUViewport full{0, 0, float(w), float(h), 0, 1};
            SDL_SetGPUViewport(pass, &full);
        }
        if (overlay) overlay->render(cmd, pass);
        SDL_EndGPURenderPass(pass);
        SDL_SubmitGPUCommandBuffer(cmd);
        if (!logged_present_) {
            logged_present_ = true;
            std::fprintf(stderr, "render: present %dx%d -> swapchain %ux%u (%dx%d at %d,%d)\n", tw, th, w, h, dw, dh, x, y);
        }
    }

private:
    struct Tex { SDL_GPUTexture* t = nullptr; int w = 0, h = 0; TexFormat f = TexFormat::RGBA8; bool target = false; };
    struct Rec {
        bool clear;
        TexId target;
        int tw, th;
        struct { int x, y, w, h; } sc;
        uint32_t pipe;
        TexId tex[2];
        uint32_t samp[2];
        uint32_t first, count;
        VsBlock vs;
        FsBlock fs;
    };
    struct Up { TexId id; size_t off; };

    static bool fail(std::string* err, const std::string& why) {
        if (err) *err = why;
        return false;
    }

    SDL_GPUShader* shader(const unsigned char* code, size_t n, SDL_GPUShaderStage stage, Uint32 samplers, Uint32 ubos) {
        SDL_GPUShaderCreateInfo si{};
        si.code_size = n, si.code = code, si.entrypoint = "main", si.format = fmt_, si.stage = stage;
        si.num_samplers = samplers, si.num_uniform_buffers = ubos;
        return SDL_CreateGPUShader(dev_, &si);
    }

    SDL_GPUTexture* texture(SDL_GPUTextureFormat f, int w, int h, SDL_GPUTextureUsageFlags usage) {
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D, ti.format = f, ti.usage = usage;
        ti.width = uint32_t(w), ti.height = uint32_t(h), ti.layer_count_or_depth = 1, ti.num_levels = 1;
        ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
        SDL_GPUTexture* t = SDL_CreateGPUTexture(dev_, &ti);
        if (!t) std::fprintf(stderr, "render: SDL GPU texture %dx%d format %d: %s\n", w, h, int(f), SDL_GetError());
        return t;
    }

    SDL_GPUGraphicsPipeline* pipeline(uint32_t key) {
        auto it = pipes_.find(key);
        if (it != pipes_.end()) return it->second;
        SDL_GPUColorTargetDescription ct{};
        ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        if (key >> 29 & 1) {
            SDL_GPUColorTargetBlendState& b = ct.blend_state;
            b.enable_blend = true;
            b.src_color_blendfactor = factor(key >> 24 & 31), b.dst_color_blendfactor = factor(key >> 19 & 31);
            b.color_blend_op = op(key >> 16 & 7);
            b.src_alpha_blendfactor = factor(key >> 11 & 31), b.dst_alpha_blendfactor = factor(key >> 6 & 31);
            b.alpha_blend_op = op(key >> 3 & 7);
        }
        SDL_GPUVertexBufferDescription vb{0, sizeof(Vtx), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
        SDL_GPUVertexAttribute va[4] = {
            {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, uint32_t(offsetof(Vtx, pos))},
            {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, uint32_t(offsetof(Vtx, col))},
            {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, uint32_t(offsetof(Vtx, uv))},
            {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, uint32_t(offsetof(Vtx, muv))}};
        SDL_GPUGraphicsPipelineCreateInfo pi{};
        pi.vertex_shader = vs_, pi.fragment_shader = fs_;
        pi.vertex_input_state.vertex_buffer_descriptions = &vb;
        pi.vertex_input_state.num_vertex_buffers = 1;
        pi.vertex_input_state.vertex_attributes = va;
        pi.vertex_input_state.num_vertex_attributes = 4;
        pi.primitive_type = Prim(key >> 30) == Prim::Lines ? SDL_GPU_PRIMITIVETYPE_LINELIST : SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pi.rasterizer_state.enable_depth_clip = true;  // with the VS z remap: clips exactly what GL clips
        pi.target_info.color_target_descriptions = &ct;
        pi.target_info.num_color_targets = 1;
        SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(dev_, &pi);
        if (!p) std::fprintf(stderr, "render: SDL GPU pipeline %08x: %s\n", key, SDL_GetError());
        pipes_[key] = p;
        return p;
    }

    SDL_GPUSampler* sampler(uint32_t key) {
        auto it = samplers_.find(key);
        if (it != samplers_.end()) return it->second;
        SDL_GPUSamplerCreateInfo si{};
        si.mag_filter = (key >> 8 & 1) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
        si.min_filter = (key >> 9 & 1) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = SDL_GPUSamplerAddressMode(key & 15);
        si.address_mode_v = SDL_GPUSamplerAddressMode(key >> 4 & 15);
        si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        SDL_GPUSampler* s = SDL_CreateGPUSampler(dev_, &si);
        if (!s) std::fprintf(stderr, "render: SDL GPU sampler %x: %s\n", key, SDL_GetError());
        samplers_[key] = s;
        return s;
    }

    // A draw sampling the target it renders to (a GL feedback loop): sample a copy taken just before the draw.
    SDL_GPUTexture* snapshot(SDL_GPUCommandBuffer* cmd, const Tex& t, const Rec& r) {
        if (!scratch_ || scratch_w_ != t.w || scratch_h_ != t.h) {
            if (scratch_) SDL_ReleaseGPUTexture(dev_, scratch_);
            scratch_ = texture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, t.w, t.h, SDL_GPU_TEXTUREUSAGE_SAMPLER);
            scratch_w_ = t.w, scratch_h_ = t.h;
            if (!scratch_) return nullptr;
        }
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureLocation src{t.t, 0, 0, 0, 0, 0}, dst{scratch_, 0, 0, 0, 0, 0};
        SDL_CopyGPUTextureToTexture(cp, &src, &dst, uint32_t(t.w), uint32_t(t.h), 1, false);
        SDL_EndGPUCopyPass(cp);
        if (++feedback_ <= 4 || (feedback_ & (feedback_ - 1)) == 0) {
            std::fprintf(stderr, "render: SDL GPU: draw samples its own %dx%d target, flags 0x%x blend %08x (copied first; %llu so far)\n",
                         t.w, t.h, r.fs.flags, r.pipe, (unsigned long long)feedback_);
        }
        return scratch_;
    }

    void submit() {
        if (recs_.empty() && ups_.empty()) return;
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
        if (!cmd) {
            std::fprintf(stderr, "render: SDL GPU command buffer: %s\n", SDL_GetError());
            recs_.clear(), ups_.clear(), staging_.clear(), verts_.clear();
            return;
        }
        const size_t voff = (staging_.size() + 15) & ~size_t(15), vbytes = verts_.size() * sizeof(Vtx);
        const size_t total = voff + vbytes;
        if (total) {
            if (!up_ || up_size_ < total) {
                if (up_) SDL_ReleaseGPUTransferBuffer(dev_, up_);
                up_size_ = uint32_t(std::max<size_t>(total + total / 2, 4u << 20));
                SDL_GPUTransferBufferCreateInfo ti{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, up_size_, 0};
                up_ = SDL_CreateGPUTransferBuffer(dev_, &ti);
            }
            if (vbytes && (!vbuf_ || vbuf_size_ < vbytes)) {
                if (vbuf_) SDL_ReleaseGPUBuffer(dev_, vbuf_);
                vbuf_size_ = uint32_t(std::max<size_t>(vbytes + vbytes / 2, 1u << 20));
                SDL_GPUBufferCreateInfo bi{SDL_GPU_BUFFERUSAGE_VERTEX, vbuf_size_, 0};
                vbuf_ = SDL_CreateGPUBuffer(dev_, &bi);
            }
            uint8_t* p = up_ ? static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(dev_, up_, true)) : nullptr;
            if (p && (!vbytes || vbuf_)) {
                std::memcpy(p, staging_.data(), staging_.size());
                if (vbytes) std::memcpy(p + voff, verts_.data(), vbytes);
                SDL_UnmapGPUTransferBuffer(dev_, up_);
                SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
                for (const Up& u : ups_) {
                    auto it = texs_.find(u.id);
                    if (it == texs_.end()) continue;
                    SDL_GPUTextureTransferInfo src{up_, uint32_t(u.off), 0, 0};
                    SDL_GPUTextureRegion dst{};
                    dst.texture = it->second.t, dst.w = uint32_t(it->second.w), dst.h = uint32_t(it->second.h), dst.d = 1;
                    SDL_UploadToGPUTexture(cp, &src, &dst, false);
                }
                if (vbytes) {
                    SDL_GPUTransferBufferLocation src{up_, uint32_t(voff)};
                    SDL_GPUBufferRegion dst{vbuf_, 0, uint32_t(vbytes)};
                    SDL_UploadToGPUBuffer(cp, &src, &dst, true);
                }
                SDL_EndGPUCopyPass(cp);
            } else {
                std::fprintf(stderr, "render: SDL GPU upload buffers: %s\n", SDL_GetError());
                recs_.clear();
            }
        }
        SDL_GPURenderPass* pass = nullptr;
        TexId cur = 0;
        uint32_t cur_pipe = ~0u;
        VsBlock last_vs{};
        FsBlock last_fs{};
        bool have_vs = false, have_fs = false;
        auto begin = [&](TexId target, bool clear) {
            if (pass) SDL_EndGPURenderPass(pass);
            const Tex& t = texs_[target];
            SDL_GPUColorTargetInfo ct{};
            ct.texture = t.t, ct.clear_color = {0, 0, 0, 0};
            ct.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD, ct.store_op = SDL_GPU_STOREOP_STORE;
            pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
            cur = target, cur_pipe = ~0u, have_vs = have_fs = false;
            SDL_GPUViewport vp{0, 0, float(t.w), float(t.h), 0, 1};
            SDL_SetGPUViewport(pass, &vp);
            if (vbuf_) {
                SDL_GPUBufferBinding vb{vbuf_, 0};
                SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
            }
        };
        for (const Rec& r : recs_) {
            if (!texs_.count(r.target)) continue;
            if (r.clear) {
                begin(r.target, true);
                continue;
            }
            SDL_GPUTexture* bind[2];
            for (int k = 0; k < 2; ++k) bind[k] = texs_.count(r.tex[k]) ? texs_[r.tex[k]].t : texs_[white_].t;
            if (r.tex[0] == r.target || r.tex[1] == r.target) {  // feedback: copy outside the pass, then resume
                if (pass) SDL_EndGPURenderPass(pass), pass = nullptr;
                SDL_GPUTexture* s = snapshot(cmd, texs_[r.target], r);
                for (int k = 0; k < 2; ++k)
                    if (r.tex[k] == r.target) bind[k] = s ? s : texs_[white_].t;
                begin(r.target, false);
            } else if (!pass || cur != r.target) {
                begin(r.target, false);
            }
            if (r.pipe != cur_pipe) {
                SDL_GPUGraphicsPipeline* p = pipeline(r.pipe);
                if (!p) continue;
                SDL_BindGPUGraphicsPipeline(pass, p);
                cur_pipe = r.pipe;
            }
            SDL_Rect sc{r.sc.x, r.sc.y, r.sc.w, r.sc.h};
            SDL_SetGPUScissor(pass, &sc);
            SDL_GPUTextureSamplerBinding tb[2] = {{bind[0], sampler(r.samp[0])}, {bind[1], sampler(r.samp[1])}};
            SDL_BindGPUFragmentSamplers(pass, 0, tb, 2);
            if (!have_vs || std::memcmp(&last_vs, &r.vs, sizeof last_vs)) {
                SDL_PushGPUVertexUniformData(cmd, 0, &r.vs, sizeof r.vs);
                last_vs = r.vs, have_vs = true;
            }
            if (!have_fs || std::memcmp(&last_fs, &r.fs, sizeof last_fs)) {
                SDL_PushGPUFragmentUniformData(cmd, 0, &r.fs, sizeof r.fs);
                last_fs = r.fs, have_fs = true;
            }
            SDL_DrawGPUPrimitives(pass, r.count, 1, r.first, 0);
        }
        if (pass) SDL_EndGPURenderPass(pass);
        recs_.clear(), ups_.clear(), staging_.clear(), verts_.clear();
        // At most two replayed frames in flight (hidden runs have no swapchain to pace them).
        SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if (f) fences_.push_back(f);
        while (fences_.size() > 2) {
            SDL_WaitForGPUFences(dev_, true, &fences_.front(), 1);
            SDL_ReleaseGPUFence(dev_, fences_.front());
            fences_.erase(fences_.begin());
        }
    }

    SDL_GPUDevice* dev_;
    SDL_Window* win_;
    SDL_GPUShaderFormat fmt_ = SDL_GPU_SHADERFORMAT_INVALID;
    SDL_GPUShader *vs_ = nullptr, *fs_ = nullptr, *pvs_ = nullptr, *pfs_ = nullptr;
    SDL_GPUGraphicsPipeline* present_pipe_ = nullptr;
    std::unordered_map<uint32_t, SDL_GPUGraphicsPipeline*> pipes_;
    std::unordered_map<uint32_t, SDL_GPUSampler*> samplers_;
    std::unordered_map<TexId, Tex> texs_;
    TexId next_id_ = 1, white_ = 0;
    SDL_GPUTexture* scratch_ = nullptr;
    int scratch_w_ = 0, scratch_h_ = 0;
    std::vector<Rec> recs_;
    std::vector<Up> ups_;
    std::vector<uint8_t> staging_;
    std::vector<Vtx> verts_;
    SDL_GPUBuffer* vbuf_ = nullptr;
    uint32_t vbuf_size_ = 0;
    SDL_GPUTransferBuffer *up_ = nullptr, *down_ = nullptr;
    uint32_t up_size_ = 0, down_size_ = 0;
    std::vector<SDL_GPUFence*> fences_;
    bool logged_present_ = false;
    uint64_t feedback_ = 0;
};

}  // namespace

std::unique_ptr<Device> make_gpu_device(void* sdl_gpu_device, void* sdl_window, std::string* err) {
    auto d = std::make_unique<GpuDevice>(static_cast<SDL_GPUDevice*>(sdl_gpu_device), static_cast<SDL_Window*>(sdl_window));
    if (!d->create(err)) return nullptr;
    return d;
}

}  // namespace dcp::render
