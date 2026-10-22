// dcp_lobby [--port N] [--lan] [--quiet]: the mock lobby server (net/lobby_server.h) for local multiplayer tests. Point
// mobile_run instances at it with DCP_LOBBY=ws://127.0.0.1:N/ (default port 47834). Protocol: notes/map/lobby_protocol.md.
// dcp_lobby friend <url> <user_dir> <name> [request <code> | accept <account_id>]: one launcher-style friends session
// (net/friends.h) for the identity in <user_dir>/identity.ini (created if missing); prints "code", "account", the
// friends and pending requests. Used by the two-instance tests to make the two accounts friends.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "net/friends.h"
#include "net/identity.h"
#include "net/json.h"
#include "net/lobby_server.h"

static int friend_cmd(int argc, char** argv) {
    using namespace dcp::net;
    if (argc < 5) {
        std::fprintf(stderr, "usage: dcp_lobby friend <url> <user_dir> <name> [request <code> | accept <account_id>]\n");
        return 2;
    }
    Identity id = load_identity(argv[3]);
    FriendsAction a;
    if (argc >= 7 && !std::strcmp(argv[5], "request")) a = {FriendsAction::Request, argv[6]};
    else if (argc >= 7 && !std::strcmp(argv[5], "accept")) a = {FriendsAction::Accept, argv[6]};
    FriendsState s = run_friends_session(argv[2], argv[4], id, a);
    if (!s.ok) {
        std::printf("error %s\n", s.error.c_str());
        return 1;
    }
    std::printf("code %s\naccount %s\n", s.code.c_str(), u64_str(id.account_id).c_str());
    if (a.kind != FriendsAction::None) std::printf("action %s %s\n", s.action_ok ? "ok" : "failed", s.action_msg.c_str());
    for (const auto& f : s.friends)
        std::printf("friend %s %s %s %s\n", f.account_id.c_str(), f.name.c_str(), f.online ? "online" : "offline",
                    f.join_code.c_str());
    for (const auto& r : s.incoming) std::printf("incoming %s %s\n", r.account_id.c_str(), r.name.c_str());
    for (const auto& r : s.outgoing) std::printf("outgoing %s %s\n", r.account_id.c_str(), r.name.c_str());
    return a.kind == FriendsAction::None || s.action_ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 2 && !std::strcmp(argv[1], "friend")) return friend_cmd(argc, argv);
    dcp::net::LobbyServerOptions o;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) o.port = uint16_t(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--lan")) o.loopback_only = false;
        else if (!std::strcmp(argv[i], "--quiet")) o.verbose = false;
        else if (!std::strcmp(argv[i], "--grace") && i + 1 < argc) o.grace_ms = std::atoi(argv[++i]);
        else {
            std::fprintf(stderr, "usage: dcp_lobby [--port N] [--lan] [--quiet] [--grace MS (seat grace, 0 = off)]\n"
                                 "       dcp_lobby friend <url> <user_dir> <name> [request <code> | accept <account_id>]\n");
            return 2;
        }
    }
    setvbuf(stderr, nullptr, _IONBF, 0);
    dcp::net::LobbyServer srv(o);
    if (!srv.listening()) {
        std::fprintf(stderr, "dcp_lobby: cannot listen on port %u\n", o.port);
        return 1;
    }
    std::fprintf(stderr, "dcp_lobby: listening on ws://%s:%u/\n", o.loopback_only ? "127.0.0.1" : "0.0.0.0", srv.port());
    srv.run(nullptr);
    return 0;
}
