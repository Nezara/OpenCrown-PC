// Room text chat overlay (src/net/chat.h; lobby_protocol.md 4.6), drawn with the ImeBox's ImGui context on the
// render-loop thread. Closed: the newest lines sit bottom-left over the game, no frame, and fade out. Open: a
// see-through box with the recent history and a text field: Enter sends, Esc or gamepad B / Circle closes without
// sending; game input is blocked while it is open (and after closing until every key / button is released).
// Opening: the "chat" key (input.ini, default Return), or on a touch screen (Steam Deck) a tap and then a touch-and-hold
// on the same spot; both touches still reach the game as fairy touches, a plain tap or hold never opens it. Opened by
// touch under Steam, Steam's on-screen keyboard is brought up (the launcher's steam://open/keyboard link). Chat is never
// on a gamepad button. Commands: /mute <name>, /unmute <name>, /mutes, /help.
#pragma once
#include <sys/types.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "net/chat.h"
#include "touch_math.h"

union SDL_Event;
struct SDL_Window;
class SdlInputPump;

class ChatBox {
public:
    // imgui_ok = ImeBox::ok(); user_dir holds input.ini (the chat key) and chat_mutes.txt; input (may be null) says
    // whether left clicks are touches (gamescope's touch emulation).
    void init(bool imgui_ok, const std::string& user_dir, const SdlInputPump* input);
    // ime_busy: the name box is open or still blocking (the chat key and the touch gesture are ignored then).
    void handle_event(SDL_Window* win, const SDL_Event& e, bool ime_busy);
    bool blocking() const { return typing_ || blocking_; }
    // Once per presented frame: takes new lines, runs the touch gesture timer; true = draw() must run. ime_open closes
    // the field.
    bool update(SDL_Window* win, bool ime_open);
    void draw(SDL_Window* win);  // between ImeBox::begin_frame and end_frame

private:
    struct Entry {
        dcp::net::chat::Line line;
        double t = 0;  // arrival, seconds (SDL ticks)
    };
    void open_box(bool by_touch);
    void close_box(SDL_Window* win);
    void finger(SDL_Window* win, int64_t id, dcp::touch::TapHold::Phase ph, float x, float y, bool ime_busy, bool cancel);
    void submit(const std::string& text);
    void local(const std::string& text);
    void spawn(const std::string& url);  // xdg-open url, detached; reaped in update()

    bool ok_ = false;
    const SdlInputPump* input_ = nullptr;
    bool deck_ = false;   // Steam Deck / Game Mode: the touch hint instead of the key hint
    bool steam_ = false;  // started by Steam: its on-screen keyboard can be opened
    int key_ = 0;         // SDL_Scancode that opens the box (0 = unbound)
    bool typing_ = false;
    bool blocking_ = false;  // closed, but a key / button that closed it is still down
    bool focus_ = false;
    bool scroll_ = false;    // jump the history to the newest line
    bool was_in_room_ = false;
    bool hinted_ = false;    // "press Enter to talk" shown once per run
    bool keyboard_due_ = false;    // opened by touch: bring up Steam's keyboard once the field is active
    bool keyboard_shown_ = false;  // we opened Steam's keyboard (closed again with the box)
    dcp::touch::TapHold gesture_;
    bool mouse_finger_ = false;  // a left press that counts as a touch is down
    std::vector<pid_t> children_;
    std::deque<Entry> lines_;
    char buf_[512] = {};
};
