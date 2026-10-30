// Lobby client: the game side of the lobby protocol (notes/map/lobby_protocol.md), JSON over one WebSocket. Host C++,
// no guest code: a background thread owns the socket; the HLE Matching2 shim (hle/np.cpp) issues requests and drains
// responses/events from the guest thread that runs sceNpCheckCallback. Nothing connects until the first request (the
// game only reaches Matching2 when the player goes online), so offline play never opens a socket.
// Reconnect (lobby_protocol.md 1.1): when an established link drops, the thread retries for up to kReconnectMs (Lost,
// then Back once a new socket is open) with ordinary traffic paused; the owner re-sends hello / resume as urgent
// requests and calls resume_traffic(). Only when every try fails does it report Down. $DCP_LOBBY_RECONNECT=0 turns it
// off (a drop is Down at once, as before). A link counts as dropped when it closes, stays silent for 30 s, or leaves a
// request unanswered for 10 s. Unanswered requests whose repeat is harmless (room data, search, ...) are sent again
// after the reconnect instead of failing.
#pragma once
#include <cstdint>
#include <string>

#include "net/json.h"

namespace dcp::net {

constexpr int kLobbyProtocol = 1;     // lobby protocol revision ("proto" in hello)
constexpr int kGameNetVersion = 34;   // game protocol version (packed attr 0x53 bits 13:7, FUN_00098f50 = 0x22)
constexpr const char* kDefaultLobbyUrl = "ws://127.0.0.1:47834/";

struct LobbyMessage {
    // Down = link gone for good (every pending request failed); Lost = dropped, reconnecting (unanswered requests
    // failed, except the resendable ones, which wait with the newer ones); Back = reconnected, traffic paused until
    // resume_traffic().
    enum Kind { Response, Event, Down, Lost, Back } kind = Event;
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
    // urgent: sent even while traffic is paused after a reconnect (hello / resume).
    uint32_t request(const std::string& op, Json body, bool urgent = false);
    // After Back: lets the paused requests go out (once hello / resume are answered).
    void resume_traffic();
    // The app is back in front (Android): the next ping must be answered within 5 s, else the link counts as dropped.
    void check_link();
    // Pops the next incoming message (non-blocking).
    bool poll(LobbyMessage* out);
    bool connected() const;
    const std::string& url() const;

    static constexpr int32_t kErrLobbyDown = int32_t(0x80550c01);  // ours: "server unreachable" (game: generic -100)
    static constexpr int kReconnectMs = 25000;  // reconnect window after a drop (the lobby's seat grace is 30 s)
    static constexpr int kReconnectTries = 6;

    LobbyClient(const LobbyClient&) = delete;
    LobbyClient& operator=(const LobbyClient&) = delete;

private:
    LobbyClient();
    ~LobbyClient();
    struct Impl;
    Impl* d_;
};

}  // namespace dcp::net
