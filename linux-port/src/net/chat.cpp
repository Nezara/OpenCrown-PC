// See chat.h.
#include "net/chat.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <chrono>

namespace dcp::net::chat {
namespace {

constexpr size_t kMaxQueued = 64;  // lines waiting for the overlay; older ones are dropped

std::mutex g_mu;
bool g_in_room = false;
std::deque<std::string> g_out;
std::deque<Line> g_in;
std::map<uint64_t, std::string> g_mutes;  // account -> name when muted
std::string g_mute_path;

void push_locked(Line l) {
    g_in.push_back(std::move(l));
    while (g_in.size() > kMaxQueued) g_in.pop_front();
}

void save_mutes_locked() {
    if (g_mute_path.empty()) return;
    std::ofstream o(g_mute_path);
    o << "# Muted players (room chat): account id, then the name they had\n";
    for (const auto& [a, n] : g_mutes) o << a << ' ' << n << '\n';
}

}  // namespace

bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("DCP_CHAT");
        return !(e && std::strcmp(e, "0") == 0);
    }();
    return on;
}

size_t code_points(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xc0) != 0x80;
    return n;
}

std::string clamp_chars(const std::string& s, size_t max) {
    std::string t;
    for (unsigned char c : s)
        if (c >= 0x20 && c != 0x7f) t += char(c);
    size_t a = t.find_first_not_of(' '), z = t.find_last_not_of(' ');
    if (a == std::string::npos) return "";
    t = t.substr(a, z - a + 1);
    size_t n = 0;
    for (size_t i = 0; i < t.size(); ++i)
        if ((uint8_t(t[i]) & 0xc0) != 0x80 && n++ == max) return t.substr(0, i);
    return t;
}

bool in_room() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_in_room;
}

void send(const std::string& text) {
    std::string t = clamp_chars(text, kMaxChars);
    if (t.empty() || !enabled()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_in_room) return;
    if (g_out.size() < 8) g_out.push_back(std::move(t));
}

bool take(Line* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_in.empty()) return false;
    *out = std::move(g_in.front());
    g_in.pop_front();
    return true;
}

void notice(const std::string& text) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    Line l;
    l.text = text;
    l.notice = true;
    push_locked(std::move(l));
}

// $DCP_CHAT_SAY=<text> (scripted two-instance tests only): said every 10 s while in a room as "<text> #n", and
// received lines are logged. Chat text is never logged otherwise (last_run.log goes into bug reports).
const char* test_say() {
    static const char* s = std::getenv("DCP_CHAT_SAY");
    return s && *s ? s : nullptr;
}

void set_in_room(bool in) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (in && test_say() && enabled()) {
        static int64_t last = 0, n = 0;
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
        if (!last || now - last >= 10000) last = now, g_out.push_back(std::string(test_say()) + " #" + std::to_string(++n));
    }
    g_in_room = in;
    if (!in) g_out.clear();
}

bool next_outgoing(std::string* text) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_out.empty()) return false;
    *text = std::move(g_out.front());
    g_out.pop_front();
    return true;
}

void received(uint64_t account, const std::string& name, const std::string& text, bool self, int slot) {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!self && g_mutes.count(account)) return;
    Line l;
    l.account = account;
    l.name = clamp_chars(name, 16);
    l.text = clamp_chars(text, kMaxChars);
    l.self = self;
    l.slot = slot >= 0 && slot < 4 ? slot : -1;
    if (test_say()) std::fprintf(stderr, "CHAT test recv%s %s: %s\n", self ? " (echo)" : "", l.name.c_str(), l.text.c_str());
    if (!l.text.empty()) push_locked(std::move(l));
}

void load_mutes(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_mute_path = path;
    g_mutes.clear();
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        unsigned long long a = 0;
        std::string name;
        if (in >> a && a) {
            std::getline(in >> std::ws, name);
            g_mutes[a] = clamp_chars(name, 16);
        }
    }
}

bool muted(uint64_t account) {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_mutes.count(account) != 0;
}

void set_muted(uint64_t account, const std::string& name, bool on) {
    if (!account) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (on) g_mutes[account] = clamp_chars(name, 16);
    else g_mutes.erase(account);
    save_mutes_locked();
}

std::vector<std::pair<uint64_t, std::string>> mutes() {
    std::lock_guard<std::mutex> lk(g_mu);
    return {g_mutes.begin(), g_mutes.end()};
}

}  // namespace dcp::net::chat
