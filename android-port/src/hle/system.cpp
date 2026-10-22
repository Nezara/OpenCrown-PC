// SceSysmodule, SceCtrl, ScePower. Pad state comes from the host input layer (ctrl_set) or an input script
// (ctrl_load_script); idle without either.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>
#include <vector>

#include "hle/common.h"
#include "hle/ctrl.h"
#include <atomic>
#include <mutex>

namespace hle {
namespace {

// --- SceSysmodule: every module the game can ask for is our own code, so loading only records the id.
constexpr uint32_t SYSMODULE_ERROR_UNLOADED = 0x805a1001;
std::set<uint32_t> loaded;

void LoadModule(Cpu& c) {
  uint32_t id = arg(c, 0) & 0xffff;
  if (loaded.insert(id).second) HLE_NOTE("sceSysmoduleLoadModule(0x%04x)\n", id);
  ret(c, 0);
}
void UnloadModule(Cpu& c) {
  loaded.erase(arg(c, 0) & 0xffff);
  ret(c, 0);
}
void IsLoaded(Cpu& c) { ret(c, loaded.count(arg(c, 0) & 0xffff) ? 0 : SYSMODULE_ERROR_UNLOADED); }

// --- SceCtrl
uint32_t sampling_mode = 0;  // SCE_CTRL_MODE_DIGITAL
const auto t0 = std::chrono::steady_clock::now();

// int sceCtrlSetSamplingMode(mode): returns the previous mode.
void SetSamplingMode(Cpu& c) {
  uint32_t prev = sampling_mode;
  sampling_mode = arg(c, 0);
  ret(c, prev);
}

std::mutex pad_lock;
PadState pad;  // from the host input layer (ctrl_set)

// Input script (ctrl_load_script): sorted by flip.
struct ScriptEvent {
  uint64_t flip;
  uint32_t buttons;
};
std::vector<ScriptEvent> script;
bool script_on = false;
constexpr uint32_t LS_LEFT = 1u << 28, LS_RIGHT = 1u << 29, LS_UP = 1u << 30, LS_DOWN = 1u << 31;  // script only

bool parse_buttons(const std::string& tok, uint32_t& out) {
  static const struct {
    const char* name;
    uint32_t bit;
  } names[] = {{"SELECT", CTRL_SELECT}, {"START", CTRL_START}, {"OPTIONS", CTRL_START}, {"UP", CTRL_UP},
               {"RIGHT", CTRL_RIGHT}, {"DOWN", CTRL_DOWN}, {"LEFT", CTRL_LEFT}, {"L", CTRL_L}, {"L1", CTRL_L},
               {"R", CTRL_R}, {"R1", CTRL_R}, {"TRIANGLE", CTRL_TRIANGLE}, {"CIRCLE", CTRL_CIRCLE},
               {"CROSS", CTRL_CROSS}, {"SQUARE", CTRL_SQUARE}, {"LS_LEFT", LS_LEFT}, {"LS_RIGHT", LS_RIGHT},
               {"LS_UP", LS_UP}, {"LS_DOWN", LS_DOWN}};
  out = 0;
  if (tok.empty()) return false;
  if (std::isdigit(static_cast<unsigned char>(tok[0]))) {
    char* end = nullptr;
    out = uint32_t(std::strtoul(tok.c_str(), &end, 0));
    return !*end;
  }
  for (size_t i = 0; i <= tok.size();) {
    size_t j = tok.find('|', i);
    if (j == std::string::npos) j = tok.size();
    std::string n = tok.substr(i, j - i);
    for (char& ch : n) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    bool found = false;
    for (const auto& b : names)
      if (n == b.name) out |= b.bit, found = true;
    if (!found) return false;
    i = j + 1;
  }
  return true;
}

uint32_t script_buttons_at(uint64_t flip) {
  uint32_t b = 0;
  for (const ScriptEvent& e : script) {
    if (e.flip > flip) break;
    b = e.buttons;
  }
  return b;
}

// int sceCtrlReadBufferPositive(port, SceCtrlData* data, count): fills `count` 0x20-byte records
// {u64 timestamp, u32 buttons, u8 lx, ly, rx, ry, ...}.
void ReadBufferPositive(Cpu& c) {
  uint32_t data = arg(c, 1);
  int32_t count = int32_t(arg(c, 2));
  if (count < 1 || count > 64 || !c.mem().valid(data, 0x20u * count)) return ret(c, 0x80340001);  // unverified code
  uint64_t us = uint64_t(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
  PadState s;
  {
    std::lock_guard lk(pad_lock);
    s = pad;
    if (script_on) {
      uint32_t b = script_buttons_at(display_flip_count());
      s = PadState{};
      s.buttons = b & 0xffff;
      s.lx = b & LS_LEFT ? 0 : b & LS_RIGHT ? 255 : 128;
      s.ly = b & LS_UP ? 0 : b & LS_DOWN ? 255 : 128;
    }
  }
  for (int32_t i = 0; i < count; i++) {
    uint8_t* p = c.mem().ptr(data + 0x20u * i);
    if (!p) break;
    std::memset(p, 0, 0x20);
    std::memcpy(p, &us, 8);
    std::memcpy(p + 8, &s.buttons, 4);
    p[0x0c] = s.lx;
    p[0x0d] = s.ly;
    p[0x0e] = s.rx;
    p[0x0f] = s.ry;
  }
  ret(c, uint32_t(count));
}

// --- ScePower
void SetConfigurationMode(Cpu& c) {
  HLE_NOTE("scePowerSetConfigurationMode(0x%x)\n", arg(c, 0));
  ret(c, 0);
}

}  // namespace

void ctrl_set(const PadState& s) {
  std::lock_guard lk(pad_lock);
  pad = s;
}

bool ctrl_load_script(const std::string& text, std::string& err) {
  std::vector<ScriptEvent> ev;
  std::istringstream in(text);
  std::string line;
  for (int n = 1; std::getline(in, line); n++) {
    if (size_t h = line.find('#'); h != std::string::npos) line.resize(h);
    std::istringstream ls(line);
    std::string f, b, extra;
    if (!(ls >> f)) continue;
    char* end = nullptr;
    uint64_t flip = std::strtoull(f.c_str(), &end, 10);
    ScriptEvent e{flip, 0};
    if (*end || !(ls >> b) || (ls >> extra) || !parse_buttons(b, e.buttons)) {
      err = "line " + std::to_string(n) + ": " + line;
      return false;
    }
    ev.push_back(e);
  }
  std::stable_sort(ev.begin(), ev.end(), [](const ScriptEvent& a, const ScriptEvent& b) { return a.flip < b.flip; });
  std::lock_guard lk(pad_lock);
  script = std::move(ev);
  script_on = true;
  HLE_NOTE("input script: %zu events, live pad input off\n", script.size());
  return true;
}

void register_sysmodule() {
  REG(sceSysmoduleLoadModule, LoadModule);
  REG(sceSysmoduleUnloadModule, UnloadModule);
  REG(sceSysmoduleIsLoaded, IsLoaded);
}
void register_ctrl() {
  REG(sceCtrlSetSamplingMode, SetSamplingMode);
  REG(sceCtrlReadBufferPositive, ReadBufferPositive);
}
void register_power() { REG(scePowerSetConfigurationMode, SetConfigurationMode); }

}  // namespace hle
