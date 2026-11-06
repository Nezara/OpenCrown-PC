// Launcher window "Controller setup": press each button once, the result goes to gamepad_mappings.txt (pad_mapping.h).
// Gamepad support is only initialised while the window is open, so it never holds a controller from the game.
#pragma once
#include <SDL3/SDL.h>
#include <imgui.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../common/pad_mapping.h"

namespace dcp {

class PadSetup {
public:
    void open(const std::string& path) {
        path_ = path;
        ready_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        message_ = ready_ ? "" : std::string("Controllers are unavailable: ") + SDL_GetError();
        std::ifstream f(path_);
        std::stringstream ss;
        ss << f.rdbuf();
        file_ = ss.str();
        open_ = true;
        ImGui::OpenPopup(kTitle);
    }

    // Waiting for a press: launcher menus must not act on the controller meanwhile.
    bool capturing() const { return open_ && joy_ && step_ < padmap::kStepCount; }

    void draw(float scale, const ImVec4& dim) {
        if (!open_) return;
        ImGui::SetNextWindowSize(ImVec2(620 * scale, 300 * scale), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) return;
        if (ready_) SDL_UpdateJoysticks();
        if (joy_ && !SDL_JoystickConnected(joy_)) {
            release();
            message_ = "The controller was disconnected.";
        }
        if (!joy_) draw_picker(dim);
        else if (step_ < padmap::kStepCount) draw_step(dim);
        else draw_review(dim);
        ImGui::EndPopup();
    }

private:
    static constexpr const char* kTitle = "Controller setup";

    void close() {
        release();
        if (ready_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        ready_ = false;
        open_ = false;
        ImGui::CloseCurrentPopup();
    }

    void release() {
        if (test_) SDL_CloseGamepad(test_);
        if (joy_) SDL_CloseJoystick(joy_);
        test_ = nullptr, joy_ = nullptr;
    }

    std::string guid() const {
        char buf[33] = {};
        SDL_GUIDToString(SDL_GetJoystickGUID(joy_), buf, sizeof buf);
        return buf;
    }

    bool write_file(const std::string& text) {
        std::ofstream o(path_, std::ios::binary | std::ios::trunc);
        o << text;
        if (!o.good()) return false;
        file_ = text;
        return true;
    }

    void draw_picker(const ImVec4& dim) {
        ImGui::TextWrapped("Use this if buttons do the wrong thing in the game, as some third-party controllers do.");
        ImGui::TextColored(dim, "Pick a controller:");
        int n = 0;
        SDL_JoystickID* ids = ready_ ? SDL_GetJoysticks(&n) : nullptr;
        if (ready_ && n == 0) ImGui::TextColored(dim, "No controller found. Plug one in or pair it now.");
        for (int i = 0; i < n; ++i) {
            const char* name = SDL_GetJoystickNameForID(ids[i]);
            char label[200];
            std::snprintf(label, sizeof label, "%s (%04x:%04x)##%d", name ? name : "Controller",
                          SDL_GetJoystickVendorForID(ids[i]), SDL_GetJoystickProductForID(ids[i]), i);
            if (ImGui::Button(label)) {
                joy_ = SDL_OpenJoystick(ids[i]);
                message_ = joy_ ? "" : std::string("Could not open it: ") + SDL_GetError();
                step_ = 0, armed_ = false, got_.clear(), saved_ = false;
            }
        }
        SDL_free(ids);
        if (!message_.empty()) ImGui::TextColored(dim, "%s", message_.c_str());
        if (ImGui::Button("Close")) close();
    }

    void draw_step(const ImVec4& dim) {
        const padmap::Step& s = padmap::kSteps[step_];
        ImGui::TextColored(dim, "%s: step %d of %d", SDL_GetJoystickName(joy_), int(step_) + 1, int(padmap::kStepCount));
        ImGui::Spacing();
        ImGui::TextUnformatted(s.prompt);
        ImGui::Spacing();
        if (!message_.empty()) ImGui::TextColored(dim, "%s", message_.c_str());
        poll_step(s);
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeightWithSpacing() * 1.5f);
        if (ImGui::Button("Skip")) step_++, armed_ = false, message_.clear();
        ImGui::SameLine();
        if (step_ > 0 && ImGui::Button("Back")) {
            step_--, armed_ = false, message_.clear();
            if (!got_.empty() && got_.back().first == padmap::kSteps[step_].target) got_.pop_back();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) release(), message_.clear();
    }

    // What is held right now (empty = nothing). Waits for everything to be let go between steps.
    void poll_step(const padmap::Step& s) {
        std::string input;
        bool any = false;
        for (int i = 0, n = SDL_GetNumJoystickButtons(joy_); i < n; ++i)
            if (SDL_GetJoystickButton(joy_, i)) {
                any = true;
                if (input.empty() && s.kind != padmap::Kind::Stick) input = "b" + std::to_string(i);
            }
        for (int i = 0, n = SDL_GetNumJoystickHats(joy_); i < n; ++i) {
            const int v = SDL_GetJoystickHat(joy_, i);
            if (v != SDL_HAT_CENTERED) any = true;
            if (input.empty() && s.kind != padmap::Kind::Stick && (v == 1 || v == 2 || v == 4 || v == 8))
                input = padmap::hat_token(i, v);
        }
        for (int i = 0, n = SDL_GetNumJoystickAxes(joy_); i < n; ++i) {
            Sint16 rest = 0;
            SDL_GetJoystickAxisInitialState(joy_, i, &rest);
            const int v = SDL_GetJoystickAxis(joy_, i);
            if (v - rest > 8000 || rest - v > 8000) any = true;
            if (!(v - rest > 20000 || rest - v > 20000) || !input.empty()) continue;
            const bool centred = rest > -8000 && rest < 8000;
            if (s.kind == padmap::Kind::Stick) {
                if (centred) input = padmap::stick_axis_token(i, v);
            } else if (s.kind != padmap::Kind::Button || !centred) {
                input = padmap::button_axis_token(i, rest, v);
            }
        }
        if (!any) armed_ = true;
        if (!armed_ || input.empty()) return;
        armed_ = false;
        for (const auto& [target, used] : got_)
            if (used == input) {
                message_ = "That is already used. Press a different one.";
                return;
            }
        got_.emplace_back(s.target, input);
        message_.clear();
        step_++;
    }

    void draw_review(const ImVec4& dim) {
        if (!saved_) {
            ImGui::TextWrapped("All done for %s.", SDL_GetJoystickName(joy_));
            if (ImGui::Button("Save")) {
                const std::string id = guid();
                const std::string line = padmap::build_line(id, SDL_GetJoystickName(joy_), SDL_GetPlatform(), got_);
                if (write_file(padmap::merge_line(file_, id, line)) && SDL_AddGamepadMapping(line.c_str()) >= 0) {
                    saved_ = true;
                    message_ = "Saved. The game uses this the next time it starts.";
                    test_ = SDL_OpenGamepad(SDL_GetJoystickID(joy_));
                } else {
                    message_ = "Could not save " + path_;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Start over")) step_ = 0, armed_ = false, got_.clear(), message_.clear();
        } else {
            static const std::pair<SDL_GamepadButton, const char*> kNames[] = {
                {SDL_GAMEPAD_BUTTON_SOUTH, "Cross"}, {SDL_GAMEPAD_BUTTON_EAST, "Circle"},
                {SDL_GAMEPAD_BUTTON_WEST, "Square"}, {SDL_GAMEPAD_BUTTON_NORTH, "Triangle"},
                {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "L1"}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "R1"},
                {SDL_GAMEPAD_BUTTON_BACK, "Touchpad"}, {SDL_GAMEPAD_BUTTON_START, "Options"},
                {SDL_GAMEPAD_BUTTON_LEFT_STICK, "L3"}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, "R3"},
                {SDL_GAMEPAD_BUTTON_DPAD_UP, "Up"}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, "Down"},
                {SDL_GAMEPAD_BUTTON_DPAD_LEFT, "Left"}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "Right"},
            };
            std::string held;
            if (test_) {
                for (const auto& [b, name] : kNames)
                    if (SDL_GetGamepadButton(test_, b)) held += std::string(held.empty() ? "" : ", ") + name;
                if (SDL_GetGamepadAxis(test_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000) held += held.empty() ? "L2" : ", L2";
                if (SDL_GetGamepadAxis(test_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000) held += held.empty() ? "R2" : ", R2";
            }
            ImGui::TextWrapped("Try your buttons. Pressed: %s", held.empty() ? "(nothing)" : held.c_str());
            if (ImGui::Button("Done")) release(), message_.clear(), close();
        }
        if (joy_ && !saved_ && padmap::has_line(file_, guid())) {
            ImGui::SameLine();
            if (ImGui::Button("Remove saved setup")) {
                message_ = write_file(padmap::merge_line(file_, guid(), "")) ? "Removed. Restart the launcher to test without it."
                                                                             : "Could not save " + path_;
            }
        }
        if (open_ && !message_.empty()) ImGui::TextColored(dim, "%s", message_.c_str());
    }

    std::string path_, file_, message_;
    std::vector<std::pair<std::string, std::string>> got_;
    SDL_Joystick* joy_ = nullptr;
    SDL_Gamepad* test_ = nullptr;
    size_t step_ = 0;
    bool open_ = false, ready_ = false, armed_ = false, saved_ = false;
};

}  // namespace dcp
