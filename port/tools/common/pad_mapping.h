// Controller setup: the SDL mapping lines the launcher writes to gamepad_mappings.txt (user dir) and dcp_run loads.
// For pads SDL maps wrongly or not at all. No SDL here, so tests can use it.
#pragma once
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace dcp::padmap {

enum class Kind { Button, Trigger, Dpad, Stick };

struct Step {
    const char* target;  // SDL mapping field
    const char* prompt;
    Kind kind;
};

inline constexpr Step kSteps[] = {
    {"a", "Press the button you want as Cross (bottom button)", Kind::Button},
    {"b", "Press the button you want as Circle (right button)", Kind::Button},
    {"x", "Press the button you want as Square (left button)", Kind::Button},
    {"y", "Press the button you want as Triangle (top button)", Kind::Button},
    {"leftshoulder", "Press L1 (left bumper, L)", Kind::Button},
    {"rightshoulder", "Press R1 (right bumper, R)", Kind::Button},
    {"lefttrigger", "Press L2 (left trigger, ZL)", Kind::Trigger},
    {"righttrigger", "Press R2 (right trigger, ZR)", Kind::Trigger},
    {"back", "Press Touchpad (Back, Minus)", Kind::Button},
    {"start", "Press Options (Start, Plus)", Kind::Button},
    {"leftstick", "Click the left stick (L3)", Kind::Button},
    {"rightstick", "Click the right stick (R3)", Kind::Button},
    {"dpup", "Press D-pad up", Kind::Dpad},
    {"dpdown", "Press D-pad down", Kind::Dpad},
    {"dpleft", "Press D-pad left", Kind::Dpad},
    {"dpright", "Press D-pad right", Kind::Dpad},
    {"leftx", "Push the left stick right", Kind::Stick},
    {"lefty", "Push the left stick down", Kind::Stick},
    {"rightx", "Push the right stick right", Kind::Stick},
    {"righty", "Push the right stick down", Kind::Stick},
};
inline constexpr size_t kStepCount = sizeof kSteps / sizeof kSteps[0];

// An axis pressed for a button-like step. Triggers rest at one end and use the whole axis; others use the half moved.
inline std::string button_axis_token(int axis, int rest, int value) {
    const std::string n = std::to_string(axis);
    if (rest <= -24000) return "a" + n;
    if (rest >= 24000) return "a" + n + "~";
    return (value > rest ? "+a" : "-a") + n;
}

// An axis pushed for a stick step, towards the positive end of the target.
inline std::string stick_axis_token(int axis, int value) {
    return "a" + std::to_string(axis) + (value > 0 ? "" : "~");
}

inline std::string hat_token(int hat, int bit) { return "h" + std::to_string(hat) + "." + std::to_string(bit); }

// guid,name,target:input,...,platform:<platform>,
inline std::string build_line(const std::string& guid, std::string name, const std::string& platform,
                              const std::vector<std::pair<std::string, std::string>>& inputs) {
    for (char& ch : name)
        if (ch == ',' || ch == '\n' || ch == '\r') ch = ' ';
    std::string line = guid + "," + (name.empty() ? "Controller" : name) + ",";
    for (const auto& [target, input] : inputs) line += target + ":" + input + ",";
    return line + "platform:" + platform + ",";
}

// The file text with this controller's line replaced by `line` (empty = removed).
inline std::string merge_line(const std::string& text, const std::string& guid, const std::string& line) {
    std::string out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string l = text.substr(at, end - at);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!l.empty() && l.rfind(guid + ",", 0) != 0) out += l + "\n";
        at = end + 1;
    }
    if (!line.empty()) out += line + "\n";
    return out;
}

inline bool has_line(const std::string& text, const std::string& guid) {
    const std::string key = guid + ",";
    return text.rfind(key, 0) == 0 || text.find("\n" + key) != std::string::npos;
}

}  // namespace dcp::padmap
