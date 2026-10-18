// Launcher self-updater tests (no internet): version parse / compare, release selection and notes from a fixture
// shaped like the GitHub releases API (drafts, prereleases, missing assets / digests), Markdown -> text, then the HTTP
// client, download + verify and install against an in-process loopback server (Content-Length, chunked, redirects,
// a redirect off the allow-list, wrong digest, wrong size) in a scratch dir under /tmp.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "net/http.h"
#include "net/update.h"

using namespace dcp::net;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static const char* kFixture = R"JSON([
 {"tag_name":"v1.2.0-rc1","name":"RC","draft":false,"prerelease":true,"html_url":"https://github.com/x/rc",
  "body":"rc notes","assets":[{"name":"OpenCrown-Linux-1.2.0-x86_64.AppImage","size":5,"digest":"sha256:00",
  "browser_download_url":"https://github.com/x/rc.AppImage"}]},
 {"tag_name":"v1.3","name":"draft","draft":true,"prerelease":false,"body":"secret","assets":[]},
 {"tag_name":"v1.1.0","name":"v1.1.0","draft":false,"prerelease":false,"html_url":"https://github.com/x/v1.1.0",
  "body":"## Fixes\r\n- **Crash** on `stage 3` fixed\r\n- See [the issue](https://github.com/x/1)\r\n\r\n  - nested\r\n","assets":[
   {"name":"OpenCrownPC-v1.1.0-win64.zip","size":10,"digest":"sha256:ab","browser_download_url":"https://github.com/x/w.zip"}]},
 {"tag_name":"v1.0.2","name":"Linux fix","draft":false,"prerelease":false,"html_url":"https://github.com/x/v1.0.2",
  "body":null,"assets":[
   {"name":"OpenCrown-Linux-1.0.2-x86_64.AppImage","size":1234,
    "digest":"sha256:ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
    "browser_download_url":"https://github.com/x/l.AppImage"}]},
 {"tag_name":"v1.0","name":"v1.0","draft":false,"prerelease":false,"body":"first","assets":[
   {"name":"OpenCrownPC-v1.0-win64.zip","size":10,"digest":null,"browser_download_url":"https://github.com/x/w0.zip"}]}
])JSON";

static void test_versions() {
    using namespace update;
    Version a = parse_version("v1.0"), b = parse_version("1.0.0"), c = parse_version("1.0.1"), d = parse_version("V10.2");
    CHECK(a.ok && b.ok && c.ok && d.ok);
    CHECK(compare(a, b) == 0);
    CHECK(compare(a, c) < 0 && compare(c, a) > 0);
    CHECK(compare(d, c) > 0);
    CHECK(compare(parse_version("1.0.10"), parse_version("1.0.9")) > 0);
    CHECK(str(a) == "1.0.0" && str(c) == "1.0.1");
    for (const char* bad : {"", "v", "beta", "1.", "1..2", "1.0-rc1", "1.2.3.4.5", "1.x"}) CHECK(!parse_version(bad).ok);
    CHECK(is_linux_asset("OpenCrown-Linux-1.0.1-x86_64.AppImage"));
    CHECK(!is_linux_asset("OpenCrownPC-v1.0-win64.zip") && !is_linux_asset("OpenCrown-Linux--x86_64.AppImage.zsync"));
}

static void test_markdown() {
    using namespace update;
    CHECK(md_inline("**bold** and `code` and [link](https://x) ![img](y.png) __u__") == "bold and code and link img u");
    CHECK(md_inline("a \\* b") == "a * b");
    auto n = notes_from_markdown("# Title #\n\nText line\n- one\n* two\n    - deep\n\n\n---\n1. first\n```\nraw **x**\n```\n");
    std::string t = notes_text(n);
    CHECK(t == "# Title\n\nText line\n- one\n- two\n    - deep\n\n1. first\nraw **x**\n");
    if (g_fail) std::printf("%s", t.c_str());
}

static void test_selection() {
    using namespace update;
    std::vector<Release> rels;
    std::string err;
    CHECK(parse_releases(kFixture, &rels, &err));
    CHECK(rels.size() == 5);
    CHECK(rels[3].assets.size() == 1 && rels[3].assets[0].sha256 ==
          "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
    CHECK(rels[1].assets.empty() && rels[2].assets[0].sha256.empty());  // malformed digest dropped
    CHECK(!parse_releases("{\"message\":\"API rate limit exceeded\"}", &rels, &err) &&
          err.find("rate limit") != std::string::npos);
    CHECK(!parse_releases("not json", &rels, &err));
    CHECK(parse_releases(kFixture, &rels, &err));

    // 1.0.1: v1.1.0 is newest but Windows-only -> target v1.0.2; notes from both, newest first; draft / rc ignored.
    Result r = evaluate(rels, "1.0.1");
    CHECK(r.ok && r.latest == "1.1.0" && r.have_target && r.target_tag == "v1.0.2");
    CHECK(r.asset.name == "OpenCrown-Linux-1.0.2-x86_64.AppImage" && r.asset.size == 1234);
    CHECK(r.page == "https://github.com/x/v1.0.2");
    std::string t = notes_text(r.notes);
    CHECK(t == "== v1.1.0 ==\n# Fixes\n- Crash on stage 3 fixed\n- See the issue\n\n  - nested\n\n"
               "== v1.0.2 - Linux fix ==\n(no release notes)\n");
    if (t.find("rc notes") != std::string::npos || t.find("secret") != std::string::npos) CHECK(false);
    if (g_fail) std::printf("%s", t.c_str());

    // 1.0.2: newer only the Windows release -> no target, but latest + notes.
    r = evaluate(rels, "1.0.2");
    CHECK(r.latest == "1.1.0" && !r.have_target && r.page == "https://github.com/x/v1.1.0");
    // 1.1: nothing newer (the draft v1.3 and the prerelease do not count) -> up to date.
    r = evaluate(rels, "1.1");
    CHECK(r.ok && r.latest.empty() && !r.have_target && r.notes.empty());
    // The real situation at 1.0.1 against only v1.0 (Windows asset): up to date.
    std::vector<Release> only_v10(rels.end() - 1, rels.end());
    r = evaluate(only_v10, "1.0.1");
    CHECK(r.latest.empty() && !r.have_target);
}

static void test_urls() {
    http::Url u;
    CHECK(http::parse_url("https://API.GitHub.com/repos/a?b=1#frag", &u) && u.secure && u.host == "api.github.com" &&
          u.port == 443 && u.path == "/repos/a?b=1");
    CHECK(http::parse_url("http://127.0.0.1:8080", &u) && !u.secure && u.port == 8080 && u.path == "/");
    CHECK(!http::parse_url("ftp://x/", &u) && !http::parse_url("https://user@github.com/", &u) &&
          !http::parse_url("https://github.com:0/", &u) && !http::parse_url("https://gi thub.com/", &u) &&
          !http::parse_url("https://github.com/a b", &u));
    auto allowed = [](const char* s, bool local) {
        http::Url x;
        return http::parse_url(s, &x) && http::update_host_allowed(x, local);
    };
    CHECK(allowed("https://github.com/a", false) && allowed("https://api.github.com/a", false) &&
          allowed("https://objects.githubusercontent.com/a", false) &&
          allowed("https://release-assets.githubusercontent.com/a", false));
    CHECK(!allowed("http://github.com/a", false) && !allowed("https://evilgithub.com/a", false) &&
          !allowed("https://github.com.evil.net/a", false) && !allowed("https://githubusercontent.com.x/a", false) &&
          !allowed("http://127.0.0.1:1/a", false) && !allowed("http://example.com/", true));
    CHECK(allowed("http://127.0.0.1:1/a", true) && allowed("http://localhost:1/a", true));
    CHECK(http::parse_url("https://github.com/x/y/z?q", &u));
    CHECK(http::resolve(u, "https://objects.githubusercontent.com/o") == "https://objects.githubusercontent.com/o");
    CHECK(http::resolve(u, "//a.github.com/p") == "https://a.github.com/p");
    CHECK(http::resolve(u, "/abs") == "https://github.com/abs");
    CHECK(http::resolve(u, "rel") == "https://github.com/x/y/rel");
}

// ---- loopback server ----

struct Server {
    int ls = -1;
    uint16_t port = 0;
    std::string payload;  // served at /file (Content-Length), /chunked (chunked)
    std::atomic<bool> stop{false};
    std::thread th;
    void start() {
        ls = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a);
        listen(ls, 8);
        socklen_t len = sizeof a;
        getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        th = std::thread([this] { run(); });
    }
    void run() {
        for (;;) {
            int c = accept(ls, nullptr, nullptr);
            if (c < 0 || stop) {
                if (c >= 0) close(c);
                return;
            }
            std::string req;
            char b[4096];
            while (req.find("\r\n\r\n") == std::string::npos) {
                ssize_t k = recv(c, b, sizeof b, 0);
                if (k <= 0) break;
                req.append(b, size_t(k));
            }
            std::string path = req.substr(4, req.find(' ', 4) - 4);
            std::string out;
            const std::string base = "http://127.0.0.1:" + std::to_string(port);
            if (req.find("User-Agent: OpenCrown-Launcher/") == std::string::npos) out = "HTTP/1.1 400 Bad\r\nContent-Length: 0\r\n\r\n";
            else if (path == "/file")
                out = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(payload.size()) + "\r\n\r\n" + payload;
            else if (path == "/chunked") {
                out = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
                for (size_t i = 0; i < payload.size(); i += 7000) {
                    std::string part = payload.substr(i, 7000);
                    char h[32];
                    std::snprintf(h, sizeof h, "%zx;ext=1\r\n", part.size());
                    out += h + part + "\r\n";
                }
                out += "0\r\nX-Trailer: 1\r\n\r\n";
            } else if (path == "/close") out = "HTTP/1.1 200 OK\r\n\r\n" + payload;
            else if (path == "/short")
                out = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(payload.size() + 10) + "\r\n\r\n" + payload;
            else if (path == "/redir1") out = "HTTP/1.1 302 Found\r\nLocation: /redir2\r\nContent-Length: 0\r\n\r\n";
            else if (path == "/redir2") out = "HTTP/1.1 301 Moved\r\nlocation: " + base + "/file\r\n\r\n";
            else if (path == "/loop") out = "HTTP/1.1 302 Found\r\nLocation: /loop\r\n\r\n";
            else if (path == "/offsite") out = "HTTP/1.1 302 Found\r\nLocation: http://example.com/x\r\n\r\n";
            else out = "HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\n\r\nnop";
            send(c, out.data(), out.size(), MSG_NOSIGNAL);
            close(c);
        }
    }
    void finish() {
        stop = true;
        shutdown(ls, SHUT_RDWR);
        int c = socket(AF_INET, SOCK_STREAM, 0);  // wake accept()
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        connect(c, reinterpret_cast<sockaddr*>(&a), sizeof a);
        close(c);
        th.join();
        close(ls);
    }
};

static std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void test_http_and_install() {
    Server s;
    for (int i = 0; i < 100000; ++i) s.payload += char('A' + (i * 7919) % 61);
    s.start();
    const std::string base = "http://127.0.0.1:" + std::to_string(s.port);
    auto rq = [&](const std::string& path) {
        http::Request r;
        r.url = base + path;
        r.headers = {"User-Agent: OpenCrown-Launcher/test"};
        r.allowed = [](const http::Url& u) { return http::update_host_allowed(u, true); };
        r.timeout_ms = 3000;
        return r;
    };
    std::string body;
    for (const char* p : {"/file", "/chunked", "/close", "/redir1"}) {
        uint64_t last = 0;
        http::Request r = rq(p);
        r.progress = [&](uint64_t got, int64_t) { last = got; };
        http::Response resp = http::get_string(r, &body);
        CHECK(resp.ok && body == s.payload && last == s.payload.size());
        if (!resp.ok) std::printf("  %s: %s\n", p, resp.error.c_str());
    }
    http::Response resp = http::get_string(rq("/redir1"), &body);
    CHECK(resp.final_url == base + "/file");
    CHECK(!http::get_string(rq("/short"), &body).ok);
    resp = http::get_string(rq("/loop"), &body);
    CHECK(!resp.ok && resp.error == "too many redirects");
    resp = http::get_string(rq("/offsite"), &body);
    CHECK(!resp.ok && resp.error.find("refusing") != std::string::npos);
    resp = http::get_string(rq("/missing"), &body);
    CHECK(!resp.ok && resp.status == 404);
    CHECK(!http::get_string(rq("/file"), &body, 1000).ok);  // over max_bytes
    {
        http::Request r = rq("/file");
        r.allowed = [](const http::Url& u) { return http::update_host_allowed(u, false); };  // no localhost
        CHECK(!http::get_string(r, &body).ok && body.empty());
    }

    // download + verify + install in a scratch dir. allow_local comes from $DCP_UPDATE_API pointing at loopback.
    setenv("DCP_UPDATE_API", (base + "/api").c_str(), 1);
    char tmpl[] = "/tmp/oc_update_test_XXXXXX";
    std::string dir = mkdtemp(tmpl);
    const std::string target = dir + "/OpenCrown-Linux-1.0.1-x86_64.AppImage", part = target + ".part";
    { std::ofstream(target) << "old build"; }
    chmod(target.c_str(), 0700);
    update::Asset a;
    a.name = "OpenCrown-Linux-9.9.9-x86_64.AppImage";
    a.url = base + "/redir1";
    a.size = int64_t(s.payload.size());
    CHECK(update::sha256_file(target, &a.sha256) && a.sha256.size() == 64);
    {
        std::ofstream(dir + "/p") << s.payload;
        CHECK(update::sha256_file(dir + "/p", &a.sha256));
        unlink((dir + "/p").c_str());
    }
    std::string err;
    update::Asset bad = a;
    bad.sha256[0] = bad.sha256[0] == '0' ? '1' : '0';
    CHECK(!update::download(bad, part, "1.0.1", nullptr, nullptr, &err) && err.find("SHA-256") != std::string::npos);
    CHECK(access(part.c_str(), F_OK) != 0 && read_file(target) == "old build");
    bad = a, bad.size -= 1;
    CHECK(!update::download(bad, part, "1.0.1", nullptr, nullptr, &err) && access(part.c_str(), F_OK) != 0);
    bad = a, bad.sha256.clear();
    CHECK(!update::download(bad, part, "1.0.1", nullptr, nullptr, &err) && err.find("digest") != std::string::npos);
    bad = a, bad.url = base + "/offsite";
    CHECK(!update::download(bad, part, "1.0.1", nullptr, nullptr, &err) && access(part.c_str(), F_OK) != 0);
    std::atomic<bool> cancel{true};
    CHECK(!update::download(a, part, "1.0.1", nullptr, &cancel, &err) && access(part.c_str(), F_OK) != 0);
    uint64_t seen = 0;
    err.clear();
    CHECK(update::download(a, part, "1.0.1", [&](uint64_t g, int64_t) { seen = g; }, nullptr, &err));
    if (!err.empty()) std::printf("  download: %s\n", err.c_str());
    CHECK(seen == s.payload.size() && read_file(part) == s.payload);
    CHECK(update::install_over(part, target, &err));
    struct stat st{};
    CHECK(stat(target.c_str(), &st) == 0 && (st.st_mode & 0777) == 0755 && read_file(target) == s.payload);
    CHECK(access(part.c_str(), F_OK) != 0);
    unsetenv("DCP_UPDATE_API");
    // Without the loopback override the same URL is refused.
    CHECK(!update::download(a, part, "1.0.1", nullptr, nullptr, &err) && err.find("refusing") != std::string::npos);
    unlink(target.c_str());
    rmdir(dir.c_str());
    s.finish();
}

int main() {
    test_versions();
    test_markdown();
    test_selection();
    test_urls();
    test_http_and_install();
    std::printf(g_fail ? "test_update: %d FAILED\n" : "test_update: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
