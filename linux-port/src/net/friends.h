// Launcher side of the lobby friends (lobby_protocol.md 4.2): one short blocking session per refresh / button press -
// connect, hello with the identity's secret and "presence": false (a status check does not show us online), the
// optional action, then friend_requests + friend_list, close. Run it on a worker thread (a few hundred ms; 5 s timeout).
// Opens its own socket: it does not touch the game's LobbyClient singleton.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "net/identity.h"

namespace dcp::net {

struct FriendInfo {
    std::string account_id;  // u64 decimal
    std::string name;
    bool online = false;
    std::string join_code;   // non-empty when the friend is in a room
};

struct FriendRequestInfo {
    std::string account_id;
    std::string name;
    int64_t ms = 0;          // unix ms the request was sent
};

struct FriendsAction {
    enum Kind { None, Request, Accept, Decline, Remove } kind = None;
    std::string arg;  // Request: friend code (any form friend_code_normalize takes); others: account id
};

struct FriendsState {
    bool ok = false;           // connected, hello verified, lists fetched
    std::string error;         // why not (connection, identity, old lobby)
    std::string code;          // own friend code, XXXX-XXXX
    std::vector<FriendInfo> friends;
    std::vector<FriendRequestInfo> incoming, outgoing;
    bool action_ok = false;    // only meaningful when an action was given
    std::string action_msg;    // result text for the action ("Request sent to X.", error text)
};

// name: the launcher's configured name (online_name() applies the game's rule, "" = the login name).
FriendsState run_friends_session(const std::string& lobby_url, const std::string& name, const Identity& id,
                                 const FriendsAction& action);

// Text for a lobby error code in the friends context ("" for 0).
std::string friends_error_text(int32_t err);

}  // namespace dcp::net
