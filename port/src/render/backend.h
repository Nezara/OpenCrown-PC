// Replay of captured frames (capture.h) through a Device (device.h: OpenGL 3.3 or SDL_gpu). One thread only: the one
// that owns the GL context / submits to the SDL_gpu device.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "capture.h"
#include "device.h"

namespace dcp::render {

class Backend {
public:
    Backend();
    ~Backend();
    // OpenGL: GL entry points must already be loaded (dcp::gl::load). Returns false on shader / GL setup failure.
    bool init();
    // Any device (make_gl_device / make_gpu_device); takes ownership. False if dev->init() fails.
    bool init(std::unique_ptr<Device> dev);
    const char* device_name() const;
    // Replays one frame into the game's render targets (one device texture per target address).
    void draw(const Frame& f);
    // The texture shown by the last frame (row 0 = top) and its size; 0 if nothing drawn yet.
    unsigned shown_texture(int* w, int* h) const;
    // RGB8 rows, top first, of the shown target.
    std::vector<uint8_t> read_rgb(int* w, int* h) const;
    // Draws the shown target into the window (w x h pixels), letterboxed. GL: into the default framebuffer (the
    // caller swaps); SDL_gpu: acquires, draws (then overlay) and submits the swapchain image.
    void present(int w, int h, Device::Overlay* overlay = nullptr) const;
    // Button prompts (prompts.h): kbm = show keyboard/mouse glyphs instead of the pad's; bindings = input.ini action ->
    // first bound input name.
    void set_prompt_kbm(bool kbm);
    void set_prompt_bindings(const std::map<std::string, std::string>& bindings);

private:
    struct Impl;
    std::unique_ptr<Impl> m_;
};

}  // namespace dcp::render
