// See chat_box.h.
#include "chat_box.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>
#include <map>

#include "ime_box.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "sdl_input.h"

namespace chat = dcp::net::chat;

namespace {

// Placement, as fractions of the window: bottom-left corner of the chat area, its width.
constexpr float kLeft = 0.02f, kBottom = 0.80f, kWidth = 0.36f;
constexpr double kShowSec = 8.0, kFadeSec = 1.5;  // a closed line stays this long, then fades out
constexpr size_t kShownClosed = 5;                 // newest lines shown while closed
constexpr size_t kHistory = 60;
constexpr float kTextScale = 0.85f;  // of the ImGui font size (the name box's)

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

}  // namespace

void ChatBox::init(bool imgui_ok, const std::string& user_dir) {
    ok_ = imgui_ok && chat::enabled();
    if (!ok_) return;
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

void ChatBox::handle_event(const SDL_Event& e, bool ime_busy) {
    if (!ok_) return;
    if (typing_) {
        ImGui_ImplSDL3_ProcessEvent(&e);
        return;
    }
    if (e.type != SDL_EVENT_KEY_DOWN || e.key.repeat || !key_ || ime_busy || blocking_) return;
    if (int(e.key.scancode) != key_ && !(key_ == SDL_SCANCODE_RETURN && e.key.scancode == SDL_SCANCODE_KP_ENTER)) return;
    if (!chat::in_room()) {
        if (hinted_) local("You are not in an online room.");  // offline players never see chat lines
        return;
    }
    typing_ = focus_ = scroll_ = true;
    buf_[0] = 0;
    // ImGui only sees input while the box is open: whatever it last saw held (the Enter that sent the last line) is
    // still down there and would repeat into this field.
    ImGuiIO& io = ImGui::GetIO();
    io.ClearEventsQueue();
    io.ClearInputKeys();
    io.ClearInputMouse();
}

bool ChatBox::update(SDL_Window* win, bool ime_open) {
    if (!ok_) return false;
    const bool in = chat::in_room();
    if (in && !was_in_room_ && !hinted_) {
        hinted_ = true;
        std::string name = key_ ? SDL_GetScancodeName(SDL_Scancode(key_)) : "";
        if (name == "Return") name = "Enter";
        local(!name.empty() ? "Press " + name + " to talk."
                    : std::string("Room chat: bind the \"chat\" key in the launcher's controls to talk."));
    }
    was_in_room_ = in;
    for (chat::Line l; chat::take(&l);) {
        lines_.push_back({std::move(l), now_s()});
        while (lines_.size() > kHistory) lines_.pop_front();
        scroll_ = true;
    }
    if (typing_ && (ime_open || !in)) {  // the game's own name box wins; leaving the room ends the chat
        typing_ = false, blocking_ = true;
        SDL_StopTextInput(win);
    }
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
        local("Chat goes to the players in your room. Enter sends, Esc closes.");
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
    ImGui::TextDisabled("%zu / %zu   Enter: send   Esc: close   /help", chat::code_points(buf_), chat::kMaxChars);
    const bool close = send || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    ImGui::End();
    ImGui::PopStyleVar(2);
    if (send) submit(buf_);
    if (close) {
        typing_ = false, blocking_ = true;
        buf_[0] = 0;
        SDL_StopTextInput(win);
    }
}
