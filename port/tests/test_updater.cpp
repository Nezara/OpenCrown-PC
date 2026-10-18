// Launcher updater (tools/launcher/update.h), no network: versions, the releases JSON (drafts, prereleases, missing
// assets / digests), asset choice, release notes, Markdown -> text, the URL allow-list, SHA-256, and the zip install
// (tar.exe extraction, .old renames, rollback, leftover cleanup) in a temp folder.
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "../tools/launcher/update.h"

using namespace dcp::update;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), ++g_fail; \
    } while (0)

static const char* kFixture = R"([
 {"tag_name":"v1.2.0-beta","draft":false,"prerelease":true,"html_url":"https://github.com/x/y/releases/tag/v1.2.0-beta",
  "body":"beta","assets":[{"name":"OpenCrownPC-v1.2.0-win64.zip","size":1,"digest":"sha256:00","browser_download_url":"https://github.com/a"}]},
 {"tag_name":"v1.1.5","draft":true,"prerelease":false,"html_url":"h","body":"draft","assets":[]},
 {"tag_name":"v1.1","draft":false,"prerelease":false,"html_url":"https://github.com/Nezara/OpenCrown-PC/releases/tag/v1.1",
  "body":"## Fixes\r\n- **Crash** on start fixed\r\n- see [the wiki](https://example.com/w) and `dcp_run`\r\n\r\n\r\n  - nested item\r\nPlain line",
  "assets":[{"name":"OpenCrown-Linux-1.1-x86_64.AppImage","size":10,"digest":"sha256:1111111111111111111111111111111111111111111111111111111111111111","browser_download_url":"https://github.com/l"}]},
 {"tag_name":"v1.0.2","draft":false,"prerelease":false,"html_url":"https://github.com/Nezara/OpenCrown-PC/releases/tag/v1.0.2",
  "body":"Small fixes.","assets":[
   {"name":"OpenCrownPC-v1.0.2-win64.zip","size":1234,"digest":"sha256:ABCDEFabcdef0123456789abcdef0123456789abcdef0123456789abcdef0123","browser_download_url":"https://github.com/Nezara/OpenCrown-PC/releases/download/v1.0.2/OpenCrownPC-v1.0.2-win64.zip"},
   {"name":"OpenCrownPC-v1.0.1-win64.zip","size":5,"browser_download_url":"https://github.com/old"}]},
 {"tag_name":"v1.0","draft":false,"prerelease":false,"html_url":"https://github.com/Nezara/OpenCrown-PC/releases/tag/v1.0",
  "body":"First","assets":[{"name":"OpenCrownPC-v1.0-win64.zip","size":5006587,"digest":"sha256:52870f63f2d47fda1a07f39423dcd34bb5957856cd2128add7c084eb798796f4","browser_download_url":"https://github.com/Nezara/OpenCrown-PC/releases/download/v1.0/OpenCrownPC-v1.0-win64.zip"}]},
 {"tag_name":"nightly","draft":false,"prerelease":false,"body":"","assets":[]}
])";

static void test_versions() {
    CHECK(parse_version("v1.0").ok && compare(parse_version("v1.0"), parse_version("1.0.0")) == 0);
    CHECK(compare(parse_version("1.0.1"), parse_version("v1.0")) > 0);
    CHECK(compare(parse_version("1.0.10"), parse_version("1.0.9")) > 0);
    CHECK(compare(parse_version("2"), parse_version("1.99.99")) > 0);
    CHECK(compare(parse_version("V1.2.3.4"), parse_version("1.2.3.5")) < 0);
    for (const char* bad : {"", "v", "1.", ".1", "1..2", "1.0-beta", "1.0.0.0.0", "abc", "1.a"}) CHECK(!parse_version(bad).ok);
    CHECK(to_string(parse_version("v1.0")) == "1.0");
    CHECK(to_string(parse_version("1.0.0")) == "1.0");
    CHECK(to_string(parse_version("1.0.1")) == "1.0.1");
    CHECK(to_string(parse_version("9.9.9")) == "9.9.9");
}

static void test_releases() {
    std::vector<Release> rels;
    std::string err;
    CHECK(parse_releases(kFixture, &rels, &err));
    CHECK(rels.size() == 6);
    CHECK(!parse_releases("{\"message\":\"API rate limit exceeded\"}", &rels, &err) && err.find("rate limit") != std::string::npos);
    CHECK(!parse_releases("<html>", &rels, &err));
    parse_releases(kFixture, &rels, &err);
    CHECK(rels[3].assets[0].sha256 == "abcdefabcdef0123456789abcdef0123456789abcdef0123456789abcdef0123");
    CHECK(rels[3].assets[1].sha256.empty());
    CHECK(rels[0].assets[0].sha256.empty());  // "sha256:00" is not a digest
    // Windows asset: only the one whose version matches the release (the stray v1.0.1 zip in v1.0.2 is skipped).
    const Asset* a = find_asset(rels[3], windows_pattern());
    CHECK(a && a->name == "OpenCrownPC-v1.0.2-win64.zip" && a->size == 1234);
    CHECK(find_asset(rels[2], windows_pattern()) == nullptr);
    CHECK(find_asset(rels[2], linux_pattern()) != nullptr);
    CHECK(find_asset(rels[4], windows_pattern()) != nullptr);  // v1.0 tag, "1.0" in the asset name

    // From 1.0.1: v1.1 (no Windows asset) and v1.0.2 are newer; the target is v1.0.2; the beta and draft are ignored.
    CheckResult r = evaluate(rels, "1.0.1", windows_pattern());
    CHECK(r.ok && r.newer && r.installable);
    CHECK(r.latest == "1.1");
    CHECK(r.target.tag == "v1.0.2" && r.asset.size == 1234);
    CHECK(!r.notes.empty() && r.notes[0].kind == NoteLine::Release && r.notes[0].text == "v1.1");
    bool saw_102 = false, saw_beta = false, saw_draft = false;
    for (const NoteLine& l : r.notes) {
        saw_102 |= l.kind == NoteLine::Release && l.text == "v1.0.2";
        saw_beta |= l.text.find("beta") != std::string::npos;
        saw_draft |= l.text.find("draft") != std::string::npos;
    }
    CHECK(saw_102 && !saw_beta && !saw_draft);
    // Linux from 1.0.1: v1.1 has the AppImage.
    CheckResult rl = evaluate(rels, "1.0.1", linux_pattern());
    CHECK(rl.installable && rl.target.tag == "v1.1");
    // Up to date.
    CheckResult u = evaluate(rels, "1.1", windows_pattern());
    CHECK(u.ok && !u.newer && !u.installable && u.notes.empty() && u.latest == "1.1");
    // Real situation today: only v1.0 published, current 1.0.1.
    std::vector<Release> only10 = {rels[4]};
    CheckResult t = evaluate(only10, "1.0.1", windows_pattern());
    CHECK(t.ok && !t.newer && t.latest == "1.0");
    // From 1.0: 1.0.2 is installable, its notes come after v1.1's.
    CheckResult f = evaluate(rels, "1.0", windows_pattern());
    CHECK(f.installable && f.target.tag == "v1.0.2");
    // Newer release without any asset of ours: newer but not installable.
    std::vector<Release> one = {rels[2]};
    CheckResult n = evaluate(one, "1.0.1", windows_pattern());
    CHECK(n.newer && !n.installable);
}

static void test_markdown() {
    std::vector<NoteLine> l = markdown_lines("## Fixes\r\n- **Crash** on start fixed\r\n- see [the wiki](https://e/w) "
                                             "and `dcp_run`\r\n\r\n\r\n  - nested item\r\nPlain line\n---\n# Big #\n");
    CHECK(l.size() >= 7);
    CHECK(l[0].kind == NoteLine::Heading && l[0].text == "Fixes");
    CHECK(l[1].kind == NoteLine::Bullet && l[1].text == "Crash on start fixed");
    CHECK(l[2].text == "see the wiki and dcp_run");
    CHECK(l[3].kind == NoteLine::Blank);
    CHECK(l[4].kind == NoteLine::Bullet && l[4].indent == 1 && l[4].text == "nested item");
    CHECK(l[5].kind == NoteLine::Text && l[5].text == "Plain line");
    CHECK(l.back().kind == NoteLine::Heading && l.back().text == "Big");
    CHECK(markdown_to_text("- a\n  * b\n**c** <https://x.y>") == "- a\n  - b\nc https://x.y\n");
    CHECK(markdown_to_text("![logo](a.png) __x__") == "logo x\n");
    CHECK(markdown_lines("").empty());
}

static void test_urls() {
    CHECK(url_allowed("https://api.github.com/repos/a/b/releases?per_page=20", false));
    CHECK(url_allowed("https://github.com/Nezara/OpenCrown-PC/releases/download/v1.0/x.zip", false));
    CHECK(url_allowed("https://objects.githubusercontent.com/github-production-release-asset/1?a=b", false));
    CHECK(url_allowed("https://release-assets.githubusercontent.com/x", false));
    CHECK(!url_allowed("http://github.com/x", false));
    CHECK(!url_allowed("https://github.com.evil.com/x", false));
    CHECK(!url_allowed("https://evilgithub.com/x", false));
    CHECK(!url_allowed("https://githubusercontent.com.x/x", false));
    CHECK(!url_allowed("https://user@github.com/x", false));
    CHECK(!url_allowed("https://github.com:8443/x", false));
    CHECK(!url_allowed("ftp://github.com/x", false));
    CHECK(!url_allowed("http://127.0.0.1:8000/x", false));
    CHECK(url_allowed("http://127.0.0.1:8000/x", true));
    CHECK(url_allowed("http://localhost:8000/releases.json", true));
    CHECK(!url_allowed("http://10.0.0.1/x", true));
    CHECK(!url_allowed("http://example.com/x", true));
    Url u = parse_url("http://127.0.0.1:8123/api/releases?x=1");
    CHECK(u.ok && u.host == "127.0.0.1" && u.port == 8123 && u.rest == "/api/releases?x=1");
    Url g = parse_url("https://GitHub.com");
    CHECK(g.ok && g.host == "github.com" && g.port == 443 && g.rest == "/");
}

static void test_sha() {
    CHECK(sha256_hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

// ---- install in a temp folder ----

static std::string g_tmp;

static void put(const std::string& p, const std::string& text) {
    std::ofstream o(p, std::ios::binary);
    o << text;
}
static std::string get(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return f ? ss.str() : "<missing>";
}
static bool has(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static bool make_zip(const std::string& zip, const std::string& src_dir, const std::string& top) {
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    std::string cmd = std::string("\"") + sys + "\\tar.exe\" -a -c -f \"" + zip + "\" -C \"" + src_dir + "\" " + top;
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess), CloseHandle(pi.hThread);
    return code == 0;
}

static void test_install() {
    char t[MAX_PATH];
    GetTempPathA(MAX_PATH, t);
    g_tmp = std::string(t) + "opencrown_updater_test_" + std::to_string(GetCurrentProcessId());
    const std::string src = g_tmp + "\\src", inst = g_tmp + "\\install";
    CreateDirectoryA(g_tmp.c_str(), nullptr);
    CreateDirectoryA(src.c_str(), nullptr);
    CreateDirectoryA((src + "\\OpenCrownPC").c_str(), nullptr);
    CreateDirectoryA((src + "\\OpenCrownPC\\licenses").c_str(), nullptr);
    CreateDirectoryA(inst.c_str(), nullptr);
    for (const char* f : {"OpenCrownPC Launcher.exe", "dcp_run.exe", "SDL3.dll", "README.txt"})
        put(src + "\\OpenCrownPC\\" + f, std::string("new ") + f);
    put(src + "\\OpenCrownPC\\licenses\\SDL3.txt", "new license");
    CHECK(make_zip(g_tmp + "\\good.zip", src, "OpenCrownPC"));
    DeleteFileA((src + "\\OpenCrownPC\\SDL3.dll").c_str());
    CHECK(make_zip(g_tmp + "\\incomplete.zip", src, "OpenCrownPC"));
    put(g_tmp + "\\notazip.zip", "this is not a zip");

    for (const char* f : {"OpenCrownPC Launcher.exe", "dcp_run.exe", "SDL3.dll"}) put(inst + "\\" + f, std::string("old ") + f);
    put(inst + "\\launcher_user_file.txt", "mine");
    std::string err;
    // Incomplete and broken zips change nothing.
    CHECK(!install_zip(g_tmp + "\\incomplete.zip", inst, &err) && err.find("SDL3.dll") != std::string::npos);
    CHECK(!install_zip(g_tmp + "\\notazip.zip", inst, &err));
    CHECK(get(inst + "\\dcp_run.exe") == "old dcp_run.exe" && !has(inst + "\\update_staging"));

    // A folder where the payload has a file: fails midway, earlier files are rolled back.
    CreateDirectoryA((inst + "\\SDL3.dll.dir").c_str(), nullptr);
    MoveFileA((inst + "\\SDL3.dll").c_str(), (inst + "\\SDL3.dll.keep").c_str());
    CreateDirectoryA((inst + "\\SDL3.dll").c_str(), nullptr);
    CHECK(!install_zip(g_tmp + "\\good.zip", inst, &err));
    CHECK(get(inst + "\\dcp_run.exe") == "old dcp_run.exe");
    CHECK(get(inst + "\\OpenCrownPC Launcher.exe") == "old OpenCrownPC Launcher.exe");
    CHECK(!has(inst + "\\dcp_run.exe.old") && !has(inst + "\\README.txt"));
    RemoveDirectoryA((inst + "\\SDL3.dll").c_str());
    RemoveDirectoryA((inst + "\\SDL3.dll.dir").c_str());
    MoveFileA((inst + "\\SDL3.dll.keep").c_str(), (inst + "\\SDL3.dll").c_str());

    // Good zip: replaced, .old kept until cleanup, user file untouched.
    err.clear();
    CHECK(install_zip(g_tmp + "\\good.zip", inst, &err));
    if (!err.empty()) std::printf("install: %s\n", err.c_str());
    CHECK(get(inst + "\\OpenCrownPC Launcher.exe") == "new OpenCrownPC Launcher.exe");
    CHECK(get(inst + "\\dcp_run.exe") == "new dcp_run.exe");
    CHECK(get(inst + "\\SDL3.dll") == "new SDL3.dll");
    CHECK(get(inst + "\\README.txt") == "new README.txt");
    CHECK(get(inst + "\\licenses\\SDL3.txt") == "new license");
    CHECK(get(inst + "\\dcp_run.exe.old") == "old dcp_run.exe");
    CHECK(get(inst + "\\launcher_user_file.txt") == "mine");
    CHECK(!has(inst + "\\update_staging"));
    put(inst + "\\OpenCrownPC-v9.9.9-win64.zip.part", "partial");
    cleanup_leftovers(inst, 0);
    CHECK(!has(inst + "\\dcp_run.exe.old") && !has(inst + "\\OpenCrownPC Launcher.exe.old") && !has(inst + "\\SDL3.dll.old"));
    CHECK(!has(inst + "\\OpenCrownPC-v9.9.9-win64.zip.part") && !has(inst + "\\update_old.lst"));
    CHECK(get(inst + "\\dcp_run.exe") == "new dcp_run.exe" && get(inst + "\\launcher_user_file.txt") == "mine");
    // Second install over the first: licenses file now replaced too (listed in the manifest), then cleaned.
    CHECK(install_zip(g_tmp + "\\good.zip", inst, &err));
    CHECK(has(inst + "\\licenses\\SDL3.txt.old"));
    cleanup_leftovers(inst, 0);
    CHECK(!has(inst + "\\licenses\\SDL3.txt.old"));
    CHECK(dir_writable(inst));
    CHECK(sha256_file(g_tmp + "\\notazip.zip") == sha256_hex("this is not a zip", 17));
    std::string cmd = "rmdir /s /q \"" + g_tmp + "\"";
    std::system(cmd.c_str());
}

int main() {
    test_versions();
    test_releases();
    test_markdown();
    test_urls();
    test_sha();
    test_install();
    std::printf(g_fail ? "test_updater: %d FAILED\n" : "test_updater: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
