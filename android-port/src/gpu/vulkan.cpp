// Vulkan backend: device, swapchain, GXM scenes as render passes into persistent render targets, and the game's
// draws with its own shaders translated by our USSE translator (src/gpu/usse/).
//
// Two threads drive it. The GXM rendering thread records scenes and draws into one command buffer per frame; flush()
// (sceGxmDisplayQueueAddEntry) submits it (two frames in flight, DCP_FRAMES_IN_FLIGHT=1 = one: each frame slot has
// its own command buffers, fence, host-visible rings for vertex / index / uniform / staging data copied from guest
// memory and descriptor pools, reset when the slot is reused after its fence, i.e. a frame waits for the one before
// the previous). The GXM display queue thread calls present() (the game's display callback, while the
// next frame is being recorded): it acquires a swapchain image and blits the target the game flips to (or, when
// nothing was rendered there, a copy of the guest framebuffer) letterboxed to it, on its own command pool / buffer /
// fence (one present in flight), and never touches the recording state. Hand-over: at flush every render target is
// returned to COLOR_ATTACHMENT_OPTIMAL and the flushed ones are published (shown_) before the submit, under
// shown_lock_; present's blit goes COLOR_ATTACHMENT -> TRANSFER_SRC -> COLOR_ATTACHMENT, so any order of the two
// threads' submissions sees the same layouts, and queue order puts a frame's work before its flip. Locks: state_lock_
// (recording state, rendering thread) -> shown_lock_ -> queue_lock_ (every vkQueue* / vkDeviceWaitIdle);
// swap_lock_ (surface + swapchain, present and the platform's surface_lost / restored) -> shown_lock_ -> queue_lock_.
// Textures are decoded by src/gpu/texture.* and cached by control words (change detection: see Texture); their
// uploads are recorded into a per-frame upload command buffer submitted with the frame (no CPU wait), memory
// sub-allocated from 64 MB blocks.
//
// Not yet: depth / stencil, culling, points. BC textures are decompressed on the CPU when the device can't sample them.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "gpu/vulkan.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include "gpu/overlay.h"
#include "gpu/overlay_shaders.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>

#include "gpu/compute_shaders.h"
#include "gpu/draw_stats.h"
#include "gpu/texture.h"
#include "gpu/usse/translator.h"
#include "guest/memory.h"

namespace gpu {
namespace {

// DEBUG: DCP_LOG_GPU=1 logs per-frame events (presents and their source, texture uploads, pipeline builds).
const bool gpu_log = std::getenv("DCP_LOG_GPU") != nullptr;
// Resident set size of this process (0 if unknown), for the periodic [mem] log line.
uint64_t platform_rss_bytes() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc{};
  return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? uint64_t(pmc.WorkingSetSize) : 0;
#else
  unsigned long long pages = 0, rss = 0;
  FILE* f = std::fopen("/proc/self/statm", "r");
  if (!f) return 0;
  if (std::fscanf(f, "%llu %llu", &pages, &rss) != 2) rss = 0;
  std::fclose(f);
  return rss * 4096ull;
#endif
}

uint64_t now_us() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}

#define VK_FUNCS(X)                            \
  X(vkCreateInstance)                          \
  X(vkEnumerateInstanceLayerProperties)        \
  X(vkDestroyInstance)                         \
  X(vkEnumeratePhysicalDevices)                \
  X(vkGetPhysicalDeviceProperties)             \
  X(vkGetPhysicalDeviceFeatures)               \
  X(vkGetPhysicalDeviceFormatProperties)       \
  X(vkGetPhysicalDeviceQueueFamilyProperties)  \
  X(vkGetPhysicalDeviceMemoryProperties)       \
  X(vkGetPhysicalDeviceSurfaceSupportKHR)      \
  X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
  X(vkGetPhysicalDeviceSurfaceFormatsKHR)      \
  X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
  X(vkCreateDevice)                            \
  X(vkGetDeviceProcAddr)                       \
  X(vkDestroySurfaceKHR)
#define VK_DEVICE_FUNCS(X)         \
  X(vkDestroyDevice)               \
  X(vkGetDeviceQueue)              \
  X(vkDeviceWaitIdle)              \
  X(vkQueueWaitIdle)               \
  X(vkCreateSwapchainKHR)          \
  X(vkDestroySwapchainKHR)         \
  X(vkGetSwapchainImagesKHR)       \
  X(vkAcquireNextImageKHR)         \
  X(vkQueuePresentKHR)             \
  X(vkQueueSubmit)                 \
  X(vkCreateCommandPool)           \
  X(vkDestroyCommandPool)          \
  X(vkAllocateCommandBuffers)      \
  X(vkFreeCommandBuffers)          \
  X(vkBeginCommandBuffer)          \
  X(vkEndCommandBuffer)            \
  X(vkResetCommandBuffer)          \
  X(vkCmdPipelineBarrier)          \
  X(vkCmdCopyBufferToImage)        \
  X(vkCmdBlitImage)                \
  X(vkCmdCopyImageToBuffer)        \
  X(vkCmdCopyImage)                \
  X(vkCmdClearColorImage)          \
  X(vkCreateSemaphore)             \
  X(vkDestroySemaphore)            \
  X(vkCreateFence)                 \
  X(vkDestroyFence)                \
  X(vkWaitForFences)               \
  X(vkResetFences)                 \
  X(vkCreateBuffer)                \
  X(vkDestroyBuffer)               \
  X(vkGetBufferMemoryRequirements) \
  X(vkBindBufferMemory)            \
  X(vkCreateImage)                 \
  X(vkDestroyImage)                \
  X(vkGetImageMemoryRequirements)  \
  X(vkBindImageMemory)             \
  X(vkAllocateMemory)              \
  X(vkFreeMemory)                  \
  X(vkMapMemory)                   \
  X(vkCreateRenderPass)            \
  X(vkDestroyRenderPass)           \
  X(vkCreateFramebuffer)           \
  X(vkDestroyFramebuffer)          \
  X(vkCreateImageView)             \
  X(vkDestroyImageView)            \
  X(vkCmdBeginRenderPass)          \
  X(vkCmdEndRenderPass)            \
  X(vkCreateShaderModule)          \
  X(vkDestroyShaderModule)         \
  X(vkCreatePipelineLayout)        \
  X(vkDestroyPipelineLayout)       \
  X(vkCreateGraphicsPipelines)     \
  X(vkCreatePipelineCache)         \
  X(vkDestroyPipelineCache)        \
  X(vkGetPipelineCacheData)        \
  X(vkCreateComputePipelines)      \
  X(vkCmdDispatch)                 \
  X(vkDestroyPipeline)             \
  X(vkCreateDescriptorSetLayout)   \
  X(vkDestroyDescriptorSetLayout)  \
  X(vkCreateDescriptorPool)        \
  X(vkDestroyDescriptorPool)       \
  X(vkResetDescriptorPool)         \
  X(vkAllocateDescriptorSets)      \
  X(vkUpdateDescriptorSets)        \
  X(vkCreateSampler)               \
  X(vkDestroySampler)              \
  X(vkCmdBindPipeline)             \
  X(vkCmdBindDescriptorSets)       \
  X(vkCmdBindVertexBuffers)        \
  X(vkCmdBindIndexBuffer)          \
  X(vkCmdDrawIndexed)              \
  X(vkCmdDraw)                     \
  X(vkCmdPushConstants)            \
  X(vkCmdSetViewport)              \
  X(vkCmdSetScissor)

class VulkanRenderer final : public Renderer {
 public:
  ~VulkanRenderer() override { destroy(); }
  const char* name() const override { return "vulkan"; }

  bool init(const VulkanSetup& s, std::string& err);

  void begin_scene(guest::Memory& mem, const Scene& scene) override;
  void draw(guest::Memory& mem, const Draw& d) override;
  void end_scene(guest::Memory& mem) override;
  void flush(guest::Memory& mem) override;
  void present(guest::Memory& mem, const Frame& f) override;
  void surface_lost() override;
  void surface_restored() override;
  void save_caches() override;
  void drop_surface();  // swap_lock_ held

 private:
  PFN_vkGetInstanceProcAddr gipa_ = nullptr;
#define DECL(n) PFN_##n n = nullptr;
  VK_FUNCS(DECL)
  VK_DEVICE_FUNCS(DECL)
#undef DECL

  VulkanSetup setup_;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  uint32_t family_ = 0;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;
  VkExtent2D extent_{};
  std::vector<VkImage> images_;
  // Rendering thread: the recording frame slot's command buffer (see FrameSlot), one-off texture uploads (own fence).
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence upload_fence_ = VK_NULL_HANDLE;
  // Display thread (present): own pool, command buffer and fence; acquire semaphore, one "blit done" semaphore per
  // swapchain image (reused only once that image is acquired again, i.e. its last present has consumed it).
  VkCommandPool present_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer present_cmd_ = VK_NULL_HANDLE;
  VkFence present_fence_ = VK_NULL_HANDLE;
  VkSemaphore acquired_ = VK_NULL_HANDLE;
  std::vector<VkSemaphore> rendered_;
  VkPhysicalDeviceMemoryProperties memprops_{};

  // Guest framebuffer upload (display thread).
  VkBuffer staging_ = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem_ = VK_NULL_HANDLE;
  void* staging_ptr_ = nullptr;
  VkImage frame_ = VK_NULL_HANDLE;
  VkDeviceMemory frame_mem_ = VK_NULL_HANDLE;
  uint32_t frame_w_ = 0, frame_h_ = 0;
  bool frame_fresh_ = true;  // image layout still UNDEFINED

  // Render targets: one persistent image per GXM colour surface address (what scenes draw into and what the game
  // flips to with sceDisplaySetFrameBuf).
  struct Target {
    uint32_t w = 0, h = 0;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t stride = 0;   // surface stride in pixels
    uint64_t version = 0;  // bumped by every draw into it
  };
  std::map<uint32_t, Target> targets_;
  // Targets whose rendering has been submitted, by surface address: what present() may show (shown_lock_). Between
  // command buffers every target is in COLOR_ATTACHMENT_OPTIMAL.
  struct Shown {
    VkImage image = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0;
  };
  std::map<uint32_t, Shown> shown_;
  VkRenderPass color_pass_ = VK_NULL_HANDLE;
  std::atomic<bool> recording_{false};
  bool in_pass_ = false;

  Target* target(uint32_t address, uint32_t w, uint32_t h);
  void begin_recording();
  uint32_t scene_w_ = 0, scene_h_ = 0;
  Target* cur_ = nullptr;  // target of the open scene
  void begin_pass(Target* t);

  // Draws: translated programs by GXP hash, pipelines by (vertex program, fragment program, topology).
  struct Program {
    usse::Shader shader;
    VkShaderModule module = VK_NULL_HANDLE;
    std::vector<uint8_t> gxp;                                     // vertex: the program, for the variants
    std::map<std::vector<uint8_t>, VkShaderModule> variants;      // vertex: by per-location Fetch bits
  };
  std::map<uint64_t, Program> programs_;
  // GXM attribute format (SceGxmAttributeFormat 0-9) x components -> the vertex format used + the shader's Fetch bits.
  // The GXM formats map to USCALED / SSCALED and 3-component 8 / 16-bit formats that Vulkan does not require (Mali
  // has none of them: draws with them came out empty); unsupported ones are replaced at device creation.
  struct VertexFormat {
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint8_t fetch = 0;
  };
  VertexFormat vertex_formats_[10][4];
  void init_vertex_formats();
  VkShaderModule vertex_module(Program& p, const std::vector<uint8_t>& fetch);
  // Frame timing, microseconds summed over the log window (see log_timing). Rendering thread: fence = waiting for
  // the frame slot's last frame (two frames back) before recording, flush = closing + submitting a frame. Display
  // thread: the rest.
  std::atomic<uint64_t> t_fence_{0}, t_flush_{0};
  uint64_t t_present_fence_ = 0, t_acquire_ = 0, t_queue_present_ = 0, t_present_ = 0, t_last_ = 0, t_window_ = 0;
  uint64_t t_max_interval_ = 0, t_frames_ = 0;
  void log_timing(uint64_t start);
  struct GxpSeen { uint64_t frame, hash; };
  std::unordered_map<uint32_t, GxpSeen> gxp_seen_;  // program address -> hash, rehashed once per frame
  std::map<std::tuple<const void*, const void*, uint32_t>, VkPipeline> pipelines_;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkDescriptorPool dpool_ = VK_NULL_HANDLE;  // the recording slot's
  // Per-draw fast paths (rendering thread). memo_: the last draw's programs + pipeline (this frame). sets_: one
  // descriptor set per (texture view, sampler bits) and frame. last_ub_: ring offset of the last uniform block per
  // stage (reused when the next one is equal). bound_: what this render pass has bound (reset by begin_pass).
  struct DrawMemo {
    uint64_t frame = ~0ull;
    const void *vertex = nullptr, *fragment = nullptr;
    VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    Program *vs = nullptr, *fs = nullptr;
    VkPipeline pipe = VK_NULL_HANDLE;
  } memo_;
  std::map<std::pair<VkImageView, uint32_t>, VkDescriptorSet> sets_;
  VkDescriptorSet last_set_ = VK_NULL_HANDLE;
  VkImageView last_set_view_ = VK_NULL_HANDLE;
  uint32_t last_set_sampler_ = ~0u;
  VkDeviceSize last_ub_[2] = {VK_WHOLE_SIZE, VK_WHOLE_SIZE};
  alignas(16) uint8_t last_ub_data_[2][usse::kUniformVec4s * 16] = {};
  struct Bound {
    VkPipeline pipe = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint32_t uoff[2] = {~0u, ~0u};
    int index_type = -1;
    bool pc_valid = false;
    float pc[8] = {};
  } bound_;
  bool bc_ = false;  // textureCompressionBC enabled
  Program* program(guest::Memory& mem, uint32_t gxp);
  VkPipeline pipeline(const Draw& d, Program& vs, const Program& fs, VkPrimitiveTopology topo);

  // Upload ring of the recording frame slot (vertices, indices, uniforms), reset when the slot's fence has signalled.
  VkBuffer ring_ = VK_NULL_HANDLE;
  uint8_t* ring_ptr_ = nullptr;
  VkDeviceSize ring_size_ = VkDeviceSize(32) << 20, ring_used_ = 0, ubo_align_ = 256;
  VkDeviceSize ring_alloc(const void* data, VkDeviceSize n, VkDeviceSize align);  // offset, or VK_WHOLE_SIZE if full
  bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf, VkDeviceMemory& mem, void** map);

  // Textures by control words. Replaced images are destroyed one frame later.
  // Change detection, once per frame at the first use: the game's art (BC swizzled, over kSampledMinBytes: ~95% of
  // the ~20 MB live in combat) never changes in place except when a new file is loaded over a freed heap block, which
  // rewrites every block, so a sample check (64 x 32 bytes spread over the data) runs each frame and the full hash
  // only every kFullCheckFrames frames (staggered) or when the samples differ. Staleness bound: an in-place edit that
  // misses every sample shows up within kFullCheckFrames frames. Everything else (small, linear, uncompressed:
  // the CPU-written kind, e.g. glyph caches) is fully hashed every frame, as is any texture seen changing in place
  // during the last kVolatileFrames frames. Render targets sampled as textures don't go through this (versions).
  static constexpr uint64_t kFullCheckFrames = 16, kVolatileFrames = 600, kSampledMinBytes = 64 << 10;
  // Texture memory: device-local, sub-allocated from 64 MB blocks (images only, all optimal tiling: no
  // bufferImageGranularity concerns); requests over 16 MB get their own allocation.
  struct MemBlock {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t type = 0;
    VkDeviceSize used = 0;
    std::map<VkDeviceSize, VkDeviceSize> free;  // offset -> size
  };
  struct MemAlloc {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0, size = 0;
    int block = -1;  // -1 = own allocation
  };
  static constexpr VkDeviceSize kMemBlock = VkDeviceSize(64) << 20;
  std::vector<MemBlock> mem_blocks_;
  bool alloc_memory(const VkMemoryRequirements& req, MemAlloc& out);
  void free_memory(MemAlloc& a);
  // Texture uploads of the frame being recorded go into upload_cmd_ (submitted ahead of cmd_ by flush) with their
  // data in the slot's staging ring (reset after the slot's fence) or, when it is full, in buffers of their own
  // destroyed after that fence.
  VkCommandBuffer upload_cmd_ = VK_NULL_HANDLE;
  bool upload_open_ = false;
  VkBuffer up_ring_ = VK_NULL_HANDLE;
  uint8_t* up_ring_ptr_ = nullptr;
  VkDeviceSize up_ring_size_ = VkDeviceSize(32) << 20, up_ring_used_ = 0;
  struct TempBuffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
  };
  struct Staging {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    uint8_t* ptr = nullptr;
  };
  bool staging(VkDeviceSize n, VkDeviceSize align, Staging& s);
  VkCommandBuffer begin_upload();
  void end_upload(VkCommandBuffer cb);
  struct Texture {
    VkImage image = VK_NULL_HANDLE;
    MemAlloc memory;
    VkImageView view = VK_NULL_HANDLE;
    uint64_t key = 0, checked = ~0ull;
    uint64_t skey = 0, full_at = 0, volatile_until = 0;  // sample key, last full check (frame)
    uint32_t source_bytes = 0;
    bool sampled = false;  // sample check per frame instead of the full hash
    uint64_t version = ~0ull;  // render-target copies: the target version copied
    uint64_t bytes = 0, used = 0;  // device memory size; last frame sampled (eviction)
  };
  // Textures not sampled for kEvictFrames are destroyed (the game streams textures through ever-changing heap
  // addresses, so control-word keys alone grow without bound); past kTextureBudget the least recently used go first.
  static constexpr uint64_t kEvictFrames = 600;
  static constexpr uint64_t kTextureBudget = uint64_t(384) << 20;
  uint64_t texture_bytes_ = 0;
  void evict_textures();
  void log_memory();
  std::map<std::array<uint32_t, 4>, Texture> textures_;
  std::map<uint32_t, VkSampler> samplers_;
  Texture dummy_;  // 1x1 white, for units a shader does not sample
  Texture* texture(guest::Memory& mem, const std::array<uint32_t, 4>& words);
  std::map<std::array<uint32_t, 4>, Texture> target_textures_;
  Texture* target_texture(const std::array<uint32_t, 4>& words, Target& src);
  bool upload_texture(const tex::Decoded& dec, Texture& t);
  // GPU BC decode (compute_shaders.h) on devices without BC sampling.
  static constexpr uint32_t kBcSets = 256;  // mip levels decoded per frame (more: CPU path)
  bool gpu_bc_ = false;
  VkDescriptorSetLayout bc_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout bc_layout_ = VK_NULL_HANDLE;
  VkPipeline bc_pipe_ = VK_NULL_HANDLE;
  VkDescriptorPool bc_pool_ = VK_NULL_HANDLE;  // the recording slot's
  VkDeviceSize ssbo_align_ = 256;

  // Frames in flight: frame N records into slot N % frames_in_flight_ while frame N - 1 may still run on the GPU;
  // begin_recording waits for the slot's fence (frame N - 2: with every earlier submission, as fences cover all work
  // submitted before them), then frees what that frame held. The recording slot's handles are copied into cmd_,
  // upload_cmd_, ring_, up_ring_, dpool_ and bc_pool_ (use_slot). DCP_FRAMES_IN_FLIGHT=1: one slot, as before.
  static constexpr uint32_t kFramesInFlight = 2;
  struct FrameSlot {
    VkCommandBuffer cmd = VK_NULL_HANDLE, upload_cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;  // created signalled
    VkBuffer ring = VK_NULL_HANDLE, up_ring = VK_NULL_HANDLE;
    VkDeviceMemory ring_mem = VK_NULL_HANDLE, up_ring_mem = VK_NULL_HANDLE;
    uint8_t *ring_ptr = nullptr, *up_ring_ptr = nullptr;
    VkDescriptorPool dpool = VK_NULL_HANDLE, bc_pool = VK_NULL_HANDLE;
    uint64_t frame = 0;  // frames_ while it was last recorded (texture eviction)
    // Destroyed after the slot's fence: overflow staging buffers, BC per-level storage views, replaced textures.
    std::vector<TempBuffer> retired_buffers;
    std::vector<VkImageView> retired_views;
    std::vector<Texture> retired;
  };
  FrameSlot slots_[kFramesInFlight];
  uint32_t frames_in_flight_ = kFramesInFlight;
  std::atomic<uint32_t> slot_{0};  // the recording (or next) slot; advanced by flush under state_lock_
  FrameSlot& cur_slot() { return slots_[slot_.load()]; }
  void use_slot();
  // Persistent pipeline cache (<cache_dir>/pipelines.bin): our header (format version, vendor / device / driver
  // version, pipelineCacheUUID, size, checksum) + the driver's data. Loaded at start-up when the header matches this
  // device and driver; saved on exit / background (save_caches) and from flush() once new pipelines have been built
  // and none for 120 frames (data taken under the lock, file written on a short-lived thread, tmp + rename).
  VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
  std::string cache_path_;
  VkPhysicalDeviceProperties dev_props_{};
  uint64_t pipeline_builds_ = 0, saved_builds_ = 0, last_build_frame_ = 0;
  void load_pipeline_cache();
  std::vector<uint8_t> pipeline_cache_file();  // header + data, empty if unavailable
  bool init_gpu_bc();
  bool upload_bc_gpu(const tex::Desc& d, const uint8_t* src, Texture& t);
  // DEBUG: DCP_BC_VERIFY=1 reads back level 0 of every GPU-decoded texture after its frame and compares it with the
  // CPU decompression (tex::decompress_bc), logging the result per texture.
  struct BcCheck {
    VkImage image;
    uint32_t w, h;
    std::vector<uint8_t> want;
  };
  std::vector<BcCheck> bc_checks_;
  void verify_bc();
  bool create_texture_image(Texture& t, VkFormat format, uint32_t w, uint32_t h, uint32_t mips, VkImageUsageFlags usage,
                            const tex::Swizzle& swz);
  VkSampler sampler(uint32_t word0);
  void destroy_texture(Texture& t);

  std::set<std::string> warned_;
  void warn_once(const std::string& what) {
    if (warned_.insert(what).second) std::fprintf(stderr, "[vulkan] %s\n", what.c_str());
  }

  std::mutex state_lock_, shown_lock_, queue_lock_, swap_lock_;  // order: see the file comment
  // frames_ = frames flushed (the rendering thread's frame number: texture re-hash, eviction); presents_ = flips.
  std::atomic<uint64_t> scenes_{0}, draws_{0}, frames_{0}, presents_{0};

  bool create_swapchain(std::string& err);
  void destroy_swapchain();
  // Surface state the swapchain was made for: with an IDENTITY swapchain on a rotated Android panel every present
  // is SUBOPTIMAL, so we rebuild only when the rotation or size really changed.
  VkSurfaceTransformFlagBitsKHR made_transform_{};
  VkExtent2D made_extent_{};
  bool surface_changed() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps);
    return caps.currentTransform != made_transform_ || caps.currentExtent.width != made_extent_.width ||
           caps.currentExtent.height != made_extent_.height;
  }

  // Host UI overlay (touch controls): drawn into the swapchain image after the picture, alpha-blended.
  VkRenderPass overlay_pass_ = VK_NULL_HANDLE;
  VkFormat overlay_format_ = VK_FORMAT_UNDEFINED;
  VkPipelineLayout overlay_layout_ = VK_NULL_HANDLE;
  VkPipeline overlay_pipe_ = VK_NULL_HANDLE;
  std::vector<VkImageView> sc_views_;
  std::vector<VkFramebuffer> sc_fbs_;
  // Overlay vertices (display thread; one present in flight, so rewritten after the present fence).
  VkBuffer overlay_buf_ = VK_NULL_HANDLE;
  VkDeviceMemory overlay_mem_ = VK_NULL_HANDLE;
  void* overlay_ptr_ = nullptr;
  VkDeviceSize overlay_size_ = 0;
  bool create_overlay();
  void destroy_overlay();
  bool draw_overlay(uint32_t index);  // false: nothing drawn (the image stays in TRANSFER_DST)
  bool ensure_frame_image(uint32_t w, uint32_t h);
  int32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) const;
  void destroy();
};

int32_t VulkanRenderer::memory_type(uint32_t bits, VkMemoryPropertyFlags want) const {
  for (uint32_t i = 0; i < memprops_.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (memprops_.memoryTypes[i].propertyFlags & want) == want) return int32_t(i);
  return -1;
}

bool VulkanRenderer::init(const VulkanSetup& s, std::string& err) {
  setup_ = s;
  ticks_per_us();  // calibrate the draw-path timer now, not in the first log
  gipa_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(s.get_instance_proc_addr);
  if (!gipa_) return err = "no vkGetInstanceProcAddr", false;
  vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa_(VK_NULL_HANDLE, "vkCreateInstance"));
  vkEnumerateInstanceLayerProperties = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
      gipa_(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties"));
  if (!vkCreateInstance) return err = "Vulkan loader unusable", false;

  std::vector<const char*> layers;
  if (s.validation && vkEnumerateInstanceLayerProperties) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> props(n);
    vkEnumerateInstanceLayerProperties(&n, props.data());
    for (auto& p : props)
      if (!std::strcmp(p.layerName, "VK_LAYER_KHRONOS_validation")) layers.push_back("VK_LAYER_KHRONOS_validation");
    if (layers.empty()) std::fprintf(stderr, "[vulkan] validation layer not installed\n");
  }
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "OpenCrown Mobile";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = uint32_t(s.instance_extensions.size());
  ici.ppEnabledExtensionNames = s.instance_extensions.data();
  ici.enabledLayerCount = uint32_t(layers.size());
  ici.ppEnabledLayerNames = layers.data();
  if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) return err = "vkCreateInstance failed", false;
#define LOAD(n) n = reinterpret_cast<PFN_##n>(gipa_(instance_, #n));
  VK_FUNCS(LOAD)
#undef LOAD

  surface_ = reinterpret_cast<VkSurfaceKHR>(uintptr_t(s.create_surface(instance_)));
  if (!surface_) return err = "window surface creation failed", false;

  // A device with a queue family that does graphics and presents to our surface; discrete GPUs first.
  uint32_t n = 0;
  vkEnumeratePhysicalDevices(instance_, &n, nullptr);
  std::vector<VkPhysicalDevice> gpus(n);
  vkEnumeratePhysicalDevices(instance_, &n, gpus.data());
  int best = -1;
  for (VkPhysicalDevice g : gpus) {
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> q(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, q.data());
    for (uint32_t i = 0; i < qn; i++) {
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(g, i, surface_, &present);
      if (!(q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(g, &p);
      int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
      if (score > best) best = score, gpu_ = g, family_ = i;
      break;
    }
  }
  if (!gpu_) return err = "no Vulkan device can present to the window", false;
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(gpu_, &props);
  vkGetPhysicalDeviceMemoryProperties(gpu_, &memprops_);
  std::fprintf(stderr, "[vulkan] %s (API %u.%u)\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
               VK_API_VERSION_MINOR(props.apiVersion));

  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = family_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  const char* dev_ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = dev_ext;
  VkPhysicalDeviceFeatures have{}, want{};
  vkGetPhysicalDeviceFeatures(gpu_, &have);
  // DCP_NO_BC=1 (debug): take the CPU decompression path on desktop GPUs too, as on most Android devices.
  want.textureCompressionBC = bc_ = have.textureCompressionBC && !std::getenv("DCP_NO_BC");  // the game's textures are BC3
  dci.pEnabledFeatures = &want;
  ubo_align_ = std::max<VkDeviceSize>(16, props.limits.minUniformBufferOffsetAlignment);
  if (vkCreateDevice(gpu_, &dci, nullptr, &device_) != VK_SUCCESS) return err = "vkCreateDevice failed", false;
#define LOAD(n) n = reinterpret_cast<PFN_##n>(vkGetDeviceProcAddr(device_, #n));
  VK_DEVICE_FUNCS(LOAD)
#undef LOAD
  vkGetDeviceQueue(device_, family_, 0, &queue_);
  dev_props_ = props;
  init_vertex_formats();
  if (!s.cache_dir.empty()) cache_path_ = s.cache_dir + "/pipelines.bin";
  load_pipeline_cache();

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = family_;
  vkCreateCommandPool(device_, &pci, nullptr, &pool_);
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool_;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  // DEBUG / A-B: DCP_FRAMES_IN_FLIGHT=1 = one frame in flight (the next frame waits for the previous one's GPU work).
  if (const char* e = std::getenv("DCP_FRAMES_IN_FLIGHT"))
    frames_in_flight_ = std::clamp<uint32_t>(uint32_t(std::strtoul(e, nullptr, 10)), 1, kFramesInFlight);
  for (uint32_t i = 0; i < frames_in_flight_; i++) {
    vkAllocateCommandBuffers(device_, &cai, &slots_[i].cmd);
    vkAllocateCommandBuffers(device_, &cai, &slots_[i].upload_cmd);
  }
  vkCreateCommandPool(device_, &pci, nullptr, &present_pool_);
  cai.commandPool = present_pool_;
  vkAllocateCommandBuffers(device_, &cai, &present_cmd_);
  VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  vkCreateSemaphore(device_, &sci, nullptr, &acquired_);
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  vkCreateFence(device_, &fci, nullptr, &upload_fence_);
  fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  for (uint32_t i = 0; i < frames_in_flight_; i++) vkCreateFence(device_, &fci, nullptr, &slots_[i].fence);
  vkCreateFence(device_, &fci, nullptr, &present_fence_);

  // Scene render pass: the target keeps its contents between scenes (GXM has no implicit clear).
  VkAttachmentDescription color{};
  color.format = VK_FORMAT_R8G8B8A8_UNORM;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpi.attachmentCount = 1;
  rpi.pAttachments = &color;
  rpi.subpassCount = 1;
  rpi.pSubpasses = &sub;
  if (vkCreateRenderPass(device_, &rpi, nullptr, &color_pass_) != VK_SUCCESS) return err = "render pass", false;

  // Draw resources: set 0 = vertex uniforms, fragment uniforms (dynamic offsets into the ring), texture unit 0 (the
  // translator's interface);
  // push constants = the GXM viewport transform (vertex stage).
  VkDescriptorSetLayoutBinding binds[3] = {};
  binds[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
  binds[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  binds[2] = {usse::kTextureBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dli.bindingCount = 3;
  dli.pBindings = binds;
  if (vkCreateDescriptorSetLayout(device_, &dli, nullptr, &set_layout_) != VK_SUCCESS) return err = "set layout", false;
  VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &set_layout_;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pcr;
  if (vkCreatePipelineLayout(device_, &pli, nullptr, &layout_) != VK_SUCCESS) return err = "pipeline layout", false;
  VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 * 4096},
                                   {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096}};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 4096;  // (texture, sampler) pairs per frame
  dpi.poolSizeCount = 2;
  dpi.pPoolSizes = sizes;
  for (uint32_t i = 0; i < frames_in_flight_; i++) {
    FrameSlot& f = slots_[i];
    if (vkCreateDescriptorPool(device_, &dpi, nullptr, &f.dpool) != VK_SUCCESS) return err = "descriptor pool", false;
    if (!create_buffer(ring_size_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, f.ring, f.ring_mem, reinterpret_cast<void**>(&f.ring_ptr)))
      return err = "upload ring", false;
    if (!create_buffer(up_ring_size_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, f.up_ring,
                       f.up_ring_mem, reinterpret_cast<void**>(&f.up_ring_ptr)))
      return err = "staging ring", false;
  }
  gpu_bc_ = !bc_ && init_gpu_bc();
  use_slot();
  std::fprintf(stderr, "[vulkan] %u frame%s in flight\n", frames_in_flight_, frames_in_flight_ > 1 ? "s" : "");
  tex::Decoded white;
  white.desc.ok = true;
  white.desc.width = white.desc.height = 1;
  white.data = {255, 255, 255, 255};
  white.levels = {{0, 1, 1}};
  if (!upload_texture(white, dummy_)) return err = "dummy texture", false;
  if (!bc_)
    std::fprintf(stderr, "[vulkan] no BC texture support: BC textures are decompressed %s\n",
                 gpu_bc_ ? "by a compute shader" : "on the CPU");
  return create_swapchain(err);
}

bool VulkanRenderer::create_swapchain(std::string& err) {
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps);
  uint32_t w = 0, h = 0;
  if (setup_.drawable_size) setup_.drawable_size(w, h);
  if (caps.currentExtent.width != UINT32_MAX) w = caps.currentExtent.width, h = caps.currentExtent.height;
  if (!w || !h) return true;  // minimised: try again on the next present
  extent_ = {std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width),
             std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height)};
  // On a rotated Android panel (portrait-native handhelds like the Retroid Pocket 3+) currentTransform is 90/270.
  // We render unrotated (IDENTITY) and let the compositor rotate. Some drivers report currentExtent in the panel's
  // native orientation, others (the Retroid's Mali) already in the window's: swap only when it disagrees with the
  // window's aspect.
  bool identity = caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  uint32_t ww = 0, wh = 0;
  if (setup_.drawable_size) setup_.drawable_size(ww, wh);
  if (identity && (caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) &&
      ww && wh && (ww > wh) != (extent_.width > extent_.height))
    std::swap(extent_.width, extent_.height);
  made_transform_ = caps.currentTransform, made_extent_ = caps.currentExtent;
  std::fprintf(stderr, "[vulkan] swapchain %ux%u (surface transform 0x%x, %s, images min %u max %u)\n", extent_.width,
               extent_.height, (unsigned)caps.currentTransform, identity ? "rendered unrotated" : "surface transform kept",
               caps.minImageCount, caps.maxImageCount);
  uint32_t n = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(n);
  vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, formats.data());
  VkSurfaceFormatKHR fmt = formats.empty() ? VkSurfaceFormatKHR{} : formats[0];
  for (auto& f : formats)
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) fmt = f;
  format_ = fmt.format;
  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = surface_;
  // DEBUG: DCP_SWAPCHAIN_IMAGES=<n> asks for more images (default 2: the game paces itself on vblank anyway).
  const char* want_images = std::getenv("DCP_SWAPCHAIN_IMAGES");
  ci.minImageCount = std::max(caps.minImageCount, want_images ? uint32_t(std::strtoul(want_images, nullptr, 10)) : 2u);
  if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
  ci.imageFormat = fmt.format;
  ci.imageColorSpace = fmt.colorSpace;
  ci.imageExtent = extent_;
  ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = identity ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
  ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always available; the game paces itself on vblank anyway
  ci.clipped = VK_TRUE;
  ci.oldSwapchain = swapchain_;
  VkSwapchainKHR sc;
  if (vkCreateSwapchainKHR(device_, &ci, nullptr, &sc) != VK_SUCCESS) return err = "vkCreateSwapchainKHR failed", false;
  if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
  swapchain_ = sc;
  vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
  images_.resize(n);
  vkGetSwapchainImagesKHR(device_, swapchain_, &n, images_.data());
  for (VkSemaphore s : rendered_) vkDestroySemaphore(device_, s, nullptr);  // callers waited for the device
  rendered_.assign(n, VK_NULL_HANDLE);
  VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (VkSemaphore& s : rendered_) vkCreateSemaphore(device_, &sci, nullptr, &s);
  if (!create_overlay()) std::fprintf(stderr, "[vulkan] overlay unavailable (touch controls not drawn)\n");
  return true;
}

void VulkanRenderer::destroy_swapchain() {
  destroy_overlay();
  if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
  swapchain_ = VK_NULL_HANDLE;
  images_.clear();
  for (VkSemaphore s : rendered_) vkDestroySemaphore(device_, s, nullptr);
  rendered_.clear();
}

bool VulkanRenderer::ensure_frame_image(uint32_t w, uint32_t h) {
  if (frame_ && w == frame_w_ && h == frame_h_) return true;
  if (frame_) vkDestroyImage(device_, frame_, nullptr), vkFreeMemory(device_, frame_mem_, nullptr);
  if (staging_) vkDestroyBuffer(device_, staging_, nullptr), vkFreeMemory(device_, staging_mem_, nullptr);
  frame_ = VK_NULL_HANDLE;
  staging_ = VK_NULL_HANDLE;
  frame_w_ = w;
  frame_h_ = h;
  frame_fresh_ = true;
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = VkDeviceSize(w) * h * 4;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (vkCreateBuffer(device_, &bci, nullptr, &staging_) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device_, staging_, &req);
  int32_t type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = uint32_t(type);
  if (type < 0 || vkAllocateMemory(device_, &mai, nullptr, &staging_mem_) != VK_SUCCESS) return false;
  vkBindBufferMemory(device_, staging_, staging_mem_, 0);
  vkMapMemory(device_, staging_mem_, 0, bci.size, 0, &staging_ptr_);

  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_R8G8B8A8_UNORM;  // the Vita's A8B8G8R8 is R,G,B,A in byte order
  ici.extent = {w, h, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (vkCreateImage(device_, &ici, nullptr, &frame_) != VK_SUCCESS) return false;
  vkGetImageMemoryRequirements(device_, frame_, &req);
  mai.allocationSize = req.size;
  type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  mai.memoryTypeIndex = uint32_t(type);
  if (type < 0 || vkAllocateMemory(device_, &mai, nullptr, &frame_mem_) != VK_SUCCESS) return false;
  vkBindImageMemory(device_, frame_, frame_mem_, 0);
  return true;
}

void barrier(PFN_vkCmdPipelineBarrier fn, VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
             VkAccessFlags src, VkAccessFlags dst) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.oldLayout = from;
  b.newLayout = to;
  b.srcAccessMask = src;
  b.dstAccessMask = dst;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  fn(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void VulkanRenderer::use_slot() {
  FrameSlot& f = cur_slot();
  cmd_ = f.cmd, upload_cmd_ = f.upload_cmd, dpool_ = f.dpool, bc_pool_ = f.bc_pool;
  ring_ = f.ring, ring_ptr_ = f.ring_ptr, up_ring_ = f.up_ring, up_ring_ptr_ = f.up_ring_ptr;
}

// state_lock_ held. The slot's fence: its ring space, descriptor sets and retired textures are free again.
void VulkanRenderer::begin_recording() {
  if (recording_) return;
  FrameSlot& f = cur_slot();
  uint64_t tw = now_us();
  vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX);
  t_fence_ += now_us() - tw;
  vkResetFences(device_, 1, &f.fence);
  use_slot();
  f.frame = frames_.load();
  if (!bc_checks_.empty()) verify_bc();  // before recording_: one-off command buffers
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkResetCommandBuffer(cmd_, 0);
  vkBeginCommandBuffer(cmd_, &bi);
  // The previous frame may still run on the GPU: its render target / copy / compute writes and its reads of the
  // same images come before this frame's (render passes keep targets in COLOR_ATTACHMENT_OPTIMAL with no barrier).
  // Fragment-side stages only, so this frame's vertex work may overlap the previous frame's fragment work.
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                     VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd_,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       0, 1, &mb, 0, nullptr, 0, nullptr);
  recording_ = true;
  ring_used_ = up_ring_used_ = 0;  // the slot's previous frame is done
  for (TempBuffer& b : f.retired_buffers) vkDestroyBuffer(device_, b.buf, nullptr), vkFreeMemory(device_, b.mem, nullptr);
  f.retired_buffers.clear();
  for (VkImageView v : f.retired_views) vkDestroyImageView(device_, v, nullptr);
  f.retired_views.clear();
  if (bc_pool_) vkResetDescriptorPool(device_, bc_pool_, 0);
  vkResetDescriptorPool(device_, dpool_, 0);
  sets_.clear();
  last_set_ = VK_NULL_HANDLE, last_set_view_ = VK_NULL_HANDLE, last_set_sampler_ = ~0u;
  last_ub_[0] = last_ub_[1] = VK_WHOLE_SIZE;
  for (Texture& t : f.retired) destroy_texture(t);
  f.retired.clear();
  evict_textures();
  log_memory();
}

void VulkanRenderer::evict_textures() {
  uint64_t frame = frames_.load();
  static uint64_t last = 0;
  if (frame - last < 60 && texture_bytes_ <= kTextureBudget) return;
  last = frame;
  // Called right after the slot's fence wait: frames recorded since (the other slots) may still be on the GPU, so
  // only images last used before the oldest of them are destroyed.
  uint64_t busy = frame;
  for (uint32_t i = 0; i < frames_in_flight_; i++)
    if (i != slot_.load()) busy = std::min(busy, slots_[i].frame);
  for (auto* m : {&textures_, &target_textures_})
    for (auto it = m->begin(); it != m->end();)
      if (frame - it->second.used > kEvictFrames && it->second.used < busy) destroy_texture(it->second), it = m->erase(it);
      else ++it;
  while (texture_bytes_ > kTextureBudget) {
    std::map<std::array<uint32_t, 4>, Texture>* om = nullptr;
    std::map<std::array<uint32_t, 4>, Texture>::iterator oldest;
    for (auto* m : {&textures_, &target_textures_})
      for (auto it = m->begin(); it != m->end(); ++it)
        if (it->second.used + 2 < frame && it->second.used < busy && (!om || it->second.used < oldest->second.used))
          om = m, oldest = it;
    if (!om) break;  // everything is in use right now
    destroy_texture(oldest->second);
    om->erase(oldest);
  }
}

void VulkanRenderer::log_memory() {
  static uint64_t last_us = 0;
  uint64_t t = now_us();
  if (t - last_us < 10000000) return;
  last_us = t;
  std::fprintf(stderr, "[mem] rss %llu MB, textures %zu + %zu rt (%llu MB), pipelines %zu, programs %zu\n",
               (unsigned long long)(platform_rss_bytes() >> 20), textures_.size(), target_textures_.size(),
               (unsigned long long)(texture_bytes_ >> 20), pipelines_.size(), programs_.size());
}

bool VulkanRenderer::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf, VkDeviceMemory& mem,
                                   void** map) {
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = usage;
  if (vkCreateBuffer(device_, &bci, nullptr, &buf) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device_, buf, &req);
  int32_t type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = uint32_t(type);
  if (type < 0 || vkAllocateMemory(device_, &mai, nullptr, &mem) != VK_SUCCESS) return false;
  vkBindBufferMemory(device_, buf, mem, 0);
  return vkMapMemory(device_, mem, 0, size, 0, map) == VK_SUCCESS;
}

VkDeviceSize VulkanRenderer::ring_alloc(const void* data, VkDeviceSize n, VkDeviceSize align) {
  VkDeviceSize off = (ring_used_ + align - 1) / align * align;
  if (off + n > ring_size_) return VK_WHOLE_SIZE;
  if (data) std::memcpy(ring_ptr_ + off, data, size_t(n));
  ring_used_ = off + n;
  return off;
}

// --- persistent pipeline cache ---------------------------------------------------------------------------------------
struct CacheHeader {
  char magic[8];  // "OCPIPE\0\0"
  uint32_t format, vendor, device, driver;
  uint8_t uuid[VK_UUID_SIZE];
  uint64_t size, checksum;  // of the driver data that follows
};
constexpr uint32_t kCacheFormat = 1;
constexpr char kCacheMagic[8] = {'O', 'C', 'P', 'I', 'P', 'E', 0, 0};

uint64_t fnv64(const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

// tmp + rename, so a crash or kill mid-write never leaves a torn file. One writer at a time.
void write_cache_file(const std::string& path, const std::vector<uint8_t>& data) {
  static std::mutex mu;
  if (path.empty() || data.empty()) return;
  std::lock_guard lk(mu);
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  std::string tmp = path + ".tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return;
  bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  ok = std::fclose(f) == 0 && ok;
  if (ok) std::filesystem::rename(tmp, path, ec);
  if (!ok || ec) std::fprintf(stderr, "[vulkan] pipeline cache not saved to %s\n", path.c_str());
}

void VulkanRenderer::load_pipeline_cache() {
  std::vector<uint8_t> file;
  if (!cache_path_.empty())
    if (std::FILE* f = std::fopen(cache_path_.c_str(), "rb")) {
      std::fseek(f, 0, SEEK_END);
      long n = std::ftell(f);
      std::fseek(f, 0, SEEK_SET);
      if (n > 0) {
        file.resize(size_t(n));
        if (std::fread(file.data(), 1, file.size(), f) != file.size()) file.clear();
      }
      std::fclose(f);
    }
  const char* why = file.empty() ? "none yet" : nullptr;
  CacheHeader h{};
  if (!why) {
    if (file.size() < sizeof h) why = "truncated";
    else std::memcpy(&h, file.data(), sizeof h);
  }
  if (!why && (std::memcmp(h.magic, kCacheMagic, 8) || h.format != kCacheFormat)) why = "unknown format";
  if (!why && (h.vendor != dev_props_.vendorID || h.device != dev_props_.deviceID || h.driver != dev_props_.driverVersion ||
               std::memcmp(h.uuid, dev_props_.pipelineCacheUUID, VK_UUID_SIZE)))
    why = "other device or driver";
  if (!why && (h.size != file.size() - sizeof h || h.checksum != fnv64(file.data() + sizeof h, size_t(h.size))))
    why = "damaged";
  VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  if (!why) ci.initialDataSize = size_t(h.size), ci.pInitialData = file.data() + sizeof h;
  if (vkCreatePipelineCache(device_, &ci, nullptr, &pipeline_cache_) != VK_SUCCESS) {
    why = "rejected by the driver";
    ci.initialDataSize = 0, ci.pInitialData = nullptr;
    if (vkCreatePipelineCache(device_, &ci, nullptr, &pipeline_cache_) != VK_SUCCESS) pipeline_cache_ = VK_NULL_HANDLE;
  }
  if (cache_path_.empty()) return;
  if (why) std::fprintf(stderr, "[vulkan] pipeline cache %s: new (%s)\n", cache_path_.c_str(), why);
  else std::fprintf(stderr, "[vulkan] pipeline cache %s: %llu KB loaded\n", cache_path_.c_str(),
                    (unsigned long long)(h.size >> 10));
}

std::vector<uint8_t> VulkanRenderer::pipeline_cache_file() {
  if (!pipeline_cache_) return {};
  size_t n = 0;
  if (vkGetPipelineCacheData(device_, pipeline_cache_, &n, nullptr) != VK_SUCCESS || !n) return {};
  std::vector<uint8_t> out(sizeof(CacheHeader) + n);
  if (vkGetPipelineCacheData(device_, pipeline_cache_, &n, out.data() + sizeof(CacheHeader)) != VK_SUCCESS) return {};
  out.resize(sizeof(CacheHeader) + n);
  CacheHeader h{};
  std::memcpy(h.magic, kCacheMagic, 8);
  h.format = kCacheFormat;
  h.vendor = dev_props_.vendorID, h.device = dev_props_.deviceID, h.driver = dev_props_.driverVersion;
  std::memcpy(h.uuid, dev_props_.pipelineCacheUUID, VK_UUID_SIZE);
  h.size = n;
  h.checksum = fnv64(out.data() + sizeof h, n);
  std::memcpy(out.data(), &h, sizeof h);
  return out;
}

void VulkanRenderer::save_caches() {
  if (cache_path_.empty() || !device_) return;
  std::vector<uint8_t> data;
  {
    std::lock_guard lk(state_lock_);  // no pipeline build in between; pipeline_builds_ is the rendering thread's
    if (pipeline_builds_ == saved_builds_) return;
    saved_builds_ = pipeline_builds_;
    data = pipeline_cache_file();
  }
  write_cache_file(cache_path_, data);
  std::fprintf(stderr, "[vulkan] pipeline cache saved (%zu KB)\n", data.size() >> 10);
}


// --- programs and pipelines ----------------------------------------------------------------------------------------
VulkanRenderer::Program* VulkanRenderer::program(guest::Memory& mem, uint32_t gxp) {
  if (!mem.valid(gxp, 0x10)) return nullptr;
  uint32_t size = mem.read<uint32_t>(gxp + 8);
  if (!mem.valid(gxp, size)) return nullptr;
  // A program's bytes are hashed once per frame, not per draw (byte-wise FNV of the whole program on every draw).
  GxpSeen& seen = gxp_seen_[gxp];
  if (seen.frame != frames_.load() + 1) seen = {frames_.load() + 1, gxp_hash(mem, gxp)};
  uint64_t h = seen.hash;
  auto it = programs_.find(h);
  if (it == programs_.end()) {
    uint64_t t0 = ticks();
    Program p;
    p.shader = usse::translate({mem.ptr(gxp), size});
    if (p.shader.ok && p.shader.stage == usse::Stage::Vertex) p.gxp.assign(mem.ptr(gxp), mem.ptr(gxp) + size);
    if (p.shader.ok) {
      VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      sci.codeSize = p.shader.spirv.size() * 4;
      sci.pCode = p.shader.spirv.data();
      if (vkCreateShaderModule(device_, &sci, nullptr, &p.module) != VK_SUCCESS) p.shader.ok = false;
    } else {
      std::fprintf(stderr, "[vulkan] program %016llx not translated: %s\n", (unsigned long long)h,
                   p.shader.error.c_str());
    }
    it = programs_.emplace(h, std::move(p)).first;
    stat_add(kTranslations, 1);
    stat_time(kTranslateTicks, t0);
  }
  return it->second.shader.ok ? &it->second : nullptr;
}

std::string vertex_format_name(uint32_t format, uint32_t comps) {  // SceGxmAttributeFormat x components
  static const char* n[10] = {"U8", "S8", "U16", "S16", "U8N", "S8N", "U16N", "S16N", "F16", "F32"};
  return (format < 10 ? n[format] : std::to_string(format)) + "x" + std::to_string(comps);
}

void VulkanRenderer::init_vertex_formats() {
  static const VkFormat f[10][4] = {
      {VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8A8_USCALED},
      {VK_FORMAT_R8_SSCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R8G8B8A8_SSCALED},
      {VK_FORMAT_R16_USCALED, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16A16_USCALED},
      {VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED},
      {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM},
      {VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM},
      {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM},
      {VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM},
      {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT},
      {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT}};
  // The integer formats a USCALED / SSCALED one is fetched as instead (converted in the shader).
  static const VkFormat ints[4][4] = {
      {VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT},
      {VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT},
      {VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT},
      {VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT}};
  // DEBUG: DCP_VERTEX_FALLBACK=1 treats every format Vulkan does not require as unsupported (the Mali path on a
  // desktop GPU).
  std::set<VkFormat> optional;  // scaled, and 3-component 8 / 16-bit
  if (std::getenv("DCP_VERTEX_FALLBACK"))
    for (uint32_t t = 0; t < 10; t++)
      for (uint32_t c = 0; c < 4; c++) {
        if (t < 4 || (c == 2 && t < 9)) optional.insert(f[t][c]);
        if (t < 4 && c == 2) optional.insert(ints[t][c]);
      }
  auto ok = [&](VkFormat v) {
    if (optional.count(v)) return false;
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(gpu_, v, &fp);
    return (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
  };
  std::string replaced, missing;
  for (uint32_t t = 0; t < 10; t++)
    for (uint32_t c = 0; c < 4; c++) {
      VertexFormat& out = vertex_formats_[t][c];
      out = {f[t][c], 0};
      if (ok(out.format)) continue;
      // Integer instead of scaled, then 4 components instead of 3 (w forced to 1).
      if (t < 4) out = {ints[t][c], uint8_t(t & 1 ? usse::kFetchSint : usse::kFetchUint)};
      if (!ok(out.format) && c == 2) {
        out.format = t < 4 ? ints[t][3] : f[t][3];
        out.fetch |= usse::kFetchWOne;
      }
      std::string name = vertex_format_name(t, c + 1);
      if (ok(out.format)) {
        replaced += " " + name;
      } else {
        out = {};
        missing += " " + name;
      }
    }
  if (!replaced.empty()) std::fprintf(stderr, "[vulkan] vertex formats fetched another way:%s\n", replaced.c_str());
  if (!missing.empty()) std::fprintf(stderr, "[vulkan] vertex formats not supported:%s\n", missing.c_str());
}

// The vertex program translated for substitute vertex formats (cached per Fetch combination).
VkShaderModule VulkanRenderer::vertex_module(Program& p, const std::vector<uint8_t>& fetch) {
  auto it = p.variants.find(fetch);
  if (it != p.variants.end()) return it->second;
  VkShaderModule& m = p.variants[fetch];
  usse::Shader s = usse::translate(p.gxp, fetch);
  if (!s.ok) return m;
  VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  sci.codeSize = s.spirv.size() * 4;
  sci.pCode = s.spirv.data();
  if (vkCreateShaderModule(device_, &sci, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
  return m;
}

VkBlendFactor blend_factor(uint32_t f) {  // SceGxmBlendFactor
  static const VkBlendFactor m[12] = {
      VK_BLEND_FACTOR_ZERO,      VK_BLEND_FACTOR_ONE,       VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
      VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
      VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};  // DST_ALPHA_SATURATE has no equivalent
  return m[f < 12 ? f : 1];
}
VkBlendOp blend_op(uint32_t f) {  // SceGxmBlendFunc (NONE handled by the caller)
  static const VkBlendOp m[6] = {VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT,
                                 VK_BLEND_OP_MIN, VK_BLEND_OP_MAX};
  return m[f < 6 ? f : 1];
}

// The six in-shader (SOP2) blends as fixed-function state (notes/map/usse_gxp.md 6.10).
VkPipelineColorBlendAttachmentState sop2_blend(usse::Blend b) {
  VkPipelineColorBlendAttachmentState s{};
  s.blendEnable = VK_TRUE;
  s.colorWriteMask = 0xf;
  bool dest = b >= usse::Blend::DestAlpha;
  VkBlendFactor a = dest ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_SRC_ALPHA;
  VkBlendFactor inv = dest ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  bool sub = b == usse::Blend::Subtract || b == usse::Blend::DestAlphaSubtract;
  bool plain = b == usse::Blend::Alpha || b == usse::Blend::DestAlpha;
  s.srcColorBlendFactor = s.srcAlphaBlendFactor = a;
  s.dstColorBlendFactor = plain ? inv : VK_BLEND_FACTOR_ONE;
  s.dstAlphaBlendFactor = inv;
  s.colorBlendOp = s.alphaBlendOp = sub ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
  return s;
}

VkPipeline VulkanRenderer::pipeline(const Draw& d, Program& vs, const Program& fs, VkPrimitiveTopology topo) {
  auto key = std::make_tuple(static_cast<const void*>(d.vertex), static_cast<const void*>(d.fragment), uint32_t(topo));
  if (auto it = pipelines_.find(key); it != pipelines_.end()) return it->second;
  VkPipeline& out = pipelines_[key];  // VK_NULL_HANDLE is cached too: a failed combination is not retried

  // Vertex input: each GXM attribute feeds the shader input that reads its register.
  std::vector<VkVertexInputAttributeDescription> attrs;
  std::vector<VkVertexInputBindingDescription> binds;
  std::set<uint32_t> fed;
  std::vector<uint8_t> fetch;  // Fetch bits by input location; empty = every input as declared
  for (const VertexAttribute& a : d.vertex->attributes) {
    const usse::VertexInput* in = nullptr;
    for (const auto& i : vs.shader.inputs)
      if (i.pa_reg == a.reg) in = &i;
    if (!in) continue;  // the program does not read it
    VertexFormat f;
    if (a.format < 10 && a.components >= 1 && a.components <= 4) f = vertex_formats_[a.format][a.components - 1];
    if (f.format == VK_FORMAT_UNDEFINED || a.stream >= d.vertex->streams.size()) {
      warn_once("vertex attribute format " + vertex_format_name(a.format, a.components));
      return out;
    }
    if (f.fetch) {
      warn_once("vertex format " + vertex_format_name(a.format, a.components) + " fetched another way");
      if (fetch.size() <= in->location) fetch.resize(in->location + 1);
      fetch[in->location] = f.fetch;
    }
    attrs.push_back({in->location, a.stream, f.format, a.offset});
    fed.insert(in->location);
    bool have = false;
    for (auto& b : binds) have |= b.binding == a.stream;
    if (!have) {
      const VertexStreamLayout& st = d.vertex->streams[a.stream];
      if (st.index_source & 2) warn_once("instanced vertex stream (drawn as per-vertex)");
      binds.push_back({a.stream, st.stride, VK_VERTEX_INPUT_RATE_VERTEX});
    }
  }
  for (const auto& i : vs.shader.inputs)
    if (!fed.count(i.location)) {
      warn_once("vertex program input without a GXM attribute");
      return out;
    }
  if (fs.shader.varyings & ~vs.shader.varyings) warn_once("fragment program reads varyings the vertex program lacks");
  VkShaderModule vmod = fetch.empty() ? vs.module : vertex_module(vs, fetch);
  if (!vmod) {
    warn_once("vertex program not translated for substitute vertex formats");
    return out;
  }

  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                               {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vmod;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs.module;
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = uint32_t(binds.size());
  vi.pVertexBindingDescriptions = binds.data();
  vi.vertexAttributeDescriptionCount = uint32_t(attrs.size());
  vi.pVertexAttributeDescriptions = attrs.data();
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = topo;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cb{};
  cb.colorWriteMask = 0xf;
  if (fs.shader.blend != usse::Blend::None) {
    cb = sop2_blend(fs.shader.blend);
    if (d.fragment->has_blend) warn_once("program blends in the shader and in the blend unit");
  } else if (d.fragment->has_blend) {
    uint32_t w = d.fragment->blend, mask = w & 0xff;
    cb.colorWriteMask = (mask & 2 ? VK_COLOR_COMPONENT_R_BIT : 0) | (mask & 4 ? VK_COLOR_COMPONENT_G_BIT : 0) |
                        (mask & 8 ? VK_COLOR_COMPONENT_B_BIT : 0) | (mask & 1 ? VK_COLOR_COMPONENT_A_BIT : 0);
    uint32_t cfunc = (w >> 8) & 15, afunc = (w >> 12) & 15;
    if (cfunc || afunc) {
      cb.blendEnable = VK_TRUE;
      cb.colorBlendOp = blend_op(cfunc);
      cb.alphaBlendOp = blend_op(afunc);
      cb.srcColorBlendFactor = cfunc ? blend_factor((w >> 16) & 15) : VK_BLEND_FACTOR_ONE;
      cb.dstColorBlendFactor = cfunc ? blend_factor((w >> 20) & 15) : VK_BLEND_FACTOR_ZERO;
      cb.srcAlphaBlendFactor = afunc ? blend_factor((w >> 24) & 15) : VK_BLEND_FACTOR_ONE;
      cb.dstAlphaBlendFactor = afunc ? blend_factor((w >> 28) & 15) : VK_BLEND_FACTOR_ZERO;
    }
  }
  VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cbs.attachmentCount = 1;
  cbs.pAttachments = &cb;
  VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  ds.dynamicStateCount = 2;
  ds.pDynamicStates = dyn;
  VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gpi.stageCount = 2;
  gpi.pStages = stages;
  gpi.pVertexInputState = &vi;
  gpi.pInputAssemblyState = &ia;
  gpi.pViewportState = &vp;
  gpi.pRasterizationState = &rs;
  gpi.pMultisampleState = &ms;
  gpi.pColorBlendState = &cbs;
  gpi.pDynamicState = &ds;
  gpi.layout = layout_;
  gpi.renderPass = color_pass_;
  uint64_t t0 = now_us(), k0 = ticks();
  VkResult pr = vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &gpi, nullptr, &out);
  pipeline_builds_++;
  last_build_frame_ = frames_.load();
  stat_add(kPipelineBuilds, 1);
  stat_time(kPipelineBuildTicks, k0);
  if (gpu_log)
    std::fprintf(stderr, "[gpu] frame %llu pipeline %zu built in %.1f ms\n", (unsigned long long)frames_.load(),
                 pipelines_.size(), (now_us() - t0) / 1000.0);
  if (pr != VK_SUCCESS) {
    out = VK_NULL_HANDLE;
    warn_once("pipeline creation failed");
  }
  return out;
}

// --- textures --------------------------------------------------------------------------------------------------------
void VulkanRenderer::destroy_texture(Texture& t) {
  if (t.view) vkDestroyImageView(device_, t.view, nullptr);
  if (t.image) vkDestroyImage(device_, t.image, nullptr);
  if (t.memory.memory) free_memory(t.memory);
  texture_bytes_ -= std::min(texture_bytes_, t.bytes);
  t = Texture{};
}

// Device-local memory for an image: first fit in a 64 MB block of the right type (new block when none fits); big
// requests get their own allocation.
bool VulkanRenderer::alloc_memory(const VkMemoryRequirements& req, MemAlloc& out) {
  int32_t type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (type < 0) return false;
  if (req.size > kMemBlock / 4) {
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = uint32_t(type);
    if (vkAllocateMemory(device_, &mai, nullptr, &out.memory) != VK_SUCCESS) return false;
    out.offset = 0, out.size = req.size, out.block = -1;
    return true;
  }
  VkDeviceSize align = std::max<VkDeviceSize>(req.alignment, 1);
  for (int pass = 0; pass < 2; pass++) {
    for (size_t i = 0; i < mem_blocks_.size(); i++) {
      MemBlock& b = mem_blocks_[i];
      if (!b.memory || b.type != uint32_t(type)) continue;
      for (auto it = b.free.begin(); it != b.free.end(); ++it) {
        VkDeviceSize start = it->first, end = it->first + it->second;
        VkDeviceSize a = (start + align - 1) / align * align;
        if (a + req.size > end) continue;
        b.free.erase(it);
        if (a > start) b.free[start] = a - start;
        if (a + req.size < end) b.free[a + req.size] = end - (a + req.size);
        out.memory = b.memory, out.offset = a, out.size = req.size, out.block = int(i);
        b.used += req.size;
        return true;
      }
    }
    if (pass) break;
    MemBlock nb;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = kMemBlock;
    mai.memoryTypeIndex = uint32_t(type);
    if (vkAllocateMemory(device_, &mai, nullptr, &nb.memory) != VK_SUCCESS) return false;
    nb.type = uint32_t(type);
    nb.free[0] = kMemBlock;
    auto slot = std::find_if(mem_blocks_.begin(), mem_blocks_.end(), [](const MemBlock& m) { return !m.memory; });
    if (slot != mem_blocks_.end()) *slot = std::move(nb);
    else mem_blocks_.push_back(std::move(nb));
  }
  return false;
}

void VulkanRenderer::free_memory(MemAlloc& a) {
  if (a.block < 0) {
    vkFreeMemory(device_, a.memory, nullptr);
  } else {
    MemBlock& b = mem_blocks_[size_t(a.block)];
    VkDeviceSize start = a.offset, size = a.size;
    auto next = b.free.lower_bound(start);
    if (next != b.free.end() && next->first == start + size) size += next->second, next = b.free.erase(next);
    if (next != b.free.begin()) {
      auto prev = std::prev(next);
      if (prev->first + prev->second == start) start = prev->first, size += prev->second, b.free.erase(prev);
    }
    b.free[start] = size;
    b.used -= std::min(b.used, a.size);
    // An empty block goes back to the driver unless it is the only one of its type.
    if (!b.used) {
      size_t same = 0;
      for (const MemBlock& o : mem_blocks_) same += o.memory && o.type == b.type;
      if (same > 1) vkFreeMemory(device_, b.memory, nullptr), b = MemBlock{};
    }
  }
  a = MemAlloc{};
}

// Staging space for an upload: the slot's staging ring (reused after the slot's fence), else a buffer of its own that
// is destroyed after that fence.
bool VulkanRenderer::staging(VkDeviceSize n, VkDeviceSize align, Staging& s) {
  VkDeviceSize o = (up_ring_used_ + align - 1) / align * align;
  if (recording_ && o + n <= up_ring_size_) {
    up_ring_used_ = o + n;
    s = {up_ring_, o, up_ring_ptr_ + o};
    return true;
  }
  TempBuffer tb;
  void* map = nullptr;
  if (!create_buffer(std::max<VkDeviceSize>(n, 16), VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     tb.buf, tb.mem, &map))
    return false;
  cur_slot().retired_buffers.push_back(tb);
  s = {tb.buf, 0, static_cast<uint8_t*>(map)};
  return true;
}

// Upload commands: while a frame records, into upload_cmd_ (flush submits it ahead of the frame's command buffer in
// the same vkQueueSubmit, so its barriers order it before the draws; no CPU wait). Otherwise (start-up) a one-off
// command buffer that end_upload submits and waits for.
VkCommandBuffer VulkanRenderer::begin_upload() {
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (recording_) {
    if (!upload_open_) {
      vkResetCommandBuffer(upload_cmd_, 0);
      vkBeginCommandBuffer(upload_cmd_, &bi);
      upload_open_ = true;
    }
    return upload_cmd_;
  }
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool_;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer cb;
  vkAllocateCommandBuffers(device_, &cai, &cb);
  vkBeginCommandBuffer(cb, &bi);
  return cb;
}

void VulkanRenderer::end_upload(VkCommandBuffer cb) {
  if (cb == upload_cmd_) return;
  vkEndCommandBuffer(cb);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  {
    std::lock_guard ql(queue_lock_);
    vkQueueSubmit(queue_, 1, &si, upload_fence_);
  }
  vkWaitForFences(device_, 1, &upload_fence_, VK_TRUE, UINT64_MAX);
  vkResetFences(device_, 1, &upload_fence_);
  vkFreeCommandBuffers(device_, pool_, 1, &cb);
}

// GPU BC decode (devices without textureCompressionBC): the compute pipeline of compute_shaders.h, one descriptor
// set per mip level from a pool reset with each frame. Off when the device lacks what it needs (or DCP_CPU_BC=1):
// BC textures are then decompressed on the CPU as before.
bool VulkanRenderer::init_gpu_bc() {
  if (std::getenv("DCP_CPU_BC")) return false;
  uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(gpu_, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> q(qn);
  vkGetPhysicalDeviceQueueFamilyProperties(gpu_, &qn, q.data());
  if (family_ >= qn || !(q[family_].queueFlags & VK_QUEUE_COMPUTE_BIT)) return false;
  VkFormatProperties fp{};
  vkGetPhysicalDeviceFormatProperties(gpu_, VK_FORMAT_R8G8B8A8_UNORM, &fp);
  if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return false;
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(gpu_, &props);
  ssbo_align_ = std::max<VkDeviceSize>(16, props.limits.minStorageBufferOffsetAlignment);

  VkDescriptorSetLayoutBinding binds[2] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dli.bindingCount = 2;
  dli.pBindings = binds;
  if (vkCreateDescriptorSetLayout(device_, &dli, nullptr, &bc_set_layout_) != VK_SUCCESS) return false;
  VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BcDecodePush)};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &bc_set_layout_;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pcr;
  if (vkCreatePipelineLayout(device_, &pli, nullptr, &bc_layout_) != VK_SUCCESS) return false;
  std::vector<uint32_t> code = bc_decode_spirv();
  VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  sm.codeSize = code.size() * 4;
  sm.pCode = code.data();
  VkShaderModule mod = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device_, &sm, nullptr, &mod) != VK_SUCCESS) return false;
  VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cpi.stage.module = mod;
  cpi.stage.pName = "main";
  cpi.layout = bc_layout_;
  VkResult r = vkCreateComputePipelines(device_, pipeline_cache_, 1, &cpi, nullptr, &bc_pipe_);
  vkDestroyShaderModule(device_, mod, nullptr);
  if (r != VK_SUCCESS) return bc_pipe_ = VK_NULL_HANDLE, false;
  VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kBcSets}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kBcSets}};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = kBcSets;
  dpi.poolSizeCount = 2;
  dpi.pPoolSizes = sizes;
  for (uint32_t i = 0; i < frames_in_flight_; i++)
    if (vkCreateDescriptorPool(device_, &dpi, nullptr, &slots_[i].bc_pool) != VK_SUCCESS) return false;
  return true;
}

// A BC texture decoded to RGBA8 by the compute shader from its stored (swizzled) blocks; false = not possible here
// (the caller decompresses on the CPU instead).
bool VulkanRenderer::upload_bc_gpu(const tex::Desc& d, const uint8_t* src, Texture& t) {
  uint32_t mode = d.format == tex::HostFormat::BC1 ? 1 : d.format == tex::HostFormat::BC2 ? 2 : 3;
  uint32_t mips = std::min(d.mips, 16u);
  VkDescriptorSet sets[16];
  VkDescriptorSetLayout layouts[16];
  for (uint32_t m = 0; m < mips; m++) layouts[m] = bc_set_layout_;
  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = bc_pool_;
  dai.descriptorSetCount = mips;
  dai.pSetLayouts = layouts;
  if (vkAllocateDescriptorSets(device_, &dai, sets) != VK_SUCCESS) return false;  // pool used up this frame
  struct Timed {
    uint64_t t0 = ticks();
    ~Timed() { stat_time(kTextureUploadTicks, t0); }
  } timed;
  stat_add(kTextureUploads, 1);
  stat_add(kTextureUploadBytes, d.source_bytes);
  if (!create_texture_image(t, VK_FORMAT_R8G8B8A8_UNORM, d.width, d.height, mips,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, d.swizzle))
    return false;
  VkDeviceSize bytes = (VkDeviceSize(d.source_bytes) + 3) & ~VkDeviceSize(3);
  Staging s;
  if (!staging(bytes, ssbo_align_, s)) return destroy_texture(t), false;
  std::memcpy(s.ptr, src, d.source_bytes);

  VkCommandBuffer cb = begin_upload();
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = t.image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &b);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, bc_pipe_);
  auto log2u = [](uint32_t v) { uint32_t r = 0; while ((1u << r) < v) r++; return r; };
  uint32_t pw = 1u << log2u(d.width), ph = 1u << log2u(d.height), block = mode == 1 ? 8 : 16;
  VkDeviceSize off = 0;
  for (uint32_t m = 0; m < mips; m++) {
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    vkCreateImageView(device_, &vci, nullptr, &view);
    cur_slot().retired_views.push_back(view);  // destroyed after the slot's fence
    VkDescriptorBufferInfo bi{s.buf, s.off, bytes};
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    wr[0].dstSet = wr[1].dstSet = sets[m];
    wr[0].dstBinding = 0;
    wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr[0].pBufferInfo = &bi;
    wr[1].dstBinding = 1;
    wr[1].descriptorCount = 1;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wr[1].pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 2, wr, 0, nullptr);
    uint32_t lw = std::max(1u, d.width >> m), lh = std::max(1u, d.height >> m);
    uint32_t mw = std::max(1u, (pw >> m) / 4), mh = std::max(1u, (ph >> m) / 4);
    BcDecodePush pc{uint32_t(off / 4), lw, lh, mw, std::min(log2u(mw), log2u(mh)), mw > mh ? 1u : 0u, mode, 1};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, bc_layout_, 0, 1, &sets[m], 0, nullptr);
    vkCmdPushConstants(cb, bc_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (lw + 7) / 8, (lh + 7) / 8, 1);
    off += VkDeviceSize(mw) * mh * block;
  }
  b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &b);
  end_upload(cb);
  return true;
}

void VulkanRenderer::verify_bc() {
  static int ok = 0, bad = 0;
  for (BcCheck& c : bc_checks_) {
    TempBuffer tb;
    void* map = nullptr;
    VkDeviceSize n = VkDeviceSize(c.w) * c.h * 4;
    if (!create_buffer(n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, tb.buf, tb.mem, &map)) continue;
    VkCommandBuffer cb = begin_upload();
    barrier(vkCmdPipelineBarrier, cb, c.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {c.w, c.h, 1};
    vkCmdCopyImageToBuffer(cb, c.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tb.buf, 1, &copy);
    barrier(vkCmdPipelineBarrier, cb, c.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
    end_upload(cb);
    const uint8_t* got = static_cast<const uint8_t*>(map);
    size_t diff = 0, first = 0;
    for (size_t i = 0; i < size_t(n); i += 4)
      if (std::memcmp(got + i, c.want.data() + i, 4) && !diff++) first = i / 4;
    diff ? bad++ : ok++;
    if (diff)
      std::fprintf(stderr, "[vulkan] BC verify %ux%u: %zu texels differ, first (%zu,%zu) gpu %02x%02x%02x%02x cpu %02x%02x%02x%02x\n",
                   c.w, c.h, diff, first % c.w, first / c.w, got[first * 4], got[first * 4 + 1], got[first * 4 + 2],
                   got[first * 4 + 3], c.want[first * 4], c.want[first * 4 + 1], c.want[first * 4 + 2],
                   c.want[first * 4 + 3]);
    vkDestroyBuffer(device_, tb.buf, nullptr);
    vkFreeMemory(device_, tb.mem, nullptr);
  }
  std::fprintf(stderr, "[vulkan] BC verify: %d textures identical, %d differ\n", ok, bad);
  bc_checks_.clear();
}

// Image, memory and the sampled view (with the texture's component mapping).
bool VulkanRenderer::create_texture_image(Texture& t, VkFormat format, uint32_t w, uint32_t h, uint32_t mips,
                                          VkImageUsageFlags usage, const tex::Swizzle& swz) {
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = format;
  ici.extent = {w, h, 1};
  ici.mipLevels = mips;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = usage;
  if (vkCreateImage(device_, &ici, nullptr, &t.image) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device_, t.image, &req);
  if (!alloc_memory(req, t.memory)) return destroy_texture(t), false;
  t.bytes = req.size;
  texture_bytes_ += req.size;
  vkBindImageMemory(device_, t.image, t.memory.memory, t.memory.offset);
  static const VkComponentSwizzle comp[] = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
                                            VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE};
  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = t.image;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = format;
  vci.components = {comp[int(swz.r)], comp[int(swz.g)], comp[int(swz.b)], comp[int(swz.a)]};
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
  if (vkCreateImageView(device_, &vci, nullptr, &t.view) != VK_SUCCESS) return destroy_texture(t), false;
  return true;
}

bool VulkanRenderer::upload_texture(const tex::Decoded& dec, Texture& t) {
  struct Timed {
    uint64_t t0 = ticks();
    ~Timed() { stat_time(kTextureUploadTicks, t0); }
  } timed;
  stat_add(kTextureUploads, 1);
  stat_add(kTextureUploadBytes, dec.data.size());
  static const VkFormat formats[] = {VK_FORMAT_R8G8B8A8_UNORM,     VK_FORMAT_R8_UNORM,           VK_FORMAT_R8G8_UNORM,
                                     VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK,  VK_FORMAT_BC3_UNORM_BLOCK,
                                     VK_FORMAT_R16G16B16A16_SFLOAT};
  const tex::Desc& d = dec.desc;
  uint32_t mips = uint32_t(dec.levels.size());
  if (!create_texture_image(t, formats[int(d.format)], d.width, d.height, mips,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, d.swizzle))
    return false;
  Staging s;
  if (!staging(dec.data.size(), 16, s)) return destroy_texture(t), false;  // 16: any texel block size
  std::memcpy(s.ptr, dec.data.data(), dec.data.size());
  VkCommandBuffer cb = begin_upload();
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = t.image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                       1, &b);
  VkBufferImageCopy copies[16];
  for (uint32_t i = 0; i < mips && i < 16; i++) {
    VkBufferImageCopy& c = copies[i];
    c = {};
    c.bufferOffset = s.off + dec.levels[i].offset;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
    c.imageExtent = {std::max(1u, d.width >> i), std::max(1u, d.height >> i), 1};
  }
  vkCmdCopyBufferToImage(cb, s.buf, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, std::min(mips, 16u), copies);
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &b);
  end_upload(cb);
  return true;
}

VulkanRenderer::Texture* VulkanRenderer::texture(guest::Memory& mem, const std::array<uint32_t, 4>& words) {
  if (auto tg = targets_.find(words[2]); tg != targets_.end()) return target_texture(words, tg->second);
  auto it = textures_.find(words);
  uint64_t frame = frames_.load();
  if (it != textures_.end() && it->second.checked == frame) return it->second.used = frame, &it->second;
  tex::Words w = {words[0], words[1], words[2], words[3]};
  tex::ReadFn read = [&](uint32_t va, uint32_t n) -> const uint8_t* { return mem.valid(va, n) ? mem.ptr(va) : nullptr; };
  uint64_t th = ticks();
  uint64_t volatile_until = 0;  // carried over when the texture changed in place
  if (it != textures_.end()) {
    // Change detection (see the Texture comment): sampled textures get a sample check, and a full check every
    // kFullCheckFrames frames or when the samples differ; the rest a full check every frame.
    Texture& t = it->second;
    bool full = !t.sampled || t.volatile_until > frame || frame - t.full_at >= kFullCheckFrames;
    if (!full) {
      uint64_t sk = tex::sample_key(w, read);
      stat_add(kTextureSamples, 1);
      if (sk && sk == t.skey) {
        stat_time(kTextureHashTicks, th);
        t.checked = t.used = frame;
        return &t;
      }
    }
    uint64_t key = tex::cache_key(w, read);
    stat_add(kTextureHashes, 1);
    stat_add(kTextureHashBytes, t.source_bytes);
    if (key && key == t.key) {
      if (t.sampled) t.skey = tex::sample_key(w, read);
      stat_time(kTextureHashTicks, th);
      t.checked = t.used = t.full_at = frame;
      return &t;
    }
    // Rewritten in place: from now on checked in full every frame for a while (fonts, streamed textures).
    volatile_until = frame + kVolatileFrames;
    if (gpu_log)
      std::fprintf(stderr, "[gpu] frame %llu texture %08x changed in place (%s, last full check frame %llu)\n",
                   (unsigned long long)frame, words[2], t.sampled ? "sampled" : "full", (unsigned long long)t.full_at);
    cur_slot().retired.push_back(t);  // still referenced by commands recorded this frame (and maybe the previous)
    textures_.erase(it);
  }
  uint64_t key = tex::cache_key(w, read);
  tex::Desc desc = tex::describe(w);
  stat_add(kTextureHashes, 1);
  stat_add(kTextureHashBytes, desc.source_bytes);
  stat_time(kTextureHashTicks, th);
  if (!key) {
    tex::Desc d = tex::describe(w);
    warn_once(d.ok ? "texture data not in guest memory" : "texture: " + d.reason);
    if (gpu_log)
      std::fprintf(stderr, "[gpu] frame %llu texture %08x %08x %08x %08x unusable: %s (data 0x%08x, %u bytes)\n",
                   (unsigned long long)frame, words[0], words[1], words[2], words[3],
                   d.ok ? "not in guest memory" : d.reason.c_str(), d.data, d.source_bytes);
    return nullptr;
  }
  uint64_t td = ticks();
  bool bc_desc = desc.format == tex::HostFormat::BC1 || desc.format == tex::HostFormat::BC2 ||
                 desc.format == tex::HostFormat::BC3;
  if (bc_desc && gpu_bc_ && desc.type != tex::Type::Linear && desc.type != tex::Type::LinearStrided) {
    // Compressed blocks straight to the GPU, decoded there (no CPU decode, a quarter of the bytes).
    Texture t;
    if (const uint8_t* src = read(desc.data, desc.source_bytes); src && upload_bc_gpu(desc, src, t)) {
      stat_add(kTextureDecodes, 1);
      stat_time(kTextureDecodeTicks, td);
      t.key = key;
      t.checked = t.used = frame;
      t.source_bytes = desc.source_bytes;
      t.sampled = desc.source_bytes > kSampledMinBytes && desc.type == tex::Type::Swizzled;
      if (t.sampled) t.skey = tex::sample_key(w, read);
      t.full_at = frame - (key % kFullCheckFrames);
      t.volatile_until = volatile_until;
      static const bool verify = std::getenv("DCP_BC_VERIFY") != nullptr;
      if (verify) {
        tex::Decoded ref;
        if (tex::decode(w, read, ref)) {
          tex::decompress_bc(ref);
          size_t n = size_t(desc.width) * desc.height * 4;
          bc_checks_.push_back({t.image, desc.width, desc.height, std::vector<uint8_t>(ref.data.begin(), ref.data.begin() + n)});
        }
      }
      return &(textures_[words] = t);
    }
  }
  tex::Decoded dec;
  if (!tex::decode(w, read, dec)) {
    warn_once("texture: " + dec.desc.reason);
    return nullptr;
  }
  bool bc = dec.desc.format == tex::HostFormat::BC1 || dec.desc.format == tex::HostFormat::BC2 ||
            dec.desc.format == tex::HostFormat::BC3;
  if (bc && !bc_) tex::decompress_bc(dec);  // no BC sampling on this device (most Android GPUs)
  stat_add(kTextureDecodes, 1);
  stat_time(kTextureDecodeTicks, td);
  Texture t;
  uint64_t t0 = now_us();
  if (gpu_log)
    std::fprintf(stderr, "[gpu] frame %llu texture %ux%u fmt %d (%zu bytes)", (unsigned long long)frame, dec.desc.width,
                 dec.desc.height, int(dec.desc.format), dec.data.size());
  if (!upload_texture(dec, t)) {
    warn_once("texture upload failed");
    return nullptr;
  }
  if (gpu_log) std::fprintf(stderr, " uploaded in %.1f ms\n", (now_us() - t0) / 1000.0);
  t.key = key;
  t.checked = t.used = frame;
  t.source_bytes = desc.source_bytes;
  // Block-compressed swizzled textures are file data (the game's art); CPU-written ones are linear / uncompressed.
  t.sampled = desc.source_bytes > kSampledMinBytes && desc.type == tex::Type::Swizzled &&
              (desc.format == tex::HostFormat::BC1 || desc.format == tex::HostFormat::BC2 ||
               desc.format == tex::HostFormat::BC3);
  if (t.sampled) t.skey = tex::sample_key(w, read);
  // Staggered so textures loaded together don't all get their full check in the same frame.
  t.full_at = frame - (key % kFullCheckFrames);
  t.volatile_until = volatile_until;
  return &(textures_[words] = t);
}

VkSampler VulkanRenderer::sampler(uint32_t word0) {
  uint32_t key = word0 & 0x3ff8;  // address modes 3-8, mip filter 9, min 10-11, mag 12-13
  if (auto it = samplers_.find(key); it != samplers_.end()) return it->second;
  auto filter = [](uint32_t f) { return f == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR; };
  auto addr = [](uint32_t a) {
    switch (a) {
      case 0: case 4: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
      case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
      case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
      default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
  };
  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = filter((word0 >> 12) & 3);
  sci.minFilter = filter((word0 >> 10) & 3);
  sci.mipmapMode = (word0 >> 9) & 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sci.addressModeU = addr((word0 >> 6) & 7);
  sci.addressModeV = addr((word0 >> 3) & 7);
  sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.maxLod = (word0 >> 9) & 1 ? VK_LOD_CLAMP_NONE : 0.0f;
  sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  VkSampler s = VK_NULL_HANDLE;
  vkCreateSampler(device_, &sci, nullptr, &s);
  return samplers_[key] = s;
}

// --- draws -----------------------------------------------------------------------------------------------------------
void VulkanRenderer::draw(guest::Memory& mem, const Draw& d) {
  struct Timed {  // draw-path sub-timers (draw_stats.h)
    uint64_t t0 = ticks();
    ~Timed() { stat_time(kDrawTicks, t0); }
  } timed;
  std::lock_guard lk(state_lock_);
  stat_add(kDraws, 1);
  draws_++;
  if (!in_pass_ || !d.vertex || !d.fragment || !d.index_count) {
    if (gpu_log && !in_pass_) std::fprintf(stderr, "[gpu] frame %llu draw outside a render pass dropped\n",
                                           (unsigned long long)frames_.load());
    return;
  }
  // Stencil func NEVER: the draw writes stencil only (its fragments all fail, colour is never written). The game's
  // only such draws are its stencil scissor quads (1.09 f813bc350: NEVER/REPLACE full-screen + window rectangle,
  // then EQUAL for what follows; notes/map/vita_dark_stages_109.md). Without stencil emulation they painted the
  // clear program's black over the whole target: the black layers in the Ghost Ship Cove caves / lava stages.
  if (d.state.front_stencil.func == kStencilNever) return;
  uint64_t t = ticks();
  VkPrimitiveTopology topo;
  switch (d.primitive) {
    case 0x00000000: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case 0x04000000: topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case 0x0C000000: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case 0x10000000: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    case 0x14000000: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;  // triangle edges: drawn filled for now
    default: return warn_once("primitive type " + std::to_string(d.primitive));
  }
  // Consecutive draws mostly share programs: the last lookup holds for the rest of the frame (programs are
  // re-identified by hash once per frame, as before).
  uint64_t frame = frames_.load();
  if (memo_.frame != frame || memo_.vertex != d.vertex || memo_.fragment != d.fragment || memo_.topo != topo) {
    Program* vs = program(mem, d.vertex->gxp);
    Program* fs = program(mem, d.fragment->gxp);
    t = stat_time(kProgramTicks, t);
    VkPipeline pipe = vs && fs ? pipeline(d, *vs, *fs, topo) : VK_NULL_HANDLE;
    t = stat_time(kPipelineTicks, t);
    memo_ = {frame, d.vertex, d.fragment, topo, vs, fs, pipe};
  }
  Program* vs = memo_.vs;
  Program* fs = memo_.fs;
  VkPipeline pipe = memo_.pipe;
  if (!vs || !fs || !pipe) return;

  // Indices, and from them how many vertices each stream needs.
  bool u32 = d.index_format == 0x01000000;
  uint32_t isize = u32 ? 4 : 2;
  if (!mem.valid(d.indices, d.index_count * isize)) return warn_once("index buffer not in guest memory");
  const uint8_t* ip = mem.ptr(d.indices);
  uint32_t max_index = 0;
  if (u32) {
    const uint8_t* end = ip + 4 * size_t(d.index_count);
    for (const uint8_t* p = ip; p < end; p += 4) {
      uint32_t v;
      std::memcpy(&v, p, 4);
      max_index = std::max(max_index, v);
    }
  } else {
    uint16_t m = 0;
    for (uint32_t i = 0; i < d.index_count; i++) {
      uint16_t v;
      std::memcpy(&v, ip + 2 * i, 2);
      m = std::max(m, v);
    }
    max_index = m;
  }
  VkDeviceSize copied = VkDeviceSize(d.index_count) * isize;
  VkDeviceSize ioff = ring_alloc(ip, copied, 4);
  if (ioff == VK_WHOLE_SIZE) return warn_once("upload ring full");

  uint32_t stream_mask = 0;  // streams the attributes read (at most 16)
  for (const VertexAttribute& a : d.vertex->attributes) stream_mask |= 1u << (a.stream & 15);
  VkDeviceSize voff[16];
  for (uint32_t s = 0; s < 16; s++) {
    if (!(stream_mask >> s & 1)) continue;
    uint32_t stride = s < d.vertex->streams.size() ? d.vertex->streams[s].stride : 0;
    uint32_t end = 0;  // bytes of the last vertex an attribute of this stream reaches
    for (const VertexAttribute& a : d.vertex->attributes)
      if (a.stream == s) end = std::max(end, uint32_t(a.offset) + 4u * a.components);
    VkDeviceSize bytes = VkDeviceSize(stride) * max_index + end;
    uint32_t src = d.streams[s];
    // A trailing attribute may be narrower than 4 bytes per component: clamp to what is mapped.
    while (bytes && !mem.valid(src, uint32_t(bytes))) bytes = bytes > 16 ? bytes - 16 : 0;
    if (!bytes) return warn_once("vertex stream not in guest memory");
    voff[s] = ring_alloc(mem.ptr(src), bytes, 16);
    if (voff[s] == VK_WHOLE_SIZE) return warn_once("upload ring full");
    copied += bytes;
  }

  // Default uniform buffers: the program's words, zero padded to the block size, bound as dynamic offsets. A block
  // equal to the previous draw's (same stage) reuses its copy.
  constexpr uint32_t block = usse::kUniformVec4s * 16;
  const uint32_t ubuf[2] = {d.vertex_uniforms, d.fragment_uniforms};
  const uint32_t words[2] = {vs->shader.uniform_words, fs->shader.uniform_words};
  uint32_t uoff[2];
  for (int k = 0; k < 2; k++) {
    alignas(16) uint8_t data[block] = {};
    if (words[k] && ubuf[k] && mem.valid(ubuf[k], words[k] * 4)) std::memcpy(data, mem.ptr(ubuf[k]), words[k] * 4);
    // Compared with our own copy: the ring is mapped write-combined on some drivers (reads crawl).
    if (last_ub_[k] != VK_WHOLE_SIZE && !std::memcmp(last_ub_data_[k], data, block)) {
      uoff[k] = uint32_t(last_ub_[k]);
      continue;
    }
    VkDeviceSize o = ring_alloc(data, block, ubo_align_);
    if (o == VK_WHOLE_SIZE) return warn_once("upload ring full");
    last_ub_[k] = o;
    std::memcpy(last_ub_data_[k], data, block);
    uoff[k] = uint32_t(o);
    copied += block;
  }
  stat_add(kCopyBytes, copied);
  t = stat_time(kCopyTicks, t);

  Texture* tex = &dummy_;
  uint32_t word0 = 0;
  if (fs->shader.texture_units & 1) {
    if (Texture* tx = texture(mem, d.textures[0])) tex = tx, word0 = d.textures[0][0];
  }
  t = stat_time(kTextureTicks, t);
  // One descriptor set per (texture view, sampler state) and frame: the uniform blocks are dynamic offsets.
  uint32_t skey = word0 & 0x3ff8;
  VkDescriptorSet set;
  if (tex->view == last_set_view_ && skey == last_set_sampler_) {
    set = last_set_;
  } else {
    auto [it, fresh] = sets_.try_emplace({tex->view, skey}, VK_NULL_HANDLE);
    if (fresh) {
      VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      dai.descriptorPool = dpool_;
      dai.descriptorSetCount = 1;
      dai.pSetLayouts = &set_layout_;
      if (vkAllocateDescriptorSets(device_, &dai, &it->second) != VK_SUCCESS) {
        sets_.erase(it);
        return warn_once("descriptor pool exhausted");
      }
      VkDescriptorBufferInfo ub[2] = {{ring_, 0, block}, {ring_, 0, block}};
      VkDescriptorImageInfo ii{sampler(word0), tex->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet wr[3] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
      for (int k = 0; k < 3; k++) {
        wr[k].dstSet = it->second;
        wr[k].dstBinding = k < 2 ? uint32_t(k) : usse::kTextureBinding;
        wr[k].descriptorCount = 1;
        wr[k].descriptorType = k < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        if (k < 2) wr[k].pBufferInfo = &ub[k];
        else wr[k].pImageInfo = &ii;
      }
      vkUpdateDescriptorSets(device_, 3, wr, 0, nullptr);
      stat_add(kDescriptorAllocs, 1);
    }
    set = it->second;
    last_set_ = set, last_set_view_ = tex->view, last_set_sampler_ = skey;
  }
  t = stat_time(kDescriptorTicks, t);

  // GXM viewport -> Vulkan clip space (see the translator): window = offset + scale * ndc, or ndc = pixels when the
  // viewport is disabled; Vulkan ndc = 2 * window / size - 1.
  float W = float(scene_w_), H = float(scene_h_), pc[8];
  const float* v = d.state.viewport;  // x offset, x scale, y offset, y scale, z offset, z scale
  if (d.state.viewport_enabled) {
    float s[8] = {2 * v[1] / W, 2 * v[3] / H, v[5], 1, 2 * v[0] / W - 1, 2 * v[2] / H - 1, v[4], 0};
    std::memcpy(pc, s, sizeof pc);
  } else {
    float s[8] = {2 / W, 2 / H, 1, 1, -1, -1, 0, 0};
    std::memcpy(pc, s, sizeof pc);
  }
  // Recorded after texture(): a render target sampled as a texture ends and resumes the render pass to copy it.
  // State already bound in this render pass (bound_, reset by begin_pass) is not bound again.
  if (bound_.pipe != pipe) vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe), bound_.pipe = pipe;
  for (uint32_t s = 0; s < 16;) {  // contiguous streams in one call
    if (!(stream_mask >> s & 1)) { s++; continue; }
    uint32_t e = s;
    while (e + 1 < 16 && (stream_mask >> (e + 1) & 1)) e++;
    VkBuffer bufs[16];
    for (uint32_t i = s; i <= e; i++) bufs[i - s] = ring_;
    vkCmdBindVertexBuffers(cmd_, s, e - s + 1, bufs, &voff[s]);
    s = e + 1;
  }
  int itype = u32 ? 1 : 0;
  if (bound_.index_type != itype)
    vkCmdBindIndexBuffer(cmd_, ring_, 0, u32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16), bound_.index_type = itype;
  if (bound_.set != set || bound_.uoff[0] != uoff[0] || bound_.uoff[1] != uoff[1]) {
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 2, uoff);
    bound_.set = set, bound_.uoff[0] = uoff[0], bound_.uoff[1] = uoff[1];
  }
  if (!bound_.pc_valid || std::memcmp(bound_.pc, pc, sizeof pc)) {
    vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
    std::memcpy(bound_.pc, pc, sizeof pc);
    bound_.pc_valid = true;
  }
  vkCmdDrawIndexed(cmd_, d.index_count, 1, uint32_t(ioff / isize), 0, 0);
  stat_time(kCmdTicks, t);
  if (cur_) cur_->version++;
}

VulkanRenderer::Target* VulkanRenderer::target(uint32_t address, uint32_t w, uint32_t h) {
  auto it = targets_.find(address);
  if (it != targets_.end() && it->second.w == w && it->second.h == h) return &it->second;
  if (it != targets_.end()) {  // resized: recreate (the old one may still be in flight or about to be shown)
    {
      std::lock_guard sl(shown_lock_);  // a present that found it has submitted its blit before we get the lock
      shown_.erase(address);
    }
    {
      std::lock_guard ql(queue_lock_);
      vkDeviceWaitIdle(device_);
    }
    Target& o = it->second;
    vkDestroyFramebuffer(device_, o.framebuffer, nullptr);
    vkDestroyImageView(device_, o.view, nullptr);
    vkDestroyImage(device_, o.image, nullptr);
    vkFreeMemory(device_, o.memory, nullptr);
    targets_.erase(it);
  }
  Target t;
  t.w = w;
  t.h = h;
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_R8G8B8A8_UNORM;
  ici.extent = {w, h, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
              VK_IMAGE_USAGE_SAMPLED_BIT;
  if (vkCreateImage(device_, &ici, nullptr, &t.image) != VK_SUCCESS) return nullptr;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device_, t.image, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  int32_t type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  mai.memoryTypeIndex = uint32_t(type);
  if (type < 0 || vkAllocateMemory(device_, &mai, nullptr, &t.memory) != VK_SUCCESS) return nullptr;
  vkBindImageMemory(device_, t.image, t.memory, 0);
  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = t.image;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = VK_FORMAT_R8G8B8A8_UNORM;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCreateImageView(device_, &vci, nullptr, &t.view);
  VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fci.renderPass = color_pass_;
  fci.attachmentCount = 1;
  fci.pAttachments = &t.view;
  fci.width = w;
  fci.height = h;
  fci.layers = 1;
  vkCreateFramebuffer(device_, &fci, nullptr, &t.framebuffer);
  std::fprintf(stderr, "[vulkan] render target 0x%08x %ux%u\n", address, w, h);
  return &(targets_[address] = t);
}

void VulkanRenderer::begin_scene(guest::Memory&, const Scene& scene) {
  struct Timed {
    uint64_t t0 = ticks();
    ~Timed() { stat_time(kSceneTicks, t0); }
  } timed;
  stat_add(kScenes, 1);
  if (!recording_) {  // first scene of a frame: wait for its slot's last frame without holding the lock
    uint64_t tw = now_us(), k0 = ticks();
    vkWaitForFences(device_, 1, &cur_slot().fence, VK_TRUE, UINT64_MAX);
    stat_time(kSceneFenceTicks, k0);
    t_fence_ += now_us() - tw;
  }
  std::lock_guard lk(state_lock_);
  scenes_++;
  if (!scene.color.enabled || !scene.color.data || in_pass_) return;
  Target* t = target(scene.color.data, scene.color.width, scene.color.height);
  if (!t) return;
  scene_w_ = t->w;
  scene_h_ = t->h;
  begin_recording();
  t->stride = scene.color.stride;
  begin_pass(t);
}

// Opens the colour render pass on `t` (also used to resume a scene after a copy out of a render target).
void VulkanRenderer::begin_pass(Target* t) {
  cur_ = t;
  if (t->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
    if (t->layout == VK_IMAGE_LAYOUT_UNDEFINED) {  // first use: start black
      barrier(vkCmdPipelineBarrier, cmd_, t->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
              VK_ACCESS_TRANSFER_WRITE_BIT);
      VkClearColorValue black{};
      VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdClearColorImage(cmd_, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
      t->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    barrier(vkCmdPipelineBarrier, cmd_, t->image, t->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    t->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }
  VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rbi.renderPass = color_pass_;
  rbi.framebuffer = t->framebuffer;
  rbi.renderArea = {{0, 0}, {t->w, t->h}};
  vkCmdBeginRenderPass(cmd_, &rbi, VK_SUBPASS_CONTENTS_INLINE);
  in_pass_ = true;
  bound_ = Bound{};
  VkViewport vpt{0, 0, float(t->w), float(t->h), 0, 1};  // the whole target; the GXM viewport is in the shader
  VkRect2D sc{{0, 0}, {t->w, t->h}};
  vkCmdSetViewport(cmd_, 0, 1, &vpt);
  vkCmdSetScissor(cmd_, 0, 1, &sc);
}

void VulkanRenderer::end_scene(guest::Memory&) {
  std::lock_guard lk(state_lock_);
  if (!in_pass_) return;
  vkCmdEndRenderPass(cmd_);
  in_pass_ = false;
  cur_ = nullptr;
}

// A texture whose data is one of our render targets (the game samples what it rendered, e.g. a screen capture for a
// transition): the target's pixels are copied into an image of the texture's size when the draw is recorded, which
// is when the GPU would read that memory. Rows line up when the texture is as wide as the surface stride.
VulkanRenderer::Texture* VulkanRenderer::target_texture(const std::array<uint32_t, 4>& words, Target& src) {
  tex::Words w = {words[0], words[1], words[2], words[3]};
  tex::Desc d = tex::describe(w);
  if (!d.ok || d.format != tex::HostFormat::RGBA8 || d.type == tex::Type::Swizzled || d.mips != 1) {
    warn_once("render target sampled with an unsupported texture layout");
    return nullptr;
  }
  if (src.stride && d.width != src.stride) warn_once("render target sampled with a width other than its stride");
  Texture& t = target_textures_[words];
  t.used = frames_.load();
  if (t.image && t.version == src.version) return &t;
  if (!t.image) {
    tex::Decoded blank;
    blank.desc = d;
    blank.desc.swizzle = {};
    blank.data.assign(size_t(d.width) * d.height * 4, 0);
    blank.levels = {{0, d.width, d.height}};
    if (!upload_texture(blank, t)) return warn_once("render target texture creation failed"), nullptr;
  }
  bool resume = in_pass_;
  if (in_pass_) vkCmdEndRenderPass(cmd_), in_pass_ = false;
  Target* cur = cur_;
  barrier(vkCmdPipelineBarrier, cmd_, src.image, src.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  src.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barrier(vkCmdPipelineBarrier, cmd_, t.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  VkImageCopy region{};
  region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {std::min(src.w, d.width), std::min(src.h, d.height), 1};
  vkCmdCopyImage(cmd_, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 1, &region);
  stat_add(kTargetCopies, 1);
  barrier(vkCmdPipelineBarrier, cmd_, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
  t.version = src.version;
  if (resume && cur) begin_pass(cur);  // back to the scene's target (LOAD keeps what was drawn)
  return &t;
}

void VulkanRenderer::surface_lost() {
  save_caches();  // Android may kill a backgrounded app without another callback
  std::lock_guard lk(swap_lock_);
  drop_surface();
}

void VulkanRenderer::drop_surface() {
  if (!surface_) return;
  {
    std::lock_guard ql(queue_lock_);
    vkDeviceWaitIdle(device_);
  }
  destroy_swapchain();
  vkDestroySurfaceKHR(instance_, surface_, nullptr);
  surface_ = VK_NULL_HANDLE;
  std::fprintf(stderr, "[vulkan] surface lost (background)\n");
}

void VulkanRenderer::surface_restored() {
  std::lock_guard lk(swap_lock_);
  if (surface_) return;
  surface_ = reinterpret_cast<VkSurfaceKHR>(uintptr_t(setup_.create_surface(instance_)));
  VkBool32 ok = VK_FALSE;
  if (surface_) vkGetPhysicalDeviceSurfaceSupportKHR(gpu_, family_, surface_, &ok);
  std::fprintf(stderr, "[vulkan] surface restored: %s\n", surface_ && ok ? "ok" : "failed");
}

// Every 300 presents: average flip interval and where each thread waited. Display thread (in present): fence = the
// previous present's blit, acquire / queue present = the swapchain (FIFO: the panel's vblank). Rendering thread:
// flush = closing and submitting a frame, gpu wait = the GPU work of the frame before the previous one (of the
// previous one with DCP_FRAMES_IN_FLIGHT=1) before recording the next. vblank = the game's own
// sceDisplayWaitVblankStart* on our 59.94 Hz clock (all threads).
void VulkanRenderer::log_timing(uint64_t start) {
  uint64_t end = now_us();
  t_present_ += end - start;
  if (t_last_) {
    uint64_t iv = end - t_last_;
    t_window_ += iv, t_frames_++;
    t_max_interval_ = std::max(t_max_interval_, iv);
  }
  t_last_ = end;
  if (t_frames_ < 300) return;
  double n = double(t_frames_), ms = 1000.0;
  uint64_t vb = vblank_wait_us.exchange(0), fl = t_flush_.exchange(0), fe = t_fence_.exchange(0);
  std::fprintf(stderr,
               "[timing] %.1f fps, frame %.2f ms (max %.2f) | in present %.2f: fence %.2f acquire %.2f queue-present "
               "%.2f | flush %.2f gpu wait %.2f | game vblank waits %.2f ms/frame\n",
               n * 1e6 / double(t_window_), t_window_ / n / ms, t_max_interval_ / ms, t_present_ / n / ms,
               t_present_fence_ / n / ms, t_acquire_ / n / ms, t_queue_present_ / n / ms, fl / n / ms, fe / n / ms,
               vb / n / ms);
  static DrawStatsSnapshot last_stats;
  DrawStatsSnapshot stats = draw_stats_snapshot();
  std::string extra = draw_stats_text(last_stats, stats, n);
  last_stats = stats;
  if (frame_log_extra) extra += frame_log_extra(n);
  std::fputs(extra.c_str(), stderr);
#ifdef __ANDROID__
  // One logcat call per line: logcat cuts a message at ~4 KB, and the block (spike lines, callers) is longer.
  __android_log_print(ANDROID_LOG_INFO, "OpenCrown",
                      "[timing] %.1f fps, frame %.2f ms (max %.2f), present %.2f (acquire %.2f), flush %.2f, gpu wait "
                      "%.2f",
                      n * 1e6 / double(t_window_), t_window_ / n / ms, t_max_interval_ / ms, t_present_ / n / ms,
                      t_acquire_ / n / ms, fl / n / ms, fe / n / ms);
  for (size_t p = 0; p < extra.size();) {
    size_t e = extra.find('\n', p);
    if (e == std::string::npos) e = extra.size();
    if (e > p) __android_log_print(ANDROID_LOG_INFO, "OpenCrown", "%.*s", int(std::min<size_t>(e - p, 3900)), extra.c_str() + p);
    p = e + 1;
  }
#endif
  t_present_fence_ = t_acquire_ = t_queue_present_ = t_present_ = t_window_ = t_max_interval_ = t_frames_ = 0;
}

// The end of a frame on the rendering thread: close its recording and submit it; its targets become showable.
void VulkanRenderer::flush(guest::Memory&) {
  uint64_t t0 = now_us();
  std::lock_guard lk(state_lock_);
  frames_++;
  if (in_pass_) vkCmdEndRenderPass(cmd_), in_pass_ = false, cur_ = nullptr;
  if (recording_) {
    for (auto& [a, t] : targets_)
      if (t.layout != VK_IMAGE_LAYOUT_UNDEFINED && t.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        barrier(vkCmdPipelineBarrier, cmd_, t.image, t.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        t.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      }
    vkEndCommandBuffer(cmd_);
    recording_ = false;
    VkCommandBuffer cbs[2];
    uint32_t ncb = 0;
    if (upload_open_) vkEndCommandBuffer(upload_cmd_), cbs[ncb++] = upload_cmd_, upload_open_ = false;
    cbs[ncb++] = cmd_;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = ncb;
    si.pCommandBuffers = cbs;
    // Published and submitted under shown_lock_: a present that sees these targets submits its blit after this.
    std::lock_guard sl(shown_lock_);
    for (auto& [a, t] : targets_)
      if (t.layout != VK_IMAGE_LAYOUT_UNDEFINED) shown_[a] = {t.image, t.w, t.h};
    std::lock_guard ql(queue_lock_);
    vkQueueSubmit(queue_, 1, &si, cur_slot().fence);
    slot_ = (slot_ + 1) % frames_in_flight_;  // the next frame records into the next slot
  }
  if (pipeline_builds_ > saved_builds_ && frames_ - last_build_frame_ >= 120 && !cache_path_.empty()) {
    saved_builds_ = pipeline_builds_;
    std::thread([this, data = pipeline_cache_file()] { write_cache_file(cache_path_, data); }).detach();
  }
  if (frames_ % 600 == 1)
    std::fprintf(stderr, "[vulkan] frame %llu: %llu scenes / %llu draws received, %zu render targets\n",
                 (unsigned long long)frames_.load(), (unsigned long long)scenes_.load(),
                 (unsigned long long)draws_.load(), targets_.size());
  t_flush_ += now_us() - t0;
}

void VulkanRenderer::present(guest::Memory& mem, const Frame& f) {
  std::lock_guard lk(swap_lock_);
  presents_++;
  uint64_t t_start = now_us();
  struct Done { VulkanRenderer* r; uint64_t s; ~Done() { r->log_timing(s); } } done{this, t_start};
  if (!surface_) return;  // in the background: flush() still runs every frame's GPU work, nothing is shown
  if (!swapchain_) {
    std::string err;
    if (!create_swapchain(err) || !swapchain_) return;
  }
  // One present in flight: its command buffer, overlay vertices and guest-copy staging are reused below.
  uint64_t tw = now_us();
  vkWaitForFences(device_, 1, &present_fence_, VK_TRUE, UINT64_MAX);
  t_present_fence_ += now_us() - tw;
  uint32_t index = 0;
  // Finite wait: on a window Android has already taken away (going to the background) the acquire can block forever,
  // which would stop the guest thread. A flip that gets no image is dropped (the next one shows a newer frame).
  uint64_t ta = now_us();
  VkResult r = vkAcquireNextImageKHR(device_, swapchain_, 250000000ull, acquired_, VK_NULL_HANDLE, &index);
  t_acquire_ += now_us() - ta;
  if (r == VK_ERROR_SURFACE_LOST_KHR) {
    drop_surface();  // surface_restored() makes a new one when the window is back
    return;
  }
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    {
      std::lock_guard ql(queue_lock_);
      vkDeviceWaitIdle(device_);
    }
    std::string err;
    create_swapchain(err);
    return;
  }
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;

  VkCommandBuffer cmd = present_cmd_;
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkResetCommandBuffer(cmd, 0);
  vkBeginCommandBuffer(cmd, &bi);
  // DEBUG (local only): DCP_SCREENSHOT=<file.ppm> at flip DCP_SCREENSHOT_FRAME (default 600) saves the image shown.
  // A comma-separated list of flips saves one file per flip, <file>_<flip>.ppm.
  static const char* shot_path = std::getenv("DCP_SCREENSHOT");
  static const std::vector<uint64_t> shot_frames = [] {
    std::vector<uint64_t> v;
    const char* e = std::getenv("DCP_SCREENSHOT_FRAME");
    for (const char* p = e ? e : "600"; *p;) {
      char* end = nullptr;
      v.push_back(std::strtoull(p, &end, 10));
      if (end == p) break;
      p = *end == ',' ? end + 1 : end;
    }
    return v;
  }();
  const uint64_t shot_frame = presents_.load();
  bool shot_now = std::find(shot_frames.begin(), shot_frames.end(), shot_frame) != shot_frames.end();
  VkBuffer shot = VK_NULL_HANDLE;
  VkDeviceMemory shot_mem = VK_NULL_HANDLE;
  void* shot_ptr = nullptr;
  uint32_t sw = f.width, sh = f.height;  // source size
  {
    // From finding the target to submitting its blit: a target being recreated waits for this (target()).
    std::lock_guard sl(shown_lock_);
    // Source: our render target for that address (rendering submitted by flush), else the guest memory copy.
    VkImage src = VK_NULL_HANDLE, target_src = VK_NULL_HANDLE;
    if (auto it = shown_.find(f.base); it != shown_.end()) {
      src = target_src = it->second.image;
      sw = std::min(sw, it->second.w), sh = std::min(sh, it->second.h);
      barrier(vkCmdPipelineBarrier, cmd, src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    } else if (f.base && f.width && f.height && mem.valid(f.base, f.pitch * f.height * 4) &&
               ensure_frame_image(f.width, f.height)) {
      // A8B8G8R8 rows of `pitch` pixels -> tightly packed RGBA8 (alpha forced opaque for display).
      uint8_t* dst = static_cast<uint8_t*>(staging_ptr_);
      for (uint32_t y = 0; y < f.height; y++) {
        const uint8_t* row = mem.ptr(f.base + y * f.pitch * 4);
        std::memcpy(dst + size_t(y) * f.width * 4, row, size_t(f.width) * 4);
        for (uint32_t x = 0; x < f.width; x++) dst[(size_t(y) * f.width + x) * 4 + 3] = 255;
      }
      barrier(vkCmdPipelineBarrier, cmd, frame_,
              frame_fresh_ ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
      frame_fresh_ = false;
      VkBufferImageCopy copy{};
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {f.width, f.height, 1};
      vkCmdCopyBufferToImage(cmd, staging_, frame_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      barrier(vkCmdPipelineBarrier, cmd, frame_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
      src = frame_;
      if (gpu_log) std::fprintf(stderr, "[gpu] flip %llu shows the guest copy of 0x%08x\n", (unsigned long long)presents_.load(), f.base);
    } else if (gpu_log) {
      std::fprintf(stderr, "[gpu] flip %llu shows nothing (0x%08x)\n", (unsigned long long)presents_.load(), f.base);
    }

    VkImage dst_image = images_[index];
    barrier(vkCmdPipelineBarrier, cmd, dst_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue black{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    if (src && sw && sh) {
      // Letterboxed blit: keep the source aspect.
      float scale = std::min(float(extent_.width) / f.width, float(extent_.height) / f.height);
      int32_t w = int32_t(f.width * scale), h = int32_t(f.height * scale);
      int32_t x0 = (int32_t(extent_.width) - w) / 2, y0 = (int32_t(extent_.height) - h) / 2;
      VkImageBlit blit{};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.srcOffsets[1] = {int32_t(sw), int32_t(sh), 1};
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.dstOffsets[0] = {x0, y0, 0};
      blit.dstOffsets[1] = {x0 + w, y0 + h, 1};
      vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                     &blit, VK_FILTER_LINEAR);
    }
    if (!draw_overlay(index))
      barrier(vkCmdPipelineBarrier, cmd, dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
              VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    if (shot_path && src && sw && sh && shot_now &&
        create_buffer(VkDeviceSize(sw) * sh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, shot, shot_mem, &shot_ptr)) {
      VkBufferImageCopy copy{};
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {sw, sh, 1};
      vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot, 1, &copy);
    }
    if (target_src)  // back to the layout the rendering thread expects between command buffers
      barrier(vkCmdPipelineBarrier, cmd, target_src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                  VK_ACCESS_SHADER_READ_BIT);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired_;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered_[index];
    vkResetFences(device_, 1, &present_fence_);
    std::lock_guard ql(queue_lock_);
    vkQueueSubmit(queue_, 1, &si, present_fence_);
  }
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &rendered_[index];
  pi.swapchainCount = 1;
  pi.pSwapchains = &swapchain_;
  pi.pImageIndices = &index;
  uint64_t tq = now_us();
  {
    std::lock_guard ql(queue_lock_);
    r = vkQueuePresentKHR(queue_, &pi);
  }
  t_queue_present_ += now_us() - tq;
  if (shot) {
    vkWaitForFences(device_, 1, &present_fence_, VK_TRUE, UINT64_MAX);
    std::string file = shot_path;
    if (shot_frames.size() > 1) {
      size_t dot = file.find_last_of('.');
      std::string tag = "_" + std::to_string(shot_frame);
      file = dot == std::string::npos ? file + tag : file.substr(0, dot) + tag + file.substr(dot);
    }
    if (std::FILE* out = std::fopen(file.c_str(), "wb")) {
      std::fprintf(out, "P6\n%u %u\n255\n", sw, sh);
      const uint8_t* px = static_cast<const uint8_t*>(shot_ptr);
      for (size_t i = 0; i < size_t(sw) * sh; i++) std::fwrite(px + 4 * i, 1, 3, out);
      std::fclose(out);
      std::fprintf(stderr, "[vulkan] screenshot of flip %llu: %s\n", (unsigned long long)shot_frame, file.c_str());
    }
    vkDestroyBuffer(device_, shot, nullptr);
    vkFreeMemory(device_, shot_mem, nullptr);
  }
  if (r == VK_ERROR_SURFACE_LOST_KHR) {
    drop_surface();
  } else if (r == VK_ERROR_OUT_OF_DATE_KHR || (r == VK_SUBOPTIMAL_KHR && surface_changed())) {
    {
      std::lock_guard ql(queue_lock_);
      vkDeviceWaitIdle(device_);
    }
    std::string err;
    create_swapchain(err);
  }
}

// --- host UI overlay -------------------------------------------------------------------------------------------------
// Pass and pipeline follow the swapchain format; views / framebuffers follow the swapchain images.
bool VulkanRenderer::create_overlay() {
  if (overlay_format_ != format_) {
    if (overlay_pipe_) vkDestroyPipeline(device_, overlay_pipe_, nullptr), overlay_pipe_ = VK_NULL_HANDLE;
    if (overlay_pass_) vkDestroyRenderPass(device_, overlay_pass_, nullptr), overlay_pass_ = VK_NULL_HANDLE;
    overlay_format_ = format_;
    VkAttachmentDescription a{};
    a.format = format_;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &a;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    if (vkCreateRenderPass(device_, &rp, nullptr, &overlay_pass_) != VK_SUCCESS) return false;

    if (!overlay_layout_) {
      VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
      VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      pl.pushConstantRangeCount = 1;
      pl.pPushConstantRanges = &pcr;
      if (vkCreatePipelineLayout(device_, &pl, nullptr, &overlay_layout_) != VK_SUCCESS) return false;
    }
    std::vector<uint32_t> code[2] = {overlay_vertex_spirv(), overlay_fragment_spirv()};
    VkShaderModule mods[2] = {};
    for (int i = 0; i < 2; i++) {
      VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      sm.codeSize = code[i].size() * 4;
      sm.pCode = code[i].data();
      vkCreateShaderModule(device_, &sm, nullptr, &mods[i]);
    }
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = mods[0];
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = mods[1];
    stages[1].pName = "main";
    VkVertexInputBindingDescription bind{0, sizeof(OverlayVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[3] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(OverlayVertex, x)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(OverlayVertex, r)},
        {2, 0, VK_FORMAT_R32_SFLOAT, offsetof(OverlayVertex, inner)}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cb{};
    cb.blendEnable = VK_TRUE;
    cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cb.colorBlendOp = VK_BLEND_OP_ADD;
    cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cb.alphaBlendOp = VK_BLEND_OP_ADD;
    cb.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cbs;
    gpi.pDynamicState = &ds;
    gpi.layout = overlay_layout_;
    gpi.renderPass = overlay_pass_;
    VkResult r = mods[0] && mods[1] ? vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpi, nullptr, &overlay_pipe_)
                                    : VK_ERROR_INITIALIZATION_FAILED;
    for (VkShaderModule m : mods)
      if (m) vkDestroyShaderModule(device_, m, nullptr);
    if (r != VK_SUCCESS) return overlay_pipe_ = VK_NULL_HANDLE, false;
  }
  destroy_overlay();
  for (VkImage img : images_) {
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format_;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(device_, &vci, nullptr, &v) != VK_SUCCESS) return destroy_overlay(), false;
    sc_views_.push_back(v);
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = overlay_pass_;
    fci.attachmentCount = 1;
    fci.pAttachments = &v;
    fci.width = extent_.width;
    fci.height = extent_.height;
    fci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(device_, &fci, nullptr, &fb) != VK_SUCCESS) return destroy_overlay(), false;
    sc_fbs_.push_back(fb);
  }
  return true;
}

void VulkanRenderer::destroy_overlay() {
  for (VkFramebuffer fb : sc_fbs_) vkDestroyFramebuffer(device_, fb, nullptr);
  for (VkImageView v : sc_views_) vkDestroyImageView(device_, v, nullptr);
  sc_fbs_.clear();
  sc_views_.clear();
}

bool VulkanRenderer::draw_overlay(uint32_t index) {
  if (!overlay_pipe_ || index >= sc_fbs_.size()) return false;
  std::vector<OverlayVertex> v = overlay();
  if (v.empty()) return false;
  VkDeviceSize bytes = VkDeviceSize(v.size() * sizeof(OverlayVertex)), off = 0;
  if (bytes > overlay_size_) {  // the previous present has finished (present fence): safe to replace
    if (overlay_buf_) vkDestroyBuffer(device_, overlay_buf_, nullptr), vkFreeMemory(device_, overlay_mem_, nullptr);
    overlay_buf_ = VK_NULL_HANDLE;
    overlay_size_ = 0;
    VkDeviceSize size = std::max<VkDeviceSize>(bytes * 2, 64 << 10);
    if (!create_buffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, overlay_buf_, overlay_mem_, &overlay_ptr_)) return false;
    overlay_size_ = size;
  }
  std::memcpy(overlay_ptr_, v.data(), size_t(bytes));
  VkCommandBuffer cmd = present_cmd_;  // recorded on the display thread
  barrier(vkCmdPipelineBarrier, cmd, images_[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  bi.renderPass = overlay_pass_;
  bi.framebuffer = sc_fbs_[index];
  bi.renderArea = {{0, 0}, extent_};
  vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport vp{0, 0, float(extent_.width), float(extent_.height), 0, 1};
  VkRect2D sc{{0, 0}, extent_};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &sc);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlay_pipe_);
  vkCmdBindVertexBuffers(cmd, 0, 1, &overlay_buf_, &off);
  float size[2] = {float(extent_.width), float(extent_.height)};
  vkCmdPushConstants(cmd, overlay_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, size);
  vkCmdDraw(cmd, uint32_t(v.size()), 1, 0, 0);
  vkCmdEndRenderPass(cmd);
  return true;
}

void VulkanRenderer::destroy() {
  if (device_) {
    vkDeviceWaitIdle(device_);
    if (frame_) vkDestroyImage(device_, frame_, nullptr), vkFreeMemory(device_, frame_mem_, nullptr);
    if (staging_) vkDestroyBuffer(device_, staging_, nullptr), vkFreeMemory(device_, staging_mem_, nullptr);
    for (auto& [a, t] : targets_) {
      vkDestroyFramebuffer(device_, t.framebuffer, nullptr);
      vkDestroyImageView(device_, t.view, nullptr);
      vkDestroyImage(device_, t.image, nullptr);
      vkFreeMemory(device_, t.memory, nullptr);
    }
    for (auto& [k, p] : pipelines_)
      if (p) vkDestroyPipeline(device_, p, nullptr);
    for (auto& [h, p] : programs_) {
      if (p.module) vkDestroyShaderModule(device_, p.module, nullptr);
      for (auto& [f, m] : p.variants)
        if (m) vkDestroyShaderModule(device_, m, nullptr);
    }
    for (auto& [w, t] : textures_) destroy_texture(t);
    for (auto& [w, t] : target_textures_) destroy_texture(t);
    destroy_texture(dummy_);
    for (auto& [k, s] : samplers_) vkDestroySampler(device_, s, nullptr);
    for (FrameSlot& f : slots_) {
      for (Texture& t : f.retired) destroy_texture(t);
      if (f.ring) vkDestroyBuffer(device_, f.ring, nullptr), vkFreeMemory(device_, f.ring_mem, nullptr);
      if (f.up_ring) vkDestroyBuffer(device_, f.up_ring, nullptr), vkFreeMemory(device_, f.up_ring_mem, nullptr);
      for (TempBuffer& b : f.retired_buffers) vkDestroyBuffer(device_, b.buf, nullptr), vkFreeMemory(device_, b.mem, nullptr);
      for (VkImageView v : f.retired_views) vkDestroyImageView(device_, v, nullptr);
      if (f.bc_pool) vkDestroyDescriptorPool(device_, f.bc_pool, nullptr);
      if (f.dpool) vkDestroyDescriptorPool(device_, f.dpool, nullptr);
      if (f.fence) vkDestroyFence(device_, f.fence, nullptr);
    }
    if (bc_pipe_) vkDestroyPipeline(device_, bc_pipe_, nullptr);
    if (bc_layout_) vkDestroyPipelineLayout(device_, bc_layout_, nullptr);
    if (bc_set_layout_) vkDestroyDescriptorSetLayout(device_, bc_set_layout_, nullptr);
    for (MemBlock& b : mem_blocks_)
      if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
    if (overlay_buf_) vkDestroyBuffer(device_, overlay_buf_, nullptr), vkFreeMemory(device_, overlay_mem_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (pipeline_cache_) vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    if (color_pass_) vkDestroyRenderPass(device_, color_pass_, nullptr);
    destroy_overlay();
    if (overlay_pipe_) vkDestroyPipeline(device_, overlay_pipe_, nullptr);
    if (overlay_layout_) vkDestroyPipelineLayout(device_, overlay_layout_, nullptr);
    if (overlay_pass_) vkDestroyRenderPass(device_, overlay_pass_, nullptr);
    destroy_swapchain();
    if (upload_fence_) vkDestroyFence(device_, upload_fence_, nullptr);
    if (present_fence_) vkDestroyFence(device_, present_fence_, nullptr);
    if (acquired_) vkDestroySemaphore(device_, acquired_, nullptr);
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
    if (present_pool_) vkDestroyCommandPool(device_, present_pool_, nullptr);
    vkDestroyDevice(device_, nullptr);
  }
  if (surface_ && vkDestroySurfaceKHR) vkDestroySurfaceKHR(instance_, surface_, nullptr);
  if (instance_ && vkDestroyInstance) vkDestroyInstance(instance_, nullptr);
  device_ = VK_NULL_HANDLE;
  instance_ = VK_NULL_HANDLE;
}

}  // namespace

std::unique_ptr<Renderer> make_vulkan_renderer(const VulkanSetup& setup, std::string& err) {
  auto r = std::make_unique<VulkanRenderer>();
  if (!r->init(setup, err)) return nullptr;
  return r;
}

}  // namespace gpu
