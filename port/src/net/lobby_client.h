// Lobby client: the game side of the lobby protocol (notes/map/lobby_protocol.md), JSON over one WebSocket. Host C++,
// no guest code: a background thread owns the socket; the HLE Matching2 shim (hle/np.cpp) issues requests and drains
// responses/events from the guest thread that runs sceNpCheckCallback. Nothing connects until the first request (the
// game only reaches Matching2 when the player goes online), so offline play never opens a socket.
#pragma once
#include <cstdint>
#include <string>

#include "net/json.h"

namespace dcp::net {

constexpr int kLobbyProtocol = 1;     // lobby protocol revision ("proto" in hello)
constexpr int kGameNetVersion = 34;   // game protocol version (packed attr 0x53 bits 13:7, FUN_00098f50 = 0x22)
constexpr const char* kDefaultLobbyUrl = "ws://127.0.0.1:47834/";

struct LobbyMessage {
    enum Kind { Response, Event, Down } kind = Event;
    uint32_t rid = 0;     // Response: request id from request()
    int32_t err = 0;      // Response: 0 ok, else a Matching2-style error code (negative int32)
    std::string ev;       // Event: name
    Json body;            // the whole message object
};

class LobbyClient {
public:
    // URL from $DCP_LOBBY (or kDefaultLobbyUrl). Starts the connection thread on first use.
    static LobbyClient& instance();

    // Queues a request ({"op": op, "rid": n, ...body}); returns rid. If the link is down or fails, a Response with
    // err = kErrLobbyDown is delivered for it instead.
    uint32_t request(const std::string& op, Json body);
    // Pops the next incoming message (non-blocking).
    bool poll(LobbyMessage* out);
    bool connected() const;
    const std::string& url() const;

    static constexpr int32_t kErrLobbyDown = int32_t(0x80550c01);  // ours: "server unreachable" (game: generic -100)

    LobbyClient(const LobbyClient&) = delete;
    LobbyClient& operator=(const LobbyClient&) = delete;

private:
    LobbyClient();
    ~LobbyClient();
    struct Impl;
    Impl* d_;
};

}  // namespace dcp::net
