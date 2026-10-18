// HTTP/1.1 GET client (http.h) on BSD sockets + net/tls.
#include "net/http.h"

#include "net/tls.h"
#include "net/ws.h"  // ws::startup (SIGPIPE ignored)

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace dcp::net::http {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

// Non-blocking connect bounded by timeout_ms, IPv4 addresses first. -1 + *err on failure.
int tcp_connect(const std::string& host, uint16_t port, int timeout_ms, std::string* err) {
    ws::startup();
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const std::string ps = std::to_string(port);
    if (getaddrinfo(host.c_str(), ps.c_str(), &hints, &res) != 0 || !res) {
        *err = "cannot resolve " + host;
        return -1;
    }
    std::vector<addrinfo*> addrs;
    for (addrinfo* a = res; a; a = a->ai_next) addrs.push_back(a);
    std::stable_partition(addrs.begin(), addrs.end(), [](addrinfo* a) { return a->ai_family == AF_INET; });
    int s = -1;
    for (addrinfo* a : addrs) {
        s = socket(a->ai_family, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        if (s < 0) continue;
        int fl = fcntl(s, F_GETFL);
        fcntl(s, F_SETFL, fl | O_NONBLOCK);
        connect(s, a->ai_addr, a->ai_addrlen);
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        int r = select(s + 1, nullptr, &wf, nullptr, &tv);
        int so_err = 0;
        socklen_t len = sizeof so_err;
        if (r > 0) getsockopt(s, SOL_SOCKET, SO_ERROR, &so_err, &len);
        if (r > 0 && !so_err) {
            fcntl(s, F_SETFL, fl);
            break;
        }
        ::close(s);
        s = -1;
    }
    freeaddrinfo(res);
    if (s < 0) {
        *err = "cannot connect to " + host + ":" + ps;
        return -1;
    }
    timeval tmo{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof tmo);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return s;
}

// One connection (TCP, TLS on top for https) with a read buffer.
struct Conn {
    int s = -1;
    tls::Session* t = nullptr;
    std::string buf;  // received, not yet consumed
    size_t pos = 0;
    bool eof = false, failed = false;
    const std::atomic<bool>* cancel = nullptr;
    ~Conn() {
        tls::close(t);
        if (s >= 0) ::close(s);
    }
    bool send_all(const std::string& b) {
        if (t) return tls::send_all(t, b.data(), b.size());
        size_t off = 0;
        while (off < b.size()) {
            ssize_t k = ::send(s, b.data() + off, b.size() - off, MSG_NOSIGNAL);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) return false;
            off += size_t(k);
        }
        return true;
    }
    // Appends more bytes to buf. False on close / error / timeout / cancel.
    bool fill() {
        if (eof || failed) return false;
        if (cancel && cancel->load()) return failed = true, false;
        if (pos > 0 && pos == buf.size()) buf.clear(), pos = 0;
        char tmp[16384];
        int k;
        if (t) k = tls::recv(t, tmp, int(sizeof tmp));
        else {
            ssize_t r;
            do r = ::recv(s, tmp, sizeof tmp, 0);
            while (r < 0 && errno == EINTR);
            k = int(r);
        }
        if (k < 0) return failed = true, false;
        if (k == 0) return eof = true, false;
        buf.append(tmp, size_t(k));
        return true;
    }
    size_t avail() const { return buf.size() - pos; }
    // One line without the CRLF; false if none within max bytes.
    bool line(std::string* out, size_t max = 16384) {
        for (;;) {
            size_t nl = buf.find('\n', pos);
            if (nl != std::string::npos) {
                size_t end = nl > pos && buf[nl - 1] == '\r' ? nl - 1 : nl;
                *out = buf.substr(pos, end - pos);
                pos = nl + 1;
                return true;
            }
            if (avail() > max || !fill()) return false;
        }
    }
};

std::string header_value(const std::vector<std::pair<std::string, std::string>>& h, const char* name) {
    for (const auto& kv : h)
        if (kv.first == name) return kv.second;
    return "";
}

}  // namespace

bool parse_url(const std::string& url, Url* out) {
    Url u;
    std::string rest;
    if (url.compare(0, 8, "https://") == 0) u.secure = true, rest = url.substr(8);
    else if (url.compare(0, 7, "http://") == 0) rest = url.substr(7);
    else return false;
    size_t end = rest.find_first_of("/?#");
    std::string hp = rest.substr(0, end);
    u.path = end == std::string::npos ? "/" : rest.substr(end);
    if (u.path[0] != '/') u.path = "/" + u.path;  // "?query" right after the host
    if (size_t h = u.path.find('#'); h != std::string::npos) u.path.resize(h);
    if (hp.empty() || hp.find('@') != std::string::npos || hp[0] == '[') return false;
    u.port = u.secure ? 443 : 80;
    if (size_t c = hp.find(':'); c != std::string::npos) {
        std::string p = hp.substr(c + 1);
        if (p.empty() || p.size() > 5 || !std::all_of(p.begin(), p.end(), ::isdigit)) return false;
        long v = std::strtol(p.c_str(), nullptr, 10);
        if (v <= 0 || v > 65535) return false;
        u.port = uint16_t(v);
        hp.resize(c);
    }
    u.host = lower(hp);
    for (char c : u.host)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-')) return false;
    if (u.host.empty()) return false;
    for (char c : u.path)
        if (static_cast<unsigned char>(c) <= ' ') return false;  // no spaces / control characters on the request line
    *out = u;
    return true;
}

std::string resolve(const Url& base, const std::string& loc) {
    if (loc.compare(0, 7, "http://") == 0 || loc.compare(0, 8, "https://") == 0) return loc;
    std::string origin = std::string(base.secure ? "https://" : "http://") + base.host;
    if (base.port != (base.secure ? 443 : 80)) origin += ":" + std::to_string(base.port);
    if (loc.compare(0, 2, "//") == 0) return std::string(base.secure ? "https:" : "http:") + loc;
    if (!loc.empty() && loc[0] == '/') return origin + loc;
    // Relative to the current path's directory.
    std::string dir = base.path.substr(0, base.path.find('?'));
    dir.resize(dir.rfind('/') + 1);
    return origin + dir + loc;
}

bool update_host_allowed(const Url& u, bool allow_local) {
    const bool local = u.host == "127.0.0.1" || u.host == "localhost";
    if (local) return allow_local;
    if (!u.secure) return false;
    return u.host == "github.com" || ends_with(u.host, ".github.com") || ends_with(u.host, ".githubusercontent.com");
}

Response get(const Request& rq) {
    Response r;
    std::string url = rq.url;
    for (int hop = 0;; ++hop) {
        Url u;
        if (!parse_url(url, &u)) {
            r.error = "bad URL: " + url.substr(0, 200);
            return r;
        }
        if (!rq.allowed || !rq.allowed(u)) {
            r.error = "refusing to connect to " + std::string(u.secure ? "https://" : "http://") + u.host +
                      (hop ? " (redirect)" : "");
            return r;
        }
        r.final_url = url;
        Conn c;
        c.cancel = rq.cancel;
        c.s = tcp_connect(u.host, u.port, rq.timeout_ms, &r.error);
        if (c.s < 0) return r;
        if (u.secure && !(c.t = tls::handshake(uintptr_t(c.s), u.host, &r.error))) return r;
        const bool default_port = u.port == (u.secure ? 443 : 80);
        std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host +
                          (default_port ? "" : ":" + std::to_string(u.port)) +
                          "\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
        for (const std::string& h : rq.headers) req += h + "\r\n";
        req += "\r\n";
        if (!c.send_all(req)) {
            r.error = "send to " + u.host + " failed";
            return r;
        }
        // Status line + headers (skipping any 1xx interim response).
        std::string line;
        std::vector<std::pair<std::string, std::string>> hdr;
        for (;;) {
            if (!c.line(&line)) {
                r.error = c.cancel && c.cancel->load() ? "cancelled" : "no HTTP response from " + u.host;
                return r;
            }
            if (line.compare(0, 5, "HTTP/") != 0 || line.size() < 12) {
                r.error = "bad HTTP response from " + u.host;
                return r;
            }
            r.status = std::atoi(line.c_str() + 9);
            hdr.clear();
            size_t total = 0;
            for (;;) {
                if (!c.line(&line)) {
                    r.error = "truncated HTTP headers from " + u.host;
                    return r;
                }
                if (line.empty()) break;
                if ((total += line.size()) > 65536) {
                    r.error = "HTTP headers too long";
                    return r;
                }
                size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string v = line.substr(colon + 1);
                v.erase(0, v.find_first_not_of(" \t"));
                while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
                hdr.emplace_back(lower(line.substr(0, colon)), v);
            }
            if (r.status >= 200 || r.status == 101) break;
        }
        if (r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308) {
            std::string loc = header_value(hdr, "location");
            if (loc.empty()) {
                r.error = "redirect without a location";
                return r;
            }
            if (hop >= rq.max_redirects) {
                r.error = "too many redirects";
                return r;
            }
            url = resolve(u, loc);
            continue;
        }
        if (r.status != 200) {
            r.error = "HTTP " + std::to_string(r.status) + " from " + u.host;
            if (r.status == 403 || r.status == 429) r.error += " (rate limited? try again later)";
            return r;
        }
        const bool chunked = lower(header_value(hdr, "transfer-encoding")).find("chunked") != std::string::npos;
        if (std::string cl = header_value(hdr, "content-length"); !chunked && !cl.empty()) {
            char* endp = nullptr;
            long long v = std::strtoll(cl.c_str(), &endp, 10);
            if (v < 0 || endp == cl.c_str()) {
                r.error = "bad Content-Length";
                return r;
            }
            r.length = v;
        }
        uint64_t got = 0;
        auto deliver = [&](const char* p, size_t n) {
            if (!n) return true;
            got += n;
            if (rq.sink && !rq.sink(p, n)) {
                r.error = "aborted";
                return false;
            }
            if (rq.progress) rq.progress(got, r.length);
            return true;
        };
        auto read_fail = [&](const char* what) {
            r.error = c.cancel && c.cancel->load() ? "cancelled" : std::string(what) + " from " + u.host;
            return r;
        };
        if (chunked) {
            for (;;) {
                if (!c.line(&line)) return read_fail("truncated chunked body");
                char* endp = nullptr;
                unsigned long long n = std::strtoull(line.c_str(), &endp, 16);
                if (endp == line.c_str()) {
                    r.error = "bad chunk header";
                    return r;
                }
                if (n == 0) {  // trailers until the empty line (best effort: the body is complete either way)
                    while (c.line(&line) && !line.empty()) {}
                    break;
                }
                while (n) {
                    if (!c.avail() && !c.fill()) return read_fail("truncated chunked body");
                    size_t k = size_t(std::min<unsigned long long>(n, c.avail()));
                    if (!deliver(c.buf.data() + c.pos, k)) return r;
                    c.pos += k, n -= k;
                }
                if (!c.line(&line) || !line.empty()) return read_fail("bad chunk end");
            }
        } else if (r.length >= 0) {
            uint64_t left = uint64_t(r.length);
            while (left) {
                if (!c.avail() && !c.fill()) return read_fail("truncated body");
                size_t k = size_t(std::min<uint64_t>(left, c.avail()));
                if (!deliver(c.buf.data() + c.pos, k)) return r;
                c.pos += k, left -= k;
            }
        } else {  // until the server closes
            for (;;) {
                if (c.avail()) {
                    size_t k = c.avail();
                    if (!deliver(c.buf.data() + c.pos, k)) return r;
                    c.pos += k;
                }
                if (!c.fill()) {
                    if (c.failed) return read_fail("read failed");
                    break;
                }
            }
        }
        r.ok = true;
        return r;
    }
}

Response get_string(Request rq, std::string* body, size_t max_bytes) {
    body->clear();
    auto outer = rq.sink;
    bool too_large = false;
    rq.sink = [&](const char* p, size_t n) {
        if (body->size() + n > max_bytes) return too_large = true, false;
        body->append(p, n);
        return !outer || outer(p, n);
    };
    Response r = get(rq);
    if (too_large) r.error = "response too large";
    return r;
}

}  // namespace dcp::net::http
