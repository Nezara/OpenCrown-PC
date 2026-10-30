// mobile_run: loads the user's Vita eboot, binds every import to our HLE dispatch and runs module_start on one
// guest core under Dynarmic until the entry returns, a fault, or the first unimplemented import.
//
//   mobile_run <eboot.bin | eboot.elf> [--keep-going] [--trace | --trace-only <a,b,..>] [--nids <nid-name.txt>] [--list-imports]
//            [--app0 <dir>]... [--save <dir>] [--root <dir>] [--audio-wav <file>] [--font a.ttf,b.ttc]
//            [--window [--validation] [--touch-controls]] [--input <script>] [--lobby <ws(s)://url> [--online-name <name>]]
// app0: defaults to the eboot's folder, plus a sibling Game/ when that folder is Update/ (patch over base game).
// --root: one folder for saves/, input.ini, identity.ini (online account) and cache/.
// --lobby (or $DCP_LOBBY_URL): online play through that lobby (hle/online.h); without one the game stays offline.
//   --online-name = the player's online id (default: the login name). A loopback lobby also turns STUN and UPnP off
//   unless $DCP_STUN / $DCP_UPNP say otherwise (two instances on one PC).
// --input: frame-timed pad script (`<flip> <BUTTON|BUTTON|...|0>` per line, hle/ctrl.h); replaces live pad input.
//
// Android: the same program is libmain.so behind SDL's activity. The launcher (android/java LauncherActivity) passes
// the eboot, the --app0 folders and --root = Internal storage/OpenCrown/ (saves/, input.ini, log.txt). Started without
// arguments it falls back to the app's own files folder (dev: dump copied in with adb run-as).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>
#include <map>
#include <string>

#include <dynarmic/interface/exclusive_monitor.h>

#include "hle/common.h"
#include "hle/audio.h"
#include "hle/ctrl.h"
#include "hle/io.h"
#include "hle/online.h"
#include "hle/pvf.h"
#include "hle/confirm_button.h"
#include "hle/skip_offers.h"
#ifdef OC_WINDOW
#include "platform/sdl_app.h"
#endif
#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  // main() becomes SDL_main, which SDLActivity calls
#endif
#include "hle/threads.h"
#include "guest/cpu.h"
#include "guest/guest_prof.h"
#include "guest/hle.h"
#include "guest/image.h"
#include "guest/memory.h"
#include "guest/module.h"
#include "net/identity.h"

using namespace guest;

namespace {

constexpr uint32_t DEFAULT_STACK = 256 * 1024;

int usage() {
  std::fprintf(stderr, "usage: mobile_run <eboot.bin|eboot.elf> [--keep-going] [--trace | --trace-only <a,b>] [--nids <file>] [--list-imports]\n"
                       "                [--app0 <dir>]... [--save <dir>] [--root <dir>] [--window] [--input <script>]\n"
                       "                [--lobby <ws(s)://url>] [--online-name <name>]\n");
  return 2;
}

void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

// ws://127.x / ws://localhost / ws://[::1]: a lobby on this machine.
bool loopback_url(const std::string& url) {
  size_t p = url.find("://");
  std::string host = url.substr(p == std::string::npos ? 0 : p + 3);
  host = host.substr(0, host.find('/'));
  if (!host.empty() && host[0] == '[') return host.compare(0, 5, "[::1]") == 0;
  host = host.substr(0, host.find(':'));
  return host == "localhost" || host.compare(0, 4, "127.") == 0;
}

// SceProcessParam: size, 'PSP2', version, fw version, main thread name*, priority*, stack size*, ...
uint32_t main_stack_size(const Memory& mem, uint32_t param) {
  if (!param || mem.read<uint32_t>(param + 4) != 0x32505350) return DEFAULT_STACK;
  uint32_t p = mem.read<uint32_t>(param + 0x18);
  uint32_t v = p ? mem.read<uint32_t>(p) : 0;
  return v >= 0x4000 && v <= 0x4000000 ? v : DEFAULT_STACK;
}

}  // namespace

#ifdef __ANDROID__
// Debug switches without a shell environment: `adb shell setprop debug.opencrown.env "DCP_GUEST_PROF=1 DCP_X=1"`
// sets those DCP_* variables at game start (cleared with setprop debug.opencrown.env ""). Any DCP_* name passes
// (debug switches, the skip switches of hle/skip_offers.h, online overrides). Only switches read after main() starts
// see it (DCP_LOG_GPU is read before).
void android_env_from_property() {
  char v[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.opencrown.env", v) <= 0) return;
  std::string all = v;
  for (size_t p = 0; p < all.size();) {
    size_t e = all.find(' ', p);
    std::string kv = all.substr(p, e == std::string::npos ? std::string::npos : e - p);
    p = e == std::string::npos ? all.size() : e + 1;
    size_t eq = kv.find('=');
    if (eq == std::string::npos || eq <= 4 || kv.compare(0, 4, "DCP_") != 0) continue;
    setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
    std::fprintf(stderr, "[android] env %s (debug.opencrown.env)\n", kv.c_str());
  }
}

// stdout + stderr to a log file the user can attach to bug reports.
void android_log(const std::string& file) {
  // Truncated once, then both streams append: with "w" stdout kept its own offset from 0 and overwrote what stderr
  // had appended (the first [vulkan] lines were lost).
  if (FILE* f = std::fopen(file.c_str(), "w")) std::fclose(f);
  std::freopen(file.c_str(), "a", stdout);
  std::freopen(file.c_str(), "a", stderr);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  // adb cannot read Internal storage/OpenCrown/ on every device: a thread follows log.txt and copies each line to
  // logcat (tag OpenCrownLog). The file keeps getting every line directly (a fault report is never stuck in a pipe);
  // the copy is best effort.
  std::thread([file] {
    FILE* in = std::fopen(file.c_str(), "r");
    if (!in) return;
    std::string line;
    for (char buf[4096];;) {
      size_t n = std::fread(buf, 1, sizeof buf, in);
      if (n == 0) {
        std::clearerr(in);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      line.append(buf, n);
      for (size_t p; (p = line.find('\n')) != std::string::npos; line.erase(0, p + 1))
        __android_log_write(ANDROID_LOG_INFO, "OpenCrownLog", line.substr(0, p).c_str());
    }
  }).detach();
}

// Without --root (started by adb / a dev shortcut, not the launcher): working folder + log, and the eboot to run;
// false (after telling the user) when there is no dump yet.
// The working folder (vita_data/, log.txt) is the app's internal files folder; the dump is looked for there first
// (dev builds are debuggable: adb + run-as copies it in), then in the external files folder.
bool android_setup(std::string& path) {
  const char* internal = SDL_GetAndroidInternalStoragePath();
  const char* base = SDL_GetAndroidExternalStoragePath();
  if (!internal || chdir(internal) != 0) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenCrown", "No app storage folder available.", nullptr);
    return false;
  }
  android_log("log.txt");
  std::error_code ec;
  for (std::string dir : {std::string("."), std::string(base ? base : "")}) {
    if (dir.empty()) continue;
    for (const char* p : {"/dump/Update/eboot.bin", "/dump/Game/eboot.bin", "/dump/eboot.bin"})
      if (std::filesystem::exists(dir + p, ec)) return path = dir + p, true;
  }
  if (!base) base = internal;
  std::string msg = std::string("Copy your PS Vita dump of Dragon's Crown (PCSE00019) to\n") + base +
                    "/dump/\n\nas dump/Game/ (base game) and dump/Update/ (update 1.09), then start the app again.";
  std::fprintf(stderr, "[android] no dump: %s\n", msg.c_str());
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "OpenCrown - game files needed", msg.c_str(), nullptr);
  return false;
}
#endif

int main(int argc, char** argv) {
  std::string path, nids, save;
  std::string root;  // --root <dir>: one folder for saves/, input.ini (and log.txt on Android); the Android launcher's choice
  bool save_given = false;
  std::vector<std::string> app0;
  std::vector<uint32_t> watch;  // debug: pointer chain base:off:off..
  bool window = false;
#ifdef OC_WINDOW
  platform::AppOptions app;
#endif
  std::vector<uint32_t> probes;  // debug: Thumb addresses to stop at (udf)
  HleOptions opt;
  bool list = false;
  std::string lobby, online_name, input;
  if (const char* e = std::getenv("DCP_LOBBY_URL"); e && *e) lobby = e;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--keep-going")) opt.keep_going = true;
    else if (!std::strcmp(argv[i], "--trace")) opt.trace = true;
    else if (!std::strcmp(argv[i], "--trace-only") && i + 1 < argc) {  // comma-separated name substrings
      opt.trace = true;
      for (std::string list = argv[++i], item; !list.empty();) {
        size_t comma = list.find(',');
        item = list.substr(0, comma);
        list = comma == std::string::npos ? "" : list.substr(comma + 1);
        if (!item.empty()) opt.trace_only.push_back(item);
      }
    }
    else if (!std::strcmp(argv[i], "--list-imports")) list = true;
    else if (!std::strcmp(argv[i], "--app0") && i + 1 < argc) app0.push_back(argv[++i]);
    else if (!std::strcmp(argv[i], "--audio-wav") && i + 1 < argc) hle::audio_set_wav(argv[++i]);
    else if (!std::strcmp(argv[i], "--watch") && i + 1 < argc) {
      for (char* p = argv[++i]; *p;) {
        watch.push_back(uint32_t(std::strtoul(p, &p, 0)));
        if (*p == ':') p++;
      }
    }
    else if (!std::strcmp(argv[i], "--probe") && i + 1 < argc) probes.push_back(uint32_t(std::strtoul(argv[++i], nullptr, 0)));
    else if (!std::strcmp(argv[i], "--font") && i + 1 < argc) {  // comma-separated host font files for ScePvf
      std::vector<std::string> fonts;
      for (std::string list = argv[++i]; !list.empty();) {
        size_t comma = list.find(',');
        fonts.push_back(list.substr(0, comma));
        list = comma == std::string::npos ? "" : list.substr(comma + 1);
      }
      hle::pvf_set_system_fonts(fonts);
    }
    else if (!std::strcmp(argv[i], "--window")) window = true;
#ifdef OC_WINDOW
    else if (!std::strcmp(argv[i], "--validation")) app.validation = true;
    else if (!std::strcmp(argv[i], "--touch-controls")) app.touch_controls = true;
#endif
    else if (!std::strcmp(argv[i], "--save") && i + 1 < argc) save = argv[++i], save_given = true;
    else if (!std::strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
    else if (!std::strcmp(argv[i], "--nids") && i + 1 < argc) nids = argv[++i];
    else if (!std::strcmp(argv[i], "--lobby") && i + 1 < argc) lobby = argv[++i];
    else if (!std::strcmp(argv[i], "--online-name") && i + 1 < argc) online_name = argv[++i];
    else if (!std::strcmp(argv[i], "--no-upnp")) set_env("DCP_UPNP", "0");  // launcher's UPnP checkbox off
    else if (!std::strcmp(argv[i], "--confirm-circle")) set_env("DCP_CONFIRM", "circle");  // Japanese layout
    else if (!std::strcmp(argv[i], "--input") && i + 1 < argc) input = argv[++i];
    else if (argv[i][0] == '-' || !path.empty()) return usage();
    else path = argv[i];
  }
  if (!root.empty()) {
    dcp::net::set_user_dir(root);  // identity.ini (online account) lives there too
    if (!save_given) save = root + "/saves";
#ifdef OC_WINDOW
    app.input_ini = root + "/input.ini";
    app.cache_dir = root + "/cache";
#endif
  }
#ifdef __ANDROID__
  window = true;
  if (!root.empty()) android_log(root + "/log.txt");
  else if (path.empty() && !android_setup(path)) return 0;
  android_env_from_property();
  // adb can read the app's internal files folder (run-as on debuggable builds), not Internal storage/OpenCrown/.
  if (const char* internal = SDL_GetAndroidInternalStoragePath()) {
    prof::set_default_output(std::string(internal) + "/guest_prof.txt");
    app.cache_dir = std::string(internal) + "/cache";  // app-private: generated on this device, never shared
  }
#endif
  if (root.empty()) {  // the default user folder, in the final working folder (Android dev runs chdir above)
    const std::string user = dcp::net::default_user_dir();
    if (!save_given) save = user + "/savedata0";
#ifdef OC_WINDOW
    app.input_ini = user + "/input.ini";
    if (app.cache_dir.empty()) app.cache_dir = user + "/cache";
#endif
  }
  if (path.empty()) return usage();
  if (!input.empty()) {
    std::ifstream in(input, std::ios::binary);
    if (!in) {
      std::fprintf(stderr, "mobile_run: cannot open input script %s\n", input.c_str());
      return 1;
    }
    std::string text((std::istreambuf_iterator<char>(in)), {}), why;
    if (!hle::ctrl_load_script(text, why)) {
      std::fprintf(stderr, "mobile_run: input script %s: bad %s\n", input.c_str(), why.c_str());
      return 1;
    }
  }
  if (!lobby.empty()) {
    set_env("DCP_LOBBY", lobby);  // the lobby client's URL (net/lobby_client.h)
    if (loopback_url(lobby)) {    // no NAT to learn about on this machine
      if (!std::getenv("DCP_STUN")) set_env("DCP_STUN", "0");
      if (!std::getenv("DCP_UPNP")) set_env("DCP_UPNP", "0");
    }
  }
  hle::online_configure(lobby, online_name);
  if (hle::online_enabled()) app.title += " - " + hle::np_identity().online_id;  // tells two test instances apart

  std::string err;
  Image img;
  Memory mem;
  Module mod;
  if (!load_image(path, img, err) || !mem.init(err) || !map_image(mem, img, err) || !parse_module(mem, img, mod, err) ||
      !patch_import_stubs(mem, mod, err)) {
    std::fprintf(stderr, "mobile_run: %s\n", err.c_str());
    return 1;
  }
  hle::skip_offers_install(mem);  // story / tutorial skip questions for every new hero (hle/skip_offers.h)
  hle::confirm_button_install(mem);  // $DCP_CONFIRM=circle: Japanese button layout (hle/confirm_button.h)

  std::map<std::string, int> per_lib;
  for (const ImportFunc& f : mod.imports) per_lib[f.lib]++;
  std::printf("[load] %s: %zu segment(s), module_start 0x%08x, %zu function imports from %zu libraries "
              "(%u variable/TLS imports)\n",
              mod.name.c_str(), img.segments.size(), mod.module_start, mod.imports.size(), per_lib.size(),
              mod.num_import_vars);

  if (app0.empty()) {
    namespace fs = std::filesystem;
    fs::path dir = fs::absolute(path).parent_path();
    app0.push_back(dir.string());
    std::error_code ec;
    if (dir.filename() == "Update" && fs::is_directory(dir.parent_path() / "Game", ec))
      app0.push_back((dir.parent_path() / "Game").string());
  }
  for (const std::string& d : app0) {
    hle::io_mount("app0", d, false);
    std::printf("[io] app0: -> %s\n", d.c_str());
  }
  hle::io_mount("savedata0", save, true);
  std::printf("[io] savedata0: -> %s\n", save.c_str());

  for (uint32_t a : probes) mem.write<uint16_t>(a & ~1u, 0xDEFE);  // Thumb udf #0xfe: stop with exact registers
  hle::register_all();
  {
    std::vector<std::string> unbound;
    if (!bind_import_vars(mem, mod, [&](const ImportVar& v) { return hle_var_address(mem, v.nid); }, unbound, err)) {
      std::fprintf(stderr, "mobile_run: %s\n", err.c_str());
      return 1;
    }
    for (const std::string& u : unbound) std::fprintf(stderr, "[load] variable import %s not provided\n", u.c_str());
  }
  Hle hle(mod, opt);
  if (!nids.empty()) hle.load_names(nids);
  prof::set_import_names([&hle, &mod](uint32_t i) { return i < mod.imports.size() ? hle.describe(mod.imports[i]) : std::string("?"); });
  if (list) {
    for (const ImportFunc& f : mod.imports) std::printf("  %08x %s\n", f.stub, hle.describe(f).c_str());
    return 0;
  }

  // Return trampolines (ARM: svc #n ; b .) for thread entries and guest calls; call_return - 8 = thread return.
  uint32_t tramp = mem.alloc(PAGE, "return trampolines");
  if (!tramp) return 1;
  mem.write<uint32_t>(tramp, 0xef000000u | SVC_THREAD_RETURN);
  mem.write<uint32_t>(tramp + 4, 0xeafffffeu);
  mem.write<uint32_t>(tramp + 8, 0xef000000u | SVC_CALL_RETURN);
  mem.write<uint32_t>(tramp + 12, 0xeafffffeu);
  Cpu::call_return = tramp + 8;

  Dynarmic::ExclusiveMonitor monitor(hle::MAX_CORES);
  // Debug: poll a guest pointer chain (base, then +off derefs) and log each change with a timestamp.
  if (!watch.empty())
    std::thread([&mem, watch] {
      auto t0 = std::chrono::steady_clock::now();
      uint32_t last = 0xdeadbeef;
      for (;;) {
        uint32_t v = watch[0];
        bool ok = true;
        for (size_t k = 0; ok && k < watch.size(); k++) {
          uint32_t a = k == 0 ? watch[0] : v + watch[k];
          ok = mem.valid(a, 4);
          if (ok) v = mem.read<uint32_t>(a);
          if (ok && !v && k + 1 < watch.size()) ok = false;
        }
        uint32_t now = ok ? v : 0xffffffff;
        if (now != last) {
          long us = long(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
          std::fprintf(stderr, "[watch] %ld us: 0x%08x\n", us, now);
          last = now;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    }).detach();
  hle::init_threads({&mem, &monitor, [&](Cpu& c, uint32_t imm) { hle.on_svc(c, imm); }});
  auto guest = [&] {
    uint32_t code = hle::run_main_thread(mod.module_start, main_stack_size(mem, mod.process_param), 0, 0);
    std::printf("[run] module_start returned 0x%08x (%zu unimplemented calls)\n", code, hle.unimplemented_calls.load());
  };
#ifdef OC_WINDOW
  if (window) return platform::run_with_window(app, guest);
#endif
  guest();
  std::fflush(stdout);
  std::_Exit(0);  // other guest threads may still be running
}
