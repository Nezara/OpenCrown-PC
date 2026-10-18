#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <pthread.h>

class SdlInputPump;

struct RenderOptions {
    bool window = false;                                    // show the window (else hidden: screenshots only)
    std::vector<std::pair<uint64_t, std::string>> shots;    // (vblank, png path), ascending
    bool fullscreen = false;                                // borderless desktop fullscreen (with window)
    int width = 1280, height = 720;                         // initial window size
    SdlInputPump* input = nullptr;                          // live keyboard/mouse/gamepad source, or null
    std::string input_dir;                                  // folder of input.ini (button-prompt labels)
};

// Runs on the process main thread until the game exits or the window closes (calls finish, never returns normally).
// game_thread: the eboot entry thread; once it has returned, the loop joins it and calls finish(0).
int run_render_loop(const RenderOptions& o, pthread_t game_thread, void (*finish)(int));
