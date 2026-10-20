// Mock lobby server: the reference implementation of notes/map/lobby_protocol.md (rooms, search, join codes, friends,
// member events, endpoint exchange) on one WebSocket listener. Used by tools/dcp_lobby (local testing: two game
// instances on one PC) and by tests/test_lobby. The Cloudflare Worker + Durable Object must behave the same.
#pragma once
#include <atomic>
#include <cstdint>

namespace dcp::net {

struct LobbyServerOptions {
    uint16_t port = 47834;          // TCP listen port (0 = any; see LobbyServer::port())
    bool loopback_only = true;      // bind 127.0.0.1 (false: 0.0.0.0 for LAN tests)
    bool verbose = true;            // log every message to stderr
    int grace_ms = 30000;           // seat grace after a verified socket drops (lobby_protocol.md 1.1); 0 = leave at once
};

class LobbyServer {
public:
    explicit LobbyServer(const LobbyServerOptions& o);
    ~LobbyServer();
    bool listening() const;
    uint16_t port() const;
    // Serve until *stop becomes true (polls every 20 ms).
    void run(const std::atomic<bool>* stop);

    LobbyServer(const LobbyServer&) = delete;
    LobbyServer& operator=(const LobbyServer&) = delete;

private:
    struct Impl;
    Impl* d_;
};

}  // namespace dcp::net
