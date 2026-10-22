// Window + input + Vulkan surface on SDL3 (desktop now; SDL3 also covers Android). The game runs on a worker
// thread; the SDL event loop owns the calling (main) thread until the window closes.
#pragma once
#include <functional>
#include <string>

namespace platform {

struct AppOptions {
  std::string input_ini;                   // written with the defaults when missing (mobile_run: <user folder>/input.ini)
  bool validation = false;                 // Vulkan validation layer, if installed
  int scale = 2;                           // initial window size = 960x544 * scale
  std::string cache_dir;                   // Vulkan pipeline cache (Android: the app's internal files)
  std::string title = "OpenCrown Mobile";  // window title (mobile_run adds the online name)
  // On-screen touch controls while no gamepad is connected. Android: always; desktop: --touch-controls (test mode,
  // the left mouse button is a finger).
#ifdef __ANDROID__
  bool touch_controls = true;
#else
  bool touch_controls = false;
#endif
};

// Opens the window, installs the Vulkan renderer, runs `guest` on its own thread and pumps events. Returns the
// process exit code (the window closing ends the process).
int run_with_window(const AppOptions& opt, const std::function<void()>& guest);

}  // namespace platform
