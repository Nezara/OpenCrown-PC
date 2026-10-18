// Launcher self-updater (GitHub releases of Nezara/OpenCrown-PC). update.cpp: platform-neutral logic (versions, the
// releases JSON, asset choice, release notes as text, the download host allow-list), unit-tested without network
// (tests/test_updater.cpp). update_win.cpp: WinHTTP GET / download, BCrypt SHA-256, zip install with rollback.
//
// Install (Windows): the asset is streamed to <install>/<asset>.part (size + SHA-256 checked against the API), extracted
// with %SystemRoot%\System32\tar.exe into <install>/update_staging, the payload is the folder holding
// "OpenCrownPC Launcher.exe" (needs dcp_run.exe + SDL3.dll beside it). Each file it replaces is renamed to <name>.old
// first (the running exe may be renamed), then the payload is copied in; any failure restores the .old files. The new
// launcher is started with --updated <old version>; on every start leftovers (.old files listed in update_old.lst,
// update_staging, *.part) are removed.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dcp::update {

constexpr const char* kRepo = "Nezara/OpenCrown-PC";
constexpr const char* kApiUrl = "https://api.github.com/repos/Nezara/OpenCrown-PC/releases?per_page=20";
constexpr const char* kReleasesPage = "https://github.com/Nezara/OpenCrown-PC/releases";
constexpr const char* kLauncherExe = "OpenCrownPC Launcher.exe";

// "v1.0.1" / "1.0" -> numeric parts, missing parts 0 (1.0 == 1.0.0). Digits and dots only (after an optional v).
struct Version {
    int part[4] = {0, 0, 0, 0};
    bool ok = false;
};
Version parse_version(const std::string& s);
int compare(const Version& a, const Version& b);  // <0, 0, >0
std::string to_string(const Version& v);         // "1.0.1" (trailing .0 parts after the second dropped)

struct Asset {
    std::string name, url;
    int64_t size = 0;
    std::string sha256;  // lower-case hex from "digest": "sha256:..."; "" = the API gave none
};

struct Release {
    std::string tag, body, html_url;
    Version ver;  // from the tag; !ok = not a version tag (ignored)
    bool draft = false, prerelease = false;
    std::vector<Asset> assets;
};

// Platform asset name = prefix + <version> + suffix, e.g. "OpenCrownPC-v" "1.0.1" "-win64.zip".
struct AssetPattern {
    std::string prefix, suffix;
};
inline AssetPattern windows_pattern() { return {"OpenCrownPC-v", "-win64.zip"}; }
inline AssetPattern linux_pattern() { return {"OpenCrown-Linux-", "-x86_64.AppImage"}; }

// The API's array of releases. false (with err) on bad JSON / not an array.
bool parse_releases(const std::string& json, std::vector<Release>* out, std::string* err);
// This platform's asset of a release (name matches the pattern and its version equals the release's), or nullptr.
const Asset* find_asset(const Release& r, const AssetPattern& p);

// One line of release notes for display.
struct NoteLine {
    enum Kind { Release, Heading, Bullet, Text, Blank } kind = Text;
    std::string text;
    int indent = 0;  // bullet nesting level
};
// Markdown -> readable lines: headings, "- " / "* " bullets (kept as bullets), paragraphs; ** __ ` removed,
// [text](url) -> text, <url> -> url, \r dropped, runs of blank lines collapsed.
std::vector<NoteLine> markdown_lines(const std::string& md);
std::string markdown_to_text(const std::string& md);  // same, joined ("- " bullets, headings as plain lines)

struct CheckResult {
    bool ok = false;           // the API answered and parsed
    std::string error;         // !ok: why
    std::string latest;        // newest non-draft, non-prerelease version overall ("" = none)
    std::string latest_url;    // its release page
    bool newer = false;        // some release is newer than the current version
    bool installable = false;  // a newer release has this platform's asset (target below)
    Release target;            // highest newer release with the asset
    Asset asset;               // its asset
    std::vector<NoteLine> notes;  // every newer release, newest first, each under a Release line with its tag
};
CheckResult evaluate(const std::vector<Release>& rels, const std::string& current, const AssetPattern& p);

// URL allow-list. API and download hops: https to github.com / *.github.com / *.githubusercontent.com; with the
// test override (DCP_UPDATE_API set) also http(s) to 127.0.0.1 / localhost.
struct Url {
    std::string scheme, host, rest;  // rest = path + query, starts with '/'
    int port = 0;
    bool ok = false;
};
Url parse_url(const std::string& u);
bool url_allowed(const std::string& u, bool test_override);

// ---- platform side (update_win.cpp) ----

struct HttpResult {
    int status = 0;  // final HTTP status; 0 = no answer
    std::string body, error, final_url;
};
// GET with redirects followed by hand (each hop must pass url_allowed). body capped at max_body.
HttpResult http_get(const std::string& url, const std::string& user_agent, const std::string& accept,
                    bool test_override, size_t max_body = 4u << 20);

// The whole check: API GET + evaluate. api_url "" = kApiUrl (or %DCP_UPDATE_API%, see api_url_from_env).
std::string api_url_from_env(bool* test_override);
CheckResult check(const std::string& current_version, const AssetPattern& p);

struct Progress {
    std::atomic<int64_t> done{0}, total{0};
    std::atomic<bool> cancel{false};
};
// Streams url to path (created / truncated), then checks size == want_size and SHA-256 == want_sha (hex). Any failure
// deletes the file. true = verified.
bool download_verified(const std::string& url, const std::string& user_agent, bool test_override,
                       const std::string& path, int64_t want_size, const std::string& want_sha, Progress* prog,
                       std::string* err);
std::string sha256_file(const std::string& path);  // lower-case hex, "" on error
std::string sha256_hex(const void* p, size_t n);

std::string exe_dir();                       // folder of this exe (UTF-8, backslashes)
bool dir_writable(const std::string& dir);   // can create (and delete) a file there
// Extract + install the verified zip into dir (see top). true = files replaced (old ones kept as .old until restart).
bool install_zip(const std::string& zip, const std::string& dir, std::string* err);
// Removes update leftovers in dir; retries a locked .old (the previous launcher still exiting) for up to wait_ms.
void cleanup_leftovers(const std::string& dir, int wait_ms);
// Starts dir/OpenCrownPC Launcher.exe --updated <old_version> (same working dir, same environment).
bool relaunch(const std::string& dir, const std::string& old_version, std::string* err);

}  // namespace dcp::update
