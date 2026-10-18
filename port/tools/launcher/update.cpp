// Platform-neutral part of the launcher updater (update.h).
#include "update.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "net/json.h"

namespace dcp::update {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with(const std::string& s, const std::string& t) {
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

std::string rtrim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    return s;
}

// Inline Markdown -> text: [text](url) / ![alt](url) -> text, <http..> -> url, ** __ ` removed.
std::string inline_text(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size();) {
        char c = s[i];
        if ((c == '[' || (c == '!' && i + 1 < s.size() && s[i + 1] == '['))) {
            size_t open = c == '!' ? i + 1 : i;
            size_t close = s.find(']', open + 1);
            if (close != std::string::npos && close + 1 < s.size() && s[close + 1] == '(') {
                size_t end = s.find(')', close + 2);
                if (end != std::string::npos) {
                    o += inline_text(s.substr(open + 1, close - open - 1));
                    i = end + 1;
                    continue;
                }
            }
        }
        if (c == '<') {
            size_t end = s.find('>', i + 1);
            std::string in = end == std::string::npos ? "" : s.substr(i + 1, end - i - 1);
            if (in.rfind("http://", 0) == 0 || in.rfind("https://", 0) == 0) {
                o += in;
                i = end + 1;
                continue;
            }
        }
        if ((c == '*' || c == '_') && i + 1 < s.size() && s[i + 1] == c) { i += 2; continue; }
        if (c == '`') { ++i; continue; }
        o += c;
        ++i;
    }
    return o;
}

}  // namespace

Version parse_version(const std::string& s0) {
    Version v;
    std::string s = s0;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    if (i < s.size() && (s[i] == 'v' || s[i] == 'V')) ++i;
    int n = 0;
    while (i < s.size()) {
        if (n == 4 || !std::isdigit(static_cast<unsigned char>(s[i]))) return {};
        long x = 0;
        size_t start = i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            x = x * 10 + (s[i] - '0');
            if (x > 1000000 || i - start > 7) return {};
            ++i;
        }
        v.part[n++] = int(x);
        if (i == s.size()) break;
        if (s[i] != '.' || i + 1 == s.size()) return {};
        ++i;
    }
    v.ok = n > 0;
    return v;
}

int compare(const Version& a, const Version& b) {
    for (int i = 0; i < 4; ++i)
        if (a.part[i] != b.part[i]) return a.part[i] < b.part[i] ? -1 : 1;
    return 0;
}

std::string to_string(const Version& v) {
    int n = 4;
    while (n > 2 && v.part[n - 1] == 0) --n;
    std::string s;
    for (int i = 0; i < n; ++i) s += (i ? "." : "") + std::to_string(v.part[i]);
    return s;
}

bool parse_releases(const std::string& json, std::vector<Release>* out, std::string* err) {
    out->clear();
    net::Json j;
    if (!net::Json::parse(json, &j)) return *err = "the update server sent an unreadable answer", false;
    if (!j.is_array()) {
        const std::string& m = j.get("message").as_string();
        return *err = m.empty() ? "unexpected answer from the update server" : "update server: " + m, false;
    }
    for (const net::Json& r : j.elems()) {
        if (!r.is_object()) continue;
        Release rel;
        rel.tag = r.get("tag_name").as_string();
        rel.body = r.get("body").as_string();
        rel.html_url = r.get("html_url").as_string();
        rel.draft = r.get("draft").as_bool(false);
        rel.prerelease = r.get("prerelease").as_bool(false);
        rel.ver = parse_version(rel.tag);
        for (const net::Json& a : r.get("assets").elems()) {
            Asset as;
            as.name = a.get("name").as_string();
            as.url = a.get("browser_download_url").as_string();
            as.size = a.get("size").as_int(0);
            std::string d = lower(a.get("digest").as_string());
            if (d.rfind("sha256:", 0) == 0 && d.size() == 7 + 64 &&
                std::all_of(d.begin() + 7, d.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }))
                as.sha256 = d.substr(7);
            rel.assets.push_back(as);
        }
        out->push_back(std::move(rel));
    }
    return true;
}

const Asset* find_asset(const Release& r, const AssetPattern& p) {
    for (const Asset& a : r.assets) {
        if (a.name.size() <= p.prefix.size() + p.suffix.size() || a.name.rfind(p.prefix, 0) != 0 ||
            !ends_with(a.name, p.suffix))
            continue;
        Version v = parse_version(a.name.substr(p.prefix.size(), a.name.size() - p.prefix.size() - p.suffix.size()));
        if (v.ok && r.ver.ok && compare(v, r.ver) == 0 && !a.url.empty()) return &a;
    }
    return nullptr;
}

std::vector<NoteLine> markdown_lines(const std::string& md) {
    std::vector<NoteLine> out;
    auto blank = [&] {
        if (!out.empty() && out.back().kind != NoteLine::Blank) out.push_back({NoteLine::Blank, "", 0});
    };
    size_t pos = 0;
    while (pos <= md.size()) {
        size_t nl = md.find('\n', pos);
        std::string line = rtrim(md.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = nl == std::string::npos ? md.size() + 1 : nl + 1;
        size_t sp = 0;
        int col = 0;
        while (sp < line.size() && (line[sp] == ' ' || line[sp] == '\t')) col += line[sp++] == '\t' ? 4 : 1;
        std::string body = line.substr(sp);
        if (body.empty()) { blank(); continue; }
        if (body.find_first_not_of("-*_ ") == std::string::npos && body.size() >= 3) { blank(); continue; }  // rule
        if (body[0] == '#') {
            size_t h = body.find_first_not_of('#');
            if (h != std::string::npos && h <= 6 && body[h] == ' ') {
                blank();
                std::string t = body.substr(h + 1);
                while (!t.empty() && (t.back() == '#' || t.back() == ' ')) t.pop_back();
                out.push_back({NoteLine::Heading, inline_text(t), 0});
                continue;
            }
        }
        if (body.size() >= 2 && (body[0] == '-' || body[0] == '*' || body[0] == '+') && body[1] == ' ') {
            out.push_back({NoteLine::Bullet, inline_text(body.substr(2)), col / 2});
            continue;
        }
        if (body[0] == '>') body = body.substr(body.size() > 1 && body[1] == ' ' ? 2 : 1);  // quote
        out.push_back({NoteLine::Text, inline_text(body), col / 2});
    }
    while (!out.empty() && out.back().kind == NoteLine::Blank) out.pop_back();
    return out;
}

std::string markdown_to_text(const std::string& md) {
    std::string o;
    for (const NoteLine& l : markdown_lines(md)) {
        if (l.kind == NoteLine::Bullet) o += std::string(size_t(l.indent) * 2, ' ') + "- ";
        o += l.text + "\n";
    }
    return o;
}

CheckResult evaluate(const std::vector<Release>& rels, const std::string& current, const AssetPattern& p) {
    CheckResult r;
    r.ok = true;
    const Version cur = parse_version(current);
    std::vector<const Release*> v;
    for (const Release& x : rels)
        if (!x.draft && !x.prerelease && x.ver.ok) v.push_back(&x);
    std::stable_sort(v.begin(), v.end(), [](const Release* a, const Release* b) { return compare(a->ver, b->ver) > 0; });
    if (!v.empty()) r.latest = to_string(v[0]->ver), r.latest_url = v[0]->html_url;
    for (const Release* x : v) {
        if (!cur.ok || compare(x->ver, cur) <= 0) break;
        r.newer = true;
        if (!r.installable)
            if (const Asset* a = find_asset(*x, p)) r.installable = true, r.target = *x, r.asset = *a;
        if (!r.notes.empty()) r.notes.push_back({NoteLine::Blank, "", 0});
        r.notes.push_back({NoteLine::Release, x->tag, 0});
        std::vector<NoteLine> n = markdown_lines(x->body);
        if (n.empty()) n.push_back({NoteLine::Text, "(no notes)", 0});
        r.notes.insert(r.notes.end(), n.begin(), n.end());
    }
    return r;
}

Url parse_url(const std::string& u) {
    Url r;
    size_t s = u.find("://");
    if (s == std::string::npos) return r;
    r.scheme = lower(u.substr(0, s));
    size_t a = s + 3, e = u.find_first_of("/?#", a);
    std::string auth = u.substr(a, e == std::string::npos ? std::string::npos : e - a);
    r.rest = e == std::string::npos ? "/" : u.substr(e);
    if (!r.rest.empty() && r.rest[0] != '/') r.rest = "/" + r.rest;
    if (auth.empty() || auth.find('@') != std::string::npos || auth.find('\\') != std::string::npos) return r;
    size_t colon = auth.rfind(':');
    if (colon != std::string::npos && auth.find(']') == std::string::npos) {
        std::string ps = auth.substr(colon + 1);
        if (ps.empty() || ps.size() > 5 || !std::all_of(ps.begin(), ps.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            return r;
        r.port = std::atoi(ps.c_str());
        auth = auth.substr(0, colon);
    }
    r.host = lower(auth);
    if (!r.port) r.port = r.scheme == "https" ? 443 : r.scheme == "http" ? 80 : 0;
    r.ok = !r.host.empty() && r.port > 0 && r.port < 65536 && (r.scheme == "https" || r.scheme == "http") &&
           u.find_first_of(" \r\n\t") == std::string::npos;
    return r;
}

bool url_allowed(const std::string& u, bool test_override) {
    Url p = parse_url(u);
    if (!p.ok) return false;
    if (test_override && (p.host == "127.0.0.1" || p.host == "localhost")) return true;
    if (p.scheme != "https" || p.port != 443) return false;
    return p.host == "github.com" || ends_with(p.host, ".github.com") || ends_with(p.host, ".githubusercontent.com");
}

std::string api_url_from_env(bool* test_override) {
    const char* e = std::getenv("DCP_UPDATE_API");
    *test_override = e && *e;
    return *test_override ? std::string(e) : std::string(kApiUrl);
}

CheckResult check(const std::string& current_version, const AssetPattern& p) {
    bool test = false;
    std::string url = api_url_from_env(&test);
    CheckResult r;
    if (!url_allowed(url, test)) return r.error = "update address not allowed: " + url, r;
    HttpResult h = http_get(url, std::string("OpenCrown-Launcher/") + current_version, "application/vnd.github+json", test);
    if (h.status == 0) return r.error = h.error.empty() ? "no answer from GitHub" : h.error, r;
    if (h.status == 403 || h.status == 429)
        return r.error = "GitHub's request limit was reached; try again in an hour", r;
    if (h.status != 200) return r.error = "GitHub answered HTTP " + std::to_string(h.status), r;
    std::vector<Release> rels;
    if (!parse_releases(h.body, &rels, &r.error)) return r;
    return evaluate(rels, current_version, p);
}

}  // namespace dcp::update
