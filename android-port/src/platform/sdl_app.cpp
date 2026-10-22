#include "platform/sdl_app.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#include <vector>

#include "gpu/vulkan.h"
#include "hle/audio.h"
#include "hle/ctrl.h"
#include "hle/ime.h"
#include "hle/touch.h"
#include "platform/text_entry.h"
#include "platform/touch_controls.h"

namespace platform {
namespace {

// Default bindings: the Pro port's scheme (tools/common/input_defaults.h there) mapped to the Vita's buttons.
// "action = input[, input...]": SDL key names or Mouse1..Mouse5 (left, right, middle, back, forward).
constexpr const char* kDefaults = R"(# OpenCrown Mobile input bindings. Delete this file to restore defaults.
# action = input[, input...]   inputs: SDL key names (W, Space, Left Shift, Escape, Up, F1...) or Mouse1..Mouse5
move_up = W
move_down = S
move_left = A
move_right = D
look_up = I
look_down = K
look_left = J
look_right = L
dpad_up = Up
dpad_down = Down
dpad_left = Left
dpad_right = Right
square = Mouse1
circle = Mouse2
triangle = E
cross = Space
l = Left Shift
r = C
start = Escape
select = Tab
# Fairy pointer = the front touch screen: while fairy is held, the mouse position is the touch point and the left
# button touches (no Square attacks while held). fairy_toggle = optional on/off key.
fairy = Mouse3
fairy_toggle =
# On-screen touch controls (touch screens, shown while no gamepad is connected): opacity 0 (hidden) .. 1
touch_opacity = 0.6
# Controller buttons (the launcher's Controls page): SDL gamepad names by position - a bottom, b right, x left, y top,
# leftshoulder, rightshoulder, lefttrigger, righttrigger, leftstick, rightstick, start, back, dpup ... ; empty = none
pad_cross = a
pad_square = x
pad_circle = b
pad_triangle = y
pad_l = leftshoulder
pad_r = rightshoulder
pad_start = start
pad_select = back
pad_dpad_up = dpup
pad_dpad_down = dpdown
pad_dpad_left = dpleft
pad_dpad_right = dpright
)";

enum Action {
  MOVE_UP, MOVE_DOWN, MOVE_LEFT, MOVE_RIGHT, LOOK_UP, LOOK_DOWN, LOOK_LEFT, LOOK_RIGHT,
  DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT, SQUARE, CIRCLE, TRIANGLE, CROSS, L, R, START, SELECT,
  FAIRY, FAIRY_TOGGLE, ACTION_COUNT
};
const char* const kNames[ACTION_COUNT] = {
    "move_up", "move_down", "move_left", "move_right", "look_up", "look_down", "look_left", "look_right",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right", "square", "circle", "triangle", "cross", "l", "r",
    "start", "select", "fairy", "fairy_toggle"};

// An input: a scancode, or a mouse button as -1..-5.
std::vector<int> bindings[ACTION_COUNT];
float touch_opacity = 0.6f;

// Controller bindings (pad_* keys): one gamepad input per game button.
enum PadAction {
  P_CROSS, P_SQUARE, P_CIRCLE, P_TRIANGLE, P_L, P_R, P_START, P_SELECT, P_UP, P_DOWN, P_LEFT, P_RIGHT, PAD_COUNT
};
const char* const kPadNames[PAD_COUNT] = {
    "pad_cross", "pad_square", "pad_circle", "pad_triangle", "pad_l", "pad_r", "pad_start", "pad_select",
    "pad_dpad_up", "pad_dpad_down", "pad_dpad_left", "pad_dpad_right"};
const char* const kPadDefaults[PAD_COUNT] = {
    "a", "x", "b", "y", "leftshoulder", "rightshoulder", "start", "back", "dpup", "dpdown", "dpleft", "dpright"};
constexpr int kPadAxis = 1000;  // pad_bind value of a trigger: kPadAxis + SDL_GamepadAxis
int pad_bind[PAD_COUNT];        // SDL_GamepadButton, kPadAxis + axis, or -1 = none

int parse_pad(const std::string& s) {
  if (s.empty()) return -1;
  if (SDL_GamepadButton b = SDL_GetGamepadButtonFromString(s.c_str()); b != SDL_GAMEPAD_BUTTON_INVALID) return int(b);
  if (SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(s.c_str());
      a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
    return kPadAxis + int(a);
  return -2;
}

std::string trim(std::string s) {
  s.erase(0, s.find_first_not_of(" \t\r"));
  s.erase(s.find_last_not_of(" \t\r") + 1);
  return s;
}

void load_bindings(const std::string& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream(path) << kDefaults;
  }
  std::ifstream f(path);
  std::string text = kDefaults;
  if (f) {
    std::stringstream all;
    all << f.rdbuf();
    text = all.str();
  }
  for (int p = 0; p < PAD_COUNT; p++) pad_bind[p] = parse_pad(kPadDefaults[p]);  // files from before pad_* keys
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    if (line.empty() || line[0] == '#') continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = trim(line.substr(0, eq));
    if (key == "touch_opacity") {
      touch_opacity = std::strtof(trim(line.substr(eq + 1)).c_str(), nullptr);
      continue;
    }
    if (int p = int(std::find_if(kPadNames, kPadNames + PAD_COUNT, [&](const char* n) { return key == n; }) - kPadNames);
        p < PAD_COUNT) {
      std::string v = trim(line.substr(eq + 1));
      int b = parse_pad(v);
      if (b == -2) std::fprintf(stderr, "[input] %s: unknown controller input \"%s\"\n", key.c_str(), v.c_str());
      else pad_bind[p] = b;
      continue;
    }
    int a = int(std::find_if(kNames, kNames + ACTION_COUNT, [&](const char* n) { return key == n; }) - kNames);
    if (a == ACTION_COUNT) continue;
    bindings[a].clear();
    std::istringstream items(line.substr(eq + 1));
    for (std::string item; std::getline(items, item, ',');) {
      item = trim(item);
      if (item.empty()) continue;
      if (item.size() == 6 && item.compare(0, 5, "Mouse") == 0 && item[5] >= '1' && item[5] <= '5') {
        bindings[a].push_back(-(item[5] - '0'));
      } else if (SDL_Scancode sc = SDL_GetScancodeFromName(item.c_str()); sc != SDL_SCANCODE_UNKNOWN) {
        bindings[a].push_back(int(sc));
      } else {
        std::fprintf(stderr, "[input] %s: unknown input \"%s\"\n", key.c_str(), item.c_str());
      }
    }
  }
}

struct Input {
  bool keys[SDL_SCANCODE_COUNT] = {};
  bool mouse[6] = {};
  bool fairy_on = false;  // toggle state
  float mx = 0, my = 0;   // mouse in window pixels
  SDL_Gamepad* pad = nullptr;
  TouchControls touch;
  TextEntry entry;              // the game's text-entry dialog while open: takes the input, the game sees none
  uint32_t suppress = 0;        // pad bits held over from the dialog: kept from the game until released
  bool entry_was_open = false;
  bool touch_enabled = false;  // on-screen controls in use (hidden while a gamepad is connected)
  bool touch_shown() const { return touch_enabled && !pad && touch_opacity > 0; }

  bool held(Action a) const {
    for (int i : bindings[a])
      if (i < 0 ? mouse[-i] : keys[i]) return true;
    return false;
  }
};

uint8_t axis(bool neg, bool pos, int16_t pad_value) {
  int v = 128 + (pos ? 127 : 0) - (neg ? 128 : 0);
  if (!neg && !pos && std::abs(pad_value) > 8000) v = 128 + pad_value / 256;  // gamepad stick outside its dead zone
  return uint8_t(std::clamp(v, 0, 255));
}

void publish(SDL_Window* win, Input& in) {
  hle::PadState s;
  auto on = [&](bool b, uint32_t bit) { if (b) s.buttons |= bit; };
  SDL_Gamepad* g = in.pad;
  auto gb = [&](PadAction a) {
    const int b = pad_bind[a];
    if (!g || b < 0) return false;
    return b >= kPadAxis ? SDL_GetGamepadAxis(g, SDL_GamepadAxis(b - kPadAxis)) > 16000
                         : SDL_GetGamepadButton(g, SDL_GamepadButton(b));
  };
  auto ga = [&](SDL_GamepadAxis a) -> int16_t { return g ? SDL_GetGamepadAxis(g, a) : 0; };
  bool fairy = in.fairy_on || in.held(FAIRY);

  on(in.held(DPAD_UP) || gb(P_UP), hle::CTRL_UP);
  on(in.held(DPAD_DOWN) || gb(P_DOWN), hle::CTRL_DOWN);
  on(in.held(DPAD_LEFT) || gb(P_LEFT), hle::CTRL_LEFT);
  on(in.held(DPAD_RIGHT) || gb(P_RIGHT), hle::CTRL_RIGHT);
  // In fairy mode the mouse buttons belong to the pointer.
  bool square = fairy ? false : in.held(SQUARE);
  on(square || gb(P_SQUARE), hle::CTRL_SQUARE);
  on((!fairy && in.held(CIRCLE)) || gb(P_CIRCLE), hle::CTRL_CIRCLE);
  on(in.held(TRIANGLE) || gb(P_TRIANGLE), hle::CTRL_TRIANGLE);
  on(in.held(CROSS) || gb(P_CROSS), hle::CTRL_CROSS);
  on(in.held(L) || gb(P_L), hle::CTRL_L);
  on(in.held(R) || gb(P_R), hle::CTRL_R);
  on(in.held(START) || gb(P_START), hle::CTRL_START);
  on(in.held(SELECT) || gb(P_SELECT), hle::CTRL_SELECT);
  s.lx = axis(in.held(MOVE_LEFT), in.held(MOVE_RIGHT), ga(SDL_GAMEPAD_AXIS_LEFTX));
  s.ly = axis(in.held(MOVE_UP), in.held(MOVE_DOWN), ga(SDL_GAMEPAD_AXIS_LEFTY));
  s.rx = axis(in.held(LOOK_LEFT), in.held(LOOK_RIGHT), ga(SDL_GAMEPAD_AXIS_RIGHTX));
  s.ry = axis(in.held(LOOK_UP), in.held(LOOK_DOWN), ga(SDL_GAMEPAD_AXIS_RIGHTY));
  in.touch.apply(s);
  // Text entry: the game gets a neutral pad while the dialog is open, and a button that was held then (Start / Enter
  // pressed to confirm, the finger on Cross) only once it was released.
  bool entry = in.entry.open();
  in.suppress &= s.buttons;
  if (entry || in.entry_was_open) in.suppress |= s.buttons;
  in.entry_was_open = entry;
  s.buttons &= ~in.suppress;
  if (entry) s.lx = s.ly = s.rx = s.ry = 128;
  hle::ctrl_set(s);

  // Front touch: the mouse in fairy mode, mapped through the letterboxed 960x544 picture to panel units.
  std::vector<hle::TouchPoint> touches = entry ? std::vector<hle::TouchPoint>{} : in.touch.front_touches();
  if (!entry && fairy && in.mouse[1]) {
    int w = 1, h = 1;
    SDL_GetWindowSizeInPixels(win, &w, &h);
    float scale = std::min(w / 960.0f, h / 544.0f);
    float x0 = (w - 960 * scale) / 2, y0 = (h - 544 * scale) / 2;
    float gx = (in.mx - x0) / scale, gy = (in.my - y0) / scale;
    if (gx >= 0 && gx < 960 && gy >= 0 && gy < 544) {
      hle::TouchPoint p;
      p.id = uint8_t(touches.size());
      p.x = int16_t(gx * 2);
      p.y = int16_t(gy * 2);
      touches.push_back(p);
    }
  }
  hle::touch_set(0, touches);
  if (entry) {
    int w = 1, h = 1;
    SDL_GetWindowSizeInPixels(win, &w, &h);
    gpu::set_overlay(in.entry.geometry(float(w), float(h)));
  } else {
    gpu::set_overlay(in.touch_shown() ? in.touch.geometry() : std::vector<gpu::OverlayVertex>{});
  }
}

}  // namespace

int run_with_window(const AppOptions& opt, const std::function<void()>& guest) {
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");  // fingers are handled as fingers, never as Mouse1
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
    std::fprintf(stderr, "[sdl] init failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef __ANDROID__
  // Landscape only (SDL would otherwise ask for "full user" orientation and override the manifest), and immersive
  // fullscreen: no status / navigation bar over the picture.
  SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
  flags |= SDL_WINDOW_FULLSCREEN;
#endif
  SDL_Window* win = SDL_CreateWindow(opt.title.c_str(),960 * opt.scale, 544 * opt.scale, flags);
  if (!win) {
    std::fprintf(stderr, "[sdl] window: %s\n", SDL_GetError());
    return 1;
  }
  gpu::VulkanSetup vs;
  vs.get_instance_proc_addr = reinterpret_cast<void*>(SDL_Vulkan_GetVkGetInstanceProcAddr());
  Uint32 n = 0;
  const char* const* ext = SDL_Vulkan_GetInstanceExtensions(&n);
  vs.instance_extensions.assign(ext, ext + n);
  vs.create_surface = [win](void* instance) -> uint64_t {
    VkSurfaceKHR s = 0;
    return SDL_Vulkan_CreateSurface(win, static_cast<VkInstance>(instance), nullptr, &s) ? uint64_t(reinterpret_cast<uintptr_t>(s)) : 0;
  };
  vs.drawable_size = [win](uint32_t& w, uint32_t& h) {
    int iw = 0, ih = 0;
    SDL_GetWindowSizeInPixels(win, &iw, &ih);
    w = uint32_t(iw);
    h = uint32_t(ih);
  };
  vs.validation = opt.validation;
  vs.cache_dir = opt.cache_dir;
  std::string err;
  auto r = gpu::make_vulkan_renderer(vs, err);
  if (!r) {
    std::fprintf(stderr, "[vulkan] %s\n", err.c_str());
    return 1;
  }
  gpu::set_renderer(std::move(r));
  // Android takes the window away in the background and gives a new one on return. An event watch runs at once on
  // the thread that pumps events, before SDL blocks the loop for the pause.
  SDL_AddEventWatch(
      [](void*, SDL_Event* e) -> bool {
        if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND) gpu::renderer().surface_lost();
        else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) gpu::renderer().surface_restored();
        return true;
      },
      nullptr);
  load_bindings(opt.input_ini);

  // Sound: the game's 48 kHz s16 stereo grains (one output port: the CRI Atom mixer) into an SDL stream. The game's
  // audio thread is paced by this queue (hle/audio.cpp keeps ~40 ms in it). A grain that finds the queue empty is
  // an underrun (the game's mixer was too slow): counted and logged every 10 s, since that is what crackles.
  SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 48000};
  if (SDL_AudioStream* as = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr)) {
    SDL_ResumeAudioStreamDevice(as);
    hle::audio_set_sink(
        [as](const int16_t* lr, uint32_t frames) {
          static uint64_t grains = 0, underruns = 0;
          if (grains++ > 50 && SDL_GetAudioStreamQueued(as) == 0) underruns++;
          if (grains % 1875 == 0 && underruns)  // 1875 grains of 256 frames = 10 s
            std::fprintf(stderr, "[audio] %llu underruns in %llu grains\n", (unsigned long long)underruns,
                         (unsigned long long)grains);
          SDL_PutAudioStreamData(as, lr, int(frames * 4));
        },
        [as] { return uint32_t(std::max(0, SDL_GetAudioStreamQueued(as)) / 4); });
  } else {
    std::fprintf(stderr, "[sdl] no audio device: %s\n", SDL_GetError());
  }

  hle::ime_set_host(true);  // the game's text-entry dialog waits for platform/text_entry
  std::thread([&guest] {
    guest();
    gpu::renderer().save_caches();
    std::fflush(stdout);
    std::_Exit(0);
  }).detach();

  Input in;
  in.touch_enabled = opt.touch_controls;
  in.touch.set_opacity(touch_opacity);
  auto relayout = [&] {
    int w = 1, h = 1;
    SDL_GetWindowSizeInPixels(win, &w, &h);
    in.touch.resize(float(w), float(h));
  };
  relayout();
  // A finger in window pixels; the on-screen controls see it only while shown, else it is a front-panel touch.
  auto finger = [&](int64_t id, Uint32 type, float x, float y) {
    in.touch.set_controls(in.touch_shown());
    if (type == SDL_EVENT_FINGER_DOWN) in.touch.down(id, x, y);
    else if (type == SDL_EVENT_FINGER_MOTION) in.touch.move(id, x, y);
    else in.touch.up(id);
  };
  publish(win, in);
  for (bool running = true; running;) {
    SDL_Event e;
    bool changed = false;
    // Poll, then sleep: SDL has no blocking wait on Android, so SDL_WaitEventTimeout spun a whole core there (the
    // Retroid's T618 has only two fast cores, shared with the guest and GPU threads). 4 ms = input read at ~250 Hz.
    if (!SDL_PollEvent(&e)) {
      SDL_DelayNS(4'000'000);
      bool entry_changed = in.entry.sync(win);
      if (in.pad || entry_changed) publish(win, in);
      continue;
    }
    {
      do {
        if (in.entry.event(win, e)) {  // typing / OK / Cancel; releases fall through to the key state below
          changed = true;
          continue;
        }
        switch (e.type) {
          case SDL_EVENT_QUIT: running = false; break;
          case SDL_EVENT_KEY_DOWN:
          case SDL_EVENT_KEY_UP:
            if (e.key.scancode < SDL_SCANCODE_COUNT) in.keys[e.key.scancode] = e.type == SDL_EVENT_KEY_DOWN;
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat)
              for (int i : bindings[FAIRY_TOGGLE])
                if (i == int(e.key.scancode)) in.fairy_on = !in.fairy_on;
            changed = true;
            break;
          case SDL_EVENT_FINGER_DOWN:
          case SDL_EVENT_FINGER_MOTION:
          case SDL_EVENT_FINGER_UP:
          case SDL_EVENT_FINGER_CANCELED: {
            int w = 1, h = 1;
            SDL_GetWindowSizeInPixels(win, &w, &h);
            finger(int64_t(e.tfinger.fingerID), e.type, e.tfinger.x * w, e.tfinger.y * h);
            changed = true;
            break;
          }
          case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            relayout();
            changed = true;
            break;
          case SDL_EVENT_MOUSE_BUTTON_DOWN:
          case SDL_EVENT_MOUSE_BUTTON_UP:
            if (opt.touch_controls && e.button.button == 1 && e.button.which != SDL_TOUCH_MOUSEID) {  // test mode
              float d = SDL_GetWindowPixelDensity(win);
              finger(-1, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? SDL_EVENT_FINGER_DOWN : SDL_EVENT_FINGER_UP,
                     e.button.x * d, e.button.y * d);
              changed = true;
              break;
            }
            if (e.button.button >= 1 && e.button.button <= 5) {
              // SDL: 2 = middle, 3 = right; ours: Mouse2 = right, Mouse3 = middle
              int b = e.button.button == 2 ? 3 : e.button.button == 3 ? 2 : e.button.button;
              in.mouse[b] = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            }
            changed = true;
            break;
          case SDL_EVENT_MOUSE_MOTION:
            in.mx = e.motion.x;
            in.my = e.motion.y;
            if (opt.touch_controls && (e.motion.state & SDL_BUTTON_LMASK)) {
              float d = SDL_GetWindowPixelDensity(win);
              finger(-1, SDL_EVENT_FINGER_MOTION, e.motion.x * d, e.motion.y * d);
            }
            changed = true;
            break;
          case SDL_EVENT_GAMEPAD_ADDED:
            if (!in.pad) in.pad = SDL_OpenGamepad(e.gdevice.which);
            in.touch.clear();  // controls hide: drop what they held
            changed = true;
            break;
          case SDL_EVENT_GAMEPAD_REMOVED:
            if (in.pad && SDL_GetGamepadID(in.pad) == e.gdevice.which) SDL_CloseGamepad(in.pad), in.pad = nullptr;
            changed = true;
            break;
          default: break;
        }
      } while (SDL_PollEvent(&e));
    }
    if (in.entry.sync(win)) changed = true;
    if (changed || in.pad) publish(win, in);
  }
  gpu::renderer().save_caches();
  std::fflush(stdout);
  std::_Exit(0);
}

}  // namespace platform
