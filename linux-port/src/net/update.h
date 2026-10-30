// Launcher self-updater, platform logic without UI (Linux build): GitHub releases API -> newest release that is newer
// than this build and carries a Linux AppImage, its release notes (Markdown -> plain lines for ImGui), the download
// to <AppImage>.part with size + SHA-256 checked against the API, and the atomic rename over the running AppImage.
// The launcher (tools/launcher/main.cpp) runs check() / download() on a worker thread and does the relaunch.
// Testing: $DCP_UPDATE_API replaces the API URL; http:// (and redirects / downloads to it) only for 127.0.0.1 / localhost.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dcp::net::update {

constexpr const char* kApiUrl = "https://api.github.com/repos/Nezara/OpenCrown-PC/releases?per_page=20";
constexpr const char* kReleasesPage = "https://github.com/Nezara/OpenCrown-PC/releases";

// "v1.0.1" / "1.0" -> numeric parts, missing ones 0 (1.0 == 1.0.0). ok = false for anything else ("beta", "1.x").
struct Version {
    int part[4] = {0, 0, 0, 0};
    bool ok = false;
};
Version parse_version(const std::string& s);
int compare(const Version& a, const Version& b);  // <0, 0, >0
std::string str(const Version& v);                 // "1.0.1" (trailing .0 parts beyond the third dropped)

struct Asset {
    std::string name, url;
    std::string sha256;  // lower-case hex from "digest": "sha256:<hex>"; empty when the API gave none
    int64_t size = -1;
};
struct Release {
    std::string tag, name, body, page;  // page = html_url
    bool draft = false, prerelease = false;
    Version ver;
    std::vector<Asset> assets;
};
// The API's array of releases. False + *err on bad JSON / wrong shape.
bool parse_releases(const std::string& json, std::vector<Release>* out, std::string* err);

// OpenCrown-<ver>-x86_64.AppImage, or the pre-1.0.7 OpenCrown-Linux-<ver>-x86_64.AppImage
bool is_linux_asset(const std::string& name);

// Release notes as display lines.
struct NoteLine {
    enum Kind { Tag, Heading, Bullet, Text, Blank } kind = Text;
    int indent = 0;  // bullets: nesting level
    std::string text;
};
std::string md_inline(const std::string& s);  // **bold**, __x__, `code`, [text](url), ![alt](url) -> plain text
std::vector<NoteLine> notes_from_markdown(const std::string& body);
std::string notes_text(const std::vector<NoteLine>& lines);  // plain text (tests, logs)

struct Result {
    bool ok = false;
    std::string error;
    std::string current;      // this build
    std::string latest;       // newest release newer than current (any platform); "" = none
    bool have_target = false; // a newer release with a Linux AppImage exists
    std::string target_tag;
    Asset asset;
    std::vector<NoteLine> notes;  // bodies of every newer release, newest first, each under its tag
    std::string page;             // release page to open (target, else newest newer release, else the list)
};
// Pure: picks the target among rels (drafts / prereleases / unparseable tags ignored).
Result evaluate(const std::vector<Release>& rels, const std::string& current,
                bool (*asset_match)(const std::string&) = is_linux_asset);

// API URL ($DCP_UPDATE_API or kApiUrl) and whether localhost hosts are allowed (only with an override pointing there).
std::string api_url(bool* allow_local);

// GET the API, parse, evaluate. Blocking (run on a worker thread).
Result check(const std::string& current_version, const std::atomic<bool>* cancel = nullptr);

// Streams asset.url to part_path (created 0600, replaced if present), then checks size == asset.size and SHA-256 ==
// asset.sha256 by re-reading the file. On any failure the .part is deleted and *err says why. Refuses an asset
// without a digest. progress(got, total).
bool download(const Asset& asset, const std::string& part_path, const std::string& current_version,
              const std::function<void(uint64_t, int64_t)>& progress, const std::atomic<bool>* cancel,
              std::string* err);

// chmod part to target's mode (at least 0755), then rename() it over target (atomic, same path). *err on failure
// (part is removed then).
bool install_over(const std::string& part_path, const std::string& target, std::string* err);

// Lower-case hex SHA-256 of a file (mbedTLS). False if it cannot be read.
bool sha256_file(const std::string& path, std::string* hex);

}  // namespace dcp::net::update
