// Host UI for the game's text-entry dialog (SceImeDialog, hle/ime.h): a panel drawn in the overlay (title, the text
// with a caret, length / max, Cancel and OK) fed by SDL text input. On Android SDL_StartTextInput brings up the system
// keyboard; on the desktop the keyboard types (the OS IME composes). Enter / gamepad Start = OK, Escape / Android
// Back / gamepad B = Cancel; tapping or clicking the buttons works too, tapping the text box shows the keyboard again.
// Runs on the SDL event thread; the guest thread only sees the answer (hle::ime_answer).
#pragma once
#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "gpu/overlay.h"
#include "hle/ime.h"

namespace platform {

class TextEntry {
 public:
  ~TextEntry();
  bool open() const { return open_; }
  // Picks up a new request from the game or notices that it went away (aborted); call every loop iteration.
  // true: the panel opened, closed or needs drawing again (caret blink).
  bool sync(SDL_Window* win);
  // Input while open: true = the event belonged to the dialog (the caller keeps it from the game).
  bool event(SDL_Window* win, const SDL_Event& e);
  // The panel in window pixels (w x h).
  std::vector<gpu::OverlayVertex> geometry(float w, float h) const;

 private:
  struct Box {
    float x0, y0, x1, y1;
    bool hit(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
  };
  struct Layout {
    float s, ox, oy;  // picture scale and origin (the letterboxed 960x544 picture)
    Box panel, text, cancel, ok;
    float px(float x) const { return ox + x * s; }
    float py(float y) const { return oy + y * s; }
  };
  Layout layout(float w, float h) const;
  void start_input(SDL_Window* win);
  void finish(SDL_Window* win, bool ok);
  void insert_utf8(const char* s);
  void backspace();
  bool click(SDL_Window* win, float x, float y);  // window pixels
  void dev_test(SDL_Window* win);

  bool open_ = false;
  hle::ImeRequest req_;
  std::u16string text_;   // what the game gets
  std::string editing_;   // OS IME composition (UTF-8), shown after the text, not yet part of it
  uint64_t blink_phase_ = 0;
  bool pad_hint_ = false;  // a gamepad is connected: the hint names its buttons
  uint64_t opened_ms_ = 0;
  int test_step_ = 0;
};

}  // namespace platform
