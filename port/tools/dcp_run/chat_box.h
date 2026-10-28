// Room text chat overlay (src/net/chat.h; lobby_protocol.md 4.6), drawn with the ImeBox's ImGui context on the
// render-loop thread. Closed: the newest lines sit bottom-left over the game, no frame, and fade out. The "chat" key
// (input.ini, default Return) opens a see-through box with the recent history and a text field: Enter sends, Esc
// closes; game input is blocked while it is open. Commands: /mute <name>, /unmute <name>, /mutes, /help.
#pragma once
#include <deque>
#include <string>

#include "net/chat.h"

union SDL_Event;
struct SDL_Window;

class ChatBox {
public:
    // imgui_ok = ImeBox::ok(); user_dir holds input.ini (the chat key) and chat_mutes.txt.
    void init(bool imgui_ok, const std::string& user_dir);
    // ime_busy: the name box is open or still blocking (the chat key is ignored then).
    void handle_event(const SDL_Event& e, bool ime_busy);
    bool blocking() const { return typing_ || blocking_; }
    // Once per presented frame: takes new lines; true = draw() must run. ime_open closes the field.
    bool update(SDL_Window* win, bool ime_open);
    void draw(SDL_Window* win);  // between ImeBox::begin_frame and end_frame

private:
    struct Entry {
        dcp::net::chat::Line line;
        double t = 0;  // arrival, seconds (SDL ticks)
    };
    void submit(const std::string& text);
    void local(const std::string& text);  // a notice line

    bool ok_ = false;
    int key_ = 0;         // SDL_Scancode that opens the box (0 = unbound)
    bool typing_ = false;
    bool blocking_ = false;  // closed, but a key / button that closed it is still down
    bool focus_ = false;
    bool scroll_ = false;    // jump the history to the newest line
    bool was_in_room_ = false;
    bool hinted_ = false;    // "press Enter to chat" shown once per run
    std::deque<Entry> lines_;
    char buf_[512] = {};
};
