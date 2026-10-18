// Mock lobby server (lobby_server.h). Single-threaded select() loop; all state in memory. Message formats are
// specified in notes/map/lobby_protocol.md (keep the two in sync).
#define FD_SETSIZE 256
#include "net/lobby_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "net/identity.h"
#include "net/json.h"
#include "net/lobby_client.h"
#include "net/stun.h"
#include "net/ws.h"

namespace dcp::net {
namespace {

// Error codes (Matching2 server-error style; names/values marked unverified are ours, the game only special-cases
// 0x80550d19 / 0x80550d15 / 0x80550d0f on join, asm b9b34..b9b4b).
constexpr int32_t kErrBadRequest = int32_t(0x80550c02);  // ours
constexpr int32_t kErrNoHello = int32_t(0x80550c03);     // ours
constexpr int32_t kErrProto = int32_t(0x80550c04);       // ours: lobby protocol revision mismatch
constexpr int32_t kErrNotOwner = int32_t(0x80550c05);    // ours
constexpr int32_t kErrInRoom = int32_t(0x80550c06);      // ours: already in a room
constexpr int32_t kErrAuth = int32_t(0x80550c07);        // ours: known account, wrong secret (hello refused)
constexpr int32_t kErrUnverified = int32_t(0x80550c08);  // ours: friend op without a verified account (no secret)
constexpr int32_t kErrNotFound = int32_t(0x80550c09);    // ours: no such friend code / pending request
constexpr int32_t kErrLimit = int32_t(0x80550c0a);       // ours: friend list or pending requests full
constexpr int32_t kErrNoRoom = int32_t(0x80550d0f);      // room not found (game: -6)
constexpr int32_t kErrClosed = int32_t(0x80550d15);      // room closed (game: -7, unverified name)
constexpr int32_t kErrFull = int32_t(0x80550d19);        // room full (game: -8, unverified name)

constexpr uint32_t kFlagFull = 0x20000000, kFlagClosed = 0x40000000;  // PSN room flag bits (unverified on PS4)
constexpr size_t kMaxFriends = 200, kMaxPending = 50;  // per account; pending = incoming and outgoing separately
constexpr size_t kMaxGameData = 0x80, kMaxStatus = 0xc0;  // set_presence limits (bytes; Toolkit2 SetPresence fields)
const char* const kCodeAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // join codes and friend codes (no I O 0 1)

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

struct Attr {
    int id;
    std::string hex;
};
using Attrs = std::vector<Attr>;

Attrs attrs_from(const Json& a) {
    Attrs r;
    for (const auto& e : a.elems()) r.push_back({int(e.get("id").as_int()), e.get("data").as_string()});
    return r;
}
Json attrs_json(const Attrs& a) {
    Json r = Json::array();
    for (const auto& e : a) {
        Json o;
        o["id"] = e.id, o["data"] = e.hex;
        r.push(o);
    }
    return r;
}
void attrs_merge(Attrs& dst, const Attrs& src) {
    for (const auto& s : src) {
        bool found = false;
        for (auto& d : dst)
            if (d.id == s.id) d.hex = s.hex, found = true;
        if (!found) dst.push_back(s);
    }
}

struct Client {
    int id = 0;
    int s = -1;
    std::string head;  // HTTP request head until the upgrade
    bool ws = false;
    ws::Reader rd;
    bool hello = false;
    std::string name;
    uint64_t account = 0;
    std::string peer_ip;  // as seen on the TCP connection
    std::string udp_ip;   // reported (optional) or peer_ip
    uint16_t udp_port = 0;
    std::string local_ip;  // LAN candidate (RFC 1918 only), relayed in signal for players behind the same NAT
    uint16_t local_port = 0;
    bool verified = false;  // hello carried the account's secret (friend ops allowed)
    bool presence = true;   // hello "presence": false = launcher status check, not shown online to friends
    std::string game_data;  // set_presence (4.3): hex, the game's presence blob (0x78-B room info); "" = none
    std::string status;     // set_presence: hex, the game's 0xc0-B status field
    Attrs user_bin;
    uint64_t room = 0;
    uint16_t member = 0;
    bool closing = false;
};

// Registered account (first verified hello). The Worker keeps the same in SQLite; the mock forgets on exit.
struct Account {
    std::string secret_hash;  // sha-256 hex of the secret
    std::string name;         // last hello name
    std::string code;         // friend code, 8 chars (shown XXXX-XXXX)
};

struct Room {
    uint64_t id = 0;
    uint32_t world = 1;
    uint16_t max_slot = 4;
    uint32_t flag_attr = 0;
    uint16_t owner = 0;
    std::string code;
    std::map<uint16_t, int> members;  // memberId -> client id
    std::map<uint16_t, int64_t> joined;
    std::map<uint16_t, Attrs> member_bin;
    std::map<int, int64_t> int_attrs;
    Attrs bin_internal, bin_external;
};

}  // namespace

struct LobbyServer::Impl {
    LobbyServerOptions opt;
    int ls = -1;
    uint16_t port = 0;
    std::map<int, std::unique_ptr<Client>> clients;
    std::map<uint64_t, Room> rooms;
    std::map<uint64_t, Account> accounts;
    std::map<uint64_t, std::set<uint64_t>> friends;              // mutual: a in friends[b] <=> b in friends[a]
    std::map<std::pair<uint64_t, uint64_t>, int64_t> requests;   // (from, to) -> unix ms, pending
    struct Report { int64_t ms; uint64_t account; std::string ip; };
    std::vector<Report> reports;  // bug reports (4.4): only what the limits need; the text/log go to the verbose log
    int64_t next_report = 1;
    int next_client = 1;
    uint64_t next_room = 0x1000;
    std::mt19937 rng{uint32_t(now_ms())};

    void log(const char* dir, const Client& c, const std::string& text) {
        if (opt.verbose) std::fprintf(stderr, "LOBBYD %s #%d %s: %.400s\n", dir, c.id, c.name.c_str(), text.c_str());
    }
    void send_json(Client& c, const Json& j) {
        std::string t = j.dump();
        log("->", c, t);
        std::string f = ws::encode_frame(ws::kText, t, false);
        if (send(c.s, f.data(), f.size(), MSG_NOSIGNAL) != ssize_t(f.size())) c.closing = true;
    }
    Client* by_id(int id) {
        auto it = clients.find(id);
        return it == clients.end() ? nullptr : it->second.get();
    }
    // The connection friends see for account a: verified, presence on; one in a room wins.
    Client* present(uint64_t a) {
        Client* r = nullptr;
        for (auto& [k, c] : clients)
            if (c->hello && c->verified && c->presence && !c->closing && c->account == a && (!r || (!r->room && c->room)))
                r = c.get();
        return r;
    }
    void account_event(uint64_t a, const std::string& ev, const Json& body) {  // every verified connection of a
        for (auto& [k, c] : clients)
            if (c->hello && c->verified && !c->closing && c->account == a) event(*c, ev, body);
    }
    void reply(Client& c, const Json& req, Json body, int32_t err = 0) {
        body["rid"] = req.get("rid");
        body["err"] = int64_t(err);
        send_json(c, body);
    }
    void event(Client& c, const std::string& ev, Json body) {
        body["ev"] = ev;
        send_json(c, body);
    }
    void room_event(Room& r, const std::string& ev, const Json& body, int except = 0) {
        for (auto& [mid, cid] : r.members)
            if (cid != except)
                if (Client* m = by_id(cid)) event(*m, ev, body);
    }

    std::string new_code() {
        for (;;) {
            std::string s;
            for (int i = 0; i < 6; ++i) s += kCodeAlphabet[rng() % 32];
            bool used = false;
            for (auto& [k, r] : rooms) used |= r.code == s;
            if (!used) return s;
        }
    }
    std::string new_friend_code() {
        for (;;) {
            std::string s;
            for (int i = 0; i < 8; ++i) s += kCodeAlphabet[rng() % 32];
            bool used = false;
            for (auto& [k, a] : accounts) used |= a.code == s;
            if (!used) return s;
        }
    }

    // ---- friends (lobby_protocol.md 4.2) ----
    std::string account_name(uint64_t a) {
        auto it = accounts.find(a);
        return it == accounts.end() ? "" : it->second.name;
    }
    // The presence blob friends see for account a: from a present connection that set one (4.3).
    Client* present_with_data(uint64_t a) {
        for (auto& [k, c] : clients)
            if (c->hello && c->verified && c->presence && !c->closing && c->account == a && !c->game_data.empty())
                return c.get();
        return nullptr;
    }
    void put_presence(Json& f, uint64_t a) {
        if (Client* d = present_with_data(a)) {
            f["game_data"] = d->game_data;
            if (!d->status.empty()) f["status"] = d->status;
        }
    }
    Json friend_json(uint64_t a) {
        Json f;
        Client* o = present(a);
        f["account_id"] = u64_str(a);
        f["name"] = o ? o->name : account_name(a);
        f["online"] = o != nullptr;
        if (o && o->room) {
            f["room_id"] = u64_str(o->room);
            f["join_code"] = rooms.count(o->room) ? rooms[o->room].code : "";
        }
        put_presence(f, a);
        return f;
    }
    size_t pending_count(uint64_t a, bool incoming) {
        size_t n = 0;
        for (auto& [k, ms] : requests) n += (incoming ? k.second : k.first) == a;
        return n;
    }
    // friend_online / friend_offline to a's friends when a's presence appears / goes away.
    void presence_event(uint64_t a, bool on) {
        Json b;
        b["account_id"] = u64_str(a);
        if (on) b["name"] = account_name(a), put_presence(b, a);
        for (uint64_t f : friends[a]) account_event(f, on ? "friend_online" : "friend_offline", b);
    }
    // a accepted b's request (or sent one back): mutual from now on; b's connections get friend_added.
    void make_friends(uint64_t a, uint64_t b) {
        friends[a].insert(b), friends[b].insert(a);
        requests.erase({a, b}), requests.erase({b, a});
        account_event(b, "friend_added", friend_json(a));
    }
    bool friends_full(uint64_t a, uint64_t b) {
        return friends[a].size() >= kMaxFriends || friends[b].size() >= kMaxFriends;
    }
    Json pending_json(uint64_t me, bool incoming) {  // oldest first
        std::vector<std::pair<int64_t, uint64_t>> v;
        for (auto& [k, ms] : requests)
            if ((incoming ? k.second : k.first) == me) v.push_back({ms, incoming ? k.first : k.second});
        std::sort(v.begin(), v.end());
        Json l = Json::array();
        for (auto& [ms, a] : v) {
            Json o;
            o["account_id"] = u64_str(a), o["name"] = account_name(a), o["ms"] = ms;
            l.push(o);
        }
        return l;
    }
    void handle_friend(Client& c, const std::string& op, const Json& j) {
        const uint64_t me = c.account;
        if (op == "friend_code") {
            Json b;
            b["friend_code"] = friend_code_display(accounts[me].code);
            return reply(c, j, b);
        }
        if (op == "friend_request") {
            std::string code = friend_code_normalize(j.get("code").as_string());
            if (code.empty()) return reply(c, j, Json::object(), kErrBadRequest);
            uint64_t t = 0;
            for (auto& [a, acc] : accounts)
                if (acc.code == code) t = a;
            if (!t) return reply(c, j, Json::object(), kErrNotFound);
            if (t == me) return reply(c, j, Json::object(), kErrBadRequest);
            Json b;
            b["account_id"] = u64_str(t), b["name"] = account_name(t), b["state"] = "friends";
            if (friends[me].count(t)) return reply(c, j, b);
            if (requests.count({t, me})) {  // they asked us already: this is an accept
                if (friends_full(me, t)) return reply(c, j, Json::object(), kErrLimit);
                make_friends(me, t);
                return reply(c, j, b);
            }
            b["state"] = "pending";
            if (requests.count({me, t})) return reply(c, j, b);  // sent before: unchanged
            if (friends[me].size() >= kMaxFriends || pending_count(me, false) >= kMaxPending ||
                pending_count(t, true) >= kMaxPending)
                return reply(c, j, Json::object(), kErrLimit);
            requests[{me, t}] = now_ms();
            reply(c, j, b);
            Json ev;
            ev["account_id"] = u64_str(me), ev["name"] = account_name(me);
            account_event(t, "friend_request", ev);
            return;
        }
        if (op == "friend_requests") {
            Json b;
            b["incoming"] = pending_json(me, true);
            b["outgoing"] = pending_json(me, false);
            return reply(c, j, b);
        }
        const uint64_t a = j.get("account_id").as_u64();
        if (op == "friend_accept") {
            if (!requests.count({a, me})) return reply(c, j, Json::object(), kErrNotFound);
            if (friends_full(me, a)) return reply(c, j, Json::object(), kErrLimit);
            make_friends(me, a);
            return reply(c, j, friend_json(a));
        }
        if (op == "friend_decline") {  // declines an incoming request or withdraws an outgoing one; no event
            if (!requests.erase({a, me}) && !requests.erase({me, a})) return reply(c, j, Json::object(), kErrNotFound);
            return reply(c, j, Json::object());
        }
        if (op == "friend_remove") {  // both directions, plus any pending request between the two; idempotent
            bool was = friends[me].erase(a) > 0;
            friends[a].erase(me);
            requests.erase({a, me}), requests.erase({me, a});
            reply(c, j, Json::object());
            if (was) {
                Json ev;
                ev["account_id"] = u64_str(me);
                account_event(a, "friend_removed", ev);
            }
            return;
        }
        Json l = Json::array();  // friend_list: mutual friends only, by account id
        for (uint64_t f : friends[me]) l.push(friend_json(f));
        Json b;
        b["friends"] = l;
        reply(c, j, b);
    }
    uint32_t room_flags(const Room& r) const {
        return r.flag_attr | (r.members.size() >= r.max_slot ? kFlagFull : 0);
    }
    Json member_json(const Room& r, uint16_t mid) {
        Json m;
        Client* c = by_id(r.members.at(mid));
        m["member_id"] = int(mid);
        m["account_id"] = u64_str(c ? c->account : 0);
        m["name"] = c ? c->name : "";
        m["join_ms"] = r.joined.at(mid);
        m["nat"] = 2;
        auto it = r.member_bin.find(mid);
        m["bin_internal"] = attrs_json(it == r.member_bin.end() ? Attrs{} : it->second);
        return m;
    }
    Json int_attrs_json(const Room& r) {
        Json a = Json::array();
        for (auto& [id, v] : r.int_attrs) {
            Json o;
            o["id"] = id, o["value"] = v;
            a.push(o);
        }
        return a;
    }
    Json room_internal(Room& r) {
        Json j;
        j["room_id"] = u64_str(r.id);
        j["world_id"] = int64_t(r.world);
        j["server_id"] = 1;
        j["max_slot"] = int(r.max_slot);
        j["flag_attr"] = int64_t(room_flags(r));
        j["owner"] = int(r.owner);
        j["join_code"] = r.code;
        Json ms = Json::array();
        for (auto& [mid, cid] : r.members) ms.push(member_json(r, mid));
        j["members"] = ms;
        j["bin_internal"] = attrs_json(r.bin_internal);
        j["bin_external"] = attrs_json(r.bin_external);
        j["int_attrs"] = int_attrs_json(r);
        return j;
    }
    Json room_external(Room& r) {
        Json j;
        j["room_id"] = u64_str(r.id);
        j["world_id"] = int64_t(r.world);
        j["server_id"] = 1;
        j["max_slot"] = int(r.max_slot);
        j["cur_members"] = int(r.members.size());
        j["flag_attr"] = int64_t(room_flags(r));
        j["join_code"] = r.code;
        Client* o = r.members.count(r.owner) ? by_id(r.members[r.owner]) : nullptr;
        j["owner_account_id"] = u64_str(o ? o->account : 0);
        j["owner_name"] = o ? o->name : "";
        j["int_attrs"] = int_attrs_json(r);
        j["bin_external"] = attrs_json(r.bin_external);
        return j;
    }

    // Endpoint exchange: tell a and b about each other (when both have a UDP endpoint).
    void signal_pair(Room& r, Client& a, Client& b) {
        if (!a.udp_port || !b.udp_port) return;
        Json ja, jb;
        ja["room_id"] = jb["room_id"] = u64_str(r.id);
        ja["member_id"] = int(b.member), ja["ip"] = b.udp_ip, ja["port"] = int(b.udp_port);
        jb["member_id"] = int(a.member), jb["ip"] = a.udp_ip, jb["port"] = int(a.udp_port);
        if (!b.local_ip.empty()) ja["local_ip"] = b.local_ip, ja["local_port"] = int(b.local_port);
        if (!a.local_ip.empty()) jb["local_ip"] = a.local_ip, jb["local_port"] = int(a.local_port);
        event(a, "signal", ja);
        event(b, "signal", jb);
    }
    void signal_all(Room& r, Client& c) {
        for (auto& [mid, cid] : r.members)
            if (cid != c.id)
                if (Client* o = by_id(cid)) signal_pair(r, c, *o);
    }

    void leave_room(Client& c, int cause) {
        auto it = rooms.find(c.room);
        c.room = 0;
        if (it == rooms.end()) return;
        Room& r = it->second;
        uint16_t mid = c.member;
        c.member = 0;
        r.members.erase(mid), r.joined.erase(mid), r.member_bin.erase(mid);
        if (r.members.empty()) {
            if (opt.verbose) std::fprintf(stderr, "LOBBYD room %llu destroyed (empty)\n", (unsigned long long)r.id);
            rooms.erase(it);
            return;
        }
        Json sd;
        sd["room_id"] = u64_str(r.id), sd["member_id"] = int(mid);
        room_event(r, "signal_dead", sd);
        if (mid == r.owner) {
            uint16_t prev = r.owner;
            r.owner = r.members.begin()->first;  // lowest remaining member id
            Json oc;
            oc["room_id"] = u64_str(r.id), oc["prev"] = int(prev), oc["new"] = int(r.owner);
            room_event(r, "owner_changed", oc);
        }
        Json ml;
        ml["room_id"] = u64_str(r.id), ml["member_id"] = int(mid), ml["cause"] = cause;
        room_event(r, "member_left", ml);
    }

    int32_t join_room(Client& c, Room& r) {
        if (r.members.size() >= r.max_slot) return kErrFull;
        if (r.flag_attr & kFlagClosed) return kErrClosed;
        uint16_t mid = 0;
        for (uint16_t s = 1; s <= r.max_slot && !mid; ++s)
            if (!r.members.count(uint16_t(s << 4))) mid = uint16_t(s << 4);  // net.md 5.3: 0x10 * (slot + 1)
        if (!mid) return kErrFull;
        r.members[mid] = c.id;
        r.joined[mid] = now_ms();
        c.room = r.id, c.member = mid;
        return 0;
    }

    static bool int_match(int64_t v, int op, int64_t x) {
        switch (op) {
        case 1: return v == x;
        case 2: return v != x;
        case 3: return v < x;
        case 4: return v <= x;
        case 5: return v > x;
        case 6: return v >= x;
        default: return false;
        }
    }

    void handle(Client& c, const Json& j) {
        const std::string& op = j.get("op").as_string();
        if (op == "hello") {
            if (j.get("proto").as_int() != kLobbyProtocol) return reply(c, j, Json::object(), kErrProto);
            if (c.hello) return reply(c, j, Json::object(), kErrBadRequest);  // one identity per connection
            std::string name = j.get("name").as_string().substr(0, 16);
            uint64_t acc = j.get("account_id").as_u64();
            bool verified = !j.get("secret").as_string().empty();  // absent / "" = unverified, as before
            if (verified) {  // identity (4.1): the first hello registers the account, later ones must match
                std::string sec = j.get("secret").as_string();
                for (char& ch : sec) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                bool hex = sec.size() == 64 && sec.find_first_not_of("0123456789abcdef") == std::string::npos;
                if (!hex || !acc) return reply(c, j, Json::object(), kErrBadRequest);
                std::string h = sha256_hex(sec);
                auto it = accounts.find(acc);
                if (it == accounts.end()) accounts[acc] = {h, name, new_friend_code()};
                else if (it->second.secret_hash != h) return reply(c, j, Json::object(), kErrAuth);
                accounts[acc].name = name;
            } else if (accounts.count(acc)) {  // a registered account id needs its secret (no impersonation)
                return reply(c, j, Json::object(), kErrAuth);
            }
            if (!acc) acc = (uint64_t(rng()) << 32) | rng();
            bool was_present = verified && present(acc);
            c.hello = true;
            c.name = name;
            c.account = acc;
            c.verified = verified;
            c.presence = j.get("presence").as_bool(true);
            c.udp_port = uint16_t(j.get("udp_port").as_int());
            c.udp_ip = j.get("udp_ip").as_string().empty() ? c.peer_ip : j.get("udp_ip").as_string();
            Json b;
            b["server_id"] = 1;
            b["account_id"] = u64_str(c.account);
            b["your_ip"] = c.peer_ip;
            b["worlds"] = worlds_json();
            b["verified"] = verified;
            if (verified) b["friend_code"] = friend_code_display(accounts[acc].code);
            reply(c, j, b);
            if (verified && c.presence && !was_present) presence_event(acc, true);
            return;
        }
        if (!c.hello) return reply(c, j, Json::object(), kErrNoHello);
        if (op == "worlds") {
            Json b;
            b["worlds"] = worlds_json();
            return reply(c, j, b);
        }
        if (op == "endpoint") {
            c.udp_port = uint16_t(j.get("port").as_int());
            if (!j.get("ip").as_string().empty()) c.udp_ip = j.get("ip").as_string();
            std::string lip = j.get("local_ip").as_string();
            uint16_t lport = uint16_t(j.get("local_port").as_int());
            in_addr la{};
            bool lan = inet_pton(AF_INET, lip.c_str(), &la) == 1 && lport && stun::is_rfc1918(la.s_addr);
            c.local_ip = lan ? lip : "", c.local_port = lan ? lport : 0;
            Json b;
            b["ip"] = c.udp_ip, b["port"] = int(c.udp_port);
            reply(c, j, b);
            if (auto it = rooms.find(c.room); it != rooms.end()) signal_all(it->second, c);
            return;
        }
        if (op == "set_user_info") {
            c.user_bin = attrs_from(j.get("bin_attrs"));
            return reply(c, j, Json::object());
        }
        if (op == "create_room") {
            if (c.room) return reply(c, j, Json::object(), kErrInRoom);
            Room r;
            r.id = next_room++;
            r.world = uint32_t(j.get("world_id").as_int(1));
            r.max_slot = uint16_t(std::min<int64_t>(std::max<int64_t>(j.get("max_slot").as_int(4), 1), 4));
            r.flag_attr = uint32_t(j.get("flag_attr").as_int()) & ~kFlagFull;
            for (const auto& a : j.get("int_attrs").elems()) r.int_attrs[int(a.get("id").as_int())] = a.get("value").as_int();
            r.bin_internal = attrs_from(j.get("bin_internal"));
            r.bin_external = attrs_from(j.get("bin_external"));
            r.code = new_code();
            Room& rr = rooms[r.id] = std::move(r);
            join_room(c, rr);
            rr.owner = c.member;
            rr.member_bin[c.member] = attrs_from(j.get("member_bin_internal"));
            Json b;
            b["room"] = room_internal(rr);
            b["member_id"] = int(c.member);
            return reply(c, j, b);
        }
        if (op == "search") {
            uint32_t world = uint32_t(j.get("world_id").as_int(1));
            uint32_t ff = uint32_t(j.get("flag_filter").as_int()), fa = uint32_t(j.get("flag_attr").as_int());
            const std::string& code = j.get("join_code").as_string();
            std::vector<Room*> hits;
            for (auto& [id, r] : rooms) {
                if (!code.empty() ? r.code != code : r.world != world) continue;
                if (r.members.size() >= r.max_slot) continue;  // full rooms are not listed (server policy)
                if ((room_flags(r) & ff) != (fa & ff)) continue;
                bool ok = true;
                for (const auto& f : j.get("filters").elems()) {
                    auto it = r.int_attrs.find(int(f.get("id").as_int()));
                    ok &= int_match(it == r.int_attrs.end() ? 0 : it->second, int(f.get("op").as_int()), f.get("value").as_int());
                }
                if (ok) hits.push_back(&r);
            }
            int64_t start = std::max<int64_t>(j.get("start").as_int(1), 1), max = std::min<int64_t>(j.get("max").as_int(20), 20);
            Json list = Json::array();
            for (int64_t i = start - 1; i < int64_t(hits.size()) && int64_t(list.size()) < max; ++i) list.push(room_external(*hits[size_t(i)]));
            Json b;
            b["total"] = int64_t(hits.size());
            b["start"] = start;
            b["rooms"] = list;
            return reply(c, j, b);
        }
        if (op == "join") {
            if (c.room) return reply(c, j, Json::object(), kErrInRoom);
            Room* r = nullptr;
            const std::string& code = j.get("join_code").as_string();
            for (auto& [id, rr] : rooms)
                if ((!code.empty() && rr.code == code) || (code.empty() && id == j.get("room_id").as_u64())) r = &rr;
            if (!r) return reply(c, j, Json::object(), kErrNoRoom);
            if (int32_t e = join_room(c, *r)) return reply(c, j, Json::object(), e);
            r->member_bin[c.member] = attrs_from(j.get("member_bin_internal"));
            Json b;
            b["room"] = room_internal(*r);
            b["member_id"] = int(c.member);
            reply(c, j, b);
            Json mj;
            mj["room_id"] = u64_str(r->id);
            mj["member"] = member_json(*r, c.member);
            room_event(*r, "member_joined", mj, c.id);
            signal_all(*r, c);
            return;
        }
        if (op == "leave") {
            if (!c.room || (j.has("room_id") && j.get("room_id").as_u64() != c.room)) return reply(c, j, Json::object(), kErrNoRoom);
            leave_room(c, 0);
            return reply(c, j, Json::object());
        }
        if (op == "kick") {
            auto it = rooms.find(c.room);
            if (it == rooms.end()) return reply(c, j, Json::object(), kErrNoRoom);
            if (it->second.owner != c.member) return reply(c, j, Json::object(), kErrNotOwner);
            uint16_t mid = uint16_t(j.get("member_id").as_int());
            auto m = it->second.members.find(mid);
            if (m == it->second.members.end() || mid == c.member) return reply(c, j, Json::object(), kErrBadRequest);
            Client* t = by_id(m->second);
            uint64_t rid = c.room;
            if (t) {
                Json k;
                k["room_id"] = u64_str(rid);
                event(*t, "kicked", k);
                leave_room(*t, 1);
            }
            return reply(c, j, Json::object());
        }
        if (op == "set_room_internal" || op == "set_room_external") {
            auto it = rooms.find(c.room);
            if (it == rooms.end() || (j.has("room_id") && j.get("room_id").as_u64() != c.room))
                return reply(c, j, Json::object(), kErrNoRoom);
            Room& r = it->second;
            if (op == "set_room_internal") {
                Attrs a = attrs_from(j.get("bin_internal"));
                attrs_merge(r.bin_internal, a);
                uint32_t ff = uint32_t(j.get("flag_filter").as_int());
                r.flag_attr = ((r.flag_attr & ~ff) | (uint32_t(j.get("flag_attr").as_int()) & ff)) & ~kFlagFull;
                reply(c, j, Json::object());
                Json u;
                u["room_id"] = u64_str(r.id);
                u["flag_attr"] = int64_t(room_flags(r));
                u["bin_internal"] = attrs_json(a);
                room_event(r, "room_internal_updated", u, c.id);
                return;
            }
            for (const auto& a : j.get("int_attrs").elems()) r.int_attrs[int(a.get("id").as_int())] = a.get("value").as_int();
            attrs_merge(r.bin_external, attrs_from(j.get("bin_external")));
            return reply(c, j, Json::object());
        }
        if (op == "set_presence") {  // 4.3: held per connection, never stored; cleared by "" or on disconnect
            if (!c.verified) return reply(c, j, Json::object(), kErrUnverified);
            std::string gd = j.get("game_data").as_string(), st = j.get("status").as_string();
            std::vector<uint8_t> tmp;
            if (!hex_decode(gd, &tmp) || tmp.size() > kMaxGameData || !hex_decode(st, &tmp) || tmp.size() > kMaxStatus)
                return reply(c, j, Json::object(), kErrBadRequest);
            for (char& ch : gd) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            for (char& ch : st) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            c.game_data = gd, c.status = gd.empty() ? "" : st;
            return reply(c, j, Json::object());
        }
        if (op == "bug_report") {  // 4.4: same limits as the Worker (rolling 24 h: 5 per account, 10 per IP, 300 total)
            if (!c.verified) return reply(c, j, Json::object(), kErrUnverified);
            std::string text = j.get("text").as_string();
            size_t a = text.find_first_not_of(" \t\r\n"), z = text.find_last_not_of(" \t\r\n");
            text = a == std::string::npos ? "" : text.substr(a, z - a + 1);
            size_t chars = 0;
            for (unsigned char ch : text) chars += (ch & 0xc0) != 0x80;
            if (text.empty() || chars > 256) return reply(c, j, Json::object(), kErrBadRequest);
            const int64_t now = now_ms(), since = now - 24 * 3600 * 1000;
            int by_acc = 0, by_ip = 0, total = 0;
            for (const Report& r : reports)
                if (r.ms > since) ++total, by_acc += r.account == c.account, by_ip += r.ip == c.peer_ip;
            int left = std::min({5 - by_acc, 10 - by_ip, 300 - total});
            Json b;
            if (left <= 0) {
                b["left"] = int64_t(0);
                return reply(c, j, b, kErrLimit);
            }
            reports.push_back({now, c.account, c.peer_ip});
            if (opt.verbose)
                std::fprintf(stderr, "LOBBYD bug report %lld from #%d %s: %s (log %zu chars)\n", (long long)next_report,
                             c.id, c.name.c_str(), text.c_str(), j.get("log").as_string().size());
            b["id"] = next_report++, b["left"] = int64_t(left - 1);
            return reply(c, j, b);
        }
        if (op == "friend_code" || op == "friend_request" || op == "friend_requests" || op == "friend_accept" ||
            op == "friend_decline" || op == "friend_remove" || op == "friend_list") {
            if (!c.verified) return reply(c, j, Json::object(), kErrUnverified);
            return handle_friend(c, op, j);
        }
        reply(c, j, Json::object(), kErrBadRequest);
    }

    Json worlds_json() {
        Json w = Json::array(), o;
        int members = 0;
        for (auto& [id, r] : rooms) members += int(r.members.size());
        o["world_id"] = 1;
        o["rooms"] = int64_t(rooms.size());
        o["members"] = members;
        w.push(o);
        return w;
    }

    void drop(Client& c) {
        if (c.room) leave_room(c, 2);
        bool was_present = c.hello && c.verified && c.presence;
        c.closing = true;  // no longer present, gets no more events
        if (was_present && !present(c.account)) presence_event(c.account, false);
        ::close(c.s);
        if (opt.verbose) std::fprintf(stderr, "LOBBYD #%d %s disconnected\n", c.id, c.name.c_str());
    }

    void on_data(Client& c, const char* p, int n) {
        if (!c.ws) {
            c.head.append(p, size_t(n));
            size_t e = c.head.find("\r\n\r\n");
            if (e == std::string::npos) {
                if (c.head.size() > 8192) c.closing = true;
                return;
            }
            std::string rest = c.head.substr(e + 4);
            std::string resp = ws::server_handshake_response(c.head.substr(0, e + 4));
            if (resp.empty()) {
                std::string r = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
                send(c.s, r.data(), r.size(), MSG_NOSIGNAL);
                c.closing = true;
                return;
            }
            send(c.s, resp.data(), resp.size(), MSG_NOSIGNAL);
            c.ws = true;
            if (rest.empty()) return;
            c.rd.feed(rest.data(), rest.size());
        } else {
            c.rd.feed(p, size_t(n));
        }
        ws::Opcode op;
        std::string payload;
        while (!c.closing && c.rd.next(&op, &payload)) {
            if (op == ws::kPing) {
                std::string f = ws::encode_frame(ws::kPong, payload, false);
                send(c.s, f.data(), f.size(), MSG_NOSIGNAL);
            } else if (op == ws::kClose) {
                c.closing = true;
            } else if (op == ws::kText) {
                log("<-", c, payload);
                Json j;
                if (!Json::parse(payload, &j) || !j.is_object()) continue;
                handle(c, j);
            }
        }
        if (c.rd.error()) c.closing = true;
    }
};

LobbyServer::LobbyServer(const LobbyServerOptions& o) : d_(new Impl) {
    d_->opt = o;
    ws::startup();
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);  // restart while old connections sit in TIME_WAIT
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(o.port);
    a.sin_addr.s_addr = htonl(o.loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 16) != 0) {
        ::close(s);
        return;
    }
    socklen_t len = sizeof a;
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    d_->ls = s;
    d_->port = ntohs(a.sin_port);
}

LobbyServer::~LobbyServer() {
    for (auto& [k, c] : d_->clients) ::close(c->s);
    if (d_->ls != -1) ::close(d_->ls);
    delete d_;
}

bool LobbyServer::listening() const { return d_->ls != -1; }
uint16_t LobbyServer::port() const { return d_->port; }

void LobbyServer::run(const std::atomic<bool>* stop) {
    Impl& d = *d_;
    while (!stop || !*stop) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(d.ls, &rf);
        int maxfd = d.ls;
        for (auto& [k, c] : d.clients) FD_SET(c->s, &rf), maxfd = std::max(maxfd, c->s);
        timeval tv{0, 20000};
        int r = select(maxfd + 1, &rf, nullptr, nullptr, &tv);
        if (r < 0) break;
        if (FD_ISSET(d.ls, &rf)) {
            sockaddr_in a{};
            socklen_t len = sizeof a;
            int s = accept4(d.ls, reinterpret_cast<sockaddr*>(&a), &len, SOCK_CLOEXEC);
            if (s >= 0 && s < FD_SETSIZE) {  // select() takes fds below FD_SETSIZE only
                int one = 1;
                setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                auto c = std::make_unique<Client>();
                c->id = d.next_client++;
                c->s = s;
                char ip[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
                c->peer_ip = ip;
                if (d.opt.verbose) std::fprintf(stderr, "LOBBYD #%d connected from %s\n", c->id, ip);
                d.clients[c->id] = std::move(c);
            } else if (s >= 0) ::close(s);
        }
        std::vector<int> ids;
        for (auto& [k, c] : d.clients) ids.push_back(k);
        for (int id : ids) {
            Client* c = d.by_id(id);
            if (!c || !FD_ISSET(c->s, &rf)) continue;
            char buf[16384];
            int n = int(recv(c->s, buf, sizeof buf, 0));
            if (n <= 0) c->closing = true;
            else d.on_data(*c, buf, n);
        }
        for (int id : ids) {
            Client* c = d.by_id(id);
            if (c && c->closing) {
                d.drop(*c);
                d.clients.erase(id);
            }
        }
    }
}

}  // namespace dcp::net
