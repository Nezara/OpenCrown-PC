// Launcher bug report session (bug_report.h).
#include "net/bug_report.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pwd.h>
#include <sys/utsname.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "net/friends.h"
#include "net/json.h"
#include "net/lobby_client.h"
#include "net/ws.h"

namespace dcp::net {

namespace {
// Log bytes as sent: tab / CR / LF and printable ASCII stay, everything else becomes '?' (keeps the JSON plain ASCII).
char safe_char(char ch) {
    return (uint8_t(ch) < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') || uint8_t(ch) >= 0x7f ? '?' : ch;
}
}  // namespace

std::string log_tail_text(const std::string& path, size_t max_bytes) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return "";
    std::streamoff size = f.tellg();
    std::streamoff start = size > std::streamoff(max_bytes) ? size - std::streamoff(max_bytes) : 0;
    f.seekg(start);
    std::string s(size_t(size - start), '\0');
    f.read(s.data(), std::streamsize(s.size()));
    s.resize(size_t(f.gcount()));
    if (start > 0) s.erase(0, s.find('\n') + 1);  // start on a whole line
    for (char& ch : s) ch = safe_char(ch);
    return s;
}

namespace {

bool is_hex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_word(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Replaces every occurrence of `what` with `with`. Paths (whole_word false) match case-insensitively with '/' and
// '\\' equal; names (whole_word true) match exactly as whole words, so a user called "User" spares the word "user".
void replace_all(std::string& s, const std::string& what, const std::string& with, bool whole_word) {
    if (what.size() < 3) return;  // too short to be specific
    auto eq = [whole_word](char a, char b) {
        if (whole_word) return a == b;
        if (a == '/') a = '\\';
        if (b == '/') b = '\\';
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        bool hit = i + what.size() <= s.size() && std::equal(what.begin(), what.end(), s.begin() + i, eq);
        if (hit && whole_word)
            hit = (i == 0 || !is_word(s[i - 1])) && (i + what.size() == s.size() || !is_word(s[i + what.size()]));
        if (hit) out += with, i += what.size();
        else out += s[i++];
    }
    s.swap(out);
}

// Values as the log would show them (each through safe_char), duplicates and empties dropped.
std::vector<std::string> forms(std::initializer_list<const char*> values) {
    std::vector<std::string> v;
    for (const char* a : values) {
        if (!a || !*a) continue;
        std::string s = a;
        for (char& ch : s) ch = safe_char(ch);
        if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
    }
    return v;
}

#ifdef _WIN32
// An environment variable as UTF-8 (the log may carry either the ANSI bytes getenv() sees or these).
std::string env_utf8(const char* name) {
    wchar_t w[1024];
    std::wstring wn(name, name + std::strlen(name));
    DWORD n = GetEnvironmentVariableW(wn.c_str(), w, DWORD(std::size(w)));
    if (!n || n >= std::size(w)) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w, int(n), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, int(n), s.data(), len, nullptr, nullptr);
    return s;
}
#endif

}  // namespace

std::string redact_log(std::string s, const Identity& id) {
    if (!id.secret.empty()) replace_all(s, id.secret, "<secret>", false);
#if defined(_WIN32)
    const std::string prof = env_utf8("USERPROFILE"), user = env_utf8("USERNAME"), pc = env_utf8("COMPUTERNAME");
    for (const std::string& p : forms({std::getenv("USERPROFILE"), prof.c_str()})) replace_all(s, p, "%USERPROFILE%", false);
    for (const std::string& u : forms({std::getenv("USERNAME"), user.c_str()})) replace_all(s, u, "<user>", true);
    for (const std::string& c : forms({std::getenv("COMPUTERNAME"), pc.c_str()})) replace_all(s, c, "<pc>", true);
#elif defined(__ANDROID__)
    // The app's passwd entry: uid name (u0_a123) only; its home ("/") and the host name ("localhost") are no one's.
    const passwd* pw = getpwuid(getuid());
    for (const std::string& u : forms({pw ? pw->pw_name : nullptr})) replace_all(s, u, "<user>", true);
#else
    const passwd* pw = getpwuid(getuid());
    char host[256] = {};
    gethostname(host, sizeof host - 1);
    for (const std::string& p : forms({std::getenv("HOME"), pw ? pw->pw_dir : nullptr})) replace_all(s, p, "$HOME", false);
    for (const std::string& u : forms({std::getenv("USER"), pw ? pw->pw_name : nullptr})) replace_all(s, u, "<user>", true);
    for (const std::string& c : forms({host})) replace_all(s, c, "<pc>", true);
#endif
    const std::string own_id = u64_str(id.account_id);
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const bool start = i == 0 || !is_word(s[i - 1]);
        // Long hex runs (secrets, hashes): 32+ hex digits.
        size_t h = i;
        while (h < s.size() && is_hex(s[h])) ++h;
        if (start && h - i >= 32 && (h == s.size() || !is_word(s[h]))) {
            out += "<hex>", i = h;
            continue;
        }
        // Decimal runs: 15+ digits are account ids (others' are replaced, ours is kept for the report).
        size_t d = i;
        while (d < s.size() && is_digit(s[d])) ++d;
        if (start && d - i >= 15 && (d == s.size() || !is_word(s[d]))) {
            out += s.compare(i, d - i, own_id) == 0 && d - i == own_id.size() ? own_id : "<id>";
            i = d;
            continue;
        }
        // IPv4 a.b.c.d with an optional :port; loopback and 0.0.0.0 are kept (they say nothing about anyone).
        if (start && d > i && d - i <= 3) {
            size_t j = i;
            int parts = 0;
            std::string first;
            while (parts < 4) {
                size_t k = j;
                while (k < s.size() && is_digit(s[k]) && k - j < 3) ++k;
                if (k == j) break;
                if (parts == 0) first = s.substr(j, k - j);
                ++parts, j = k;
                if (parts < 4) {
                    if (j < s.size() && s[j] == '.') ++j;
                    else break;
                }
            }
            if (parts == 4 && (j == s.size() || (!is_digit(s[j]) && s[j] != '.'))) {
                const bool keep = first == "127" || s.compare(i, 7, "0.0.0.0") == 0;
                if (j < s.size() && s[j] == ':' && j + 1 < s.size() && is_digit(s[j + 1]))
                    for (++j; j < s.size() && is_digit(s[j]);) ++j;
                out += keep ? s.substr(i, j - i) : "<ip>";
                i = j;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

bool lobby_url_safe_for_secret(const std::string& url) {
    std::string host, path;
    uint16_t port = 0;
    bool secure = false;
    if (!ws::parse_url(url, &host, &port, &path, &secure)) return false;
    if (secure) return true;
    // Plain ws:// only to this PC or the LAN (the mock lobby, wrangler dev): the secret proves who we are.
    if (host == "localhost" || host.rfind("127.", 0) == 0 || host.rfind("10.", 0) == 0 || host.rfind("192.168.", 0) == 0)
        return true;
    int a = 0, b = 0;
    return std::sscanf(host.c_str(), "%d.%d.", &a, &b) == 2 && a == 172 && b >= 16 && b <= 31;
}

BugReportResult send_bug_report(const std::string& lobby_url, const std::string& name, const Identity& id,
                                const std::string& text, const std::string& log, const std::string& version) {
    BugReportResult res;
    if (!id.ok) {
        res.msg = "no online identity: " + id.error;
        return res;
    }
    if (!lobby_url_safe_for_secret(lobby_url)) {
        res.msg = "refusing to log in over unencrypted ws:// to a public server (use wss://)";
        return res;
    }
    ws::Conn c;
    std::string err;
    if (!c.open(lobby_url, 5000, &err)) {
        res.msg = "cannot connect to the server: " + err;
        return res;
    }
    auto send = [&](const std::string& op, int rid, Json b) {
        b["op"] = op, b["rid"] = int64_t(rid);
        return c.send_all(ws::encode_frame(ws::kText, b.dump(), true));
    };
    Json h;
    h["proto"] = kLobbyProtocol, h["version"] = kGameNetVersion, h["title"] = "CUSA10454";
    h["name"] = online_name(name);
    h["account_id"] = u64_str(id.account_id), h["secret"] = id.secret, h["presence"] = false, h["udp_port"] = 0;
    Json r;
    r["text"] = text, r["log"] = log, r["version"] = version;
    if (!send("hello", 1, h) || !send("bug_report", 2, r)) {
        res.msg = "send to the server failed";
        return res;
    }
    Json hello_r, report_r;
    bool have_hello = false, have_report = false;
    ws::Reader rd;
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!have_report && std::chrono::steady_clock::now() < end) {
        if (!c.readable(50)) continue;
        char buf[16384];
        int n = c.recv(buf, sizeof buf);
        if (n <= 0) break;
        rd.feed(buf, size_t(n));
        ws::Opcode op;
        std::string pl;
        while (rd.next(&op, &pl)) {
            Json j;
            if (op != ws::kText || !Json::parse(pl, &j)) continue;
            int64_t rid = j.get("rid").as_int();
            if (rid == 1) hello_r = j, have_hello = true;
            if (rid == 2) report_r = j, have_report = true;
        }
    }
    c.send_all(ws::encode_frame(ws::kClose, "", true));
    c.close();
    if (!have_hello) {
        res.msg = "no answer from the server within 10 s";
        return res;
    }
    if (int32_t e = int32_t(hello_r.get("err").as_int())) {
        res.msg = "server refused the login: " + friends_error_text(e);
        return res;
    }
    if (!hello_r.get("verified").as_bool()) {
        res.msg = "this server does not take bug reports";
        return res;
    }
    if (!have_report) {
        res.msg = "no answer to the report (the server may be out of date)";
        return res;
    }
    int32_t e = int32_t(report_r.get("err").as_int());
    if (report_r.has("left")) res.left = int(report_r.get("left").as_int());
    if (uint32_t(e) == 0x80550c0a) res.msg = "Daily report limit reached; please try again tomorrow.";
    else if (uint32_t(e) == 0x80550c02) res.msg = "The server refused the report (empty or too long).";
    else if (e) res.msg = "report failed: " + friends_error_text(e);
    else {
        res.ok = true;
        res.msg = "Report #" + std::to_string(report_r.get("id").as_int()) + " sent. Thank you!";
    }
    return res;
}

namespace {
// One short printable-ASCII line.
std::string ascii_line(std::string s) {
    for (char& ch : s)
        if (uint8_t(ch) < 0x20 || uint8_t(ch) >= 0x7f) ch = '?';
    if (s.size() > 100) s.resize(100);
    return s;
}
}  // namespace

#ifdef _WIN32
std::string os_description() {
    // RtlGetVersion: GetVersionEx reports Windows 8 to programs without a compatibility manifest.
    RTL_OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    using RtlGetVersionFn = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    auto get = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    if (!get || get(&v) != 0) return "Windows (version unknown)";
    std::string s = v.dwMajorVersion == 10 ? (v.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10")
                                           : "Windows " + std::to_string(v.dwMajorVersion) + "." +
                                                 std::to_string(v.dwMinorVersion);
    wchar_t dv[32] = {};  // "24H2"
    DWORD n = sizeof dv - sizeof dv[0];
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion",
                     RRF_RT_REG_SZ, nullptr, dv, &n) == ERROR_SUCCESS && dv[0]) {
        s += ' ';
        for (const wchar_t* p = dv; *p; ++p) s += *p < 0x80 ? char(*p) : '?';
    }
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    const char* arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64"
                       : si.wProcessorArchitecture == 12 ? "ARM64"  // PROCESSOR_ARCHITECTURE_ARM64
                       : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? "x86" : "other";
    return ascii_line(s + " (build " + std::to_string(v.dwBuildNumber) + ", " + arch + ")");
}
#elif defined(__ANDROID__)
std::string os_description() {
    char rel[PROP_VALUE_MAX] = {}, sdk[PROP_VALUE_MAX] = {};
    __system_property_get("ro.build.version.release", rel);
    __system_property_get("ro.build.version.sdk", sdk);
    std::string s = std::string("Android ") + (rel[0] ? rel : "?") + " (API " + (sdk[0] ? sdk : "?");
    utsname u{};
    if (uname(&u) == 0) s += std::string(", ") + u.machine;
    return ascii_line(s + ")");
}
#else
std::string os_description() {
    // Distribution from os-release (PRETTY_NAME), kernel release and machine from uname - never uname's nodename
    // (the host name).
    std::string name;
    for (const char* p : {"/etc/os-release", "/usr/lib/os-release"}) {
        std::ifstream f(p);
        for (std::string line; name.empty() && std::getline(f, line);) {
            if (line.rfind("PRETTY_NAME=", 0) != 0) continue;
            name = line.substr(12);
            if (name.size() >= 2 && (name[0] == '"' || name[0] == '\'') && name.back() == name[0])
                name = name.substr(1, name.size() - 2);
        }
        if (!name.empty()) break;
    }
    utsname u{};
    std::string kernel = uname(&u) == 0 ? std::string("Linux ") + u.release + ", " + u.machine : "Linux";
    return ascii_line((name.empty() ? "Linux" : name) + " (" + kernel + ")");
}
#endif

std::string report_log(const std::string& log_path, const Identity& id) {
    std::string tail = log_tail_text(log_path, kBugReportLogMax);
    // The OS line holds version fields only and skips redaction (a kernel "5.15.153.1" would turn into <ip>).
    return "OS: " + os_description() + "\n" + (tail.empty() ? "" : "\n" + redact_log(tail, id));
}

}  // namespace dcp::net
