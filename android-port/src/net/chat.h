// Room text chat (ours, the game has none; lobby_protocol.md 4.6). Thread-safe hand-off between the overlay (render
// thread) and the Matching2 shim (guest thread), which owns the lobby link: the overlay queues what the player typed
// and takes the lines to show; the shim sends the queue while we sit in a room and delivers "chat" events and send
// failures. Mutes are per account and kept in <user dir>/chat_mutes.txt. $DCP_CHAT=0 (launcher "Room text chat" off)
// turns chat off: nothing is sent or shown.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dcp::net::chat {

constexpr size_t kMaxChars = 120;  // code points; the lobby cuts longer text

struct Line {
  uint64_t account = 0;  // sender (0 for a notice)
  std::string name;      // sender's online name
  std::string text;
  int slot = -1;         // sender's player slot 0..3 (member id 0x10 -> 0, ...), -1 = unknown / notice
  bool self = false;     // our own message (the lobby's echo)
  bool notice = false;   // a local status line ("not sent: ..."), not from a player
};

bool enabled();  // $DCP_CHAT != "0"

// ---- overlay side ----
bool in_room();                      // chat can be sent now
void send(const std::string& text);  // trimmed; ignored when empty, chat is off or we are not in a room
bool take(Line* out);                // next line to show (muted senders already dropped)
void notice(const std::string& text);

// ---- Matching2 side ----
void set_in_room(bool in);  // false also drops anything still queued
bool next_outgoing(std::string* text);
void received(uint64_t account, const std::string& name, const std::string& text, bool self, int slot = -1);

// ---- mutes (persisted in `path`, one "account name" per line) ----
void load_mutes(const std::string& path);
bool muted(uint64_t account);
void set_muted(uint64_t account, const std::string& name, bool on);
std::vector<std::pair<uint64_t, std::string>> mutes();

// UTF-8 helpers shared by the overlay and the tests.
size_t code_points(const std::string& s);
std::string clamp_chars(const std::string& s, size_t max);  // whole code points, control characters removed, trimmed

}  // namespace dcp::net::chat
