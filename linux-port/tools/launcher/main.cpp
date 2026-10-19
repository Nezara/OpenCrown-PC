// opencrown-launcher (target opencrown_launcher): end-user launcher (Dear ImGui on SDL3 + OpenGL 3.3). Picks and validates the
// user's dump folder, edits input.ini (bindings shared with dcp_run via tools/common/input_defaults.h), sets display
// options and starts dcp_run (next to this binary, posix_spawn), stdout/stderr to <user dir>/logs/last_run.log.
// User dir: ${XDG_DATA_HOME:-~/.local/share}/OpenCrown ($DCP_USER_DIR overrides), the same one dcp_run uses.
// Settings: <user dir>/launcher.ini (key = value). --selftest: draw a few frames and exit (0 = ok; writes nothing but
// a temporary input.ini check). --tab <name>: open that page first.
// Online page (net.md 11.3): lobby URL, player name, UPnP, "Test connection" (net/online_check.h); settings reach
// dcp_run as environment. Friends there (lobby_protocol.md 4.2, net/friends.h): own friend code, add by code, requests,
// list. Save Folder page: DCP_SAVE_ROOT. The story / tutorial skip questions are always on (DCP_STORY_SKIP /
// DCP_TUTORIAL_SKIP=0 in the environment turn them off, hle/skip_offers.h).
// First start without a chosen save folder asks for one (default or custom). Report a Bug page: a short note (256
// characters) plus the redacted tail of logs/last_run.log (net/bug_report.h: IPs, ids, user and PC names removed;
// "Preview" shows exactly what is sent), through the lobby, which rate-limits it.
// Player name: never the login name - an empty name gets a random "PlayerNNNN" on first start.
// Look: sidebar navigation over the game's own store art (sce_sys/pic0.png background, icon0.png logo), read from the
// user's dump at run time like everything else (rule 1: nothing from the game ships with us); plain theme without it.
// Dump file names are matched without case (a dump copied from exFAT / NTFS may have any case).
// Folder pickers: SDL's (xdg-desktop-portal, else zenity); without either, the path is typed into a text field.
// Self-updater (net/update.h): the Update entry at the bottom of the sidebar shows the start-up check (GitHub releases
// API, worker thread; "check_updates" in launcher.ini), the Update page shows the notes of every newer release and,
// when running as an AppImage ($APPIMAGE), downloads the new AppImage to <AppImage>.part, checks size + SHA-256,
// renames it over the old file (same path, so a Steam shortcut keeps working) and starts it with --updated <old version>.
// --update-check: run the check on the console and exit (0 = done, 1 = failed).
// Steam Deck Game Mode (linux-port/README.md): gamepad navigation (Pad), touch (Touch: tap = click, drag = scroll), a
// built-in folder browser (FolderBrowser), Steam's on-screen keyboard for text fields focused by pad / touch, 1.25x UI
// on a Deck. Test hook DCP_LAUNCHER_PAD_SCRIPT (Script: virtual gamepad, injected touches, screenshots).
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

#include "../common/game_version.h"
#include "../common/input_defaults.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "imgui_internal.h"  // gamepad focus (SetFocusID), touch drag-scroll (window scroll), text-field state
#include "net/bug_report.h"
#include "net/friends.h"
#include "net/identity.h"
#include "net/online_check.h"
#include "net/update.h"

namespace {

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool name_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; }

// A name the game shows as typed (online_name keeps it unchanged): 3..16 characters of [A-Za-z0-9_-].
bool valid_name(const std::string& n) {
    return n.size() >= 3 && n.size() <= 16 && std::all_of(n.begin(), n.end(), name_char);
}

std::string random_player_name() {
    return "Player" + std::to_string(1000 + SDL_rand(9000));
}

int name_filter(ImGuiInputTextCallbackData* d) { return d->EventChar < 128 && name_char(char(d->EventChar)) ? 0 : 1; }

size_t utf8_chars(const std::string& s) {
    size_t n = 0;
    for (unsigned char ch : s) n += (ch & 0xc0) != 0x80;
    return n;
}

bool exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// dir + "/" + rel with each component of rel matched without case when the exact path is missing; "" if not found.
std::string find_ci(const std::string& dir, const std::string& rel) {
    if (exists(dir + "/" + rel)) return dir + "/" + rel;
    std::string out = dir;
    size_t a = 0;
    while (a <= rel.size()) {
        size_t b = rel.find('/', a);
        if (b == std::string::npos) b = rel.size();
        std::string comp = rel.substr(a, b - a), hit;
        if (DIR* d = opendir(out.c_str())) {
            while (dirent* e = readdir(d))
                if (strcasecmp(e->d_name, comp.c_str()) == 0) { hit = e->d_name; break; }
            closedir(d);
        }
        if (hit.empty()) return "";
        out += "/" + hit;
        a = b + 1;
    }
    return out;
}

// Openers started by open_in_explorer; reaped each frame (poll_openers) so they leave no zombies.
std::vector<pid_t> g_openers;

// Opens a folder or file in the desktop's default app: xdg-open with the path as one argv entry (no shell).
void open_in_explorer(const std::string& p) {
    const char* argv[] = {"xdg-open", p.c_str(), nullptr};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);
    pid_t pid;
    if (posix_spawnp(&pid, "xdg-open", &fa, nullptr, const_cast<char**>(argv), environ) == 0) g_openers.push_back(pid);
    else std::fprintf(stderr, "launcher: xdg-open not found; open %s by hand\n", p.c_str());
    posix_spawn_file_actions_destroy(&fa);
}

void poll_openers() {
    std::erase_if(g_openers, [](pid_t pid) { return waitpid(pid, nullptr, WNOHANG) != 0; });
}

// ---- launcher.ini ----

struct Settings {
    std::string dump;
    std::string saves;  // save root (holds SAVEDATA00); empty = <user dir>/saves. Passed to dcp_run as DCP_SAVE_ROOT
    bool saves_chosen = false;  // the user picked the save folder (or the default) once; false = ask on start
    bool fullscreen = false;
    bool uhd = false;  // 4K rendering (PS4 Pro mode, dcp_run --4k)
    int width = 1280, height = 720;
    // Online (passed as DCP_LOBBY / DCP_NP_NAME / DCP_UPNP; the game's own online menu uses them)
    static constexpr const char* kDefaultLobby = "wss://opencrown-lobby.networkhandler.workers.dev/";
    std::string lobby = kDefaultLobby;
    std::string name;   // online name shown to others: 3..16 of [A-Za-z0-9_-] (net/identity.h online_name)
    bool upnp = true;
    bool check_updates = true;  // start-up update check (Update page)
};

void load_settings(const std::string& path, Settings& s) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "dump") s.dump = v;
        else if (k == "saves") s.saves = v, s.saves_chosen = s.saves_chosen || !v.empty();
        else if (k == "saves_chosen") s.saves_chosen = s.saves_chosen || v == "1" || v == "true";
        else if (k == "fullscreen") s.fullscreen = v == "1" || v == "true";
        else if (k == "uhd") s.uhd = v == "1" || v == "true";
        else if (k == "width") s.width = std::clamp(std::atoi(v.c_str()), 320, 16384);
        else if (k == "height") s.height = std::clamp(std::atoi(v.c_str()), 180, 16384);
        else if (k == "lobby") s.lobby = v.empty() ? Settings::kDefaultLobby : v;
        else if (k == "name") s.name = v.substr(0, 16);
        else if (k == "upnp") s.upnp = v == "1" || v == "true";
        else if (k == "check_updates") s.check_updates = !(v == "0" || v == "false");
    }
}

void save_settings(const std::string& path, const Settings& s) {
    std::ofstream o(path);
    o << "# OpenCrownPC Launcher settings\n"
      << "dump = " << s.dump << "\nsaves = " << s.saves
      << "\nsaves_chosen = " << (s.saves_chosen ? "true" : "false") << "\nuhd = " << (s.uhd ? "true" : "false") << "\nfullscreen = " << (s.fullscreen ? "true" : "false") << "\nwidth = " << s.width
      << "\nheight = " << s.height << "\nlobby = " << s.lobby << "\nname = " << s.name
      << "\nupnp = " << (s.upnp ? "true" : "false") << "\ncheck_updates = " << (s.check_updates ? "true" : "false")
      << "\n";
}

// ---- input.ini model: the action lines of kInputDefaults, in order ----

struct Binding {
    std::string action;
    std::string value;  // as written in input.ini ("" = unbound; may hold "a, b" from a hand-edited file)
};

struct Controls {
    std::vector<Binding> binds;
    float fairy_speed = 1.0f;
    bool touch_screen = true, deck_trackpads = true;  // "on" / "off" in input.ini
};

bool ini_on(const std::string& v) { return v != "off" && v != "0" && v != "false"; }

// Feeds each "key = value" line of an ini text to fn (comments stripped).
template <class F> void for_each_entry(const std::string& text, F fn) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line.substr(0, line.find('#')));
        size_t eq = line.find('=');
        if (!line.empty() && eq != std::string::npos) fn(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
    }
}

Controls default_controls() {
    Controls c;
    for_each_entry(dcp::kInputDefaults, [&](const std::string& k, const std::string& v) {
        if (k == "fairy_speed") c.fairy_speed = std::strtof(v.c_str(), nullptr);
        else if (k == "touch_screen") c.touch_screen = ini_on(v);
        else if (k == "deck_trackpads") c.deck_trackpads = ini_on(v);
        else c.binds.push_back({k, v});
    });
    return c;
}

Controls load_controls(const std::string& path) {
    Controls c = default_controls();
    std::ifstream f(path);
    if (!f) return c;
    std::stringstream ss;
    ss << f.rdbuf();
    for_each_entry(ss.str(), [&](const std::string& k, const std::string& v) {
        if (k == "fairy_speed") {
            float s = std::strtof(v.c_str(), nullptr);
            if (s > 0.0f) c.fairy_speed = s;
            return;
        }
        if (k == "touch_screen") { c.touch_screen = ini_on(v); return; }
        if (k == "deck_trackpads") { c.deck_trackpads = ini_on(v); return; }
        for (Binding& b : c.binds)
            if (b.action == k) b.value = v;
    });
    return c;
}

// Writes the defaults text with each value replaced, so comments and order match what dcp_run writes.
bool save_controls(const std::string& path, const Controls& c) {
    std::ostringstream out;
    std::istringstream in(dcp::kInputDefaults);
    std::string line;
    while (std::getline(in, line)) {
        std::string body = trim(line.substr(0, line.find('#')));
        size_t eq = body.find('=');
        if (body.empty() || eq == std::string::npos) { out << line << "\n"; continue; }
        std::string k = trim(body.substr(0, eq));
        if (k == "fairy_speed") {
            char v[32];
            std::snprintf(v, sizeof v, "%.2f", c.fairy_speed);
            std::string s = v;  // "1.00" -> "1.0", "1.25" stays
            while (s.size() > 3 && s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
            out << k << " = " << s << "\n";
            continue;
        }
        if (k == "touch_screen" || k == "deck_trackpads") {
            out << k << " = " << ((k == "touch_screen" ? c.touch_screen : c.deck_trackpads) ? "on" : "off") << "\n";
            continue;
        }
        for (const Binding& b : c.binds)
            if (b.action == k) out << k << " =" << (b.value.empty() ? "" : " " + b.value) << "\n";
    }
    std::ofstream o(path);
    o << out.str();
    return bool(o);
}

const char* const kMouseNames[] = {"Mouse1", "Mouse2", "Mouse3", "Mouse4", "Mouse5"};

std::string mouse_name(Uint8 sdl_button) {
    switch (sdl_button) {  // input.ini order: left, right, middle, back, forward
    case SDL_BUTTON_LEFT: return kMouseNames[0];
    case SDL_BUTTON_RIGHT: return kMouseNames[1];
    case SDL_BUTTON_MIDDLE: return kMouseNames[2];
    case SDL_BUTTON_X1: return kMouseNames[3];
    case SDL_BUTTON_X2: return kMouseNames[4];
    }
    return "";
}

std::string pretty(const std::string& v) {  // display name for one binding value
    static const std::map<std::string, std::string> kMouse = {
        {"mouse1", "Left mouse"}, {"mouse2", "Right mouse"}, {"mouse3", "Middle mouse"},
        {"mouse4", "Mouse back"}, {"mouse5", "Mouse forward"}};
    std::string l = v;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    auto it = kMouse.find(l);
    return it != kMouse.end() ? it->second : v;
}

// Human-readable action labels (input.ini names stay the file format).
const std::map<std::string, const char*> kLabels = {
    {"move_up", "Move up"}, {"move_down", "Move down"}, {"move_left", "Move left"}, {"move_right", "Move right"},
    {"square", "Attack (Square)"}, {"circle", "Item / slide (Circle)"}, {"triangle", "Triangle"},
    {"cross", "Jump (Cross)"}, {"l1", "L1"}, {"r1", "R1"}, {"l2", "L2"}, {"r2", "R2"}, {"l3", "L3"}, {"r3", "R3"},
    {"options", "Options / menu"}, {"touchpad", "Touchpad button"}, {"town_map", "Town map (touchpad left half)"}, {"dpad_up", "D-pad up"},
    {"dpad_down", "D-pad down"}, {"dpad_left", "D-pad left"}, {"dpad_right", "D-pad right"},
    {"look_up", "Right stick up"}, {"look_down", "Right stick down"}, {"look_left", "Right stick left"},
    {"look_right", "Right stick right"}, {"fairy", "Fairy pointer (hold)"}, {"fairy_toggle", "Fairy pointer (toggle)"},
    {"fairy_tap", "Fairy tap (in fairy mode)"}, {"pointer_tap", "Pointer interact (after right stick keys)"},
};

// ---- dump validation ----

struct Check {
    bool ok;
    bool required;
    std::string text;
    const char* fail = "[missing]";  // tag when a required check fails
};

Check version_check(const std::filesystem::path& eboot) {
    switch (dcp::check_game_version(eboot)) {
    case dcp::GameVersion::Supported: return {true, true, "Game version: latest patch (1.03)", ""};
    case dcp::GameVersion::Unsupported:
        return {false, true, "Not the latest patch (1.03), or another region's copy: update the game on your PS4/PS5 and "
                             "dump it again", "[wrong version]"};
    default: return {false, true, "eboot.bin could not be read"};
    }
}

std::vector<Check> validate_dump(const std::string& dir) {
    std::vector<Check> out;
    if (dir.empty()) return {{false, true, "No dump folder selected."}};
    if (!exists(dir)) return {{false, true, "Folder does not exist."}};
    const std::string eboot = find_ci(dir, "eboot.bin");
    bool eb = !eboot.empty();
    out.push_back({eb, true, eb ? "eboot.bin found" : "eboot.bin missing (the decrypted game executable)"});
    if (eb) out.push_back(version_check(eboot));
    for (const char* cpk : {"DRACRO.CPK", "DRACRO_US.CPK"}) {
        bool ok = !find_ci(dir, cpk).empty();
        out.push_back({ok, true, std::string(cpk) + (ok ? " found" : " missing (game data)")});
    }
    bool libc = !find_ci(dir, "sce_module/libc.prx").empty();
    out.push_back({libc, false, libc ? "sce_module/libc.prx found"
                                     : "sce_module/libc.prx missing (optional; built-in libc stubs are used)"});
    return out;
}

bool dump_ok(const std::vector<Check>& v) {
    for (const Check& c : v)
        if (c.required && !c.ok) return false;
    return true;
}

// ---- game process ----

struct Game {
    pid_t proc = 0;
    unsigned long exit_code = 0;   // exit status, or 128 + signal number when killed by a signal (shell convention)
    bool failed = false;           // last run exited non-zero (or could not start)
    std::vector<std::string> tail; // last lines of the log after a failure
    bool show_tail = false;        // open the log popup on the next frame
    std::string error;             // launch error
};

std::vector<std::string> log_tail(const std::string& path, size_t n) {
    std::ifstream f(path);
    std::deque<std::string> d;
    std::string line;
    while (std::getline(f, line)) {
        d.push_back(line);
        if (d.size() > n) d.pop_front();
    }
    return {d.begin(), d.end()};
}

std::string exe_dir() {  // folder of this binary (/proc/self/exe resolves symlinks)
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    std::string s = n > 0 ? std::string(buf, size_t(n)) : ".";
    size_t slash = s.find_last_of('/');
    return slash == std::string::npos ? "." : slash == 0 ? "/" : s.substr(0, slash);
}

void set_env(const char* k, const std::string& v) {  // "" removes it; inherited by dcp_run
    if (v.empty()) unsetenv(k);
    else setenv(k, v.c_str(), 1);
}

bool start_game(Game& g, const Settings& s, const std::string& user, const std::string& log_path) {
    std::string exe = exe_dir() + "/dcp_run";
    if (access(exe.c_str(), X_OK) != 0) { g.error = "dcp_run not found next to the launcher (" + exe + ")"; return false; }
    std::string eboot = find_ci(s.dump, "eboot.bin");
    if (eboot.empty()) { g.error = "eboot.bin not found in " + s.dump; return false; }
    std::vector<std::string> args = {exe, eboot, "--render", "--window"};
    if (s.fullscreen) args.push_back("--fullscreen");
    if (s.uhd) args.push_back("--4k");
    args.push_back("--size"), args.push_back(std::to_string(s.width) + "x" + std::to_string(s.height));
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    // Custom save folder: the game's DCP_SAVE_ROOT override, set in our environment (inherited by the child).
    set_env("DCP_SAVE_ROOT", s.saves);
    // Same user dir as ours, whatever the save folder is: the game reads input.ini and identity.ini from it (without
    // this it uses the save folder's parent, so a custom save folder would mean another online account than ours).
    if (const char* d = std::getenv("DCP_USER_DIR"); !d || !*d) set_env("DCP_USER_DIR", user);
    // Online settings (net.md 11.3). The lobby is only contacted when the game goes online.
    set_env("DCP_LOBBY", trim(s.lobby));
    set_env("DCP_NP_NAME", s.name);
    set_env("DCP_UPNP", s.upnp ? "" : "0");

    // stdin /dev/null, stdout + stderr to the log; nothing else inherited (no sockets of a friends / bug-report
    // session running at the time). Working dir = the launcher's folder, like on Windows.
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);
    const std::string dir = exe_dir();
    posix_spawn_file_actions_addchdir_np(&fa, dir.c_str());
    pid_t pid = 0;
    int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    // glibc reports a failed exec / file action (log not writable) as the spawn's own error.
    if (rc != 0) { g.error = "cannot start dcp_run: " + std::string(std::strerror(rc)) + " (log " + log_path + ")"; return false; }
    g.proc = pid;
    g.failed = false, g.tail.clear(), g.error.clear();
    return true;
}

// Called each frame: notices the game's exit.
void poll_game(Game& g, const std::string& log_path) {
    int status = 0;
    if (!g.proc || waitpid(g.proc, &status, WNOHANG) != g.proc) return;
    g.exit_code = WIFEXITED(status) ? unsigned(WEXITSTATUS(status)) : WIFSIGNALED(status) ? 128u + WTERMSIG(status) : 1u;
    g.proc = 0;
    g.failed = g.exit_code != 0;
    if (g.failed) g.tail = log_tail(log_path, 30), g.show_tail = true;
}

// ---- folder dialog (SDL calls back on another thread on some platforms) ----

std::mutex g_pick_mu;
std::string g_picked;
bool g_have_pick = false;
intptr_t g_pick_target = 0;  // 0 = dump folder, 1 = saves folder
bool g_pick_failed = false;  // no dialog backend (neither xdg-desktop-portal nor zenity): type the path instead
std::string g_pick_error;

void SDLCALL on_folder(void* target, const char* const* list, int) {
    std::lock_guard<std::mutex> lk(g_pick_mu);
    if (!list) {  // error (an empty list = cancelled)
        g_pick_failed = true, g_pick_error = SDL_GetError(), g_pick_target = reinterpret_cast<intptr_t>(target);
        return;
    }
    if (!list[0]) return;
    g_picked = list[0], g_have_pick = true, g_pick_target = reinterpret_cast<intptr_t>(target);
}

// Palette: dark leather and bronze with gold accents (the game's menus), panels translucent over the background art.
const ImVec4 kGold(0.92f, 0.72f, 0.38f, 1), kGoldDim(0.70f, 0.52f, 0.26f, 1), kParchment(0.93f, 0.88f, 0.78f, 1);

void apply_theme(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0, st.ChildRounding = 8, st.FrameRounding = 5, st.PopupRounding = 6, st.GrabRounding = 5, st.ScrollbarRounding = 6;
    st.FramePadding = ImVec2(10, 6), st.ItemSpacing = ImVec2(9, 8), st.WindowPadding = ImVec2(16, 14);
    st.WindowBorderSize = 0, st.ChildBorderSize = 0, st.PopupBorderSize = 1, st.FrameBorderSize = 0;
    st.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = kParchment;
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.52f, 0.47f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.06f, 0.06f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.08f, 0.07f, 0.97f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
    c[ImGuiCol_Border] = ImVec4(0.45f, 0.34f, 0.18f, 0.6f);
    c[ImGuiCol_Separator] = ImVec4(0.55f, 0.42f, 0.22f, 0.55f);
    c[ImGuiCol_FrameBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.14f, 0.08f, 0.70f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.19f, 0.10f, 0.80f);
    c[ImGuiCol_Header] = ImVec4(0.55f, 0.38f, 0.16f, 0.45f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.62f, 0.44f, 0.20f, 0.55f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.70f, 0.50f, 0.22f, 0.65f);
    c[ImGuiCol_Button] = ImVec4(0.32f, 0.21f, 0.12f, 0.85f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.50f, 0.34f, 0.17f, 0.95f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.62f, 0.43f, 0.20f, 1);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.20f, 0.14f, 0.08f, 0.80f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 0.9f, 0.7f, 0.04f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.45f, 0.34f, 0.18f, 0.25f);
    c[ImGuiCol_SliderGrab] = kGoldDim;
    c[ImGuiCol_SliderGrabActive] = kGold;
    c[ImGuiCol_CheckMark] = kGold;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.2f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.45f, 0.32f, 0.16f, 0.7f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.70f, 0.50f, 0.22f, 0.45f);
    c[ImGuiCol_NavCursor] = kGold;
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.22f, 0.15f, 0.08f, 1);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.42f, 0.29f, 0.13f, 1);
    st.ScaleAllSizes(scale);
}

// ---- artwork from the user's dump: the PS4 store images in sce_sys (decoded with SDL's own PNG loader) ----

struct Tex {
    GLuint id = 0;
    int w = 0, h = 0;
};

// The few GL calls made here, through SDL_GL_GetProcAddress (no -lGL link; ImGui's backend loads its own).
struct Gl {
    void(APIENTRY* GenTextures)(GLsizei, GLuint*);
    void(APIENTRY* DeleteTextures)(GLsizei, const GLuint*);
    void(APIENTRY* BindTexture)(GLenum, GLuint);
    void(APIENTRY* TexParameteri)(GLenum, GLenum, GLint);
    void(APIENTRY* PixelStorei)(GLenum, GLint);
    void(APIENTRY* TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
    void(APIENTRY* Viewport)(GLint, GLint, GLsizei, GLsizei);
    void(APIENTRY* ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void(APIENTRY* Clear)(GLbitfield);
    void(APIENTRY* ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);  // test screenshots only (optional)
} gl;

bool load_gl() {  // after the context is current
    bool ok = true;
    auto get = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(SDL_GL_GetProcAddress(name));
        ok = ok && fn;
    };
    get(gl.GenTextures, "glGenTextures"), get(gl.DeleteTextures, "glDeleteTextures");
    get(gl.BindTexture, "glBindTexture"), get(gl.TexParameteri, "glTexParameteri");
    get(gl.PixelStorei, "glPixelStorei"), get(gl.TexImage2D, "glTexImage2D"), get(gl.Viewport, "glViewport");
    get(gl.ClearColor, "glClearColor"), get(gl.Clear, "glClear");
    const bool need = ok;
    get(gl.ReadPixels, "glReadPixels");
    return need;
}

// PNG -> RGBA8 texture; {} on failure (or empty path). win: also make it the window icon.
Tex load_texture(const std::string& path, SDL_Window* win = nullptr) {
    Tex t;
    if (path.empty()) return t;
    SDL_Surface* png = SDL_LoadPNG(path.c_str());
    // ABGR8888 = R, G, B, A bytes in memory (GL_RGBA / GL_UNSIGNED_BYTE), rows packed (pitch = w * 4).
    SDL_Surface* rgba = png ? SDL_ConvertSurface(png, SDL_PIXELFORMAT_ABGR8888) : nullptr;
    if (png) SDL_DestroySurface(png);
    if (!rgba) return t;
    const int w = rgba->w, h = rgba->h;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { SDL_DestroySurface(rgba); return t; }
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        std::memcpy(px.data() + size_t(y) * w * 4, static_cast<const uint8_t*>(rgba->pixels) + size_t(y) * rgba->pitch,
                    size_t(w) * 4);
    SDL_DestroySurface(rgba);
    gl.GenTextures(1, &t.id);
    gl.BindTexture(GL_TEXTURE_2D, t.id);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(w), GLsizei(h), 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    t.w = int(w), t.h = int(h);
    if (win)
        if (SDL_Surface* icon = SDL_CreateSurfaceFrom(int(w), int(h), SDL_PIXELFORMAT_ABGR8888, px.data(), int(w * 4))) {
            SDL_SetWindowIcon(win, icon);  // copies the pixels
            SDL_DestroySurface(icon);
        }
    return t;
}

// Background + logo (also the window icon) for the current dump folder; reloaded when the folder changes. The binary
// itself has no icon: it would have to embed the game's art.
struct Art {
    std::string dump;  // folder the textures came from
    Tex bg, logo;
    void update(const std::string& d, SDL_Window* win) {
        if (d == dump) return;
        dump = d;
        for (Tex* t : {&bg, &logo})
            if (t->id) gl.DeleteTextures(1, &t->id), *t = {};
        if (d.empty()) return;
        bg = load_texture(find_ci(d, "sce_sys/pic0.png"));
        logo = load_texture(find_ci(d, "sce_sys/icon0.png"), win);
    }
};

ImTextureID tex_id(const Tex& t) { return ImTextureID(intptr_t(t.id)); }

// Fills the display with the art ("cover": crop, keep aspect), then darkens it so text stays readable; a plain
// gradient without art.
void draw_background(const Art& art, ImVec2 size) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (art.bg.id) {
        float sa = size.x / size.y, ta = float(art.bg.w) / float(art.bg.h);
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (sa > ta) uv0.y = 0.5f - ta / sa * 0.5f, uv1.y = 0.5f + ta / sa * 0.5f;
        else uv0.x = 0.5f - sa / ta * 0.5f, uv1.x = 0.5f + sa / ta * 0.5f;
        dl->AddImage(tex_id(art.bg), ImVec2(0, 0), size, uv0, uv1);
        dl->AddRectFilledMultiColor(ImVec2(0, 0), size, IM_COL32(8, 6, 5, 70), IM_COL32(8, 6, 5, 30),
                                    IM_COL32(8, 6, 5, 110), IM_COL32(8, 6, 5, 150));
    } else {
        dl->AddRectFilledMultiColor(ImVec2(0, 0), size, IM_COL32(34, 24, 18, 255), IM_COL32(20, 15, 13, 255),
                                    IM_COL32(12, 10, 9, 255), IM_COL32(16, 12, 10, 255));
    }
}

// Section heading: title in the heading font, then a thin gold rule.
void heading(ImFont* font, const char* text) {
    if (font) ImGui::PushFont(font);
    ImGui::TextColored(kGold, "%s", text);
    if (font) ImGui::PopFont();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + 1.5f), IM_COL32(235, 184, 97, 200),
                                                        IM_COL32(235, 184, 97, 0), IM_COL32(235, 184, 97, 0),
                                                        IM_COL32(235, 184, 97, 200));
    ImGui::Dummy(ImVec2(0, 8));
}

// "Test connection" on a worker thread (blocks for a few seconds).
struct ConnTest {
    std::mutex mu;
    bool running = false, have = false;
    dcp::net::OnlineCheck result;
    std::thread th;
    ~ConnTest() {
        if (th.joinable()) th.join();
    }
    void start(const Settings& s) {
        if (th.joinable()) th.join();
        running = true, have = false;
        std::string url = trim(s.lobby), name = trim(s.name);
        bool upnp = s.upnp;
        th = std::thread([this, url, name, upnp] {
            dcp::net::OnlineCheck r = dcp::net::run_online_check(url, name, upnp, 3658);
            std::lock_guard<std::mutex> lk(mu);
            result = r, have = true, running = false;
        });
    }
};

// Friends (lobby_protocol.md 4.2): one short lobby session per refresh / button press on a worker thread
// (net/friends.h). The last successful lists stay on screen while the next session runs or after it fails.
struct FriendsJob {
    std::mutex mu;
    bool running = false, have = false;  // have = lists from a successful session
    dcp::net::FriendsState lists;        // last successful session
    std::string error;                   // last session's failure ("" = ok)
    std::string action_msg;              // last action's result
    bool action_ok = false;
    uint64_t done_ms = 0;                // SDL_GetTicks of the last finished session
    std::thread th;
    ~FriendsJob() {
        if (th.joinable()) th.join();
    }
    void start(const Settings& s, const dcp::net::Identity& id, dcp::net::FriendsAction a = {}) {
        if (th.joinable()) th.join();
        {
            std::lock_guard<std::mutex> lk(mu);
            running = true;
            if (a.kind != dcp::net::FriendsAction::None) action_msg.clear();
        }
        std::string url = trim(s.lobby), name = trim(s.name);
        th = std::thread([this, url, name, id, a] {
            dcp::net::FriendsState r = dcp::net::run_friends_session(url, name, id, a);
            std::lock_guard<std::mutex> lk(mu);
            if (r.ok) lists = r, have = true;
            error = r.ok ? "" : r.error;
            if (a.kind != dcp::net::FriendsAction::None)
                action_ok = r.ok && r.action_ok, action_msg = r.ok ? r.action_msg : r.error;
            running = false, done_ms = SDL_GetTicks();
        });
    }
};

// "Send report" on a worker thread (one lobby session, a few hundred ms).
struct BugJob {
    std::mutex mu;
    bool running = false, sent_new = false;  // sent_new: a report went out since the UI last looked
    dcp::net::BugReportResult result;
    std::thread th;
    ~BugJob() {
        if (th.joinable()) th.join();
    }
    void start(const Settings& s, const dcp::net::Identity& id, const std::string& text, const std::string& log_path) {
        if (th.joinable()) th.join();
        running = true, result = {};
        std::string url = trim(s.lobby), name = trim(s.name);
        th = std::thread([this, url, name, id, text, log_path] {
            std::string log = dcp::net::redact_log(dcp::net::log_tail_text(log_path, dcp::net::kBugReportLogMax), id);
            dcp::net::BugReportResult r = dcp::net::send_bug_report(url, name, id, text, log, "launcher " __DATE__);
            std::lock_guard<std::mutex> lk(mu);
            result = r, running = false, sent_new = r.ok;
        });
    }
};

// Self-updater: the check and the download run on a worker thread (net/update.h); the UI reads a snapshot.
struct UpdateJob {
    std::mutex mu;
    bool running = false, downloading = false;
    bool checked = false;         // a check finished (res holds it)
    dcp::net::update::Result res;
    uint64_t got = 0;             // download progress
    int64_t total = -1;
    std::string dl_error;         // last download / install failure
    bool installed = false;       // the new AppImage is in place: relaunch it
    std::atomic<bool> cancel{false};
    std::thread th;
    ~UpdateJob() {
        cancel = true;
        if (th.joinable()) th.join();
    }
    void check() {
        if (th.joinable()) th.join();
        {
            std::lock_guard<std::mutex> lk(mu);
            running = true;
        }
        th = std::thread([this] {
            dcp::net::update::Result r = dcp::net::update::check(OPENCROWN_VERSION, &cancel);
            if (!r.ok) std::fprintf(stderr, "launcher: update check failed: %s\n", r.error.c_str());
            std::lock_guard<std::mutex> lk(mu);
            res = r, checked = true, running = false;
        });
    }
    // Downloads res.asset next to appimage, verifies it and renames it over appimage.
    void download(const std::string& appimage) {
        if (th.joinable()) th.join();
        dcp::net::update::Asset a;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!checked || !res.have_target) return;
            a = res.asset;
            running = downloading = true, got = 0, total = a.size, dl_error.clear();
        }
        th = std::thread([this, a, appimage] {
            std::string err;
            const std::string part = appimage + ".part";
            bool ok = dcp::net::update::download(
                a, part, OPENCROWN_VERSION,
                [this](uint64_t g, int64_t t) {
                    std::lock_guard<std::mutex> lk(mu);
                    got = g;
                    if (t > 0) total = t;
                },
                &cancel, &err);
            ok = ok && dcp::net::update::install_over(part, appimage, &err);
            if (!ok) std::fprintf(stderr, "launcher: update failed: %s\n", err.c_str());
            std::lock_guard<std::mutex> lk(mu);
            dl_error = ok ? "" : err;
            installed = ok, running = downloading = false;
        });
    }
};

// Path of the running AppImage ($APPIMAGE, set by the AppImage runtime), "" for a plain build.
std::string appimage_path() {
    const char* a = std::getenv("APPIMAGE");
    if (!a || !*a || a[0] != '/') return "";
    struct stat st;
    return stat(a, &st) == 0 && S_ISREG(st.st_mode) ? a : "";
}

std::string parent_dir(const std::string& p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? "." : s == 0 ? "/" : p.substr(0, s);
}

// Starts the freshly installed AppImage (same path) with --updated <this version>. False + *err if it cannot start.
bool relaunch(const std::string& appimage, std::string* err) {
    const char* argv[] = {appimage.c_str(), "--updated", OPENCROWN_VERSION, nullptr};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);
    pid_t pid;
    int rc = posix_spawn(&pid, appimage.c_str(), &fa, nullptr, const_cast<char**>(argv), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) *err = "the update is installed, but starting it failed (" + std::string(std::strerror(rc)) +
                        "): start the launcher again by hand";
    return rc == 0;
}

const ImVec4 kGood(0.55f, 0.85f, 0.45f, 1), kBad(0.97f, 0.45f, 0.38f, 1), kWarn(0.98f, 0.62f, 0.30f, 1),
    kDim(0.66f, 0.62f, 0.55f, 1);

// Release notes (net/update.h NoteLine) as wrapped ImGui text.
void draw_notes(const std::vector<dcp::net::update::NoteLine>& lines, ImFont* font_tag, float scale) {
    using dcp::net::update::NoteLine;
    for (const NoteLine& l : lines) {
        switch (l.kind) {
        case NoteLine::Tag:
            if (font_tag) ImGui::PushFont(font_tag);
            ImGui::TextColored(kGold, "%s", l.text.c_str());
            if (font_tag) ImGui::PopFont();
            break;
        case NoteLine::Heading: ImGui::TextColored(ImVec4(0.98f, 0.86f, 0.62f, 1), "%s", l.text.c_str()); break;
        case NoteLine::Bullet:
            ImGui::Indent((8 + 16 * l.indent) * scale);
            ImGui::Bullet();
            ImGui::TextWrapped("%s", l.text.c_str());
            ImGui::Unindent((8 + 16 * l.indent) * scale);
            break;
        case NoteLine::Text: ImGui::TextWrapped("%s", l.text.c_str()); break;
        case NoteLine::Blank: ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() * 0.5f)); break;
        }
    }
}

template <class Act>
void draw_friend_lists(const dcp::net::FriendsState& L, bool busy, Act act, std::string& confirm_remove, float scale);

// Friends block of the Online page. confirm_remove: account id whose Remove was clicked once (second click removes).
void draw_friends(FriendsJob& fj, const Settings& set, const dcp::net::Identity& ident, std::string& add_code,
                  std::string& confirm_remove, float scale) {
    using dcp::net::FriendsAction;
    ImGui::Spacing();
    ImGui::SeparatorText("Friends");
    if (!ident.ok) {
        ImGui::TextColored(kBad, "No online identity: %s", ident.error.c_str());
        return;
    }
    // Snapshot under the lock; buttons only record the action, started after drawing.
    bool busy, have, action_ok;
    std::string error, action_msg;
    dcp::net::FriendsState L;
    uint64_t done_ms;
    {
        std::lock_guard<std::mutex> lk(fj.mu);
        busy = fj.running, have = fj.have, action_ok = fj.action_ok, done_ms = fj.done_ms;
        error = fj.error, action_msg = fj.action_msg;
        if (have) L = fj.lists;
    }
    bool refresh = !busy && (done_ms == 0 || SDL_GetTicks() - done_ms > 30000);  // first look, then every 30 s
    FriendsAction pending;
    auto act = [&](FriendsAction::Kind k, const std::string& arg) { pending = {k, arg}; };

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Your friend code:");
    ImGui::SameLine();
    if (have) {
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.35f, 1), "%s", L.code.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Copy##code")) ImGui::SetClipboardText(L.code.c_str());
    } else {
        ImGui::TextColored(kDim, "%s", busy ? "asking the lobby..." : "-");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(busy ? "Updating...##fr" : "Refresh##fr")) refresh = true;
    ImGui::EndDisabled();
    if (!error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kBad, "%s", error.c_str());
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Add a friend by code:");
    ImGui::SameLine();
    {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%s", add_code.c_str());
        ImGui::SetNextItemWidth(130 * scale);
        if (ImGui::InputTextWithHint("##addcode", "XXXX-XXXX", buf, 12, ImGuiInputTextFlags_CharsUppercase))
            add_code = buf;
    }
    ImGui::SameLine();
    if (ImGui::Button("Paste##addcode")) {  // no keyboard on a Deck in Game Mode: paste a code copied elsewhere
        char* clip = SDL_GetClipboardText();
        std::string c = trim(clip ? clip : "");
        SDL_free(clip);
        const std::string n = dcp::net::friend_code_normalize(c);
        if (!n.empty()) add_code = dcp::net::friend_code_display(n);
        else {
            std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) { return char(std::toupper(ch)); });
            add_code = c.substr(0, 11);
        }
    }
    ImGui::SameLine();
    const bool code_ok = !dcp::net::friend_code_normalize(add_code).empty();
    ImGui::BeginDisabled(busy || !code_ok);
    if (ImGui::Button("Send request")) {
        act(FriendsAction::Request, add_code);
        add_code.clear();
    }
    ImGui::EndDisabled();
    if (!action_msg.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(action_ok ? kGood : kBad, "%s", action_msg.c_str());
    }
    if (have) draw_friend_lists(L, busy, act, confirm_remove, scale);
    if (pending.kind != FriendsAction::None) fj.start(set, ident, pending);
    else if (refresh) fj.start(set, ident);
}

template <class Act>
void draw_friend_lists(const dcp::net::FriendsState& L, bool busy, Act act, std::string& confirm_remove, float scale) {
    using dcp::net::FriendsAction;

    if (!L.incoming.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(kWarn, "Friend requests (%d):", int(L.incoming.size()));
        for (const auto& r : L.incoming) {
            ImGui::PushID(r.account_id.c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::Bullet();
            ImGui::TextUnformatted(r.name.empty() ? "(no name)" : r.name.c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(busy);
            if (ImGui::SmallButton("Accept")) act(FriendsAction::Accept, r.account_id);
            ImGui::SameLine();
            if (ImGui::SmallButton("Decline")) act(FriendsAction::Decline, r.account_id);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
    }
    if (!L.outgoing.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(kDim, "Sent requests, waiting for an answer:");
        for (const auto& r : L.outgoing) {
            ImGui::PushID(r.account_id.c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::Bullet();
            ImGui::TextUnformatted(r.name.empty() ? "(no name)" : r.name.c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(busy);
            if (ImGui::SmallButton("Cancel")) act(FriendsAction::Decline, r.account_id);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    if (L.friends.empty()) {
        ImGui::TextColored(kDim, "No friends yet: send your code to a friend, or add theirs above.");
        return;
    }
    int online = 0;
    for (const auto& f : L.friends) online += f.online;
    ImGui::Text("Friends (%d online of %d):", online, int(L.friends.size()));
    if (ImGui::BeginTable("##friends", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("##rm", ImGuiTableColumnFlags_WidthFixed, 110 * scale);
        std::vector<dcp::net::FriendInfo> rows = L.friends;  // online first, then by name
        std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return a.online != b.online ? a.online : strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        for (const auto& f : rows) {
            ImGui::PushID(f.account_id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(f.name.empty() ? "(no name)" : f.name.c_str());
            ImGui::TableNextColumn();
            if (!f.online) ImGui::TextColored(kDim, "Offline");
            else if (!f.join_code.empty()) ImGui::TextColored(kGood, "Online, in a room");
            else ImGui::TextColored(kGood, "Online");
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(busy);
            const bool confirm = confirm_remove == f.account_id;
            if (ImGui::SmallButton(confirm ? "Really remove?" : "Remove")) {
                if (confirm) act(FriendsAction::Remove, f.account_id), confirm_remove.clear();
                else confirm_remove = f.account_id;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// System fonts (never shipped): first file that exists of a list of Debian/Ubuntu, Fedora and Arch (SteamOS) paths.
const char* first_font(std::initializer_list<const char*> files) {
    for (const char* f : files)
        if (exists(f)) return f;
    return nullptr;
}

void error_box(const char* title, const char* msg) {
    std::fprintf(stderr, "%s: %s\n", title, msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, nullptr);
}

// ---- Steam Deck Game Mode: gamepad, touch screen, built-in folder browser, Steam's on-screen keyboard ----

bool env_on(const char* k) {
    const char* v = std::getenv(k);
    return v && *v && std::strcmp(v, "0") != 0;
}

// Started by Steam (Game Mode, Big Picture, or a Steam shortcut on the desktop).
bool under_steam() { return env_on("SteamDeck") || env_on("SteamAppId") || env_on("SteamGameId"); }

// gamescope session (Steam Deck Game Mode): no desktop around us, so no folder dialog; the window is fullscreen.
bool game_mode() {
    const char* d = std::getenv("XDG_CURRENT_DESKTOP");
    return env_on("GAMESCOPE_WAYLAND_DISPLAY") || (d && strcasecmp(d, "gamescope") == 0);
}

enum class Input { Mouse, Pad, Touch };  // last input device ("Mouse" includes the keyboard)

// Gamepads, read here and fed to ImGui's gamepad navigation by hand (the SDL backend's own feeding is off): left stick
// = D-pad, right stick = scrolling, X / Square not fed (ImGui's window menu). Buttons held when the pads are opened (at
// start, and again after the game exits) or when a press is swallowed stay ignored until released, so the press that
// ended the game, or the first press that only shows the focus, never acts in the launcher.
struct Pad {
    enum Btn { A, B, Y, Start, L1, R1, Up, Down, Left, Right, Count };
    std::vector<SDL_Gamepad*> pads;
    bool rescan = true;
    bool raw[Count] = {}, held[Count] = {}, prev[Count] = {}, mute[Count] = {};
    float rx = 0, ry = 0;  // right stick, -1..1
    bool touched = false;  // a new press (or a pushed stick) this frame
    void close() {
        for (SDL_Gamepad* p : pads) SDL_CloseGamepad(p);
        pads.clear();
    }
    bool pressed(Btn b) const { return held[b] && !prev[b]; }
    void swallow() {  // what is held now stays ignored until released
        for (int i = 0; i < Count; ++i) mute[i] = mute[i] || raw[i], held[i] = false;
    }
    // enabled = false while the game runs: the pads are closed, so the launcher reads nothing.
    void update(bool enabled) {
        std::copy(held, held + Count, prev);
        touched = false;
        if (!enabled) {
            if (!pads.empty()) close();
            rescan = true;
            std::fill(raw, raw + Count, false), std::fill(held, held + Count, false);
            rx = ry = 0;
            return;
        }
        bool opened = false;
        if (rescan) {
            close();
            int n = 0;
            if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
                for (int i = 0; i < n; ++i)
                    if (SDL_Gamepad* p = SDL_OpenGamepad(ids[i])) pads.push_back(p);
                SDL_free(ids);
            }
            rescan = false, opened = true;
        }
        bool was[Count];
        std::copy(raw, raw + Count, was);
        std::fill(raw, raw + Count, false);
        float lx = 0, ly = 0;
        rx = ry = 0;
        auto axis = [](SDL_Gamepad* p, SDL_GamepadAxis a) { return std::clamp(SDL_GetGamepadAxis(p, a) / 32767.0f, -1.0f, 1.0f); };
        auto big = [](float a, float b) { return std::fabs(a) > std::fabs(b) ? a : b; };
        for (SDL_Gamepad* p : pads) {
            raw[A] |= SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_SOUTH);
            raw[B] |= SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_EAST);
            raw[Y] |= SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_NORTH);
            raw[Start] |= SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_START);
            raw[L1] |= SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
            raw[R1] |= SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
            raw[Up] |= SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_UP);
            raw[Down] |= SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
            raw[Left] |= SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
            raw[Right] |= SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
            lx = big(lx, axis(p, SDL_GAMEPAD_AXIS_LEFTX)), ly = big(ly, axis(p, SDL_GAMEPAD_AXIS_LEFTY));
            rx = big(rx, axis(p, SDL_GAMEPAD_AXIS_RIGHTX)), ry = big(ry, axis(p, SDL_GAMEPAD_AXIS_RIGHTY));
        }
        auto stick = [&](Btn b, float v) { raw[b] = raw[b] || v > (was[b] ? 0.3f : 0.5f); };  // with hysteresis
        stick(Right, lx), stick(Left, -lx), stick(Down, ly), stick(Up, -ly);
        if (opened) swallow();
        for (int i = 0; i < Count; ++i) {
            if (!raw[i]) mute[i] = false;
            held[i] = raw[i] && !mute[i];
            touched = touched || (held[i] && !prev[i]);
        }
        touched = touched || std::fabs(rx) > 0.5f || std::fabs(ry) > 0.5f;
    }
    // Into ImGui's gamepad keys (between the backend's NewFrame and ImGui::NewFrame). quiet: all released.
    // A is ImGui's "input" activation (Y), which presses buttons and also starts typing in a text field (ImGui's own
    // "activate" only selects one); a_tweak: A on a slider, where ImGui's "activate" (D-pad tweaking) is wanted.
    void feed(ImGuiIO& io, bool quiet, bool a_tweak) const {
        if (!pads.empty()) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        auto key = [&](ImGuiKey k, Btn b) { io.AddKeyEvent(k, held[b] && !quiet); };
        io.AddKeyEvent(ImGuiKey_GamepadFaceDown, held[A] && a_tweak && !quiet);
        io.AddKeyEvent(ImGuiKey_GamepadFaceUp, ((held[A] && !a_tweak) || held[Y]) && !quiet);
        key(ImGuiKey_GamepadFaceRight, B);
        key(ImGuiKey_GamepadL1, L1), key(ImGuiKey_GamepadR1, R1);
        key(ImGuiKey_GamepadDpadUp, Up), key(ImGuiKey_GamepadDpadDown, Down);
        key(ImGuiKey_GamepadDpadLeft, Left), key(ImGuiKey_GamepadDpadRight, Right);
        auto an = [&](ImGuiKey k, float v) {
            v = quiet ? 0.0f : std::clamp((v - 0.2f) / 0.8f, 0.0f, 1.0f);
            io.AddKeyAnalogEvent(k, v > 0.0f, v);
        };
        an(ImGuiKey_GamepadLStickLeft, -rx), an(ImGuiKey_GamepadLStickRight, rx);  // ImGui scrolls with "LStick"
        an(ImGuiKey_GamepadLStickUp, -ry), an(ImGuiKey_GamepadLStickDown, ry);
    }
};

// Touch screen: one finger is the mouse (touch source). A tap clicks. A drag that does not start on a slider or a
// scrollbar scrolls the scrollable window under the finger instead, and its press is dropped so nothing clicks.
// SDL's own touch -> mouse events are off (SDL_HINT_TOUCH_MOUSE_EVENTS=0), so each touch acts once.
struct Touch {
    bool down = false;
    SDL_FingerID finger = 0;
    ImVec2 start, pos;
    enum { Press, Scroll, Drag } mode = Press;
    ImGuiID scroll_win = 0;
    float scroll_dy = 0;     // pending, applied after NewFrame
    int drop_active = 0;     // frames left in which the dropped press's item is deactivated
    bool item_drag = false;  // the active item takes drags (slider, scrollbar); set each frame

    static ImGuiWindow* scrollable_at() {  // the hovered window, or the first parent that can scroll
        ImGuiContext& g = *GImGui;
        for (ImGuiWindow* w = g.HoveredWindow; w; w = (w->Flags & ImGuiWindowFlags_ChildWindow) ? w->ParentWindow : nullptr)
            if (w->ScrollMax.y > 0.0f) return w;
        return nullptr;
    }
    void event(const SDL_Event& e, ImGuiIO& io, float slop) {
        const ImVec2 p(e.tfinger.x * io.DisplaySize.x, e.tfinger.y * io.DisplaySize.y);
        if (e.type == SDL_EVENT_FINGER_DOWN) {
            if (down) return;  // one finger only
            down = true, finger = e.tfinger.fingerID, start = pos = p, mode = Press;
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMousePosEvent(p.x, p.y);
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMouseButtonEvent(0, true);
            return;
        }
        if (!down || e.tfinger.fingerID != finger) return;
        const float dy = p.y - pos.y;
        pos = p;
        if (e.type == SDL_EVENT_FINGER_MOTION) {
            if (mode == Press && std::hypot(p.x - start.x, p.y - start.y) > slop) {
                if (ImGuiWindow* w = item_drag ? nullptr : scrollable_at()) {
                    mode = Scroll, scroll_win = w->ID, drop_active = 3, scroll_dy += p.y - start.y;
                    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
                    io.AddMouseButtonEvent(0, false);
                    return;
                }
                mode = Drag;
            }
            if (mode == Scroll) scroll_dy += dy;
            else io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen), io.AddMousePosEvent(p.x, p.y);
            return;
        }
        // SDL_EVENT_FINGER_UP / _CANCELED
        down = false;
        if (mode != Scroll) {
            if (e.type == SDL_EVENT_FINGER_CANCELED) drop_active = 3;
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMousePosEvent(p.x, p.y);
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMouseButtonEvent(0, false);
        }
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);  // no hover left behind (ImGui applies it after the release)
    }
    void apply(bool log) {  // after ImGui::NewFrame
        if (drop_active > 0) {
            --drop_active;
            if (GImGui->ActiveId) ImGui::ClearActiveID();
        }
        if (scroll_dy != 0.0f) {
            if (ImGuiWindow* w = ImGui::FindWindowByID(scroll_win)) {
                ImGui::SetScrollY(w, w->Scroll.y - scroll_dy);
                if (log) std::fprintf(stderr, "launcher: touch scroll %s by %.0f\n", w->Name, -scroll_dy);
            }
            scroll_dy = 0;
        }
    }
};

// Moves the gamepad focus to the item just submitted (show: also draw the focus frame).
void focus_last_item(bool show) {
    ImGuiContext& g = *GImGui;
    ImGui::FocusWindow(g.CurrentWindow);
    ImGui::SetFocusID(g.LastItemData.ID, g.CurrentWindow);
    if (show) g.NavCursorVisible = g.NavHighlightItemUnderNav = true;
}

// Runs a program with argv (no shell), detached and reaped by poll_openers; LD_PRELOAD (Steam's overlay) removed.
void spawn_detached(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    std::vector<char*> env;
    for (char** e = environ; *e; ++e)
        if (std::strncmp(*e, "LD_PRELOAD=", 11) != 0) env.push_back(*e);
    env.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), env.data()) == 0) g_openers.push_back(pid);
    else std::fprintf(stderr, "launcher: cannot start %s\n", argv[0]);
    posix_spawn_file_actions_destroy(&fa);
}

// Steam's on-screen keyboard: the steam:// link SDL's X11 driver opens with SDL_ENABLE_STEAM_SCREEN_KEYBOARD (same
// parameters), but only when a text field was focused with the gamepad or the touch screen, once per focus.
void open_steam_keyboard(const ImVec2& pos, float line_h, bool multiline) {
    char url[192];
    std::snprintf(url, sizeof url, "steam://open/keyboard?XPosition=%d&YPosition=%d&Width=1&Height=%d&Mode=%d",
                  int(pos.x), int(pos.y), std::max(1, int(line_h)), multiline ? 1 : 0);
    std::fprintf(stderr, "launcher: on-screen keyboard: xdg-open %s\n", url);
    spawn_detached({"xdg-open", url});
}

bool is_dir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string home_dir() {
    const char* h = std::getenv("HOME");
    return h && *h == '/' && is_dir(h) ? h : "/";
}

// Built-in folder browser (a modal; gamepad / touch friendly), for when there is no desktop folder dialog (Game Mode)
// or the last input was the gamepad or the touch screen. Marks the folders that hold a complete dump.
struct FolderBrowser {
    int target = 0;  // 0 = dump folder, 1 = save folder (as g_pick_target)
    bool want_open = false, from_first_start = false;
    int refocus = 0;  // frames left to put the focus on the first row (the dialog's own focus lands on its first frame)
    std::string dir, error;
    struct Entry {
        std::string name;
        bool dump;
    };
    std::vector<Entry> entries;
    bool here_dump = false;
    std::vector<std::pair<std::string, std::string>> places;  // button label, path

    static std::string join(const std::string& d, const std::string& n) { return d == "/" ? "/" + n : d + "/" + n; }
    // Case-insensitive, numbers by value ("Folder 9" before "Folder 10").
    static bool natural_less(const std::string& a, const std::string& b) {
        auto digit = [](char c) { return c >= '0' && c <= '9'; };
        size_t i = 0, j = 0;
        while (i < a.size() && j < b.size()) {
            if (digit(a[i]) && digit(b[j])) {
                size_t i2 = i, j2 = j;
                while (i2 < a.size() && digit(a[i2])) ++i2;
                while (j2 < b.size() && digit(b[j2])) ++j2;
                std::string na = a.substr(i, i2 - i), nb = b.substr(j, j2 - j);
                na.erase(0, std::min(na.find_first_not_of('0'), na.size())), nb.erase(0, std::min(nb.find_first_not_of('0'), nb.size()));
                if (na.size() != nb.size()) return na.size() < nb.size();
                if (na != nb) return na < nb;
                i = i2, j = j2;
                continue;
            }
            const int ca = std::tolower(static_cast<unsigned char>(a[i])), cb = std::tolower(static_cast<unsigned char>(b[j]));
            if (ca != cb) return ca < cb;
            ++i, ++j;
        }
        return a.size() - i < b.size() - j;
    }
    static std::vector<std::string> subdirs(const std::string& d) {
        std::vector<std::string> out;
        if (DIR* h = opendir(d.c_str())) {
            while (dirent* e = readdir(h))
                if (e->d_name[0] != '.' && is_dir(join(d, e->d_name))) out.push_back(e->d_name);  // no hidden folders
            closedir(h);
        }
        std::sort(out.begin(), out.end(), natural_less);
        return out;
    }
    void go(std::string d) {
        while (d.size() > 1 && d.back() == '/') d.pop_back();
        if (d.empty() || !is_dir(d)) d = home_dir();
        dir = d, entries.clear(), error.clear(), refocus = 3;
        here_dump = dump_ok(validate_dump(d));
        if (DIR* h = opendir(d.c_str())) closedir(h);
        else error = std::string("Cannot open this folder: ") + std::strerror(errno);
        for (const std::string& n : subdirs(d)) entries.push_back({n, dump_ok(validate_dump(join(d, n)))});
    }
    void open(int t, const std::string& start, bool first_start) {
        target = t, from_first_start = first_start, want_open = true;
        places.clear();
        places.push_back({"Home", home_dir()});
        // SD cards / USB drives: /run/media/<user>/<label> (SteamOS), and older /run/media/<label> mount points.
        // DCP_LAUNCHER_MEDIA_ROOT replaces /run/media (tests).
        const char* mr = std::getenv("DCP_LAUNCHER_MEDIA_ROOT");
        const std::string media = mr && *mr ? mr : "/run/media";
        struct stat ms {};
        stat(media.c_str(), &ms);
        for (const std::string& a : subdirs(media)) {
            struct stat as {};
            if (stat(join(media, a).c_str(), &as) == 0 && as.st_dev != ms.st_dev) places.push_back({"SD/USB: " + a, join(media, a)});
            else
                for (const std::string& b : subdirs(join(media, a))) places.push_back({"SD/USB: " + b, join(join(media, a), b)});
        }
        for (const char* p : {"/media", "/mnt"})
            if (is_dir(p)) places.push_back({p, p});
        places.push_back({"/", "/"});
        go(start.empty() ? home_dir() : start);
    }
    // Call every frame at the main window's ID level. back: gamepad B (consumed). show_focus: the gamepad / touch is in
    // use. Returns true with *picked set when a folder was chosen; *cancelled when closed without one.
    bool draw(const ImVec2& display, bool& back, bool show_focus, std::string* picked, bool* cancelled) {
        if (want_open) ImGui::OpenPopup("Choose a folder"), want_open = false;
        ImGui::SetNextWindowSize(ImVec2(display.x * 0.9f, display.y * 0.88f), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("Choose a folder", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) return false;
        bool chosen = false, close = false;
        std::string go_to;
        ImGui::TextColored(kGold, "%s", target == 0 ? "Open the folder that holds your dump (eboot.bin and the .CPK files), then Select."
                                                    : "Open the folder for your saves (the game creates SAVEDATA00 inside it), then Select.");
        const ImGuiStyle& st = ImGui::GetStyle();
        const float avail = ImGui::GetContentRegionAvail().x;
        float x = 0;
        for (size_t i = 0; i < places.size(); ++i) {
            const float w = ImGui::CalcTextSize(places[i].first.c_str()).x + st.FramePadding.x * 2;
            if (i > 0 && x + st.ItemSpacing.x + w <= avail) ImGui::SameLine(), x += st.ItemSpacing.x + w;
            else x = w;
            ImGui::PushID(int(i));
            if (ImGui::Button(places[i].first.c_str())) go_to = places[i].second;
            ImGui::PopID();
        }
        ImGui::Spacing();
        ImGui::TextWrapped("%s", dir.c_str());
        if (here_dump) ImGui::TextColored(kGood, "[dump] This folder holds a complete dump.");
        const float foot = ImGui::GetFrameHeightWithSpacing() + st.ItemSpacing.y;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0.3f));
        ImGui::BeginChild("##dirs", ImVec2(0, -foot), ImGuiChildFlags_NavFlattened);
        ImGui::PopStyleColor();
        if (refocus == 3) ImGui::SetScrollY(0);
        bool first = true;
        auto row = [&](const std::string& label, bool dump, const std::string& path, int id) {
            ImGui::PushID(id);
            if (dump) ImGui::PushStyleColor(ImGuiCol_Text, kGold);
            if (ImGui::Selectable(label.c_str(), false, 0, ImVec2(0, ImGui::GetFrameHeight()))) go_to = path;
            if (dump) ImGui::PopStyleColor();
            if (first && refocus > 0) focus_last_item(show_focus);
            first = false;
            ImGui::PopID();
        };
        if (dir != "/") row("..   (up one folder)", false, parent_dir(dir), -1);
        for (size_t i = 0; i < entries.size(); ++i)
            row(entries[i].name + (entries[i].dump ? "      [dump]" : ""), entries[i].dump, join(dir, entries[i].name), int(i));
        if (!error.empty()) ImGui::TextColored(kBad, "%s", error.c_str());
        else if (entries.empty()) ImGui::TextColored(kDim, "(no folders in here)");
        ImGui::EndChild();
        refocus = std::max(0, refocus - 1);
        const bool good = target == 0 && here_dump;
        if (good) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.58f, 0.25f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.70f, 0.33f, 1));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.12f, 0.07f, 0.03f, 1));
        }
        if (ImGui::Button("Select this folder")) chosen = true, *picked = dir;
        if (good) ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("Cancel##browser")) close = true;
        if (target == 0 && !here_dump) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(kDim, "Not a complete dump here: folders marked [dump] are.");
        }
        if (back) back = false, close = true;
        if (chosen || close) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        if (!go_to.empty()) go(go_to);
        if (close && !chosen) *cancelled = true;
        return chosen;
    }
};

// ---- test hook: DCP_LAUNCHER_PAD_SCRIPT ----
// Drives a virtual SDL gamepad and injected touch events frame by frame, takes screenshots and logs (linux-port/README.md).
// Tokens, space separated: a b y x start back l1 r1 up down left right (pressed for 3 frames), lstick:X:Y:N /
// rstick:X:Y:N (held N frames, -1..1), wait:N, waitgame:N / waitnogame:N (until dcp_run runs / ended, at most N
// frames), tap:X:Y, drag:X0:Y0:X1:Y1 (window-normalised 0..1), shot:NAME (NAME.png in DCP_LAUNCHER_SHOT_DIR), log,
// stopgame (SIGTERM to dcp_run), quit.
struct Script {
    std::vector<std::string> toks;
    size_t next = 0;
    int resume = 0, cond = 0, cond_until = 0;  // cond: 1 = wait for the game to run, 2 = to end
    SDL_Joystick* joy = nullptr;
    std::deque<std::pair<int, std::function<void()>>> timed;
    std::string shot_dir;
    bool active() const { return !toks.empty(); }
    void init(const char* text) {
        std::istringstream in(text);
        for (std::string t; in >> t;) toks.push_back(t);
        const char* d = std::getenv("DCP_LAUNCHER_SHOT_DIR");
        shot_dir = d && *d ? d : ".";
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "OpenCrown test pad";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        joy = id ? SDL_OpenJoystick(id) : nullptr;
        std::fprintf(stderr, "script: %zu steps, virtual pad %s\n", toks.size(), joy ? "attached" : SDL_GetError());
    }
    static void finger(SDL_Window* win, Uint32 type, float x, float y) {
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = type;
        ev.tfinger.touchID = 1, ev.tfinger.fingerID = 1, ev.tfinger.x = x, ev.tfinger.y = y, ev.tfinger.pressure = 1;
        ev.tfinger.windowID = SDL_GetWindowID(win);
        SDL_PushEvent(&ev);
    }
    // Once per frame before the events are read. Sets *shot / *log / *stop / *quit for the caller.
    void step(int frame, bool game_running, SDL_Window* win, std::string* shot, bool* log, bool* stop, bool* quit) {
        while (!timed.empty() && timed.front().first <= frame) {
            auto fn = std::move(timed.front().second);
            timed.pop_front();
            fn();
        }
        if (cond) {
            if ((cond == 1) != game_running && frame < cond_until) return;
            std::fprintf(stderr, "script: frame %d: %s%s\n", frame, cond == 1 ? "game running" : "game ended",
                         (cond == 1) != game_running ? " - TIMEOUT" : "");
            cond = 0;
        }
        if (frame < resume || next >= toks.size()) return;
        const std::string t = toks[next++];
        std::fprintf(stderr, "script: frame %d: %s\n", frame, t.c_str());
        std::vector<std::string> a;
        {
            std::stringstream ss(t);
            for (std::string p; std::getline(ss, p, ':');) a.push_back(p);
        }
        auto f = [&](size_t i) { return i < a.size() ? std::strtof(a[i].c_str(), nullptr) : 0.0f; };
        auto at = [&](int dt, std::function<void()> fn) {
            auto it = std::upper_bound(timed.begin(), timed.end(), frame + dt, [](int v, const auto& e) { return v < e.first; });
            timed.insert(it, {frame + dt, std::move(fn)});
        };
        static const std::map<std::string, SDL_GamepadButton> kButtons = {
            {"a", SDL_GAMEPAD_BUTTON_SOUTH}, {"b", SDL_GAMEPAD_BUTTON_EAST}, {"x", SDL_GAMEPAD_BUTTON_WEST},
            {"y", SDL_GAMEPAD_BUTTON_NORTH}, {"start", SDL_GAMEPAD_BUTTON_START}, {"back", SDL_GAMEPAD_BUTTON_BACK},
            {"l1", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"r1", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
            {"up", SDL_GAMEPAD_BUTTON_DPAD_UP}, {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
            {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT}};
        resume = frame + 1;
        if (auto b = kButtons.find(a[0]); b != kButtons.end()) {
            SDL_Joystick* j = joy;
            const int btn = b->second;
            SDL_SetJoystickVirtualButton(j, btn, true);
            at(3, [j, btn] { SDL_SetJoystickVirtualButton(j, btn, false); });
            resume = frame + 10;
        } else if (a[0] == "lstick" || a[0] == "rstick") {
            SDL_Joystick* j = joy;
            const int ax = a[0] == "lstick" ? SDL_GAMEPAD_AXIS_LEFTX : SDL_GAMEPAD_AXIS_RIGHTX;
            SDL_SetJoystickVirtualAxis(j, ax, Sint16(std::clamp(f(1), -1.0f, 1.0f) * 32767));
            SDL_SetJoystickVirtualAxis(j, ax + 1, Sint16(std::clamp(f(2), -1.0f, 1.0f) * 32767));
            const int n = std::max(1, int(f(3)));
            at(n, [j, ax] { SDL_SetJoystickVirtualAxis(j, ax, 0), SDL_SetJoystickVirtualAxis(j, ax + 1, 0); });
            resume = frame + n + 6;
        } else if (a[0] == "wait") resume = frame + int(f(1));
        else if (a[0] == "waitgame" || a[0] == "waitnogame") cond = a[0] == "waitgame" ? 1 : 2, cond_until = frame + int(f(1));
        else if (a[0] == "tap") {
            const float x = f(1), y = f(2);
            finger(win, SDL_EVENT_FINGER_DOWN, x, y);
            at(4, [win, x, y] { finger(win, SDL_EVENT_FINGER_UP, x, y); });
            resume = frame + 10;
        } else if (a[0] == "drag") {
            const float x0 = f(1), y0 = f(2), x1 = f(3), y1 = f(4);
            finger(win, SDL_EVENT_FINGER_DOWN, x0, y0);
            for (int i = 1; i <= 12; ++i)
                at(1 + i, [win, x0, y0, x1, y1, i] {
                    finger(win, SDL_EVENT_FINGER_MOTION, x0 + (x1 - x0) * i / 12.0f, y0 + (y1 - y0) * i / 12.0f);
                });
            at(15, [win, x1, y1] { finger(win, SDL_EVENT_FINGER_UP, x1, y1); });
            resume = frame + 22;
        } else if (a[0] == "shot") *shot = shot_dir + "/" + (a.size() > 1 ? a[1] : "shot") + ".png";
        else if (a[0] == "log") *log = true;
        else if (a[0] == "stopgame") *stop = true;
        else if (a[0] == "quit") *quit = true;
        else std::fprintf(stderr, "script: unknown step %s\n", t.c_str());
    }
};

// Back buffer -> PNG (test screenshots), before the swap.
bool save_screenshot(const std::string& path, int w, int h) {
    if (!gl.ReadPixels || w <= 0 || h <= 0) return false;
    std::vector<uint8_t> px(size_t(w) * h * 4), out(px.size());
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl.ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < h; ++y) std::memcpy(out.data() + size_t(y) * w * 4, px.data() + size_t(h - 1 - y) * w * 4, size_t(w) * 4);
    for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    SDL_Surface* s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ABGR8888, out.data(), w * 4);
    const bool ok = s && SDL_SavePNG(s, path.c_str());
    if (s) SDL_DestroySurface(s);
    std::fprintf(stderr, "script: screenshot %s %s\n", path.c_str(), ok ? "saved" : SDL_GetError());
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    bool selftest = false;
    std::string first_tab = "Game";  // --tab <name>: open that page first (e.g. --tab Online)
    std::string updated_from;  // --updated <old version>: started by the self-updater, say so once
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--selftest") == 0) selftest = true;
        else if (std::strcmp(argv[i], "--tab") == 0 && i + 1 < argc) first_tab = argv[++i];
        else if (std::strcmp(argv[i], "--updated") == 0 && i + 1 < argc) updated_from = argv[++i], first_tab = "Update";
        else if (std::strcmp(argv[i], "--update-check") == 0) {
            dcp::net::update::Result r = dcp::net::update::check(OPENCROWN_VERSION);
            if (!r.ok) {
                std::printf("update check failed: %s\n", r.error.c_str());
                return 1;
            }
            if (r.latest.empty()) std::printf("up to date (v%s)\n", OPENCROWN_VERSION);
            else if (r.have_target)
                std::printf("update available: %s (%s, %lld B, sha256 %s)\n", r.target_tag.c_str(), r.asset.name.c_str(),
                            (long long)r.asset.size, r.asset.sha256.empty() ? "-" : r.asset.sha256.c_str());
            else std::printf("v%s is out, but has no Linux build newer than v%s\n", r.latest.c_str(), OPENCROWN_VERSION);
            std::printf("%s", dcp::net::update::notes_text(r.notes).c_str());
            return 0;
        }

    const std::string user = dcp::user_dir();
    SDL_CreateDirectory((user + "/logs").c_str());  // creates user too
    const std::string settings_path = user + "/launcher.ini", input_path = user + "/input.ini",
                      log_path = user + "/logs/last_run.log", saves = user + "/saves";
    Settings set;
    load_settings(settings_path, set);
    const std::string fallback_name = valid_name(set.name) ? set.name : random_player_name();
    if (!valid_name(set.name)) {
        set.name = fallback_name;
        if (!selftest) save_settings(settings_path, set);
    }
    Controls ctl = load_controls(input_path);
    std::string status;  // one-line feedback under the Controls table

    SDL_SetAppMetadata("OpenCrownPC Launcher", OPENCROWN_VERSION, nullptr);
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");      // touches are handled by Touch (tap = click, drag = scroll)
    SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");  // Steam's keyboard is opened by us, for gamepad / touch only
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD) && !SDL_Init(SDL_INIT_VIDEO)) {
        error_box("OpenCrownPC Launcher", SDL_GetError());
        return 1;
    }
    Script script;
    if (const char* s = std::getenv("DCP_LAUNCHER_PAD_SCRIPT"); s && *s && !selftest) script.init(s);
    const bool gm = game_mode(), steam = under_steam();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (!(scale > 0.0f)) scale = 1.0f;
    // Steam Deck (Game Mode, or started by Steam on a Deck): 25% larger text and touch targets on the 7" 1280x800
    // screen, window no larger than the screen (fullscreen in Game Mode). DCP_LAUNCHER_SCALE overrides the factor.
    float deck = gm || env_on("SteamDeck") ? 1.25f : 1.0f;
    if (const char* s = std::getenv("DCP_LAUNCHER_SCALE"); s && *s) deck = std::clamp(std::strtof(s, nullptr), 0.5f, 4.0f);
    int win_w = int(1040 * scale * deck), win_h = int(660 * scale * deck);
    SDL_Rect ub;
    if ((gm || deck != 1.0f) && SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &ub) && ub.w > 0 && ub.h > 0) {
        if (gm) win_w = ub.w, win_h = ub.h;
        win_w = std::min(win_w, ub.w), win_h = std::min(win_h, ub.h);
    }
    if (const char* s = std::getenv("DCP_LAUNCHER_WINDOW"); s && *s) {  // tests: WxH
        int w = 0, h = 0;
        if (std::sscanf(s, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) win_w = w, win_h = h;
    }
    SDL_Window* win = SDL_CreateWindow("OpenCrownPC Launcher", win_w, win_h,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                           (gm ? SDL_WINDOW_FULLSCREEN : 0) | (selftest ? SDL_WINDOW_HIDDEN : 0));
    if (!win) {
        error_box("OpenCrownPC Launcher", SDL_GetError());
        return 1;
    }
    const float min_w = 820 * scale, min_h = 540 * scale;
    scale *= deck;
    SDL_GLContext glctx = SDL_GL_CreateContext(win);
    if (!glctx || (SDL_GL_MakeCurrent(win, glctx), !load_gl())) {
        error_box("OpenCrownPC Launcher (OpenGL 3.3 needed)", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(win, glctx);
    SDL_GL_SetSwapInterval(1);
    SDL_SetWindowMinimumSize(win, std::min(int(min_w * deck), win_w), std::min(int(min_h * deck), win_h));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // our own launcher.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // fed by Pad (keyboard navigation stays off, as before)
    apply_theme(scale);
    // A system sans (Noto / Ubuntu / Liberation / DejaVu, in that order: the first ones are closest to Segoe UI on
    // Windows) reads better than the built-in bitmap font; fall back to that.
    const char* font_ui = first_font({"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                                      "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                                      "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
                                      "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
                                      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                                      "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
                                      "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
                                      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                      "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                      "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"});
    if (!font_ui || !io.Fonts->AddFontFromFileTTF(font_ui, 17.0f * scale)) {
        ImFontConfig fc;
        fc.SizePixels = 13.0f * scale;
        io.Fonts->AddFontDefault(&fc);
    }
    // Headings: a bold serif (Palatino on Windows) suits the game's storybook look.
    ImFont* font_head = nullptr;
    ImFont* font_nav = nullptr;
    if (const char* f = first_font({"/usr/share/fonts/truetype/noto/NotoSerif-Bold.ttf",
                                    "/usr/share/fonts/noto/NotoSerif-Bold.ttf",
                                    "/usr/share/fonts/google-noto/NotoSerif-Bold.ttf",
                                    "/usr/share/fonts/truetype/liberation/LiberationSerif-Bold.ttf",
                                    "/usr/share/fonts/liberation-serif/LiberationSerif-Bold.ttf",
                                    "/usr/share/fonts/TTF/LiberationSerif-Bold.ttf",
                                    "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf",
                                    "/usr/share/fonts/TTF/DejaVuSerif-Bold.ttf",
                                    "/usr/share/fonts/dejavu-serif-fonts/DejaVuSerif-Bold.ttf"})) {
        font_head = io.Fonts->AddFontFromFileTTF(f, 27.0f * scale);
        font_nav = io.Fonts->AddFontFromFileTTF(f, 19.0f * scale);
    }
    ImGui_ImplSDL3_InitForOpenGL(win, glctx);
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);  // Pad feeds the gamepad keys
    ImGui_ImplOpenGL3_Init("#version 330");

    Game game;
    ConnTest conn;
    Art art;
    // Online identity shared with dcp_run (identity.ini in the user dir, created on first start).
    const dcp::net::Identity ident = dcp::net::load_identity(user);
    FriendsJob friends;
    std::string add_code, confirm_remove;
    BugJob bug;
    std::string bug_text, bug_preview;
    UpdateJob upd;
    const std::string appimage = appimage_path();
    if (!appimage.empty() && !selftest) ::unlink((appimage + ".part").c_str());  // leftover of an interrupted update
    if (set.check_updates && !selftest) upd.check();
    std::string relaunch_error;
    bool relaunched = false;
    bool ask_saves = !set.saves_chosen && !selftest;  // first start: where should saves go?
    int capture = -1;      // index into ctl.binds awaiting a key / mouse button
    bool first = true;
    int page = 0;          // index into the page names (sidebar)
    std::string dump_buf = set.dump;
    std::string pick_msg;          // folder dialog failure, shown on the Game / Save Folder page
    bool type_saves = false;       // Save Folder page shows a path field (after a dialog failure)
    std::string saves_buf;
    std::vector<Check> checks = validate_dump(set.dump);
    int frames = 0;
    bool quit = false;
    // Gamepad / touch (Steam Deck Game Mode).
    Pad pad;
    Touch touch;
    Input input = Input::Mouse;
    FolderBrowser browser;
    bool pick_unavailable = gm;  // no desktop folder dialog (Game Mode, or a dialog failed): use the built-in browser
    bool pad_seen = false;       // the first gamepad press only shows the focus
    bool nav_to_sidebar = false; // move the gamepad focus to the current sidebar entry while drawing it
    bool was_quiet = false;
    int popups_last = 0;
    ImGuiID slider_id = 0;  // the fairy speed slider (gamepad A tweaks it with the D-pad instead of typing)
    bool a_tweak = false;
    // Left / Right between the sidebar and the page: ImGui only moves sideways onto items that overlap the row, so when
    // such a move leaves the focus where it was, it jumps to the page's first item (marked by page_entry) / the sidebar.
    ImGuiWindow* side_win = nullptr;
    ImGuiID move_from = 0;
    int move_check = -1, move_dir = 0, frame_no = 0;
    bool want_page_focus = false;
    auto page_entry = [&] {
        if (want_page_focus) focus_last_item(true), want_page_focus = false;
    };
    ImGuiID text_id_last = 0;    // text field being edited (Steam keyboard: once per focus)
    const bool input_log = env_on("DCP_LAUNCHER_INPUT_LOG") || script.active();
    auto browse = [&](int target, const std::string& start, bool first_start) {
        if (pick_unavailable || input != Input::Mouse) browser.open(target, start, first_start);
        else if (target == 1) SDL_ShowOpenFolderDialog(on_folder, reinterpret_cast<void*>(intptr_t(1)), win,
                                                       start.empty() ? nullptr : start.c_str(), false);
        else SDL_ShowOpenFolderDialog(on_folder, nullptr, win, start.empty() ? nullptr : start.c_str(), false);
    };
    int script_frame = 0, script_page = -1;
    while (!quit) {
        std::string shot_path;
        bool script_log = false, script_stop = false;
        if (script.active())
            script.step(script_frame++, game.proc != 0, win, &shot_path, &script_log, &script_stop, &quit);
        if (script_stop && game.proc) kill(game.proc, SIGTERM);
        if (game.proc && !script.active()) SDL_WaitEventTimeout(nullptr, 100);  // idle while the game runs
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_MOTION || e.type == SDL_EVENT_FINGER_UP ||
                e.type == SDL_EVENT_FINGER_CANCELED) {
                if (game.proc) continue;
                if (e.type == SDL_EVENT_FINGER_DOWN) input = Input::Touch;
                if (capture < 0) touch.event(e, io, 14 * scale);
                continue;
            }
            if ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.which != SDL_TOUCH_MOUSEID) ||
                (e.type == SDL_EVENT_MOUSE_WHEEL && e.wheel.which != SDL_TOUCH_MOUSEID) || e.type == SDL_EVENT_KEY_DOWN)
                input = Input::Mouse;
            if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) pad.rescan = true;
            if (capture >= 0) {  // binding capture swallows keys and mouse buttons
                if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                    const char* n = SDL_GetScancodeName(e.key.scancode);
                    if (e.key.scancode == SDL_SCANCODE_BACKSPACE || e.key.scancode == SDL_SCANCODE_DELETE)
                        ctl.binds[capture].value.clear();
                    else if (e.key.scancode != SDL_SCANCODE_ESCAPE && n && *n) {  // Escape cancels
                        // '#' starts a comment and ',' separates keys in input.ini, so such key names cannot be stored.
                        if (std::strpbrk(n, "#,")) status = std::string("The key \"") + n + "\" cannot be bound.";
                        else ctl.binds[capture].value = n;
                    }
                    capture = -1;
                    continue;
                }
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    if (std::string n = mouse_name(e.button.button); !n.empty()) ctl.binds[capture].value = n;
                    capture = -1;
                    continue;
                }
                if (e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_TEXT_INPUT)
                    continue;
            }
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) quit = true;
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(win)) quit = true;
        }
        {
            std::lock_guard<std::mutex> lk(g_pick_mu);
            if (g_have_pick) {
                g_have_pick = false;
                if (g_pick_target == 1) {
                    set.saves = g_picked, set.saves_chosen = true;
                    save_settings(settings_path, set);
                } else {
                    set.dump = dump_buf = g_picked;
                    checks = validate_dump(set.dump);
                }
            }
            if (g_pick_failed) {  // no folder dialog on this desktop: say so, and offer the typed path instead
                g_pick_failed = false;
                std::fprintf(stderr, "launcher: folder dialog failed: %s\n", g_pick_error.c_str());
                pick_msg = "No folder dialog on this desktop (it needs xdg-desktop-portal or zenity): type the path.";
                if (g_pick_target == 1) page = 4, type_saves = true, saves_buf = set.saves;
                else page = 0;
                pick_unavailable = true;  // from now on the built-in browser, opened right away
                browser.open(int(g_pick_target), g_pick_target == 1 ? set.saves : set.dump, false);
            }
        }
        poll_game(game, log_path);
        poll_openers();

        // Gamepad: read (closed while the game runs), then decide this frame's own actions from the state ImGui ended
        // the last frame with (B on a page = back to the sidebar, B in a dialog = close it, L1/R1 = page, Start = Play).
        pad.update(game.proc == 0);
        if (pad.touched) {
            input = Input::Pad;
            if (!pad_seen) pad_seen = true, pad.swallow(), nav_to_sidebar = true;  // first press: show the focus only
        }
        ImGuiContext& gx = *ImGui::GetCurrentContext();
        const bool popup_open = gx.OpenPopupStack.Size > 0, item_active = gx.ActiveId != 0;
        if (popups_last && !popup_open && input == Input::Pad && gx.NavId == 0) nav_to_sidebar = true;  // dialog closed
        popups_last = gx.OpenPopupStack.Size;
        ++frame_no;
        if (move_check >= 0 && frame_no >= move_check) {
            if (gx.NavId == move_from && !popup_open) (move_dir > 0 ? want_page_focus : nav_to_sidebar) = true;
            move_check = -1;
        }
        want_page_focus = want_page_focus && !popup_open;
        if (!popup_open && !item_active && capture < 0 && gx.NavWindow && side_win) {
            const bool in_side = gx.NavWindow == side_win, in_main = gx.NavWindow->RootWindowForNav == side_win->RootWindowForNav;
            if ((pad.pressed(Pad::Right) && in_side) || (pad.pressed(Pad::Left) && !in_side && in_main))
                move_from = gx.NavId, move_check = frame_no + 3, move_dir = pad.pressed(Pad::Right) ? 1 : -1;
        }
        if (nav_to_sidebar && popup_open) {  // first press with a dialog open: show the focus there instead
            nav_to_sidebar = false;
            gx.NavCursorVisible = gx.NavHighlightItemUnderNav = true;
        }
        bool pad_close = false, pad_play = false;
        if (capture >= 0) {
            if (pad.pressed(Pad::B)) capture = -1, pad.swallow();  // gamepad cannot bind keys: B cancels
        } else if (pad.pressed(Pad::B) && !item_active) {
            const bool nested = gx.NavWindow && gx.NavWindow->RootWindowForNav && gx.NavWindow->RootWindowForNav->ParentWindow;
            if (popup_open) pad_close = true;
            else if (!nested) nav_to_sidebar = true;  // (in a scrolling list ImGui leaves the list first)
        } else if (!popup_open && !item_active) {
            if (pad.pressed(Pad::L1) || pad.pressed(Pad::R1)) {
                const int cycle[] = {0, 4, 1, 2, 3, 5, 6};  // sidebar order, Update last
                int k = 0;
                while (k < 7 && cycle[k] != page) ++k;
                page = cycle[(k % 7 + (pad.pressed(Pad::R1) ? 1 : 6)) % 7];
                nav_to_sidebar = true;
            }
            pad_play = pad.pressed(Pad::Start);
        }
        const bool quiet = capture >= 0 || game.proc != 0;
        if (was_quiet && !quiet) pad.swallow();  // the press that ended a binding capture does not act again
        was_quiet = quiet;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        if (pad.pressed(Pad::A)) a_tweak = slider_id && gx.NavId == slider_id;
        pad.feed(io, quiet, a_tweak);
        ImGui::NewFrame();
        touch.apply(input_log);
        bool slider_active = false;  // touch: the fairy speed slider takes drags
        art.update(dump_ok(checks) ? set.dump : "", win);
        draw_background(art, io.DisplaySize);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                            ImGuiWindowFlags_NoBackground);
        // "Update" (6) is reached through the entry at the bottom of the sidebar, not the navigation list.
        const char* names[] = {"Game", "Controls", "Display", "Online", "Save Folder", "Report a Bug", "Update"};
        const int order[] = {0, 4, 1, 2, 3, 5};  // sidebar order; indexes names
        if (first)
            for (int t = 0; t < 7; ++t)
                if (strcasecmp(names[t], first_tab.c_str()) == 0) page = t;
        // Update state snapshot for this frame.
        bool u_running, u_downloading, u_checked, u_installed;
        dcp::net::update::Result u_res;
        uint64_t u_got;
        int64_t u_total;
        std::string u_dl_error;
        {
            std::lock_guard<std::mutex> lk(upd.mu);
            u_running = upd.running, u_downloading = upd.downloading, u_checked = upd.checked;
            u_installed = upd.installed, u_got = upd.got, u_total = upd.total, u_dl_error = upd.dl_error;
            if (u_checked) u_res = upd.res;
        }
        if (u_installed && relaunch_error.empty() && !relaunched) {  // new AppImage in place: start it, then leave
            save_settings(settings_path, set);  // before the new launcher reads it (no save on the way out)
            if (relaunch(appimage, &relaunch_error)) quit = relaunched = true;
        }

        // Sidebar: logo, navigation, readiness.
        const float side_w = 230 * scale;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.03f, 0.02f, 0.02f, art.bg.id ? 0.62f : 0.35f));
        ImGui::BeginChild("##side", ImVec2(side_w, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
        ImGui::PopStyleColor();
        side_win = ImGui::GetCurrentWindow();
        {
            float w = ImGui::GetContentRegionAvail().x;
            if (art.logo.id) {
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddImageRounded(tex_id(art.logo), p, ImVec2(p.x + w, p.y + w), ImVec2(0, 0),
                                                            ImVec2(1, 1), IM_COL32_WHITE, 8 * scale);
                ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + w, p.y + w), IM_COL32(235, 184, 97, 90), 8 * scale);
                ImGui::Dummy(ImVec2(w, w));
            } else {
                if (font_head) ImGui::PushFont(font_head);
                ImGui::TextColored(kGold, "Dragon's Crown\nPro");
                if (font_head) ImGui::PopFont();
            }
            ImGui::TextColored(kDim, "OpenCrownPC Launcher");
            ImGui::Dummy(ImVec2(0, 6 * scale));
            if (font_nav) ImGui::PushFont(font_nav);
            for (int t : order) {
                const bool sel = page == t;
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::PushStyleColor(ImGuiCol_Text, sel ? kGold : kParchment);
                if (ImGui::Selectable((std::string("   ") + names[t] + "##nav").c_str(), sel, 0,
                                      ImVec2(0, ImGui::GetTextLineHeight() + 12 * scale)))
                    page = t;
                if (sel && nav_to_sidebar) focus_last_item(true), nav_to_sidebar = false;
                ImGui::PopStyleColor();
                if (sel)  // gold marker at the left edge of the selected entry
                    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 4 * scale),
                                                              ImVec2(p.x + 3 * scale, p.y + ImGui::GetItemRectSize().y - 4 * scale),
                                                              IM_COL32(235, 184, 97, 255), 2 * scale);
            }
            if (font_nav) ImGui::PopFont();
            // Update entry (same style as the navigation above), its state line, then the version, pinned to the
            // bottom-left of the sidebar.
            if (font_nav) ImGui::PushFont(font_nav);
            const float upd_h = ImGui::GetTextLineHeight() + 12 * scale;
            if (font_nav) ImGui::PopFont();
            const float line_h = ImGui::GetTextLineHeight(), gap = ImGui::GetStyle().ItemSpacing.y;
            const float upd_y = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - 2 * line_h - 2 * gap - upd_h;
            if (upd_y > ImGui::GetCursorPosY()) ImGui::SetCursorPosY(upd_y);
            {
                std::string state;
                ImVec4 col = kDim;
                bool avail = false;
                char pct[64];
                if (!updated_from.empty()) state = "Updated to v" OPENCROWN_VERSION, col = kGood;
                else if (u_downloading) {
                    std::snprintf(pct, sizeof pct, "Downloading... %d%%",
                                  u_total > 0 ? int(u_got * 100 / uint64_t(u_total)) : 0);
                    state = pct, col = kGold;
                } else if (u_running) state = "Checking for updates...";
                else if (!u_checked) state = "Updates not checked";
                else if (!u_res.ok) state = "Update check failed";
                else if (u_res.have_target) state = "Update available: " + u_res.target_tag, col = kGold, avail = true;
                else state = "Up to date (v" OPENCROWN_VERSION ")";
                const bool sel = page == 6;
                if (font_nav) ImGui::PushFont(font_nav);
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::PushStyleColor(ImGuiCol_Text, sel || avail ? kGold : kParchment);
                if (ImGui::Selectable("   Update##update", sel, 0, ImVec2(0, upd_h))) page = 6;
                if (sel && nav_to_sidebar) focus_last_item(true), nav_to_sidebar = false;
                ImGui::PopStyleColor();
                if (avail || u_downloading)  // gold dot after the label
                    ImGui::GetWindowDrawList()->AddCircleFilled(
                        ImVec2(p.x + ImGui::CalcTextSize("   Update").x + 10 * scale, p.y + upd_h * 0.5f), 4.5f * scale,
                        IM_COL32(235, 184, 97, 255));
                if (font_nav) ImGui::PopFont();
                if (sel)
                    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 4 * scale),
                                                              ImVec2(p.x + 3 * scale, p.y + upd_h - 4 * scale),
                                                              IM_COL32(235, 184, 97, 255), 2 * scale);
                ImGui::TextColored(col, "%s", state.c_str());
            }
            ImGui::TextColored(kDim, "v" OPENCROWN_VERSION);
        }
        ImGui::EndChild();
        ImGui::SameLine(0, 14 * scale);

        // Content column: the page panel, then the Play bar.
        const float bar_h = ImGui::GetFrameHeight() * 1.6f + ImGui::GetStyle().WindowPadding.y * 2;
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.04f, 0.035f, art.bg.id ? 0.78f : 0.6f));
        ImGui::BeginChild("##page", ImVec2(0, -bar_h - ImGui::GetStyle().ItemSpacing.y),
                          ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
        ImGui::PopStyleColor();
        {
            const int t = page;
            heading(font_head, names[t]);
            if (t == 0) {
                ImGui::TextUnformatted("Personal game dump folder (your own copy; holds eboot.bin and the .CPK data files):");
                ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Browse...").x - ImGui::GetStyle().FramePadding.x * 4);
                char buf[1024];
                std::snprintf(buf, sizeof buf, "%s", dump_buf.c_str());
                if (ImGui::InputText("##dump", buf, sizeof buf)) {
                    dump_buf = buf;
                    set.dump = trim(dump_buf);
                    checks = validate_dump(set.dump);
                }
                page_entry();
                ImGui::SameLine();
                if (ImGui::Button("Browse...")) browse(0, set.dump, false);
                if (!pick_msg.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
                    ImGui::TextWrapped("%s", pick_msg.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::Spacing();
                for (const Check& c : checks) {
                    ImGui::TextColored(c.ok ? kGood : c.required ? kBad : kWarn, "%s", c.ok ? "[ok]" : c.required ? c.fail : "[optional]");
                    ImGui::SameLine();
                    ImGui::TextUnformatted(c.text.c_str());
                }
                ImGui::Spacing();
                if (!dump_ok(checks)) ImGui::TextColored(kBad, "The dump folder is incomplete or not supported; the game cannot start.");
            } else if (t == 1) {
                // A key on two actions is allowed (l1/r1 share Left Shift by default) but flagged. fairy_tap / pointer_tap
                // only apply in their pointer modes, so it is not compared with the normal-mode actions.
                auto key_of = [](const std::string& v) {
                    std::string l = v;
                    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
                    return l;
                };
                std::map<std::string, std::vector<std::string>> users;
                for (const Binding& b : ctl.binds)
                    if (!b.value.empty() && b.action != "fairy_tap" && b.action != "pointer_tap") users[key_of(b.value)].push_back(b.action);
                ImGui::TextColored(kDim, "Click a binding, then press a key or mouse button. Esc cancels, "
                                         "Backspace/Delete clears.");
                // The slider, the two checkboxes and the button row fit under the table (no page scrolling).
                float table_h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 4.0f;
                if (ImGui::BeginTable("##binds", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                                        ImGuiTableFlags_ScrollY, ImVec2(0, table_h))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableHeadersRow();
                    for (int i = 0; i < int(ctl.binds.size()); ++i) {
                        Binding& b = ctl.binds[i];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::AlignTextToFramePadding();
                        auto lab = kLabels.find(b.action);
                        ImGui::TextUnformatted(lab != kLabels.end() ? lab->second : b.action.c_str());
                        ImGui::TableNextColumn();
                        ImGui::PushID(i);
                        bool conflict = b.action != "fairy_tap" && b.action != "pointer_tap" && !b.value.empty() && users[key_of(b.value)].size() > 1;
                        std::string text = capture == i ? "Press a key or mouse button..."
                                         : b.value.empty() ? "(unbound)" : pretty(b.value);
                        if (conflict) ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
                        else if (b.value.empty()) ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                        if (ImGui::Button(text.c_str(), ImVec2(-FLT_MIN, 0))) capture = i;
                        if (conflict || b.value.empty()) ImGui::PopStyleColor();
                        if (conflict && ImGui::IsItemHovered()) {
                            std::string also;
                            for (const std::string& a : users[key_of(b.value)])
                                if (a != b.action) also += (also.empty() ? "" : ", ") + a;
                            ImGui::SetTooltip("Also bound to: %s", also.c_str());
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::SetNextItemWidth(220 * scale);
                ImGui::SliderFloat("Fairy speed", &ctl.fairy_speed, 0.1f, 5.0f, "%.2f");
                slider_active = slider_active || ImGui::IsItemActive();
                slider_id = ImGui::GetItemID();
                page_entry();
                ImGui::Checkbox("Touch screen moves the fairy (tap = interact)", &ctl.touch_screen);
                ImGui::Checkbox("Steam Deck trackpads: right = fairy, left click = town map", &ctl.deck_trackpads);
                if (ImGui::Button("Reset to defaults")) { ctl = default_controls(); status = "Defaults restored (not saved yet)."; }
                ImGui::SameLine();
                if (ImGui::Button("Save")) status = save_controls(input_path, ctl) ? "Saved " + input_path
                                                                                   : "Could not write " + input_path;
                ImGui::SameLine();
                if (ImGui::Button("Revert")) { ctl = load_controls(input_path); status = "Reloaded from input.ini."; }
                ImGui::SameLine();
                ImGui::TextColored(kDim, "%s", status.c_str());
            } else if (t == 2) {
                int mode = set.fullscreen ? 1 : 0;
                ImGui::RadioButton("Windowed", &mode, 0);
                page_entry();
                ImGui::SameLine();
                ImGui::RadioButton("Fullscreen (borderless)", &mode, 1);
                set.fullscreen = mode == 1;
                ImGui::Spacing();
                ImGui::Checkbox("4K rendering (PS4 Pro mode)", &set.uhd);
                ImGui::TextColored(kDim, "The game renders at 3840x2160 with its Pro assets; the window scales the picture.");
                ImGui::Spacing();
                ImGui::TextUnformatted("Window size:");
                static const int kPresets[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}};
                int sel = 3;
                for (int p = 0; p < 3; ++p)
                    if (set.width == kPresets[p][0] && set.height == kPresets[p][1]) sel = p;
                static bool custom = false;
                if (sel == 3) custom = true;
                for (int p = 0; p < 3; ++p) {
                    char l[32];
                    std::snprintf(l, sizeof l, "%dx%d", kPresets[p][0], kPresets[p][1]);
                    if (ImGui::RadioButton(l, !custom && sel == p)) {
                        set.width = kPresets[p][0], set.height = kPresets[p][1];
                        custom = false;
                    }
                    ImGui::SameLine();
                }
                if (ImGui::RadioButton("Custom", custom)) custom = true;
                if (custom) {
                    ImGui::SetNextItemWidth(120 * scale);
                    ImGui::InputInt("Width", &set.width, 0);
                    ImGui::SetNextItemWidth(120 * scale);
                    ImGui::InputInt("Height", &set.height, 0);
                    set.width = std::clamp(set.width, 320, 16384), set.height = std::clamp(set.height, 180, 16384);
                }
                ImGui::Spacing();
                ImGui::TextColored(kDim, "Fullscreen uses the desktop resolution; the window size applies when windowed.\n"
                                         "In game, F1 shows frame statistics in the title bar.");
            } else if (t == 3) {
                auto text_field = [](const char* id, std::string& v, size_t cap, float w) {
                    char buf[512];
                    std::snprintf(buf, sizeof buf, "%s", v.c_str());
                    ImGui::SetNextItemWidth(w);
                    if (ImGui::InputText(id, buf, std::min(sizeof buf, cap + 1))) v = buf;
                };
                ImGui::TextUnformatted("Lobby server (introductions only; game traffic goes directly between players):");
                text_field("##lobby", set.lobby, 400,
                           -ImGui::CalcTextSize("Default").x - ImGui::GetStyle().FramePadding.x * 4);
                page_entry();
                ImGui::SameLine();
                if (ImGui::Button("Default")) set.lobby = Settings::kDefaultLobby;
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Player name:");
                ImGui::SameLine();
                {
                    char buf[17];
                    std::snprintf(buf, sizeof buf, "%s", set.name.c_str());
                    ImGui::SetNextItemWidth(220 * scale);
                    if (ImGui::InputText("##name", buf, sizeof buf, ImGuiInputTextFlags_CallbackCharFilter, name_filter))
                        set.name = buf;
                    if (!ImGui::IsItemActive() && !valid_name(set.name)) set.name = fallback_name;  // never empty
                }
                ImGui::SameLine();
                ImGui::TextColored(kDim, "3-16 letters, digits, _ or -");
                ImGui::Checkbox("Open the game port on the router (UPnP)", &set.upnp);
                bool testing;
                {
                    std::lock_guard<std::mutex> lk(conn.mu);
                    testing = conn.running;
                }
                ImGui::BeginDisabled(testing);
                if (ImGui::Button(testing ? "Testing..." : "Test connection")) conn.start(set);
                ImGui::EndDisabled();
                {
                    std::lock_guard<std::mutex> lk(conn.mu);
                    if (conn.have) {
                        const auto& r = conn.result;
                        ImGui::SameLine();
                        ImGui::TextColored(r.lobby_ok && r.stun_ok && r.nat_same != 0 ? kGood : r.lobby_ok ? kWarn : kBad,
                                           "%s", r.verdict.c_str());
                        ImGui::TextColored(r.lobby_ok ? kGood : kBad, "Lobby:");
                        ImGui::SameLine();
                        ImGui::Text("%s%s%s (%d ms)", r.lobby_msg.c_str(), r.your_ip.empty() ? "" : ", sees you as ",
                                    r.your_ip.c_str(), r.lobby_ms);
                        ImGui::TextColored(r.stun_ok ? kGood : kBad, "Public UDP endpoint:");
                        ImGui::SameLine();
                        if (r.stun_ok)
                            ImGui::Text("%s:%u %s, NAT: %s", r.public_ip.c_str(), r.public_port, r.stun_msg.c_str(),
                                        r.nat_same == 1 ? "endpoint-independent (good)"
                                        : r.nat_same == 0 ? "symmetric (direct connections likely fail)"
                                                          : "not checked");
                        else ImGui::TextUnformatted(r.stun_msg.c_str());
                        if (r.upnp_tried) {
                            ImGui::TextColored(r.upnp_ok ? kGood : kWarn, "UPnP:");
                            ImGui::SameLine();
                            ImGui::TextWrapped("%s", r.upnp_msg.c_str());
                        }
                    }
                }
                ImGui::Spacing();
                ImGui::TextColored(kDim, "Go online from the game's own online menu; these settings apply when it "
                                         "connects.");
                draw_friends(friends, set, ident, add_code, confirm_remove, scale);
            } else if (t == 4) {
                const std::string save_dir = set.saves.empty() ? saves : set.saves;
                ImGui::TextUnformatted("Save folder (the game keeps SAVEDATA00 inside it):");
                ImGui::Text("%s%s", save_dir.c_str(), set.saves.empty() ? "  (default)" : "");
                if (type_saves) {  // no folder dialog: type the path, Use applies it
                    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
                    ImGui::TextWrapped("%s", pick_msg.c_str());
                    ImGui::PopStyleColor();
                    char buf[1024];
                    std::snprintf(buf, sizeof buf, "%s", saves_buf.c_str());
                    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Use").x - ImGui::GetStyle().FramePadding.x * 4);
                    if (ImGui::InputTextWithHint("##savespath", "/path/to/save/folder", buf, sizeof buf)) saves_buf = buf;
                    ImGui::SameLine();
                    const std::string p = trim(saves_buf);
                    ImGui::BeginDisabled(p.empty() || p[0] != '/');
                    if (ImGui::Button("Use##savespath")) {
                        SDL_CreateDirectory(p.c_str());
                        set.saves = p, set.saves_chosen = true, type_saves = false, pick_msg.clear();
                        save_settings(settings_path, set);
                    }
                    ImGui::EndDisabled();
                }
                if (ImGui::Button("Change...##saves")) browse(1, save_dir, false);
                page_entry();
                ImGui::SameLine();
                if (!set.saves.empty() && ImGui::Button("Use default##saves")) {
                    set.saves.clear(), set.saves_chosen = true;
                    save_settings(settings_path, set);
                }
                if (!set.saves.empty()) ImGui::SameLine();
                if (ImGui::Button("Open folder")) {
                    SDL_CreateDirectory(save_dir.c_str());
                    open_in_explorer(save_dir);
                }
                if (!set.saves.empty() && !exists(set.saves + "/SAVEDATA00")) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
                    ImGui::TextWrapped("No saves here yet. Existing saves are not moved: copy the SAVEDATA00 "
                                       "folder from the old location to keep your progress.");
                    ImGui::PopStyleColor();
                }
                ImGui::TextColored(kDim, "Saves use the PS4/PS5 format and can be copied to and from the console / shadPS4.");
            } else if (t == 5) {
                bool sending;
                dcp::net::BugReportResult res;
                {
                    std::lock_guard<std::mutex> lk(bug.mu);
                    sending = bug.running, res = bug.result;
                    if (bug.sent_new) bug_text.clear(), bug.sent_new = false;  // clear the note once per sent report
                }
                ImGui::TextWrapped("Describe what went wrong in a few words. The note is sent together with the "
                                   "log of your last game session, so report right after the problem happens.");
                char buf[dcp::net::kBugReportTextMax + 1];
                std::snprintf(buf, sizeof buf, "%s", bug_text.c_str());
                if (ImGui::InputTextMultiline("##bugtext", buf, sizeof buf,
                                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5.5f)))
                    bug_text = buf;
                page_entry();
                ImGui::TextColored(kDim, "%d / %d", int(utf8_chars(bug_text)), int(dcp::net::kBugReportTextMax));
                const bool have_log = exists(log_path);
                ImGui::BeginDisabled(sending || trim(bug_text).empty() || !ident.ok);
                if (ImGui::Button(sending ? "Sending..." : "Send report")) bug.start(set, ident, trim(bug_text), log_path);
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Preview##bug")) {  // exactly what Send uploads
                    bug_preview = "Note:\n" + trim(bug_text) + "\n\nLog (" + log_path + ", last " +
                                  std::to_string(dcp::net::kBugReportLogMax / 1024) + " KB, redacted):\n" +
                                  dcp::net::redact_log(dcp::net::log_tail_text(log_path, dcp::net::kBugReportLogMax), ident);
                    ImGui::OpenPopup("Report preview");
                }
                if (!sending && !res.msg.empty()) {
                    ImGui::SameLine();
                    ImGui::TextColored(res.ok ? kGood : kBad, "%s", res.msg.c_str());
                }
                if (res.left >= 0) ImGui::TextColored(kDim, "Reports left today: %d", res.left);
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextWrapped("Sent: your note, player name and online id, and the last %d KB of the game log%s. "
                                   "IP addresses, other players' ids, your user and PC name are removed first; "
                                   "Preview shows exactly what is sent. Up to 5 reports a day.",
                                   int(dcp::net::kBugReportLogMax / 1024), have_log ? "" : " (no log yet: play once first)");
                ImGui::PopStyleColor();
                ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.85f, io.DisplaySize.y * 0.75f), ImGuiCond_Appearing);
                if (ImGui::BeginPopupModal("Report preview", nullptr)) {
                    ImGui::InputTextMultiline("##preview", bug_preview.data(), bug_preview.size() + 1,
                                              ImVec2(-FLT_MIN, -ImGui::GetFrameHeightWithSpacing() * 1.3f),
                                              ImGuiInputTextFlags_ReadOnly);
                    if (ImGui::Button("Close##preview") || pad_close) pad_close = false, ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                if (have_log && ImGui::Button("Open log##bug")) open_in_explorer(log_path);
                if (!ident.ok) ImGui::TextColored(kBad, "No online identity: %s", ident.error.c_str());
            } else if (t == 6) {
                const dcp::net::update::Result& r = u_res;
                if (!updated_from.empty())
                    ImGui::TextColored(kGood, "Updated to v%s (from v%s).", OPENCROWN_VERSION, updated_from.c_str());
                ImGui::Text("Current version: v%s", OPENCROWN_VERSION);
                ImGui::SameLine(0, 30 * scale);
                if (u_running && !u_downloading) ImGui::TextColored(kDim, "Latest version: checking...");
                else if (!u_checked) ImGui::TextColored(kDim, "Latest version: not checked yet");
                else if (!r.ok) ImGui::TextColored(kDim, "Latest version: unknown");
                else ImGui::Text("Latest version: v%s", r.latest.empty() ? OPENCROWN_VERSION : r.latest.c_str());

                // What can be done, and why not when it cannot.
                const bool game_running = game.proc != 0;
                const std::string dir = appimage.empty() ? "" : parent_dir(appimage);
                const bool writable = !dir.empty() && access(dir.c_str(), W_OK) == 0;
                std::string why_not;  // installable update exists but the automatic install is off, with the reason
                if (u_checked && r.ok && r.have_target) {
                    if (appimage.empty())
                        why_not = "This launcher is not running from the AppImage (a build from source?), so it cannot "
                                  "update itself. Get the new version from the release page.";
                    else if (!writable)
                        why_not = "The folder " + dir + " is not writable, so the AppImage cannot be replaced. Get the "
                                  "new version from the release page, or move the AppImage to a folder you own.";
                    else if (r.asset.sha256.empty())
                        why_not = "The release lists no SHA-256 checksum for " + r.asset.name +
                                  ", so it is not installed automatically. Get it from the release page.";
                }
                ImGui::Spacing();
                if (u_checked && !r.ok && !u_running) ImGui::TextColored(kBad, "Update check failed: %s", r.error.c_str());
                else if (u_checked && r.ok && !u_running) {
                    if (r.have_target)
                        ImGui::TextColored(kGold, "Update available: %s (%s, %.1f MB)", r.target_tag.c_str(),
                                           r.asset.name.c_str(), double(r.asset.size) / 1048576.0);
                    else if (!r.latest.empty())
                        ImGui::TextColored(kDim, "v%s is out, but it has no Linux build yet. You are up to date.",
                                           r.latest.c_str());
                    else ImGui::TextColored(kGood, "You are up to date.");
                }
                if (!why_not.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
                    ImGui::TextWrapped("%s", why_not.c_str());
                    ImGui::PopStyleColor();
                }
                const bool can_install = u_checked && r.ok && r.have_target && why_not.empty();
                if (can_install) {
                    ImGui::BeginDisabled(u_running || game_running || u_installed);
                    if (ImGui::Button(u_downloading ? "Downloading..." : "Download and install")) upd.download(appimage);
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                }
                ImGui::BeginDisabled(u_running);
                if (ImGui::Button(u_running && !u_downloading ? "Checking...##upd" : "Check again##upd")) upd.check();
                page_entry();
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Open release page")) {
                    const std::string url = u_checked ? r.page : dcp::net::update::kReleasesPage;
                    if (url.compare(0, 8, "https://") == 0 || url.compare(0, 7, "http://") == 0) open_in_explorer(url);
                }
                if (can_install && game_running)
                    ImGui::TextColored(kWarn, "Close the game first: the update replaces the launcher and the game.");
                if (u_downloading || (u_got > 0 && !u_dl_error.empty()) || u_installed) {
                    char lbl[96];
                    const double tot = u_total > 0 ? double(u_total) : 0.0;
                    std::snprintf(lbl, sizeof lbl, "%.1f / %.1f MB", double(u_got) / 1048576.0, tot / 1048576.0);
                    ImGui::ProgressBar(tot > 0 ? float(double(u_got) / tot) : 0.0f, ImVec2(-FLT_MIN, 0), lbl);
                }
                if (u_installed && relaunch_error.empty()) ImGui::TextColored(kGood, "Installed. Restarting...");
                if (!u_dl_error.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kBad);
                    ImGui::TextWrapped("Update failed: %s", u_dl_error.c_str());
                    ImGui::PopStyleColor();
                }
                if (!relaunch_error.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kBad);
                    ImGui::TextWrapped("%s", relaunch_error.c_str());
                    ImGui::PopStyleColor();
                }
                if (ImGui::Checkbox("Check for updates when the launcher starts", &set.check_updates))
                    save_settings(settings_path, set);
                if (!appimage.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                    ImGui::TextWrapped("The AppImage keeps its file name (and old version number) after an update, so "
                                       "Steam shortcuts keep working.");
                    ImGui::PopStyleColor();
                }
                ImGui::Spacing();
                ImGui::SeparatorText("What's new");
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0.25f));
                ImGui::BeginChild("##notes", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
                ImGui::PopStyleColor();
                if (u_checked && r.ok && !r.notes.empty()) draw_notes(r.notes, font_nav, scale);
                else ImGui::TextColored(kDim, "%s", u_running && !u_checked ? "Checking..."
                                                    : u_checked && r.ok      ? "No newer release."
                                                    : u_checked              ? "Could not check for updates (see above)."
                                                                             : "Not checked yet: use Check again.");
                ImGui::EndChild();
            }
        }
        ImGui::EndChild();

        // Play bar: status on the left, a large gold Play button on the right.
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.04f, 0.035f, art.bg.id ? 0.78f : 0.6f));
        ImGui::BeginChild("##bar", ImVec2(0, bar_h), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
        ImGui::PopStyleColor();
        {
            bool running = game.proc != 0, can_play = dump_ok(checks) && !running && !u_downloading && !u_installed;
            const ImVec2 play(170 * scale, ImGui::GetFrameHeight() * 1.6f);
            // Controller hints under the status line while the gamepad is in use (text only).
            const bool hints = input == Input::Pad && !pad.pads.empty();
            const float line = ImGui::GetTextLineHeight(), gap = ImGui::GetStyle().ItemSpacing.y;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (play.y - (hints ? 2 * line + gap : line)) * 0.5f);
            if (running) ImGui::TextColored(kGood, "The game is running.");
            else if (!game.error.empty()) ImGui::TextColored(kBad, "%s", game.error.c_str());
            else if (game.failed) {
                ImGui::TextColored(kBad, "The game exited with code %lu.", (unsigned long)game.exit_code);
                ImGui::SameLine();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().FramePadding.y);
                if (ImGui::SmallButton("Show log")) ImGui::OpenPopup("Last run log");
            } else if (!dump_ok(checks)) ImGui::TextColored(kWarn, "Select a complete, supported dump folder on the Game page.");
            else ImGui::TextColored(kDim, "Ready to play.");
            if (hints) {
                std::vector<std::pair<const char*, const char*>> h;
                const ImGuiContext& gh = *GImGui;
                const bool editing = gh.ActiveId && gh.InputTextState.ID == gh.ActiveId;
                if (capture >= 0) h = {{"B", "Cancel (bindings take a keyboard key or mouse button)"}};
                else if (editing) h = {{"A", "Done"}, {"B", "Cancel"}, {"Steam+X", "Keyboard"}};
                else if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
                    h = {{"A", "Select"}, {"B", "Close"}};
                else {
                    h = {{"A", "Select"}, {"B", "Back"}, {"L1/R1", "Page"}};
                    if (can_play) h.push_back({"Start", "Play"});
                }
                for (size_t i = 0; i < h.size(); ++i) {
                    if (i) ImGui::SameLine(0, 18 * scale);
                    ImGui::TextColored(kGold, "%s", h[i].first);
                    ImGui::SameLine(0, 6 * scale);
                    ImGui::TextColored(kDim, "%s", h[i].second);
                }
            }
            ImGui::SameLine(ImGui::GetContentRegionMax().x - play.x);
            ImGui::SetCursorPosY(ImGui::GetStyle().WindowPadding.y);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.58f, 0.25f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.70f, 0.33f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.98f, 0.78f, 0.40f, 1));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.12f, 0.07f, 0.03f, 1));
            if (font_nav) ImGui::PushFont(font_nav);
            ImGui::BeginDisabled(!can_play);
            if (ImGui::Button(running ? "Running..." : "PLAY", play) || (pad_play && can_play)) {  // Start = Play
                save_settings(settings_path, set);
                if (!start_game(game, set, user, log_path)) game.failed = true;
            }
            ImGui::EndDisabled();
            if (font_nav) ImGui::PopFont();
            ImGui::PopStyleColor(4);
        }
        ImGui::EndChild();
        ImGui::EndGroup();

        if (game.show_tail) {
            ImGui::OpenPopup("Last run log");
            game.show_tail = false;
        }
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.85f, io.DisplaySize.y * 0.7f), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Last run log", nullptr)) {
            ImGui::TextColored(kBad, "The game exited with code %lu. Last lines of %s:", (unsigned long)game.exit_code,
                               log_path.c_str());
            ImGui::BeginChild("##tail", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.3f), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_HorizontalScrollbar);
            for (const std::string& l : game.tail) ImGui::TextUnformatted(l.c_str());
            ImGui::EndChild();
            if (ImGui::Button("Open log")) open_in_explorer(log_path);
            ImGui::SameLine();
            if (ImGui::Button("Close") || pad_close) pad_close = false, ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        // First start: ask where the saves go (once; later changes on the Save Folder page). Cancelling the folder
        // dialog leaves it unchosen, so the question comes back next start.
        if (ask_saves) {
            ImGui::OpenPopup("Save folder");
            ask_saves = false;
        }
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.75f, 0), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Save folder", nullptr, ImGuiWindowFlags_NoResize)) {
            ImGui::TextWrapped("Where should your save files be kept? The game creates a SAVEDATA00 folder inside "
                               "the folder you choose. You can change this later on the Save Folder page.");
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped("Default: %s%s", saves.c_str(),
                               exists(saves + "/SAVEDATA00") ? "  (saves found here)" : "");
            ImGui::PopStyleColor();
            ImGui::Spacing();
            if (ImGui::Button("Use the default")) {
                set.saves.clear(), set.saves_chosen = true;
                save_settings(settings_path, set);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Choose a folder...")) {
                browse(1, "", true);
                ImGui::CloseCurrentPopup();
            }
            if (pad_close) pad_close = false, ImGui::CloseCurrentPopup();  // gamepad B: asked again next start
            ImGui::EndPopup();
        }
        {  // built-in folder browser
            std::string picked;
            bool cancelled = false;
            if (browser.draw(io.DisplaySize, pad_close, input == Input::Pad, &picked, &cancelled)) {
                std::lock_guard<std::mutex> lk(g_pick_mu);
                g_picked = picked, g_have_pick = true, g_pick_target = browser.target;
            }
            if (cancelled && browser.from_first_start) ask_saves = true;  // back to the first-start question
        }
        ImGui::End();

        {  // touch: drags go to the active slider / scrollbar instead of scrolling
            ImGuiContext& g = *GImGui;
            touch.item_drag = slider_active || (g.ActiveId && g.ActiveIdWindow &&
                                                (g.ActiveId == ImGui::GetWindowScrollbarID(g.ActiveIdWindow, ImGuiAxis_Y) ||
                                                 g.ActiveId == ImGui::GetWindowScrollbarID(g.ActiveIdWindow, ImGuiAxis_X)));
        }
        if (input_log && (page != script_page || script_log)) {
            ImGuiContext& g = *GImGui;
            std::fprintf(stderr, "launcher: page %s, focus %s/0x%08x%s, active 0x%08x, input %s, popup %d\n", names[page],
                         g.NavWindow ? g.NavWindow->Name : "-", g.NavId, g.NavCursorVisible ? " (shown)" : "", g.ActiveId,
                         input == Input::Pad ? "pad" : input == Input::Touch ? "touch" : "mouse", g.OpenPopupStack.Size);
            script_page = page;
        }
        ImGui::Render();
        {  // Steam's on-screen keyboard when a text field was focused with the gamepad / touch screen (once per focus)
            const ImGuiContext& g = *GImGui;
            const ImGuiID text_id = g.ActiveId && g.InputTextState.ID == g.ActiveId &&
                                            !(g.InputTextState.Flags & ImGuiInputTextFlags_ReadOnly)
                                        ? g.ActiveId : 0;
            if (text_id != text_id_last) {
                text_id_last = text_id;
                if (text_id && input != Input::Mouse) {
                    if (steam) open_steam_keyboard(g.PlatformImeData.InputPos, g.PlatformImeData.InputLineHeight,
                                                   g.InputTextState.Flags & ImGuiInputTextFlags_Multiline);
                    else if (input_log) std::fprintf(stderr, "launcher: text field focused (not under Steam: no keyboard opened)\n");
                }
            }
        }
        int w, h;
        SDL_GetWindowSizeInPixels(win, &w, &h);
        gl.Viewport(0, 0, w, h);
        gl.ClearColor(0.09f, 0.09f, 0.11f, 1);
        gl.Clear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (!shot_path.empty()) save_screenshot(shot_path, w, h);
        SDL_GL_SwapWindow(win);
        first = false;
        if (selftest && ++frames >= 10) quit = true;
    }
    if (selftest) {  // input.ini round trip: defaults -> file -> parse must match, and the file must equal the defaults
        Controls d = default_controls();
        std::string p = user + "/selftest_input.ini";
        bool ok = save_controls(p, d);
        Controls r = load_controls(p);
        for (size_t i = 0; ok && i < d.binds.size(); ++i) ok = d.binds[i].value == r.binds[i].value;
        std::ifstream f(p);
        std::stringstream ss;
        ss << f.rdbuf();
        f.close();
        ok = ok && ss.str() == dcp::kInputDefaults && r.fairy_speed == d.fairy_speed && r.touch_screen == d.touch_screen &&
             r.deck_trackpads == d.deck_trackpads;
        std::remove(p.c_str());
        if (!ok) return 3;
    }
    if (!selftest && !relaunched) save_settings(settings_path, set);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(glctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    // A lobby session still waiting on the network (bad server, slow DNS) must not keep the process alive after the
    // window is gone, and joining it could block for good: leave without running the job destructors.
    auto busy = [](auto& job) {
        std::lock_guard<std::mutex> lk(job.mu);
        return job.running;
    };
    if (busy(conn) || busy(friends) || busy(bug) || busy(upd)) {
        std::fflush(nullptr);
        _exit(0);
    }
    return 0;
}
