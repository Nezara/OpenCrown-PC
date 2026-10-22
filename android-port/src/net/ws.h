// Minimal WebSocket (RFC 6455) over TCP (net/sock.h: Winsock or BSD sockets) for the lobby link: text frames carrying
// JSON, ping/pong, close.
// Client side (lobby_client) and server side (the mock lobby, tools/dcp_lobby) share the framing code. Clients use
// Conn, which speaks ws:// (mock lobby, wrangler dev) and wss:// (the Cloudflare Worker; TLS via mbedTLS, net/tls.h).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace dcp::net::tls {
struct Session;
}

namespace dcp::net::ws {

enum Opcode : uint8_t { kCont = 0, kText = 1, kBinary = 2, kClose = 8, kPing = 9, kPong = 10 };

// ws://host:port/path -> parts. Port defaults to 80. False if not a ws:// URL (wss:// is refused: no TLS here).
bool parse_url(const std::string& url, std::string* host, uint16_t* port, std::string* path);

// ws:// or wss:// URL -> parts; *secure is set for wss:// (port defaults to 443 there).
bool parse_url(const std::string& url, std::string* host, uint16_t* port, std::string* path, bool* secure);

// Process-wide socket setup once (idempotent): sock::startup() (WSAStartup / SIGPIPE ignored).
void startup();

// Blocking connect + client handshake. Returns the socket (as uintptr_t, ~0 on failure) and sets *err.
uintptr_t client_connect(const std::string& host, uint16_t port, const std::string& path, int timeout_ms,
                         std::string* err);

// Client connection (ws:// or wss://): TCP + optional TLS + WebSocket handshake. Move-only; closes on destruction.
class Conn {
public:
    Conn() = default;
    ~Conn() { close(); }
    Conn(Conn&& o) noexcept : s_(o.s_), tls_(o.tls_) { o.s_ = ~uintptr_t(0), o.tls_ = nullptr; }
    Conn& operator=(Conn&&) = delete;
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

    bool open(const std::string& url, int timeout_ms, std::string* err);
    bool valid() const { return s_ != ~uintptr_t(0); }
    bool secure() const { return tls_ != nullptr; }
    bool send_all(const std::string& bytes);
    // > 0 bytes, 0 closed, < 0 error. Blocks unless readable() said so.
    int recv(char* buf, int n);
    // Data ready within timeout_ms (buffered TLS plaintext counts; select() alone cannot see it).
    bool readable(int timeout_ms);
    void close();

private:
    uintptr_t s_ = ~uintptr_t(0);
    tls::Session* tls_ = nullptr;
};

// Server side: `head` is the HTTP request head (up to and including the blank line). Returns the 101 response to send,
// or "" if the request is not a WebSocket upgrade.
std::string server_handshake_response(const std::string& head);

// One frame. Clients must mask (mask = true), servers must not.
std::string encode_frame(Opcode op, const std::string& payload, bool mask);

// Incremental frame decoder: feed() raw bytes, then next() returns whole messages (fragments joined; control frames
// returned as they arrive). Error state on protocol violation or messages above max_message.
class Reader {
public:
    explicit Reader(size_t max_message = 1 << 20) : max_(max_message) {}
    void feed(const char* p, size_t n) { buf_.append(p, n); }
    // True with a message; false if more bytes are needed (or error() is set).
    bool next(Opcode* op, std::string* payload);
    bool error() const { return err_; }

private:
    std::string buf_, frag_;
    Opcode frag_op_ = kText;
    size_t max_;
    bool err_ = false;
};

}  // namespace dcp::net::ws
