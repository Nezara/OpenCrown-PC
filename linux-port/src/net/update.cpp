// Self-updater logic (update.h).
#include "net/update.h"

#include "net/http.h"
#include "net/json.h"

#include <mbedtls/sha256.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dcp::net::update {

Version parse_version(const std::string& s0) {
    Version v;
    std::string s = s0;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    if (s.empty()) return v;
    int n = 0;
    size_t i = 0;
    while (i < s.size()) {
        if (n == 4 || !std::isdigit(static_cast<unsigned char>(s[i]))) return v;
        long x = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            x = x * 10 + (s[i++] - '0');
            if (x > 1000000) return v;
        }
        v.part[n++] = int(x);
        if (i < s.size()) {
            if (s[i] != '.' || i + 1 == s.size()) return v;
            ++i;
        }
    }
    v.ok = true;
    return v;
}

int compare(const Version& a, const Version& b) {
    for (int i = 0; i < 4; ++i)
        if (a.part[i] != b.part[i]) return a.part[i] < b.part[i] ? -1 : 1;
    return 0;
}

std::string str(const Version& v) {
    std::string s = std::to_string(v.part[0]) + "." + std::to_string(v.part[1]) + "." + std::to_string(v.part[2]);
    if (v.part[3]) s += "." + std::to_string(v.part[3]);
    return s;
}

bool parse_releases(const std::string& text, std::vector<Release>* out, std::string* err) {
    out->clear();
    Json j;
    if (!Json::parse(text, &j)) {
        *err = "the update server sent invalid JSON";
        return false;
    }
    if (!j.is_array()) {
        std::string m = j.get("message").is_string() ? j.get("message").as_string() : "";
        *err = "unexpected answer from the update server" + (m.empty() ? std::string() : ": " + m.substr(0, 160));
        return false;
    }
    for (const Json& r : j.elems()) {
        if (!r.is_object()) continue;
        Release rel;
        rel.tag = r.get("tag_name").is_string() ? r.get("tag_name").as_string() : "";
        rel.name = r.get("name").is_string() ? r.get("name").as_string() : "";
        rel.body = r.get("body").is_string() ? r.get("body").as_string() : "";
        rel.page = r.get("html_url").is_string() ? r.get("html_url").as_string() : "";
        rel.draft = r.get("draft").as_bool();
        rel.prerelease = r.get("prerelease").as_bool();
        rel.ver = parse_version(rel.tag);
        for (const Json& a : r.get("assets").elems()) {
            if (!a.is_object()) continue;
            Asset as;
            as.name = a.get("name").is_string() ? a.get("name").as_string() : "";
            as.url = a.get("browser_download_url").is_string() ? a.get("browser_download_url").as_string() : "";
            as.size = a.get("size").is_number() ? a.get("size").as_int() : -1;
            std::string d = a.get("digest").is_string() ? a.get("digest").as_string() : "";
            if (d.compare(0, 7, "sha256:") == 0) {
                std::string h = d.substr(7);
                std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                if (h.size() == 64 && std::all_of(h.begin(), h.end(), ::isxdigit)) as.sha256 = h;
            }
            rel.assets.push_back(as);
        }
        out->push_back(std::move(rel));
    }
    return true;
}

bool is_linux_asset(const std::string& n) {
    const std::string pre = "OpenCrown-Linux-", suf = "-x86_64.AppImage";
    return n.size() > pre.size() + suf.size() && n.compare(0, pre.size(), pre) == 0 &&
           n.compare(n.size() - suf.size(), suf.size(), suf) == 0;
}

std::string md_inline(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size();) {
        // ![alt](url) / [text](url) -> alt / text
        if (s[i] == '[' || (s[i] == '!' && i + 1 < s.size() && s[i + 1] == '[')) {
            size_t open = s[i] == '!' ? i + 1 : i;
            size_t close = s.find(']', open + 1);
            if (close != std::string::npos && close + 1 < s.size() && s[close + 1] == '(') {
                size_t end = s.find(')', close + 2);
                if (end != std::string::npos) {
                    o += md_inline(s.substr(open + 1, close - open - 1));
                    i = end + 1;
                    continue;
                }
            }
        }
        if ((s[i] == '*' && i + 1 < s.size() && s[i + 1] == '*') || (s[i] == '_' && i + 1 < s.size() && s[i + 1] == '_')) {
            i += 2;
            continue;
        }
        if (s[i] == '`') {
            ++i;
            continue;
        }
        if (s[i] == '\\' && i + 1 < s.size() && std::ispunct(static_cast<unsigned char>(s[i + 1]))) {
            o += s[i + 1];
            i += 2;
            continue;
        }
        o += s[i++];
    }
    return o;
}

std::vector<NoteLine> notes_from_markdown(const std::string& body) {
    std::vector<NoteLine> out;
    auto blank = [&] {
        if (!out.empty() && out.back().kind != NoteLine::Blank) out.push_back({NoteLine::Blank, 0, ""});
    };
    size_t a = 0;
    bool fence = false;
    while (a <= body.size()) {
        size_t b = body.find('\n', a);
        if (b == std::string::npos) b = body.size();
        std::string line = body.substr(a, b - a);
        a = b + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t ind = line.find_first_not_of(" \t");
        if (ind == std::string::npos) {
            blank();
            continue;
        }
        std::string t = line.substr(ind);
        if (t.compare(0, 3, "```") == 0 || t.compare(0, 3, "~~~") == 0) {
            fence = !fence;
            continue;
        }
        if (fence) {
            out.push_back({NoteLine::Text, 0, line});
            continue;
        }
        if (t.compare(0, 4, "<!--") == 0) continue;
        if (t.find_first_not_of("-*_ ") == std::string::npos && std::count_if(t.begin(), t.end(), [](char c) {
                return c != ' ';
            }) >= 3) {  // horizontal rule
            blank();
            continue;
        }
        if (t[0] == '#') {
            size_t h = t.find_first_not_of('#');
            if (h != std::string::npos && h <= 6 && t[h] == ' ') {
                std::string txt = md_inline(t.substr(h + 1));
                while (!txt.empty() && (txt.back() == '#' || txt.back() == ' ')) txt.pop_back();
                out.push_back({NoteLine::Heading, 0, txt});
                continue;
            }
        }
        if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t.size() > 1 && t[1] == ' ') {
            std::string txt = t.substr(2);
            if (txt.compare(0, 4, "[ ] ") == 0 || txt.compare(0, 4, "[x] ") == 0) txt.erase(0, 4);
            out.push_back({NoteLine::Bullet, int(std::min<size_t>(ind / 2, 4)), md_inline(txt)});
            continue;
        }
        if (t[0] == '>') {
            t.erase(0, t.find_first_not_of("> "));
            if (t.empty()) continue;
        }
        out.push_back({NoteLine::Text, 0, md_inline(t)});
    }
    while (!out.empty() && out.back().kind == NoteLine::Blank) out.pop_back();
    return out;
}

std::string notes_text(const std::vector<NoteLine>& lines) {
    std::string o;
    for (const NoteLine& l : lines) {
        switch (l.kind) {
        case NoteLine::Tag: o += "== " + l.text + " ==\n"; break;
        case NoteLine::Heading: o += "# " + l.text + "\n"; break;
        case NoteLine::Bullet: o += std::string(size_t(l.indent) * 2, ' ') + "- " + l.text + "\n"; break;
        case NoteLine::Text: o += l.text + "\n"; break;
        case NoteLine::Blank: o += "\n"; break;
        }
    }
    return o;
}

Result evaluate(const std::vector<Release>& rels, const std::string& current,
                bool (*asset_match)(const std::string&)) {
    Result r;
    r.ok = true;
    r.current = current;
    r.page = kReleasesPage;
    const Version cur = parse_version(current);
    std::vector<const Release*> newer;
    for (const Release& rel : rels)
        if (!rel.draft && !rel.prerelease && rel.ver.ok && (!cur.ok || compare(rel.ver, cur) > 0)) newer.push_back(&rel);
    std::stable_sort(newer.begin(), newer.end(),
                     [](const Release* a, const Release* b) { return compare(a->ver, b->ver) > 0; });
    if (newer.empty()) return r;
    r.latest = str(newer[0]->ver);
    if (!newer[0]->page.empty()) r.page = newer[0]->page;
    for (const Release* rel : newer) {
        if (r.have_target) break;
        for (const Asset& a : rel->assets)
            if (asset_match(a.name) && !a.url.empty()) {
                r.have_target = true, r.target_tag = rel->tag, r.asset = a;
                if (!rel->page.empty()) r.page = rel->page;
                break;
            }
    }
    for (const Release* rel : newer) {
        std::string head = rel->tag;
        if (!rel->name.empty() && rel->name != rel->tag) head += " - " + rel->name;
        r.notes.push_back({NoteLine::Tag, 0, head});
        std::vector<NoteLine> body = notes_from_markdown(rel->body);
        if (body.empty()) body.push_back({NoteLine::Text, 0, "(no release notes)"});
        r.notes.insert(r.notes.end(), body.begin(), body.end());
        r.notes.push_back({NoteLine::Blank, 0, ""});
    }
    r.notes.pop_back();
    return r;
}

std::string api_url(bool* allow_local) {
    *allow_local = false;
    const char* e = std::getenv("DCP_UPDATE_API");
    if (!e || !*e) return kApiUrl;
    http::Url u;
    if (http::parse_url(e, &u) && (u.host == "127.0.0.1" || u.host == "localhost")) *allow_local = true;
    return e;
}

namespace {

std::vector<std::string> headers(const std::string& ver, const char* accept) {
    return {"User-Agent: OpenCrown-Launcher/" + ver, std::string("Accept: ") + accept};
}

}  // namespace

Result check(const std::string& current_version, const std::atomic<bool>* cancel) {
    Result bad;
    bad.current = current_version;
    bad.page = kReleasesPage;
    bool local = false;
    http::Request rq;
    rq.url = api_url(&local);
    rq.headers = headers(current_version, "application/vnd.github+json");
    rq.headers.push_back("X-GitHub-Api-Version: 2022-11-28");
    rq.allowed = [local](const http::Url& u) { return http::update_host_allowed(u, local); };
    rq.cancel = cancel;
    std::string body;
    http::Response resp = http::get_string(rq, &body, 16u << 20);
    if (!resp.ok) {
        bad.error = resp.error;
        return bad;
    }
    std::vector<Release> rels;
    if (!parse_releases(body, &rels, &bad.error)) return bad;
    return evaluate(rels, current_version);
}

bool sha256_file(const std::string& path, std::string* hex) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    bool ok = mbedtls_sha256_starts(&c, 0) == 0;
    unsigned char buf[65536];
    for (;;) {
        ssize_t k = ::read(fd, buf, sizeof buf);
        if (k < 0 && errno == EINTR) continue;
        if (k < 0) ok = false;
        if (k <= 0) break;
        ok = ok && mbedtls_sha256_update(&c, buf, size_t(k)) == 0;
    }
    unsigned char out[32];
    ok = ok && mbedtls_sha256_finish(&c, out) == 0;
    mbedtls_sha256_free(&c);
    ::close(fd);
    if (!ok) return false;
    static const char* d = "0123456789abcdef";
    hex->clear();
    for (unsigned char b : out) *hex += d[b >> 4], *hex += d[b & 15];
    return true;
}

bool download(const Asset& asset, const std::string& part, const std::string& current_version,
              const std::function<void(uint64_t, int64_t)>& progress, const std::atomic<bool>* cancel,
              std::string* err) {
    if (asset.sha256.empty()) {
        *err = "the release lists no SHA-256 digest for " + asset.name + ": not installing it automatically";
        return false;
    }
    if (asset.size <= 0) {
        *err = "the release lists no size for " + asset.name;
        return false;
    }
    ::unlink(part.c_str());
    int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        *err = "cannot write " + part + ": " + std::strerror(errno);
        return false;
    }
    auto fail = [&](const std::string& e) {
        if (fd >= 0) ::close(fd);
        ::unlink(part.c_str());
        *err = e;
        return false;
    };
    bool local = false;
    api_url(&local);
    http::Request rq;
    rq.url = asset.url;
    rq.headers = headers(current_version, "application/octet-stream");
    rq.allowed = [local](const http::Url& u) { return http::update_host_allowed(u, local); };
    rq.cancel = cancel;
    rq.progress = progress;
    uint64_t written = 0;
    std::string werr;
    rq.sink = [&](const char* p, size_t n) {
        if (written + n > uint64_t(asset.size)) return werr = "the download is larger than the release says", false;
        while (n) {
            ssize_t k = ::write(fd, p, n);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) return werr = "cannot write " + part + ": " + std::strerror(errno), false;
            p += k, n -= size_t(k), written += uint64_t(k);
        }
        return true;
    };
    http::Response resp = http::get(rq);
    if (!resp.ok) return fail("download failed: " + (werr.empty() ? resp.error : werr));
    if (::fsync(fd) != 0 || ::close(fd) != 0) {
        fd = -1;
        return fail("cannot write " + part + ": " + std::strerror(errno));
    }
    fd = -1;
    struct stat st;
    if (::stat(part.c_str(), &st) != 0 || st.st_size != asset.size)
        return fail("size mismatch: got " + std::to_string(written) + " bytes, the release says " +
                    std::to_string(asset.size));
    std::string h;
    if (!sha256_file(part, &h)) return fail("cannot read back " + part);
    if (h != asset.sha256) return fail("SHA-256 mismatch: the download is damaged (nothing was changed)");
    return true;
}

bool install_over(const std::string& part, const std::string& target, std::string* err) {
    struct stat st;
    mode_t mode = 0755;
    if (::stat(target.c_str(), &st) == 0) mode = (st.st_mode & 07777) | 0755;
    if (::chmod(part.c_str(), mode) != 0) {
        *err = "cannot chmod " + part + ": " + std::strerror(errno);
        ::unlink(part.c_str());
        return false;
    }
    if (::rename(part.c_str(), target.c_str()) != 0) {
        *err = "cannot replace " + target + ": " + std::strerror(errno);
        ::unlink(part.c_str());
        return false;
    }
    return true;
}

}  // namespace dcp::net::update
