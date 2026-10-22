// Launcher friends session (friends.h).
#include "net/friends.h"

#include <chrono>
#include <cstdio>
#include <map>

#include "net/bug_report.h"
#include "net/json.h"
#include "net/lobby_client.h"
#include "net/ws.h"

namespace dcp::net {

std::string friends_error_text(int32_t err) {
    switch (uint32_t(err)) {
    case 0: return "";
    case 0x80550c01: return "lobby unreachable";
    case 0x80550c02: return "invalid request (check the friend code)";
    case 0x80550c04: return "lobby protocol mismatch (update the launcher)";
    case 0x80550c07: return "the lobby knows this account with a different secret (identity.ini replaced?)";
    case 0x80550c08: return "this connection has no verified identity";
    case 0x80550c09: return "not found (no player with that code, or the request is gone)";
    case 0x80550c0a: return "list full (200 friends / 50 pending requests)";
    }
    char t[48];
    std::snprintf(t, sizeof t, "lobby error 0x%08x", uint32_t(err));
    return t;
}

FriendsState run_friends_session(const std::string& lobby_url, const std::string& name, const Identity& id,
                                 const FriendsAction& action) {
    FriendsState st;
    if (!id.ok) {
        st.error = "no online identity: " + id.error;
        return st;
    }
    if (!lobby_url_safe_for_secret(lobby_url)) {
        st.error = "refusing to log in over unencrypted ws:// to a public server (use wss://)";
        return st;
    }
    ws::Conn c;
    std::string err;
    if (!c.open(lobby_url, 5000, &err)) {
        st.error = "cannot connect to the lobby: " + err;
        return st;
    }
    // Everything goes out at once; the lobby answers in order on one connection, so the action is applied before
    // the lists are read. rid 1 hello, 2 action (optional), 3 requests, 4 list.
    auto send = [&](const std::string& op, int rid, Json b) {
        b["op"] = op, b["rid"] = int64_t(rid);
        return c.send_all(ws::encode_frame(ws::kText, b.dump(), true));
    };
    Json h;
    h["proto"] = kLobbyProtocol, h["version"] = kGameNetVersion, h["title"] = "CUSA10454";
    h["name"] = online_name(name);
    h["account_id"] = u64_str(id.account_id), h["secret"] = id.secret, h["presence"] = false, h["udp_port"] = 0;
    bool sent = send("hello", 1, h);
    if (action.kind != FriendsAction::None) {
        static const char* const kOps[] = {"", "friend_request", "friend_accept", "friend_decline", "friend_remove"};
        Json a;
        if (action.kind == FriendsAction::Request) a["code"] = action.arg;
        else a["account_id"] = action.arg;
        sent = sent && send(kOps[action.kind], 2, a);
    }
    sent = sent && send("friend_requests", 3, Json::object()) && send("friend_list", 4, Json::object());
    if (!sent) {
        st.error = "send to the lobby failed";
        return st;
    }
    std::map<int, Json> got;
    const size_t want = action.kind != FriendsAction::None ? 4 : 3;
    ws::Reader rd;
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (got.size() < want && std::chrono::steady_clock::now() < end) {
        if (!c.readable(50)) continue;
        char buf[16384];
        int n = c.recv(buf, sizeof buf);
        if (n <= 0) break;
        rd.feed(buf, size_t(n));
        ws::Opcode op;
        std::string pl;
        while (rd.next(&op, &pl)) {
            Json j;
            if (op == ws::kText && Json::parse(pl, &j) && j.has("rid")) got[int(j.get("rid").as_int())] = j;
        }
        if (got.count(1) && got[1].get("err").as_int()) break;  // hello refused: the rest fail with "no hello"
    }
    c.send_all(ws::encode_frame(ws::kClose, "", true));
    c.close();

    // Error of response rid; a missing one counts as "lobby unreachable".
    auto err_of = [&](int rid) {
        auto it = got.find(rid);
        return it == got.end() ? LobbyClient::kErrLobbyDown : int32_t(it->second.get("err").as_int());
    };
    if (!got.count(1)) {
        st.error = "no answer from the lobby within 5 s";
        return st;
    }
    const Json hr = got[1];
    if (int32_t e = err_of(1)) {
        st.error = "lobby refused the login: " + friends_error_text(e);
        return st;
    }
    if (!hr.get("verified").as_bool()) {  // a lobby from before identities ignores the secret
        st.error = "this lobby does not support friends yet (server update needed)";
        return st;
    }
    st.code = hr.get("friend_code").as_string();
    if (action.kind != FriendsAction::None) {
        int32_t e = err_of(2);
        const Json ar = e ? Json() : got[2];
        st.action_ok = e == 0;
        if (e) st.action_msg = friends_error_text(e);
        else if (action.kind == FriendsAction::Request)
            st.action_msg = ar.get("state").as_string() == "friends"
                                ? "You and " + ar.get("name").as_string() + " are now friends."
                                : "Request sent to " + ar.get("name").as_string() + ".";
        else if (action.kind == FriendsAction::Accept) st.action_msg = "Accepted.";
        else if (action.kind == FriendsAction::Decline) st.action_msg = "Request removed.";
        else st.action_msg = "Friend removed.";
    }
    if (int32_t e = err_of(3) ? err_of(3) : err_of(4)) {
        st.error = "friend lists: " + friends_error_text(e);
        return st;
    }
    const Json rq = got[3], fl = got[4];
    for (const Json& r : rq.get("incoming").elems())
        st.incoming.push_back({r.get("account_id").as_string(), r.get("name").as_string(), r.get("ms").as_int()});
    for (const Json& r : rq.get("outgoing").elems())
        st.outgoing.push_back({r.get("account_id").as_string(), r.get("name").as_string(), r.get("ms").as_int()});
    for (const Json& f : fl.get("friends").elems())
        st.friends.push_back({f.get("account_id").as_string(), f.get("name").as_string(), f.get("online").as_bool(),
                              f.get("join_code").as_string()});
    st.ok = true;
    return st;
}

}  // namespace dcp::net
