// Room text chat on the host overlay (net/chat.h; lobby_protocol.md 4.6). Closed: the newest lines bottom-left over
// the picture on a dark strip, fading out. Open: a panel in the upper half (the Android keyboard covers the lower
// half) with the recent history, the text field and Send / Close. Opening: Enter on a keyboard, or tap the picture and
// then touch and hold the same spot (both touches still reach the game as fairy touches; a plain tap or hold is never
// taken). Start / Enter / the keyboard's Done send; B / Escape / Android Back close. Game input is
// blocked while open, and the button that closed it stays from the game until released. Commands: /mute <name>,
// /unmute <name>, /mutes, /help. Runs on the SDL event thread.
#pragma once
#include <SDL3/SDL.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "gpu/overlay.h"
#include "net/chat.h"

namespace platform {

class ChatOverlay {
 public:
  void init();  // mutes file in the identity folder
  bool open() const { return open_; }
  // Takes new lines, runs fades / caret / the Select gesture timers; call every loop iteration.
  // true: the picture changed or a timer runs (publish again).
  bool sync(SDL_Window* win);
  // Input; true = it belonged to the chat (the caller keeps it from the game). entry_open: the game's own text box.
  bool event(SDL_Window* win, const SDL_Event& e, bool entry_open);
  // A finger on the picture (window pixels; down / motion / up / canceled) for the tap-then-hold gesture. on_picture:
  // not on an on-screen control. entry_open: the game's own text box is up (no gesture).
  void finger(SDL_Window* win, int64_t id, uint32_t type, float x, float y, bool on_picture, bool entry_open);
  // Overlay triangles in window pixels; `version` changes whenever they do.
  const std::vector<gpu::OverlayVertex>& geometry(float w, float h);
  uint64_t version() const { return version_; }

 private:
  struct Entry {
    dcp::net::chat::Line line;
    uint64_t t = 0;  // arrival, ms
  };
  enum class Gesture { Idle, Down1, Wait2, Down2 };
  void open_box(SDL_Window* win);
  void close_box(SDL_Window* win);
  void start_input(SDL_Window* win);
  void submit();
  void local(const std::string& text);
  bool click(SDL_Window* win, float x, float y);

  bool open_ = false;
  std::string text_;     // being typed (UTF-8)
  std::string editing_;  // OS IME composition
  std::deque<Entry> lines_;
  bool was_in_room_ = false, hinted_ = false, pad_hint_ = false;
  uint64_t version_ = 1, built_version_ = 0;
  float built_w_ = 0, built_h_ = 0;
  uint64_t fade_step_ = 0, blink_phase_ = 0;
  std::vector<gpu::OverlayVertex> geo_;
  Gesture gesture_ = Gesture::Idle;
  uint64_t gesture_ms_ = 0;
  int64_t gesture_finger_ = 0;
  float gesture_x_ = 0, gesture_y_ = 0;
};

}  // namespace platform
