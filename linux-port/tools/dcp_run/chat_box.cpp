// See chat_box.h.
#include "chat_box.h"

#include <SDL3/SDL.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "ime_box.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "sdl_input.h"

extern char** environ;

namespace chat = dcp::net::chat;
using Phase = dcp::touch::TapHold::Phase;

namespace {

// Placement, as fractions of the window: bottom-left corner of the chat area, its width.
constexpr float kLeft = 0.02f, kBottom = 0.80f, kWidth = 0.36f;
constexpr double kShowSec = 8.0, kFadeSec = 1.5;  // a closed line stays this long, then fades out
constexpr size_t kShownClosed = 5;                 // newest lines shown while closed
constexpr size_t kHistory = 60;
constexpr float kTextScale = 0.85f;  // of the ImGui font size (the name box's)
constexpr int64_t kMouseFinger = -1;  // a left press under touch_as_mouse

double now_s() { return double(SDL_GetTicksNS()) / 1e9; }

// The game's player name colours by slot (1P host .. 4P, sampled from the game's own name text; the pad light-bar
// table 0x74d3a0 has the same hues), so a name in chat matches that player on screen (notes/map/player_colours.md).
// Slot = (member id >> 4) - 1, which matches the game's player table in normal play (same lowest-free rule).
constexpr ImU32 kSlotRgb[4] = {0xe06060, 0x6060e0, 0xe0c060, 0x80e060};

ImU32 name_color(const chat::Line& l, float a) {
    const ImU32 rgb = l.slot >= 0 ? kSlotRgb[l.slot] : 0xd0d0d0;
    return ImGui::GetColorU32(ImVec4(float((rgb >> 16) & 0xff) / 255.0f, float((rgb >> 8) & 0xff) / 255.0f,
                                     float(rgb & 0xff) / 255.0f, a));
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Cuts the field back to chat::kMaxChars code points as the player types or pastes.
int clamp_length(ImGuiInputTextCallbackData* d) {
    while (d->BufTextLen > 0 && chat::code_points(std::string(d->Buf, d->BufTextLen)) > chat::kMaxChars) {
        int i = d->BufTextLen - 1;
        while (i > 0 && (uint8_t(d->Buf[i]) & 0xc0) == 0x80) --i;
        d->DeleteChars(i, d->BufTextLen - i);
    }
    return 0;
}

bool env_on(const char* k) {
    const char* v = std::getenv(k);
    return v && *v && std::strcmp(v, "0") != 0;
}

}  // namespace

void ChatBox::init(bool imgui_ok, const std::string& user_dir, const SdlInputPump* input) {
    ok_ = imgui_ok && chat::enabled();
    if (!ok_) return;
    input_ = input;
    deck_ = steam_deck_host();
    // Started by Steam (Game Mode, Big Picture or a Steam shortcut): the launcher's test for its keyboard link.
    steam_ = env_on("SteamDeck") || env_on("SteamAppId") || env_on("SteamGameId");
    chat::load_mutes(user_dir + "/chat_mutes.txt");
    auto binds = prompt_bindings(user_dir);
    const std::string& k = binds["chat"];
    key_ = k.empty() || SDL_strncasecmp(k.c_str(), "mouse", 5) == 0 ? 0 : int(SDL_GetScancodeFromName(k.c_str()));
}

void ChatBox::local(const std::string& text) {
    Entry e;
    e.line.text = text;
    e.line.notice = true;
    e.t = now_s();
    lines_.push_back(std::move(e));
    while (lines_.size() > kHistory) lines_.pop_front();
    scroll_ = true;
}

// Runs a program with argv (no shell), detached; LD_PRELOAD (Steam's overlay) removed. Same as the launcher's.
void ChatBox::spawn(const std::string& url) {
    std::string prog = "xdg-open", arg = url;
    char* argv[] = {prog.data(), arg.data(), nullptr};
    std::vector<char*> env;
    for (char** e = environ; *e; ++e)
        if (std::strncmp(*e, "LD_PRELOAD=", 11) != 0) env.push_back(*e);
    env.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], &fa, nullptr, argv, env.data()) == 0) children_.push_back(pid);
    else std::fprintf(stderr, "chat: cannot start xdg-open\n");
    posix_spawn_file_actions_destroy(&fa);
    std::fprintf(stderr, "chat: on-screen keyboard: xdg-open %s\n", url.c_str());
}

void ChatBox::open_box(bool by_touch) {
    typing_ = focus_ = scroll_ = true;
    buf_[0] = 0;
    gesture_.reset();
    keyboard_due_ = by_touch && steam_;
    if (by_touch && !steam_) std::fprintf(stderr, "chat: opened by touch (not under Steam: no on-screen keyboard)\n");
    // ImGui only sees input while the box is open: whatever it last saw held (the Enter that sent the last line) is
    // still down there and would repeat into this field.
    ImGuiIO& io = ImGui::GetIO();
    io.ClearEventsQueue();
    io.ClearInputKeys();
    io.ClearInputMouse();
}

void ChatBox::close_box(SDL_Window* win) {
    typing_ = false, blocking_ = true;
    buf_[0] = 0;
    keyboard_due_ = false;
    SDL_StopTextInput(win);
    if (keyboard_shown_) {  // as SDL's own Steam keyboard support does on SDL_StopTextInput
        keyboard_shown_ = false;
        spawn("steam://close/keyboard");
    }
}

void ChatBox::finger(SDL_Window* win, int64_t id, Phase ph, float x, float y, bool ime_busy, bool cancel) {
    int w = 1, h = 1;
    SDL_GetWindowSize(win, &w, &h);
    const bool armed = !typing_ && !blocking_ && !ime_busy && chat::in_room();
    gesture_.feed(id, ph, x, y, float(w), float(h), SDL_GetTicks(), armed, cancel);
}

void ChatBox::handle_event(SDL_Window* win, const SDL_Event& e, bool ime_busy) {
    if (!ok_) return;
    if (typing_) {
        ImGui_ImplSDL3_ProcessEvent(&e);
        return;
    }
    // Touch gesture (tap, then touch and hold): native touches, or left clicks that stand for touches (gamescope).
    // Only watched: the same events reach the game's fairy pointer through the input pump.
    switch (e.type) {
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
        int w = 1, h = 1;
        SDL_GetWindowSize(win, &w, &h);
        const Phase ph = e.type == SDL_EVENT_FINGER_DOWN ? Phase::Down : e.type == SDL_EVENT_FINGER_MOTION ? Phase::Move : Phase::Up;
        finger(win, int64_t(e.tfinger.fingerID & 0x7fffffffffffffffull), ph, e.tfinger.x * w, e.tfinger.y * h, ime_busy,
               e.type == SDL_EVENT_FINGER_CANCELED);
        return;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (e.button.button != SDL_BUTTON_LEFT || e.button.which == SDL_TOUCH_MOUSEID) break;
        const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if (down ? input_ && input_->left_mouse_is_touch() : mouse_finger_) {
            mouse_finger_ = down;
            finger(win, kMouseFinger, down ? Phase::Down : Phase::Up, e.button.x, e.button.y, ime_busy, false);
        }
        return;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (mouse_finger_) finger(win, kMouseFinger, Phase::Move, e.motion.x, e.motion.y, ime_busy, false);
        return;
    default: break;
    }
    if (e.type != SDL_EVENT_KEY_DOWN || e.key.repeat || !key_ || ime_busy || blocking_) return;
    if (int(e.key.scancode) != key_ && !(key_ == SDL_SCANCODE_RETURN && e.key.scancode == SDL_SCANCODE_KP_ENTER)) return;
    if (!chat::in_room()) {
        if (hinted_) local("You are not in an online room.");  // offline players never see chat lines
        return;
    }
    open_box(false);
}

bool ChatBox::update(SDL_Window* win, bool ime_open) {
    for (size_t i = 0; i < children_.size();) {  // xdg-open runs briefly; reap it
        if (waitpid(children_[i], nullptr, WNOHANG) != 0) children_.erase(children_.begin() + long(i));
        else ++i;
    }
    if (!ok_) return false;
    const bool in = chat::in_room();
    if (in && !was_in_room_ && !hinted_) {
        hinted_ = true;
        if (deck_) {  // Steam Deck: the touch screen is the way in (never both hints)
            local("Double tap the screen and hold to talk.");
        } else {
            std::string name = key_ ? SDL_GetScancodeName(SDL_Scancode(key_)) : "";
            if (name == "Return") name = "Enter";
            local(!name.empty() ? "Press " + name + " to talk."
                                : std::string("Room chat: bind the \"chat\" key in the launcher's controls to talk."));
        }
    }
    was_in_room_ = in;
    for (chat::Line l; chat::take(&l);) {
        lines_.push_back({std::move(l), now_s()});
        while (lines_.size() > kHistory) lines_.pop_front();
        scroll_ = true;
    }
    if (gesture_.tick(SDL_GetTicks()) && !typing_ && !blocking_ && !ime_open && in) {
        std::fprintf(stderr, "chat: opened by tap and hold\n");
        open_box(true);
    }
    if (typing_ && (ime_open || !in)) close_box(win);  // the game's own name box wins; leaving the room ends the chat
    if (!typing_ && blocking_ && !overlay_input_held()) blocking_ = false;
    if (typing_) return true;
    const double t = now_s();
    for (const Entry& e : lines_)
        if (t - e.t < kShowSec + kFadeSec) return true;
    return false;
}

void ChatBox::submit(const std::string& raw) {
    std::string text = chat::clamp_chars(raw, chat::kMaxChars);
    if (text.empty()) return;
    if (text[0] != '/') return chat::send(text);  // shown when the lobby's echo comes back
    size_t sp = text.find(' ');
    std::string cmd = lower(text.substr(0, sp)), arg = sp == std::string::npos ? "" : chat::clamp_chars(text.substr(sp + 1), 16);
    if (cmd == "/mute") {
        for (auto it = lines_.rbegin(); it != lines_.rend(); ++it) {
            const chat::Line& l = it->line;
            if (l.notice || l.self || lower(l.name) != lower(arg)) continue;
            uint64_t acc = l.account;
            std::string name = l.name;
            chat::set_muted(acc, name, true);
            lines_.erase(std::remove_if(lines_.begin(), lines_.end(), [&](const Entry& e) { return e.line.account == acc && !e.line.notice; }),
                         lines_.end());
            return local("Muted " + name + ". /unmute " + name + " to hear them again.");
        }
        return local(arg.empty() ? "Usage: /mute <name>" : "Nobody called " + arg + " has chatted recently.");
    }
    if (cmd == "/unmute") {
        for (const auto& [acc, name] : chat::mutes())
            if (lower(name) == lower(arg)) {
                chat::set_muted(acc, name, false);
                return local("Unmuted " + name + ".");
            }
        return local(arg.empty() ? "Usage: /unmute <name>" : arg + " is not muted.");
    }
    if (cmd == "/mutes") {
        auto m = chat::mutes();
        if (m.empty()) return local("Nobody is muted.");
        std::string s = "Muted:";
        for (const auto& [acc, name] : m) s += " " + name;
        return local(s);
    }
    if (cmd == "/help") {
        local(deck_ ? "Chat goes to the players in your room. Enter sends, B closes."
                    : "Chat goes to the players in your room. Enter sends, Esc closes.");
        return local("/mute <name>  /unmute <name>  /mutes");
    }
    local("Unknown command " + cmd + ". /help lists them.");
}

void ChatBox::draw(SDL_Window* win) {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 ds = io.DisplaySize;
    const float x = ds.x * kLeft, bottom = ds.y * kBottom, width = ds.x * kWidth;
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * kTextScale;

    if (!typing_) {  // newest lines only, no frame, outlined text that fades
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        const double t = now_s();
        float y = bottom;
        size_t shown = 0;
        for (auto it = lines_.rbegin(); it != lines_.rend() && shown < kShownClosed; ++it) {
            const double age = t - it->t;
            if (age >= kShowSec + kFadeSec) break;
            const float a = age < kShowSec ? 1.0f : float(1.0 - (age - kShowSec) / kFadeSec);
            const chat::Line& l = it->line;
            const std::string prefix = l.notice ? "" : l.name + ": ";
            const std::string full = prefix + l.text;
            const float h = font->CalcTextSizeA(size, FLT_MAX, width, full.c_str()).y;
            y -= h + size * 0.15f;
            const ImU32 shadow = ImGui::GetColorU32(ImVec4(0, 0, 0, 0.85f * a));
            const float o = std::max(1.0f, size / 18.0f);
            for (ImVec2 d : {ImVec2(-o, 0), ImVec2(o, 0), ImVec2(0, -o), ImVec2(0, o), ImVec2(o, o)})
                dl->AddText(font, size, ImVec2(x + d.x, y + d.y), shadow, full.c_str(), nullptr, width);
            const ImU32 body = ImGui::GetColorU32(l.notice ? ImVec4(0.75f, 0.75f, 0.75f, a) : ImVec4(1, 1, 1, a));
            dl->AddText(font, size, ImVec2(x, y), body, full.c_str(), nullptr, width);
            if (!prefix.empty()) dl->AddText(font, size, ImVec2(x, y), name_color(l, a), prefix.c_str());
            ++shown;
        }
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(x, bottom), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(size * 0.5f, size * 0.4f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(size * 0.3f, size * 0.25f));
    ImGui::Begin("##chat", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(kTextScale);
    ImGui::BeginChild("##history", ImVec2(0, size * 10), ImGuiChildFlags_None);
    for (const Entry& e : lines_) {
        const chat::Line& l = e.line;
        ImGui::PushTextWrapPos(0.0f);
        if (l.notice) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.75f, 0.75f, 1));
            ImGui::TextWrapped("%s", l.text.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, name_color(l, 1));
            ImGui::TextUnformatted((l.name + ":").c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextWrapped("%s", l.text.c_str());
        }
        ImGui::PopTextWrapPos();
    }
    if (scroll_) ImGui::SetScrollHereY(1.0f), scroll_ = false;
    ImGui::EndChild();
    ImGui::SetNextItemWidth(-1);
    if (focus_) ImGui::SetKeyboardFocusHere(), focus_ = false;
    bool send = ImGui::InputText("##say", buf_, sizeof buf_,
                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackEdit, clamp_length);
    if (keyboard_due_ && ImGui::IsItemActive()) {  // opened by touch under Steam: its keyboard, under the field
        keyboard_due_ = false, keyboard_shown_ = true;
        const ImVec2 p = ImGui::GetItemRectMin(), s = ImGui::GetItemRectSize();
        char url[192];
        std::snprintf(url, sizeof url, "steam://open/keyboard?XPosition=%d&YPosition=%d&Width=%d&Height=%d&Mode=0",
                      int(p.x), int(p.y), std::max(1, int(s.x)), std::max(1, int(s.y)));
        spawn(url);
    }
    // A click / tap elsewhere in the box took the focus off the field: give it back (an on-screen keyboard keeps typing
    // into whatever has it).
    if (!ImGui::IsItemActive() && !ImGui::IsAnyItemActive()) focus_ = true;
    ImGui::TextDisabled("%zu / %zu   Enter: send   Esc / B: close   /help", chat::code_points(buf_), chat::kMaxChars);
    // Gamepad B / Circle (the east face button) closes without sending; never opens anything.
    const bool close = send || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
    ImGui::End();
    ImGui::PopStyleVar(2);
    if (send) submit(buf_);
    if (close) close_box(win);
}
