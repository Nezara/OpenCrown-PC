// "OpenCrownPC Launcher.exe" (target DragonsCrownPro): end-user launcher (Dear ImGui on SDL3 + OpenGL 3.3, GUI subsystem). Picks and validates the
// user's dump folder, edits input.ini (bindings shared with dcp_run via tools/common/input_defaults.h), sets display
// options and starts dcp_run.exe (next to this exe) without a console, logging to <user dir>/logs/last_run.log.
// Settings: <user dir>/launcher.ini (key = value). --selftest: draw a few frames and exit (0 = ok; writes nothing but
// a temporary input.ini check). --tab <name>: open that page first.
// Online page (net.md 11.3): lobby URL, player name, UPnP, "Test connection" (net/online_check.h); settings reach
// dcp_run as environment. Friends there (lobby_protocol.md 4.2, net/friends.h): own friend code, add by code, requests,
// list. Save Folder page: DCP_SAVE_ROOT. The story / tutorial skip questions are always on (DCP_STORY_SKIP /
// DCP_TUTORIAL_SKIP=0 in the environment turn them off, hle/skip_offers.h).
// First start without a chosen save folder asks for one (default or custom). Report a Bug page: a short note (256
// characters) plus the OS line and the redacted tail of logs/last_run.log (net/bug_report.h: IPs, ids, user and PC names removed;
// "Preview" shows exactly what is sent), through the lobby, which rate-limits it.
// Player name: never the Windows user name - an empty name gets a random "PlayerNNNN" on first start.
// Update (bottom of the sidebar, update.h): checks GitHub releases at start (setting) and on demand on a worker thread,
// shows the notes of every newer release, downloads + verifies (size, SHA-256) + installs the zip, then starts the new
// launcher with --updated <old version> ("Updated to vX" once). DCP_UPDATE_API=<url> points it at a test server.
// Look: sidebar navigation over the game's own store art (sce_sys/pic0.png background, icon0.png logo), read from the
// user's dump at run time like everything else (rule 1: nothing from the game ships with us); plain theme without it.
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>

#include "../common/dump_check.h"
#include "../common/game_version.h"
#include "../common/input_defaults.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "net/bug_report.h"
#include "net/friends.h"
#include "net/identity.h"
#include "net/online_check.h"
#include "update.h"

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

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string native(std::string p) {  // forward slashes -> backslashes (Explorer, CreateProcess)
    std::replace(p.begin(), p.end(), '/', '\\');
    return p;
}

bool exists(const std::string& p) { return GetFileAttributesW(widen(p).c_str()) != INVALID_FILE_ATTRIBUTES; }

void open_in_explorer(const std::string& p) {
    ShellExecuteW(nullptr, L"open", widen(native(p)).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---- launcher.ini ----

struct Settings {
    std::string dump;
    std::string saves;  // save root (holds SAVEDATA00); empty = <user dir>/saves. Passed to dcp_run as DCP_SAVE_ROOT
    bool saves_chosen = false;  // the user picked the save folder (or the default) once; false = ask on start
    bool fullscreen = false;
    bool uhd = false;  // 4K rendering (PS4 Pro mode, dcp_run --4k)
    int width = 1280, height = 720;
    std::string language = "en";  // game language, passed as DCP_LANGUAGE (hle/services.cpp system_language)
    // Online (passed as DCP_LOBBY / DCP_NP_NAME / DCP_UPNP; the game's own online menu uses them)
    static constexpr const char* kDefaultLobby = "wss://opencrown-lobby.networkhandler.workers.dev/";
    std::string lobby = kDefaultLobby;
    std::string name;   // online name shown to others: 3..16 of [A-Za-z0-9_-] (net/identity.h online_name)
    bool upnp = true;
    bool net_trace = false;  // DCP_NET_TRACE=1: online session trace in last_run.log (hle/net_trace.h), diagnostics
    bool check_updates = true;  // ask GitHub for a newer launcher at start
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
        else if (k == "language") s.language = v;
        else if (k == "lobby") s.lobby = v.empty() ? Settings::kDefaultLobby : v;
        else if (k == "name") s.name = v.substr(0, 16);
        else if (k == "upnp") s.upnp = v == "1" || v == "true";
        else if (k == "net_trace") s.net_trace = v == "1" || v == "true";
        else if (k == "check_updates") s.check_updates = v == "1" || v == "true";
    }
}

void save_settings(const std::string& path, const Settings& s) {
    std::ofstream o(path);
    o << "# OpenCrownPC Launcher settings\n"
      << "dump = " << s.dump << "\nsaves = " << s.saves
      << "\nsaves_chosen = " << (s.saves_chosen ? "true" : "false") << "\nuhd = " << (s.uhd ? "true" : "false") << "\nfullscreen = " << (s.fullscreen ? "true" : "false") << "\nwidth = " << s.width
      << "\nheight = " << s.height << "\nlanguage = " << s.language << "\nlobby = " << s.lobby << "\nname = " << s.name
      << "\nupnp = " << (s.upnp ? "true" : "false") << "\nnet_trace = " << (s.net_trace ? "true" : "false")
      << "\ncheck_updates = " << (s.check_updates ? "true" : "false")
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
};

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
    auto path = [&](const std::string& rel) { return std::filesystem::path(widen(dir + "/" + rel)); };
    bool eb = exists(dir + "/eboot.bin");
    if (!eb) {
        out.push_back({false, true, "eboot.bin missing (the decrypted game executable)"});
    } else if (dcp::FileState s = dcp::exe_state(path("eboot.bin")); s != dcp::FileState::Ok) {
        out.push_back({false, true, dcp::exe_problem("eboot.bin", s),
                       s == dcp::FileState::Encrypted ? "[encrypted]" : "[bad file]"});
    } else {
        out.push_back({true, true, "eboot.bin found (decrypted)"});
        out.push_back(version_check(path("eboot.bin")));  // only meaningful on a decrypted eboot
    }
    for (const dcp::DumpCpk& cpk : dcp::kDumpCpks) {
        if (!exists(dir + "/" + cpk.name)) {
            out.push_back({false, true, std::string(cpk.name) + " missing (game data)"});
            continue;
        }
        std::error_code ec;
        uintmax_t have = std::filesystem::file_size(path(cpk.name), ec);
        dcp::FileState s = dcp::cpk_state(path(cpk.name), cpk.size);
        if (s == dcp::FileState::Ok) out.push_back({true, true, std::string(cpk.name) + " found"});
        else out.push_back({false, true, dcp::cpk_problem(cpk.name, s, have, cpk.size),
                            s == dcp::FileState::Truncated ? "[incomplete]" : "[bad file]"});
    }
    // Optional, but the game loads it whenever it is there, so an encrypted copy stops the game.
    bool libc = exists(dir + "/sce_module/libc.prx");
    dcp::FileState ls = libc ? dcp::exe_state(path("sce_module/libc.prx")) : dcp::FileState::Ok;
    if (!libc) out.push_back({false, false, "sce_module/libc.prx missing (optional; built-in libc stubs are used)"});
    else if (ls == dcp::FileState::Ok) out.push_back({true, false, "sce_module/libc.prx found"});
    else out.push_back({false, true, dcp::exe_problem("sce_module/libc.prx", ls) + " (or delete it)",
                        ls == dcp::FileState::Encrypted ? "[encrypted]" : "[bad file]"});
    return out;
}

// What stops Play, for the Play bar: the first failing required check.
std::string dump_problem(const std::vector<Check>& v) {
    for (const Check& c : v)
        if (c.required && !c.ok) return c.text;
    return "";
}

bool dump_ok(const std::vector<Check>& v) {
    for (const Check& c : v)
        if (c.required && !c.ok) return false;
    return true;
}

// ---- game process ----

struct Game {
    HANDLE proc = nullptr;
    DWORD exit_code = 0;
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

std::string exe_dir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    std::wstring w(buf, n);
    w = w.substr(0, w.find_last_of(L"\\/"));
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len ? len - 1 : 0, '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
}

void set_env(const wchar_t* k, const std::string& v) {  // "" removes it; inherited by dcp_run
    SetEnvironmentVariableW(k, v.empty() ? nullptr : widen(v).c_str());
}

bool start_game(Game& g, const Settings& s, const std::string& log_path) {
    std::string exe = exe_dir() + "\\dcp_run.exe";
    if (!exists(exe)) { g.error = "dcp_run.exe not found next to the launcher (" + exe + ")"; return false; }
    std::string cmd = "\"" + exe + "\" \"" + native(s.dump) + "\\eboot.bin\" --render --window";
    if (s.fullscreen) cmd += " --fullscreen";
    if (s.uhd) cmd += " --4k";
    cmd += " --size " + std::to_string(s.width) + "x" + std::to_string(s.height);

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE log = CreateFileW(widen(log_path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) { g.error = "cannot create " + log_path; return false; }
    // The game inherits only the log handle (not sockets of a friends / bug-report session running at the time).
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    HANDLE inherit[] = {log};
    bool have_list = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
                     UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit,
                                               nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = log;
    si.lpAttributeList = have_list ? attrs : nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring wcmd = widen(cmd), wdir = widen(exe_dir());
    // Custom save folder: the game's DCP_SAVE_ROOT override, set in our environment (inherited by the child).
    set_env(L"DCP_SAVE_ROOT", s.saves.empty() ? "" : native(s.saves));
    set_env(L"DCP_LANGUAGE", s.language);
    // Same user dir as ours, whatever the save folder is: the game reads input.ini and identity.ini from it (without
    // this it used the save folder's parent, so a custom save folder meant another online account than ours).
    // dcp::user_dir() rule, built from the wide USERPROFILE so non-ASCII profile paths survive.
    if (!GetEnvironmentVariableW(L"DCP_USER_DIR", nullptr, 0)) {
        wchar_t home[MAX_PATH * 2];
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, DWORD(std::size(home)));
        std::wstring u = n && n < std::size(home) ? std::wstring(home, n) : L".";
        SetEnvironmentVariableW(L"DCP_USER_DIR", (u + L"/Documents/Dragons Crown Pro").c_str());
    }
    // Online settings (net.md 11.3). The lobby is only contacted when the game goes online.
    set_env(L"DCP_LOBBY", trim(s.lobby));
    set_env(L"DCP_NP_NAME", s.name);
    set_env(L"DCP_UPNP", s.upnp ? "" : "0");
    if (s.net_trace) set_env(L"DCP_NET_TRACE", "1");  // off leaves an inherited DCP_NET_TRACE alone
    BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | (have_list ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr, wdir.c_str(),
                             &si.StartupInfo, &pi);
    if (have_list) DeleteProcThreadAttributeList(attrs);
    CloseHandle(log);
    if (!ok) { g.error = "CreateProcess failed (error " + std::to_string(GetLastError()) + ")"; return false; }
    CloseHandle(pi.hThread);
    g.proc = pi.hProcess;
    g.failed = false, g.tail.clear(), g.error.clear();
    return true;
}

// Called each frame: notices the game's exit.
void poll_game(Game& g, const std::string& log_path) {
    if (!g.proc || WaitForSingleObject(g.proc, 0) != WAIT_OBJECT_0) return;
    GetExitCodeProcess(g.proc, &g.exit_code);
    CloseHandle(g.proc);
    g.proc = nullptr;
    g.failed = g.exit_code != 0;
    if (g.failed) g.tail = log_tail(log_path, 30), g.show_tail = true;
}

// ---- folder dialog (SDL calls back on another thread on some platforms) ----

std::mutex g_pick_mu;
std::string g_picked;
bool g_have_pick = false;
intptr_t g_pick_target = 0;  // 0 = dump folder, 1 = saves folder

void SDLCALL on_folder(void* target, const char* const* list, int) {
    if (!list || !list[0]) return;
    std::lock_guard<std::mutex> lk(g_pick_mu);
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
    c[ImGuiCol_PlotHistogram] = kGoldDim;  // progress bar (update download)
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.2f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.45f, 0.32f, 0.16f, 0.7f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.70f, 0.50f, 0.22f, 0.45f);
    c[ImGuiCol_NavCursor] = kGold;
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.22f, 0.15f, 0.08f, 1);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.42f, 0.29f, 0.13f, 1);
    st.ScaleAllSizes(scale);
}

// ---- artwork from the user's dump: the PS4 store images in sce_sys (decoded with Windows' own WIC) ----

struct Tex {
    GLuint id = 0;
    int w = 0, h = 0;
};

// PNG (or anything WIC reads) -> RGBA8 texture; {} on failure. win: also make it the window / taskbar icon.
Tex load_texture(const std::string& path, SDL_Window* win = nullptr) {
    Tex t;
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICBitmapSource* rgba = nullptr;
    std::vector<uint8_t> px;
    UINT w = 0, h = 0;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                         reinterpret_cast<void**>(&f))) &&
              SUCCEEDED(f->CreateDecoderFromFilename(widen(native(path)).c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, fr, &rgba)) &&
              SUCCEEDED(rgba->GetSize(&w, &h)) && w && h && w <= 8192 && h <= 8192;
    if (ok) {
        px.resize(size_t(w) * h * 4);
        ok = SUCCEEDED(rgba->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data()));
    }
    for (IUnknown* u : {static_cast<IUnknown*>(rgba), static_cast<IUnknown*>(fr), static_cast<IUnknown*>(dec),
                        static_cast<IUnknown*>(f)})
        if (u) u->Release();
    if (!ok) return t;
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(w), GLsizei(h), 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    t.w = int(w), t.h = int(h);
    if (win)
        if (SDL_Surface* icon = SDL_CreateSurfaceFrom(int(w), int(h), SDL_PIXELFORMAT_ABGR8888, px.data(), int(w * 4))) {
            SDL_SetWindowIcon(win, icon);  // copies the pixels
            SDL_DestroySurface(icon);
        }
    return t;
}

// Background + logo (also the window icon) for the current dump folder; reloaded when the folder changes. The exe
// itself has no icon: it would have to embed the game's art.
struct Art {
    std::string dump;  // folder the textures came from
    Tex bg, logo;
    void update(const std::string& d, SDL_Window* win) {
        if (d == dump) return;
        dump = d;
        for (Tex* t : {&bg, &logo})
            if (t->id) glDeleteTextures(1, &t->id), *t = {};
        if (d.empty()) return;
        bg = load_texture(d + "/sce_sys/pic0.png");
        logo = load_texture(d + "/sce_sys/icon0.png", win);
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
            std::string log = dcp::net::report_log(log_path, id);
            dcp::net::BugReportResult r = dcp::net::send_bug_report(url, name, id, text, log, "launcher " __DATE__);
            std::lock_guard<std::mutex> lk(mu);
            result = r, running = false, sent_new = r.ok;
        });
    }
};

// Self-update (update.h): the GitHub check, then download + verify + install, on one worker thread at a time; the
// leftover cleanup of the previous update on another (bounded: it waits at most a few seconds for a locked .old).
struct UpdateJob {
    std::mutex mu;
    bool checking = false, have = false;  // have = a check finished (res is valid)
    dcp::update::CheckResult res;
    bool busy = false;        // download / install running
    bool installing = false;  // past the download: files are being replaced (never abandoned at exit)
    bool installed = false;   // done: the main loop starts the new launcher
    bool not_writable = false;
    std::string error;        // last download / install problem
    dcp::update::Progress prog;
    std::thread th, cleanup;
    ~UpdateJob() {
        if (th.joinable()) th.join();
        if (cleanup.joinable()) cleanup.join();
    }
    void start_cleanup(const std::string& dir, int wait_ms) {
        cleanup = std::thread([dir, wait_ms] { dcp::update::cleanup_leftovers(dir, wait_ms); });
    }
    void start_check() {
        if (th.joinable()) th.join();
        {
            std::lock_guard<std::mutex> lk(mu);
            checking = true;
        }
        th = std::thread([this] {
            dcp::update::CheckResult r = dcp::update::check(OPENCROWN_VERSION, dcp::update::windows_pattern());
            std::lock_guard<std::mutex> lk(mu);
            res = r, have = true, checking = false;
        });
    }
    // Caller checked: an installable result, the game not running.
    void start_install() {
        if (th.joinable()) th.join();
        if (cleanup.joinable()) cleanup.join();  // it deletes *.part files
        const std::string dir = dcp::update::exe_dir();
        dcp::update::Asset a;
        {
            std::lock_guard<std::mutex> lk(mu);
            a = res.asset, error.clear(), not_writable = false;
            if (a.sha256.empty()) {
                error = "This release has no checksum, so it can't be installed automatically. Use \"Open release "
                        "page\" and unzip it yourself.";
                return;
            }
            if (!dcp::update::dir_writable(dir)) {
                not_writable = true;
                error = "Can't write to the launcher folder (" + dir + "), for example because it is in Program Files. "
                        "Use \"Open release page\", download the zip and unzip it somewhere you can write to.";
                return;
            }
            busy = true, installing = false;
        }
        prog.done = 0, prog.total = a.size, prog.cancel = false;
        th = std::thread([this, a, dir] {
            bool test = false;
            dcp::update::api_url_from_env(&test);
            const std::string part = dir + "\\" + a.name + ".part";
            std::string err;
            bool ok = dcp::update::download_verified(a.url, "OpenCrown-Launcher/" OPENCROWN_VERSION, test, part, a.size,
                                                     a.sha256, &prog, &err);
            if (ok) {
                {
                    std::lock_guard<std::mutex> lk(mu);  // the exit path sets cancel, then reads installing
                    if (prog.cancel) ok = false, err = "cancelled";
                    else installing = true;
                }
                if (ok) ok = dcp::update::install_zip(part, dir, &err);
                DeleteFileW(widen(part).c_str());
            }
            if (!err.empty()) err[0] = char(std::toupper(static_cast<unsigned char>(err[0])));
            std::lock_guard<std::mutex> lk(mu);
            busy = installing = false, installed = ok, error = ok ? "" : err;
        });
    }
};

const ImVec4 kGood(0.55f, 0.85f, 0.45f, 1), kBad(0.97f, 0.45f, 0.38f, 1), kWarn(0.98f, 0.62f, 0.30f, 1),
    kDim(0.66f, 0.62f, 0.55f, 1);

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
            return a.online != b.online ? a.online : _stricmp(a.name.c_str(), b.name.c_str()) < 0;
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

}  // namespace

int main(int argc, char** argv) {
    bool selftest = false;
    std::string first_tab = "Game";  // --tab <name>: open that page first (e.g. --tab Online)
    std::string updated_from;        // --updated <old version>: started by the updater
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--selftest") == 0) selftest = true;
        else if (std::strcmp(argv[i], "--tab") == 0 && i + 1 < argc) first_tab = argv[++i];
        else if (std::strcmp(argv[i], "--updated") == 0 && i + 1 < argc) {
            dcp::update::Version v = dcp::update::parse_version(argv[++i]);
            if (v.ok) updated_from = dcp::update::to_string(v);
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

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        MessageBoxA(nullptr, SDL_GetError(), "OpenCrownPC Launcher", MB_ICONERROR);
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (!(scale > 0.0f)) scale = 1.0f;
    SDL_Window* win = SDL_CreateWindow("OpenCrownPC Launcher", int(1040 * scale), int(660 * scale),
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                           (selftest ? SDL_WINDOW_HIDDEN : 0));
    if (!win) {
        MessageBoxA(nullptr, SDL_GetError(), "OpenCrownPC Launcher", MB_ICONERROR);
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) {
        MessageBoxA(nullptr, SDL_GetError(), "OpenCrownPC Launcher (OpenGL 3.3 needed)", MB_ICONERROR);
        return 1;
    }
    SDL_GL_MakeCurrent(win, gl);
    SDL_GL_SetSwapInterval(1);
    SDL_SetWindowMinimumSize(win, int(820 * scale), int(540 * scale));
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));  // WIC (artwork)

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // our own launcher.ini
    apply_theme(scale);
    // Segoe UI from the user's Windows install reads better than the built-in bitmap font; fall back to that.
    // Headings: Palatino (Georgia, Segoe UI Semibold as fallbacks) suits the game's storybook look.
    const char* kFont = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (!exists(kFont) || !io.Fonts->AddFontFromFileTTF(kFont, 17.0f * scale)) {
        ImFontConfig fc;
        fc.SizePixels = 13.0f * scale;
        io.Fonts->AddFontDefault(&fc);
    }
    ImFont* font_head = nullptr;
    ImFont* font_nav = nullptr;
    for (const char* f : {"C:\\Windows\\Fonts\\palab.ttf", "C:\\Windows\\Fonts\\georgiab.ttf",
                          "C:\\Windows\\Fonts\\seguisb.ttf"})
        if (exists(f)) {
            font_head = io.Fonts->AddFontFromFileTTF(f, 27.0f * scale);
            font_nav = io.Fonts->AddFontFromFileTTF(f, 19.0f * scale);
            break;
        }
    ImGui_ImplSDL3_InitForOpenGL(win, gl);
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
    const std::string updated_msg =
        updated_from.empty() ? "" : "Updated to v" OPENCROWN_VERSION " (from v" + updated_from + ").";
    bool upd_launched = false;  // the new launcher was started (or failed to): handled once
    if (!selftest) {
        upd.start_cleanup(dcp::update::exe_dir(), updated_from.empty() ? 1500 : 10000);
        if (set.check_updates) upd.start_check();
    }
    bool ask_saves = !set.saves_chosen && !selftest;  // first start: where should saves go?
    int capture = -1;      // index into ctl.binds awaiting a key / mouse button
    bool first = true;
    int page = 0;          // index into the page names (sidebar)
    std::string dump_buf = set.dump;
    std::vector<Check> checks = validate_dump(set.dump);
    int frames = 0;
    bool quit = false;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
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
        }
        poll_game(game, log_path);
        {  // update installed: start the new launcher and leave
            bool done;
            {
                std::lock_guard<std::mutex> lk(upd.mu);
                done = upd.installed && !upd_launched;
            }
            if (done) {
                upd_launched = true;
                save_settings(settings_path, set);
                std::string err;
                if (dcp::update::relaunch(dcp::update::exe_dir(), OPENCROWN_VERSION, &err)) quit = true;
                else {
                    std::lock_guard<std::mutex> lk(upd.mu);
                    upd.error = err;
                }
            }
        }
        // Snapshot of the update state for this frame (sidebar, Update page, Play bar).
        bool u_checking, u_have, u_busy, u_installing, u_not_writable;
        dcp::update::CheckResult u_res;
        std::string u_error;
        {
            std::lock_guard<std::mutex> lk(upd.mu);
            u_checking = upd.checking, u_have = upd.have, u_busy = upd.busy, u_installing = upd.installing;
            u_not_writable = upd.not_writable, u_error = upd.error;
            if (u_have) u_res = upd.res;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        art.update(dump_ok(checks) ? set.dump : "", win);
        draw_background(art, io.DisplaySize);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                            ImGuiWindowFlags_NoBackground);
        const char* names[] = {"Game", "Controls", "Display", "Online", "Save Folder", "Report a Bug", "Update"};
        const int order[] = {0, 4, 1, 2, 3, 5};  // sidebar order; indexes names (Update sits at the bottom)
        if (first)
            for (int t = 0; t < 7; ++t)
                if (_stricmp(names[t], first_tab.c_str()) == 0) page = t;

        // Sidebar: logo, navigation, readiness.
        const float side_w = 230 * scale;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.03f, 0.02f, 0.02f, art.bg.id ? 0.62f : 0.35f));
        ImGui::BeginChild("##side", ImVec2(side_w, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleColor();
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
                ImGui::PopStyleColor();
                if (sel)  // gold marker at the left edge of the selected entry
                    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 4 * scale),
                                                              ImVec2(p.x + 3 * scale, p.y + ImGui::GetItemRectSize().y - 4 * scale),
                                                              IM_COL32(235, 184, 97, 255), 2 * scale);
            }
            // Update entry, its state and the version, pinned to the bottom-left of the sidebar.
            const float item_h = ImGui::GetTextLineHeight() + 12 * scale;  // nav font
            if (font_nav) ImGui::PopFont();
            const ImGuiStyle& st = ImGui::GetStyle();
            const float block_h = item_h + st.ItemSpacing.y * 2 + ImGui::GetTextLineHeight() * 2;
            const float bottom_y = ImGui::GetWindowHeight() - st.WindowPadding.y - block_h;
            if (bottom_y > ImGui::GetCursorPosY()) ImGui::SetCursorPosY(bottom_y);
            const bool avail = u_have && u_res.ok && u_res.installable;
            {
                if (font_nav) ImGui::PushFont(font_nav);
                const bool sel = page == 6;
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::PushStyleColor(ImGuiCol_Text, sel || avail ? kGold : kParchment);
                if (ImGui::Selectable("   Update##nav", sel, 0, ImVec2(0, item_h))) page = 6;
                ImGui::PopStyleColor();
                const ImVec2 sz = ImGui::GetItemRectSize();
                if (sel)
                    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 4 * scale),
                                                              ImVec2(p.x + 3 * scale, p.y + sz.y - 4 * scale),
                                                              IM_COL32(235, 184, 97, 255), 2 * scale);
                if (avail || u_busy) {  // dot badge after the label
                    const float tx = p.x + ImGui::CalcTextSize("   Update").x + 10 * scale;
                    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(tx, p.y + sz.y * 0.5f), 4.5f * scale,
                                                                IM_COL32(235, 184, 97, 255));
                }
                if (font_nav) ImGui::PopFont();
            }
            if (u_busy) ImGui::TextColored(kGold, "%s", u_installing ? "Installing update..." : "Downloading update...");
            else if (u_checking) ImGui::TextColored(kDim, "Checking for updates...");
            else if (!u_have) ImGui::TextColored(kDim, "Updates not checked");
            else if (!u_res.ok) ImGui::TextColored(kDim, "Update check failed");
            else if (u_res.installable) ImGui::TextColored(kGold, "Update available: v%s", dcp::update::to_string(u_res.target.ver).c_str());
            else if (u_res.newer) ImGui::TextColored(kDim, "v%s is out", u_res.latest.c_str());
            else ImGui::TextColored(kDim, "Up to date (v" OPENCROWN_VERSION ")");
            ImGui::TextColored(kDim, "v" OPENCROWN_VERSION);
        }
        ImGui::EndChild();
        ImGui::SameLine(0, 14 * scale);

        // Content column: the page panel, then the Play bar.
        const float bar_h = ImGui::GetFrameHeight() * 1.6f + ImGui::GetStyle().WindowPadding.y * 2;
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.04f, 0.035f, art.bg.id ? 0.78f : 0.6f));
        ImGui::BeginChild("##page", ImVec2(0, -bar_h - ImGui::GetStyle().ItemSpacing.y),
                          ImGuiChildFlags_AlwaysUseWindowPadding);
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
                ImGui::SameLine();
                if (ImGui::Button("Browse..."))
                    SDL_ShowOpenFolderDialog(on_folder, nullptr, win, set.dump.empty() ? nullptr : set.dump.c_str(),
                                             false);
                ImGui::Spacing();
                for (const Check& c : checks) {
                    ImGui::TextColored(c.ok ? kGood : c.required ? kBad : kWarn, "%s", c.ok ? "[ok]" : c.required ? c.fail : "[optional]");
                    ImGui::SameLine();
                    ImGui::TextWrapped("%s", c.text.c_str());
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
                float table_h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.6f;
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
                if (ImGui::Button("Reset to defaults")) { ctl = default_controls(); status = "Defaults restored (not saved yet)."; }
                ImGui::SameLine();
                if (ImGui::Button("Save")) status = save_controls(input_path, ctl) ? "Saved " + native(input_path)
                                                                                   : "Could not write " + native(input_path);
                ImGui::SameLine();
                if (ImGui::Button("Revert")) { ctl = load_controls(input_path); status = "Reloaded from input.ini."; }
                ImGui::SameLine();
                ImGui::TextColored(kDim, "%s", status.c_str());
            } else if (t == 2) {
                int mode = set.fullscreen ? 1 : 0;
                ImGui::RadioButton("Windowed", &mode, 0);
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
                ImGui::Spacing();
                // The languages the game's own files carry (DRACRO_US.CPK message sheets + localised menus).
                static const char* const kLangIds[] = {"en", "fr", "es", "de", "it"};
                static const char* const kLangNames[] = {"English", "Fran\xc3\xa7" "ais", "Espa\xc3\xb1" "ol", "Deutsch",
                                                         "Italiano"};
                int lang = 0;
                for (int i = 0; i < 5; ++i)
                    if (set.language == kLangIds[i]) lang = i;
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Language:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(160 * scale);
                if (ImGui::Combo("##language", &lang, kLangNames, 5)) set.language = kLangIds[lang];
                ImGui::TextColored(kDim, "Game text and menus; applies the next time the game starts.");
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
                ImGui::Checkbox("Net trace (online diagnostics in the log)", &set.net_trace);
                ImGui::SameLine();
                ImGui::TextColored(kDim, "only when asked for a bug report; bigger log");
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
                ImGui::Text("%s%s", native(save_dir).c_str(), set.saves.empty() ? "  (default)" : "");
                if (ImGui::Button("Change...##saves"))
                    SDL_ShowOpenFolderDialog(on_folder, reinterpret_cast<void*>(intptr_t(1)), win, save_dir.c_str(),
                                             false);
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
                if (!set.saves.empty() && !exists(set.saves + "/SAVEDATA00"))
                    ImGui::TextColored(kWarn, "No saves here yet. Existing saves are not moved: copy the SAVEDATA00 "
                                              "folder from the old location to keep your progress.");
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
                ImGui::TextColored(kDim, "%d / %d", int(utf8_chars(bug_text)), int(dcp::net::kBugReportTextMax));
                const bool have_log = exists(log_path);
                ImGui::BeginDisabled(sending || trim(bug_text).empty() || !ident.ok);
                if (ImGui::Button(sending ? "Sending..." : "Send report")) bug.start(set, ident, trim(bug_text), log_path);
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Preview##bug")) {  // exactly what Send uploads
                    bug_preview = "Note:\n" + trim(bug_text) + "\n\nLog (" + native(log_path) + ", last " +
                                  std::to_string(dcp::net::kBugReportLogMax / 1024) + " KB, redacted):\n" +
                                  dcp::net::report_log(log_path, ident);
                    ImGui::OpenPopup("Report preview");
                }
                if (!sending && !res.msg.empty()) {
                    ImGui::SameLine();
                    ImGui::TextColored(res.ok ? kGood : kBad, "%s", res.msg.c_str());
                }
                if (res.left >= 0) ImGui::TextColored(kDim, "Reports left today: %d", res.left);
                ImGui::Spacing();
                ImGui::TextColored(kDim, "Sent: your note, player name and online id, your Windows version and the\n"
                                         "last %d KB of the game log%s.\n"
                                         "IP addresses, other players' ids, your Windows user and PC name are removed\n"
                                         "first; Preview shows exactly what is sent. Up to 5 reports a day.",
                                   int(dcp::net::kBugReportLogMax / 1024), have_log ? "" : " (no log yet: play once first)");
                ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.85f, io.DisplaySize.y * 0.75f), ImGuiCond_Appearing);
                if (ImGui::BeginPopupModal("Report preview", nullptr)) {
                    ImGui::InputTextMultiline("##preview", bug_preview.data(), bug_preview.size() + 1,
                                              ImVec2(-FLT_MIN, -ImGui::GetFrameHeightWithSpacing() * 1.3f),
                                              ImGuiInputTextFlags_ReadOnly);
                    if (ImGui::Button("Close##preview")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
                if (have_log && ImGui::Button("Open log##bug")) open_in_explorer(log_path);
                if (!ident.ok) ImGui::TextColored(kBad, "No online identity: %s", ident.error.c_str());
            } else if (t == 6) {
                using dcp::update::NoteLine;
                const bool ok = u_have && u_res.ok;
                if (!updated_msg.empty()) ImGui::TextColored(kGood, "%s", updated_msg.c_str());
                ImGui::Text("Installed version:  v" OPENCROWN_VERSION);
                ImGui::Text("Latest version:");
                ImGui::SameLine();
                if (u_checking) ImGui::TextColored(kDim, "checking...");
                else if (ok && !u_res.latest.empty()) ImGui::Text("v%s", u_res.latest.c_str());
                else ImGui::TextColored(kDim, "unknown");
                if (u_checking) ImGui::TextColored(kDim, "Checking GitHub for updates...");
                else if (!u_have) ImGui::TextColored(kDim, "Not checked yet.");
                else if (!u_res.ok) ImGui::TextColored(kBad, "Update check failed: %s", u_res.error.c_str());
                else if (u_res.installable)
                    ImGui::TextColored(kGold, "Update available: v%s", dcp::update::to_string(u_res.target.ver).c_str());
                else if (u_res.newer)
                    ImGui::TextColored(kWarn, "v%s is out, but has no Windows download yet.", u_res.latest.c_str());
                else ImGui::TextColored(kGood, "You have the latest version.");
                ImGui::Spacing();
                const bool running = game.proc != nullptr;
                ImGui::BeginDisabled(!ok || !u_res.installable || u_busy || u_checking || running);
                if (ImGui::Button("Download and install")) upd.start_install();
                ImGui::EndDisabled();
                if (running && ok && u_res.installable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Close the game first.");
                ImGui::SameLine();
                ImGui::BeginDisabled(u_busy || u_checking);
                if (ImGui::Button("Check again")) upd.start_check();
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Open release page")) {
                    std::string url = ok && u_res.installable ? u_res.target.html_url : ok ? u_res.latest_url : "";
                    if (!dcp::update::url_allowed(url, false)) url = dcp::update::kReleasesPage;
                    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                if (running && ok && u_res.installable) ImGui::TextColored(kWarn, "Close the game to install the update.");
                if (u_busy) {
                    const double mb = 1024.0 * 1024.0, done = double(upd.prog.done.load()), total = double(upd.prog.total.load());
                    char lbl[64];
                    if (u_installing) std::snprintf(lbl, sizeof lbl, "Installing...");
                    else std::snprintf(lbl, sizeof lbl, "%.1f MB / %.1f MB", done / mb, total / mb);
                    ImGui::ProgressBar(u_installing ? 1.0f : total > 0 ? float(done / total) : 0.0f, ImVec2(-FLT_MIN, 0), lbl);
                }
                if (!u_error.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kBad);
                    ImGui::TextWrapped("%s", u_error.c_str());
                    ImGui::PopStyleColor();
                }
                if (u_not_writable) ImGui::TextColored(kDim, "Your settings and saves are not affected.");
                if (ImGui::Checkbox("Check for updates when the launcher starts", &set.check_updates))
                    save_settings(settings_path, set);
                ImGui::Spacing();
                ImGui::SeparatorText("What's new");
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0.25f));
                ImGui::BeginChild("##notes", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
                ImGui::PopStyleColor();
                if (!ok || u_res.notes.empty())
                    ImGui::TextColored(kDim, "%s", ok ? "No newer releases." : "Release notes appear here after a check.");
                for (const NoteLine& l : ok ? u_res.notes : std::vector<NoteLine>{}) {
                    const float ind = float(l.indent) * 16 * scale;
                    switch (l.kind) {
                    case NoteLine::Release:
                        if (font_nav) ImGui::PushFont(font_nav);
                        ImGui::TextColored(kGold, "%s", l.text.c_str());
                        if (font_nav) ImGui::PopFont();
                        break;
                    case NoteLine::Heading: ImGui::TextColored(kGoldDim, "%s", l.text.c_str()); break;
                    case NoteLine::Blank: ImGui::Spacing(); break;
                    case NoteLine::Bullet:
                    case NoteLine::Text:
                        if (ind > 0) ImGui::Indent(ind);
                        if (l.kind == NoteLine::Bullet) ImGui::Bullet();
                        ImGui::TextWrapped("%s", l.text.c_str());
                        if (ind > 0) ImGui::Unindent(ind);
                        break;
                    }
                }
                ImGui::EndChild();
            }
        }
        ImGui::EndChild();

        // Play bar: status on the left, a large gold Play button on the right.
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.04f, 0.035f, art.bg.id ? 0.78f : 0.6f));
        ImGui::BeginChild("##bar", ImVec2(0, bar_h), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleColor();
        {
            bool running = game.proc != nullptr, can_play = dump_ok(checks) && !running && !u_busy;
            const ImVec2 play(170 * scale, ImGui::GetFrameHeight() * 1.6f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (play.y - ImGui::GetTextLineHeight()) * 0.5f);
            if (running) ImGui::TextColored(kGood, "The game is running.");
            else if (!game.error.empty()) ImGui::TextColored(kBad, "%s", game.error.c_str());
            else if (game.failed) {
                ImGui::TextColored(kBad, "The game exited with code %lu.", (unsigned long)game.exit_code);
                ImGui::SameLine();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().FramePadding.y);
                if (ImGui::SmallButton("Show log")) ImGui::OpenPopup("Last run log");
            } else if (u_busy) ImGui::TextColored(kGold, "Updating the launcher...");
            else if (!dump_ok(checks)) {
                // The reason, not just "incomplete": the short form (up to the first ':') here, all of it on hover.
                std::string why = dump_problem(checks);
                ImGui::TextColored(kBad, "Can't play: %s", why.substr(0, why.find(':')).c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", why.c_str());
                if (ImGui::IsItemClicked()) page = 0;
            } else if (!updated_msg.empty()) ImGui::TextColored(kGood, "%s", updated_msg.c_str());
            else ImGui::TextColored(kDim, "Ready to play.");
            ImGui::SameLine(ImGui::GetContentRegionMax().x - play.x);
            ImGui::SetCursorPosY(ImGui::GetStyle().WindowPadding.y);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.58f, 0.25f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.70f, 0.33f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.98f, 0.78f, 0.40f, 1));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.12f, 0.07f, 0.03f, 1));
            if (font_nav) ImGui::PushFont(font_nav);
            ImGui::BeginDisabled(!can_play);
            if (ImGui::Button(running ? "Running..." : "PLAY", play)) {
                save_settings(settings_path, set);
                if (!start_game(game, set, log_path)) game.failed = true;
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
                               native(log_path).c_str());
            ImGui::BeginChild("##tail", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.3f), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_HorizontalScrollbar);
            for (const std::string& l : game.tail) ImGui::TextUnformatted(l.c_str());
            ImGui::EndChild();
            if (ImGui::Button("Open log")) open_in_explorer(log_path);
            ImGui::SameLine();
            if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
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
            ImGui::TextWrapped("Default: %s%s", native(saves).c_str(),
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
                SDL_ShowOpenFolderDialog(on_folder, reinterpret_cast<void*>(intptr_t(1)), win, nullptr, false);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::End();

        ImGui::Render();
        int w, h;
        SDL_GetWindowSizeInPixels(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.09f, 0.09f, 0.11f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
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
        ok = ok && ss.str() == dcp::kInputDefaults && r.fairy_speed == d.fairy_speed;
        DeleteFileW(widen(p).c_str());
        if (!ok) return 3;
    }
    if (!selftest) save_settings(settings_path, set);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (com) CoUninitialize();
    // A lobby session still waiting on the network (bad server, slow DNS) must not keep the process alive after the
    // window is gone, and joining it could block for good: leave without running the job destructors.
    auto busy = [](auto& job) {
        std::lock_guard<std::mutex> lk(job.mu);
        return job.running;
    };
    // An update download is abandoned (its .part is removed on the next start); replacing files is always finished.
    upd.prog.cancel = true;
    bool upd_busy;
    {
        std::lock_guard<std::mutex> lk(upd.mu);
        upd_busy = upd.checking || (upd.busy && !upd.installing);
    }
    if (!upd_busy && upd.th.joinable()) upd.th.join();
    if (upd.cleanup.joinable()) upd.cleanup.join();
    if (busy(conn) || busy(friends) || busy(bug) || upd_busy) {
        std::fflush(nullptr);
        _exit(0);
    }
    return 0;
}
