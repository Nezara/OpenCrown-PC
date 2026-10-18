// OpenGL 3.3 replay of captured frames (capture.h). One thread only: the one that owns the GL context.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "capture.h"

namespace dcp::render {

class Backend {
public:
    Backend();
    ~Backend();
    // GL entry points must already be loaded (dcp::gl::load). Returns false on shader / GL setup failure.
    bool init();
    // Replays one frame into the game's render targets (one GL texture per target address).
    void draw(const Frame& f);
    // The texture shown by the last frame (row 0 = top) and its size; 0 if nothing drawn yet.
    unsigned shown_texture(int* w, int* h) const;
    unsigned shown_fbo() const;
    // RGB8 rows, top first, of the shown target.
    std::vector<uint8_t> read_rgb(int* w, int* h) const;
    // Blits the shown target into the default framebuffer (window) of size w x h, letterboxed.
    void present(int w, int h) const;
    // Button prompts (prompts.h): kbm = show keyboard/mouse glyphs instead of the pad's; bindings = input.ini action ->
    // first bound input name.
    void set_prompt_kbm(bool kbm);
    void set_prompt_bindings(const std::map<std::string, std::string>& bindings);

private:
    struct Impl;
    std::unique_ptr<Impl> m_;
};

}  // namespace dcp::render
