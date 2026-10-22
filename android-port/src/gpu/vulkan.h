// Vulkan backend of gpu::Renderer. The platform layer hands it a vkGetInstanceProcAddr, the instance extensions the
// window system needs and a callback that creates the window surface (SDL3 on desktop and Android).
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gpu/renderer.h"

namespace gpu {

struct VulkanSetup {
  void* get_instance_proc_addr = nullptr;               // PFN_vkGetInstanceProcAddr
  std::vector<const char*> instance_extensions;          // from the window system
  std::function<uint64_t(void* instance)> create_surface;  // returns a VkSurfaceKHR handle (0 = failure)
  std::function<void(uint32_t&, uint32_t&)> drawable_size;  // current framebuffer size in pixels
  bool validation = false;                                // VK_LAYER_KHRONOS_validation if installed
  std::string cache_dir;  // persistent pipeline cache folder (created when missing); empty = none
};

// Creates the backend, or returns nullptr with the reason in `err`.
std::unique_ptr<Renderer> make_vulkan_renderer(const VulkanSetup& setup, std::string& err);

}  // namespace gpu
