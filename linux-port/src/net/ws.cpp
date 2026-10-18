// Minimal WebSocket (ws.h).
#include "net/ws.h"

#include "net/tls.h"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>

namespace dcp::net::ws {
namespace {

// ---- SHA-1 (handshake accept key only) ----
struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    void digest(const std::string& m, uint8_t out[20]) {
        std::string s = m;
        uint64_t bits = uint64_t(m.size()) * 8;
        s += char(0x80);
        while (s.size() % 64 != 56) s += char(0);
        for (int i = 7; i >= 0; --i) s += char(bits >> (8 * i));
        for (size_t i = 0; i < s.size(); i += 64) block(reinterpret_cast<const uint8_t*>(s.data() + i));
        for (int i = 0; i < 5; ++i)
            out[4 * i] = uint8_t(h[i] >> 24), out[4 * i + 1] = uint8_t(h[i] >> 16), out[4 * i + 2] = uint8_t(h[i] >> 8),
                    out[4 * i + 3] = uint8_t(h[i]);
    }
};

std::string base64(const uint8_t* p, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o += t[v >> 18], o += t[(v >> 12) & 63];
        o += i + 1 < n ? t[(v >> 6) & 63] : '=';
        o += i + 2 < n ? t[v & 63] : '=';
    }
    return o;
}

std::string accept_key(const std::string& key) {
    uint8_t d[20];
    Sha1().digest(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", d);
    return base64(d, 20);
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Header value (case-insensitive name) from an HTTP head, trimmed.
std::string header(const std::string& head, const std::string& name) {
    std::string lh = lower(head), ln = "\n" + lower(name) + ":";
    size_t p = lh.find(ln);
    if (p == std::string::npos) return "";
    p += ln.size();
    size_t e = head.find('\n', p);
    std::string v = head.substr(p, e == std::string::npos ? std::string::npos : e - p);
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
    return v;
}

std::mt19937& rng() {
    static std::mt19937 r(uint32_t(std::chrono::steady_clock::now().time_since_epoch().count()));
    return r;
}
std::mutex g_rng_mu;

}  // namespace

void startup() {
    static std::once_flag once;
    std::call_once(once, [] { signal(SIGPIPE, SIG_IGN); });
}

bool parse_url(const std::string& url, std::string* host, uint16_t* port, std::string* path, bool* secure) {
    *secure = url.compare(0, 6, "wss://") == 0;
    const std::string pre = *secure ? "wss://" : "ws://";
    if (url.compare(0, pre.size(), pre) != 0) return false;
    std::string rest = url.substr(pre.size());
    size_t slash = rest.find('/');
    std::string hp = rest.substr(0, slash);
    *path = slash == std::string::npos ? "/" : rest.substr(slash);
    size_t colon = hp.rfind(':');
    *port = *secure ? 443 : 80;
    if (colon != std::string::npos) {
        *port = uint16_t(std::atoi(hp.c_str() + colon + 1));
        hp = hp.substr(0, colon);
    }
    *host = hp;
    return !hp.empty() && *port;
}

bool parse_url(const std::string& url, std::string* host, uint16_t* port, std::string* path) {
    bool secure = false;
    return parse_url(url, host, port, path, &secure) && !secure;
}

namespace {

constexpr int kNoSock = -1;
constexpr uintptr_t kNoSockU = ~uintptr_t(0);

int tcp_connect(const std::string& host, uint16_t port, int timeout_ms, std::string* err) {
    startup();
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string ps = std::to_string(port);
    if (getaddrinfo(host.c_str(), ps.c_str(), &hints, &res) != 0 || !res) {
        *err = "cannot resolve " + host;
        return kNoSock;
    }
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (s < 0) {
        freeaddrinfo(res);
        *err = "socket() failed";
        return kNoSock;
    }
    timeval tmo{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof tmo);
    // Non-blocking connect with a timeout (a missing lobby must not hang the caller for 20 s).
    int fl = fcntl(s, F_GETFL);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int r = select(s + 1, nullptr, &wf, nullptr, &tv);
    int so_err = 0;
    socklen_t len = sizeof so_err;
    if (r > 0) getsockopt(s, SOL_SOCKET, SO_ERROR, &so_err, &len);  // a refused connect is "writable" with an error
    if (r <= 0 || so_err) {
        ::close(s);
        *err = "cannot connect to " + host + ":" + ps;
        return kNoSock;
    }
    fcntl(s, F_SETFL, fl);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return s;
}

// TCP connect, TLS when secure, then the WebSocket upgrade. On success *out_s / *out_t own the link.
bool open_link(const std::string& host, uint16_t port, const std::string& path, bool secure, int timeout_ms,
               std::string* err, int* out_s, tls::Session** out_t) {
    int s = tcp_connect(host, port, timeout_ms, err);
    if (s == kNoSock) return false;
    tls::Session* t = nullptr;
    if (secure && !(t = tls::handshake(uintptr_t(s), host, err))) {
        ::close(s);
        return false;
    }
    auto fail = [&](const std::string& e) {
        *err = e;
        tls::close(t);
        ::close(s);
        return false;
    };
    auto send_all = [&](const std::string& b) {
        if (t) return tls::send_all(t, b.data(), b.size());
        return ::send(s, b.data(), b.size(), MSG_NOSIGNAL) == ssize_t(b.size());
    };
    uint8_t nonce[16];
    {
        std::lock_guard<std::mutex> lk(g_rng_mu);
        for (auto& b : nonce) b = uint8_t(rng()());
    }
    std::string key = base64(nonce, 16);
    bool default_port = port == (secure ? 443 : 80);
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + (default_port ? "" : ":" + std::to_string(port)) +
                      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                      "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: dcp-lobby-client/1\r\n\r\n";
    if (!send_all(req)) return fail("handshake send failed");
    std::string head;
    char c;
    while (head.size() < 8192 && head.find("\r\n\r\n") == std::string::npos) {
        int k = t ? tls::recv(t, &c, 1) : ::recv(s, &c, 1, 0);
        if (k != 1) break;
        head += c;
    }
    if (head.compare(0, 12, "HTTP/1.1 101") != 0 || header(head, "Sec-WebSocket-Accept") != accept_key(key))
        return fail("handshake rejected: " + head.substr(0, head.find('\r')));
    timeval tmo{0, 0};  // back to blocking reads; callers use select() / Conn::readable()
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
    *out_s = s, *out_t = t;
    return true;
}

}  // namespace

uintptr_t client_connect(const std::string& host, uint16_t port, const std::string& path, int timeout_ms,
                         std::string* err) {
    int s = kNoSock;
    tls::Session* t = nullptr;
    if (!open_link(host, port, path, false, timeout_ms, err, &s, &t)) return kNoSockU;
    return uintptr_t(s);
}

bool Conn::open(const std::string& url, int timeout_ms, std::string* err) {
    close();
    std::string host, path;
    uint16_t port = 0;
    bool secure = false;
    if (!parse_url(url, &host, &port, &path, &secure)) {
        *err = "bad lobby URL (need ws:// or wss://host[:port]/path): " + url;
        return false;
    }
    int s = kNoSock;
    if (!open_link(host, port, path, secure, timeout_ms, err, &s, &tls_)) return false;
    s_ = uintptr_t(s);
    return true;
}

bool Conn::send_all(const std::string& b) {
    if (!valid()) return false;
    if (tls_) return tls::send_all(tls_, b.data(), b.size());
    return ::send(int(s_), b.data(), b.size(), MSG_NOSIGNAL) == ssize_t(b.size());
}

int Conn::recv(char* buf, int n) {
    if (!valid()) return -1;
    if (tls_) return tls::recv(tls_, buf, n);
    return int(::recv(int(s_), buf, size_t(n), 0));
}

bool Conn::readable(int timeout_ms) {
    if (!valid()) return false;
    if (tls_ && tls::pending(tls_)) return true;
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(int(s_), &rf);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    return select(int(s_) + 1, &rf, nullptr, nullptr, &tv) > 0;
}

void Conn::close() {
    if (tls_) tls::close(tls_), tls_ = nullptr;
    if (valid()) ::close(int(s_)), s_ = ~uintptr_t(0);
}

std::string server_handshake_response(const std::string& head) {
    if (lower(header(head, "Upgrade")) != "websocket") return "";
    std::string key = header(head, "Sec-WebSocket-Key");
    if (key.empty()) return "";
    return "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
           accept_key(key) + "\r\n\r\n";
}

std::string encode_frame(Opcode op, const std::string& payload, bool mask) {
    std::string f;
    f += char(0x80 | op);
    size_t n = payload.size();
    uint8_t m = mask ? 0x80 : 0;
    if (n < 126) f += char(m | n);
    else if (n < 65536) f += char(m | 126), f += char(n >> 8), f += char(n);
    else {
        f += char(m | 127);
        for (int i = 7; i >= 0; --i) f += char(uint64_t(n) >> (8 * i));
    }
    if (!mask) return f + payload;
    uint8_t k[4];
    {
        std::lock_guard<std::mutex> lk(g_rng_mu);
        for (auto& b : k) b = uint8_t(rng()());
    }
    f.append(reinterpret_cast<char*>(k), 4);
    size_t base = f.size();
    f += payload;
    for (size_t i = 0; i < n; ++i) f[base + i] = char(f[base + i] ^ k[i & 3]);
    return f;
}

bool Reader::next(Opcode* op, std::string* payload) {
    for (;;) {
        if (err_ || buf_.size() < 2) return false;
        const uint8_t* b = reinterpret_cast<const uint8_t*>(buf_.data());
        bool fin = b[0] & 0x80;
        Opcode o = Opcode(b[0] & 0x0f);
        bool masked = b[1] & 0x80;
        uint64_t n = b[1] & 0x7f;
        size_t hdr = 2;
        if (n == 126) {
            if (buf_.size() < 4) return false;
            n = uint64_t(b[2]) << 8 | b[3], hdr = 4;
        } else if (n == 127) {
            if (buf_.size() < 10) return false;
            n = 0;
            for (int i = 0; i < 8; ++i) n = n << 8 | b[2 + i];
            hdr = 10;
        }
        if (n > max_) return err_ = true, false;
        size_t mk = hdr;
        if (masked) hdr += 4;
        if (buf_.size() < hdr + n) return false;
        std::string data = buf_.substr(hdr, size_t(n));
        if (masked)
            for (size_t i = 0; i < data.size(); ++i) data[i] = char(data[i] ^ buf_[mk + (i & 3)]);
        buf_.erase(0, hdr + size_t(n));
        if (o >= kClose) {  // control frames are never fragmented
            *op = o, *payload = std::move(data);
            return true;
        }
        if (o != kCont) frag_op_ = o, frag_.clear();
        frag_ += data;
        if (frag_.size() > max_) return err_ = true, false;
        if (fin) {
            *op = frag_op_, *payload = std::move(frag_);
            frag_.clear();
            return true;
        }
    }
}

}  // namespace dcp::net::ws
