// Lobby protocol test (no game): the mock server (net/lobby_server.h) in-process, two raw WebSocket clients walking the
// host/join flow of notes/map/lobby_protocol.md, then the game-side LobbyClient against the same server.
// $DCP_LOBBY_TEST_URL=ws://... or wss://... runs the same checks against an external lobby instead (the Cloudflare
// Worker under `wrangler dev`, server/lobby-worker); it must have no other clients.
#include <arpa/inet.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include "net/bug_report.h"
#include "net/friends.h"
#include "net/identity.h"
#include "net/json.h"
#include "net/lobby_client.h"
#include "net/lobby_server.h"
#include "net/online_check.h"
#include "net/ws.h"

using namespace dcp::net;
using Clock = std::chrono::steady_clock;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct TestClient {
    ws::Conn s;
    ws::Reader rd;
    std::vector<Json> inbox;
    int rid = 1;
    explicit TestClient(const std::string& url) {
        std::string err;
        if (!s.open(url, 5000, &err)) std::printf("connect failed: %s\n", err.c_str());
    }
    void close() { s.close(); }
    int send(const std::string& op, Json b) {
        b["op"] = op;
        b["rid"] = rid;
        s.send_all(ws::encode_frame(ws::kText, b.dump(), true));
        return rid++;
    }
    void pump(int ms) {
        auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < end) {
            if (!s.readable(2)) continue;
            char buf[8192];
            int n = s.recv(buf, sizeof buf);
            if (n <= 0) return;
            rd.feed(buf, size_t(n));
            ws::Opcode op;
            std::string p;
            while (rd.next(&op, &p)) {
                Json j;
                if (op == ws::kText && Json::parse(p, &j)) inbox.push_back(j);
            }
        }
    }
    // Response for rid (waits up to 2 s).
    Json reply(int r) {
        for (int i = 0; i < 200; ++i) {
            for (size_t k = 0; k < inbox.size(); ++k)
                if (inbox[k].get("rid").as_int() == r) {
                    Json j = inbox[k];
                    inbox.erase(inbox.begin() + long(k));
                    return j;
                }
            pump(10);
        }
        return Json();
    }
    Json event(const std::string& ev) {
        for (int i = 0; i < 200; ++i) {
            for (size_t k = 0; k < inbox.size(); ++k)
                if (inbox[k].get("ev").as_string() == ev) {
                    Json j = inbox[k];
                    inbox.erase(inbox.begin() + long(k));
                    return j;
                }
            pump(10);
        }
        return Json();
    }
};

static Json attr(int id, const std::string& hex) {
    Json o;
    o["id"] = id, o["data"] = hex;
    return o;
}
static Json iattr(int id, int64_t v) {
    Json o;
    o["id"] = id, o["value"] = v;
    return o;
}

// Friends (lobby_protocol.md 4.1 / 4.2) between three verified accounts. Ids and secrets are random per run, so reruns
// against the Worker (whose accounts persist) start from scratch.
static void test_friends(const std::string& url) {
    std::mt19937_64 rnd(std::random_device{}() ^ uint64_t(Clock::now().time_since_epoch().count()));
    auto secret = [&] {
        std::string s;
        char t[17];
        for (int i = 0; i < 4; ++i) std::snprintf(t, sizeof t, "%016llx", (unsigned long long)rnd()), s += t;
        return s;
    };
    Identity ia, ib, ic;
    for (Identity* p : {&ia, &ib, &ic}) p->ok = true, p->account_id = rnd() | 1, p->secret = secret();
    const std::string A = u64_str(ia.account_id), B = u64_str(ib.account_id), C = u64_str(ic.account_id);
    auto hello = [&](TestClient& t, const char* name, uint64_t acc, const std::string& sec) {
        Json h;
        h["proto"] = kLobbyProtocol, h["version"] = kGameNetVersion, h["name"] = name, h["account_id"] = u64_str(acc);
        h["udp_port"] = 40100;
        if (!sec.empty()) h["secret"] = sec;
        return t.reply(t.send("hello", h));
    };
    auto req = [](TestClient& t, const std::string& op, const char* k = nullptr, const std::string& v = "") {
        Json b = Json::object();
        if (k) b[k] = v;
        return t.reply(t.send(op, b));
    };
    auto err = [](const Json& r) { return uint32_t(r.get("err").as_int(-1)); };

    TestClient fa(url), fb(url), fc(url), fx(url);
    Json r = hello(fa, "FrA", ia.account_id, ia.secret);  // first verified hello registers the account
    const std::string code_a = r.get("friend_code").as_string();
    CHECK(err(r) == 0 && r.get("verified").as_bool() && code_a.size() == 9 && code_a[4] == '-');
    CHECK(friend_code_normalize(code_a).size() == 8);
    std::string code_b = hello(fb, "FrB", ib.account_id, ib.secret).get("friend_code").as_string();
    std::string code_c = hello(fc, "FrC", ic.account_id, ic.secret).get("friend_code").as_string();
    CHECK(code_b.size() == 9 && code_c.size() == 9 && code_b != code_a && code_c != code_b);
    CHECK(req(fa, "friend_code").get("friend_code").as_string() == code_a);

    // Known account + wrong secret: hello refused (and no hello state); a malformed secret is a bad request.
    CHECK(err(hello(fx, "Evil", ia.account_id, secret())) == 0x80550c07);
    CHECK(err(req(fx, "worlds")) == 0x80550c03);
    CHECK(err(hello(fx, "Evil", ia.account_id, "abc")) == 0x80550c02);
    // No secret: a registered account id is refused (nobody can pose as an account); an unknown one (old builds) works,
    // unverified, friend ops refused; a second hello on the same connection is a bad request.
    CHECK(err(hello(fx, "Evil", ib.account_id, "")) == 0x80550c07);
    r = hello(fx, "Old", rnd() | 1, "");
    CHECK(err(r) == 0 && !r.get("verified").as_bool() && !r.has("friend_code"));
    CHECK(err(hello(fx, "Old", rnd() | 1, "")) == 0x80550c02);
    CHECK(err(req(fx, "friend_list")) == 0x80550c08 && err(req(fx, "friend_request", "code", code_a)) == 0x80550c08);

    // Codes: malformed / own -> bad request, unknown -> not found.
    CHECK(err(req(fa, "friend_request", "code", "K7QF-2M9")) == 0x80550c02);
    CHECK(err(req(fa, "friend_request", "code", "O0O0-1I1I")) == 0x80550c02);
    CHECK(err(req(fa, "friend_request", "code", code_a)) == 0x80550c02);
    std::string unused = code_a == "ZZZZ-ZZZZ" ? "YYYY-YYYY" : "ZZZZ-ZZZZ";
    CHECK(err(req(fa, "friend_request", "code", unused)) == 0x80550c09);

    // A -> B (code in lower case without the dash): pending, B is told; repeating it changes nothing.
    std::string lc;
    for (char ch : code_b)
        if (ch != '-') lc += char(std::tolower(static_cast<unsigned char>(ch)));
    r = req(fa, "friend_request", "code", lc);
    CHECK(err(r) == 0 && r.get("state").as_string() == "pending" && r.get("account_id").as_string() == B &&
          r.get("name").as_string() == "FrB");
    Json ev = fb.event("friend_request");
    CHECK(ev.get("account_id").as_string() == A && ev.get("name").as_string() == "FrA");
    CHECK(req(fa, "friend_request", "code", code_b).get("state").as_string() == "pending");
    r = req(fa, "friend_requests");
    CHECK(r.get("incoming").size() == 0 && r.get("outgoing").size() == 1 &&
          r.get("outgoing").at(0).get("account_id").as_string() == B);
    r = req(fb, "friend_requests");
    CHECK(r.get("incoming").size() == 1 && r.get("incoming").at(0).get("name").as_string() == "FrA" &&
          r.get("incoming").at(0).get("ms").as_int() > 0);
    CHECK(req(fa, "friend_list").get("friends").size() == 0 && req(fb, "friend_list").get("friends").size() == 0);

    // B accepts: A is told, both list each other (online), the request is gone.
    r = req(fb, "friend_accept", "account_id", A);
    CHECK(err(r) == 0 && r.get("account_id").as_string() == A && r.get("online").as_bool());
    CHECK(fa.event("friend_added").get("account_id").as_string() == B);
    CHECK(err(req(fb, "friend_accept", "account_id", A)) == 0x80550c09);
    CHECK(req(fb, "friend_requests").get("incoming").size() == 0);
    r = req(fa, "friend_list");
    CHECK(r.get("friends").size() == 1 && r.get("friends").at(0).get("account_id").as_string() == B &&
          r.get("friends").at(0).get("name").as_string() == "FrB" && r.get("friends").at(0).get("online").as_bool() &&
          !r.get("friends").at(0).has("room_id"));
    CHECK(req(fb, "friend_list").get("friends").at(0).get("account_id").as_string() == A);
    CHECK(req(fa, "friend_request", "code", code_b).get("state").as_string() == "friends");

    // A friend's room shows up in the list (room_id + join_code).
    Json cr;
    cr["world_id"] = 1, cr["max_slot"] = 4, cr["flag_attr"] = int64_t(0x86000000u);
    r = fa.reply(fa.send("create_room", cr));
    const std::string room_code = r.get("room").get("join_code").as_string(), room = r.get("room").get("room_id").as_string();
    r = req(fb, "friend_list");
    CHECK(!room_code.empty() && r.get("friends").at(0).get("join_code").as_string() == room_code &&
          r.get("friends").at(0).get("room_id").as_string() == room);
    // Presence blob (4.3): A publishes its game data; B's list carries it (lower-case hex); bad hex / oversize /
    // unverified refused; "" clears it.
    const std::string blob(0xf0, 'a'), status = "4564";
    auto presence = [](TestClient& t, const std::string& gd, const std::string& st) {
        Json b;
        b["game_data"] = gd;
        if (!st.empty()) b["status"] = st;
        return t.reply(t.send("set_presence", b));
    };
    CHECK(err(presence(fx, blob, "")) == 0x80550c08);
    CHECK(err(presence(fa, "abc", "")) == 0x80550c02 && err(presence(fa, "zz", "")) == 0x80550c02);
    CHECK(err(presence(fa, std::string(0x102, '0'), "")) == 0x80550c02);
    CHECK(err(presence(fa, blob, std::string(0x182, '0'))) == 0x80550c02);
    CHECK(!req(fb, "friend_list").get("friends").at(0).has("game_data"));
    CHECK(err(presence(fa, std::string(0xf0, 'A'), status)) == 0);
    r = req(fb, "friend_list").get("friends").at(0);
    CHECK(r.get("game_data").as_string() == blob && r.get("status").as_string() == status &&
          r.get("room_id").as_string() == room);
    CHECK(err(presence(fa, "", "")) == 0);
    r = req(fb, "friend_list").get("friends").at(0);
    CHECK(!r.has("game_data") && !r.has("status"));
    CHECK(err(req(fa, "leave")) == 0);

    // C -> A, A declines (C's outgoing list empties; declining again: not found).
    CHECK(req(fc, "friend_request", "code", code_a).get("state").as_string() == "pending");
    CHECK(fa.event("friend_request").get("account_id").as_string() == C);
    CHECK(err(req(fa, "friend_decline", "account_id", C)) == 0);
    CHECK(req(fc, "friend_requests").get("outgoing").size() == 0 && req(fa, "friend_requests").get("incoming").size() == 0);
    CHECK(err(req(fa, "friend_decline", "account_id", C)) == 0x80550c09);
    // C -> A again, and A -> C by code before accepting: crossing requests make them friends at once.
    CHECK(req(fc, "friend_request", "code", code_a).get("state").as_string() == "pending");
    CHECK(fa.event("friend_request").get("account_id").as_string() == C);
    CHECK(req(fa, "friend_request", "code", code_c).get("state").as_string() == "friends");
    CHECK(fc.event("friend_added").get("account_id").as_string() == A);
    CHECK(req(fc, "friend_list").get("friends").size() == 1);

    // A removes B: both directions, B is told.
    CHECK(err(req(fa, "friend_remove", "account_id", B)) == 0);
    CHECK(fb.event("friend_removed").get("account_id").as_string() == A);
    CHECK(req(fb, "friend_list").get("friends").size() == 0 && req(fa, "friend_list").get("friends").size() == 1);
    CHECK(err(req(fa, "friend_remove", "account_id", B)) == 0);  // idempotent

    // Presence: C going away -> A gets friend_offline, the list keeps C (offline, last name), its blob is gone.
    CHECK(err(presence(fc, blob, "")) == 0);
    CHECK(req(fa, "friend_list").get("friends").at(0).get("game_data").as_string() == blob);
    fc.close();
    CHECK(fa.event("friend_offline").get("account_id").as_string() == C);
    r = req(fa, "friend_list");
    CHECK(r.get("friends").size() == 1 && !r.get("friends").at(0).get("online").as_bool() &&
          r.get("friends").at(0).get("name").as_string() == "FrC" && !r.get("friends").at(0).has("game_data"));

    // Launcher sessions (net/friends.h): B requests A by code, then A accepts in a second session. The sessions say
    // "presence": false, so they do not announce anyone online.
    FriendsState s = run_friends_session(url, "FrB", ib, {FriendsAction::Request, code_a});
    CHECK(s.ok && s.action_ok && s.code == code_b && s.outgoing.size() == 1 && s.friends.empty());
    CHECK(s.action_msg.find("FrA") != std::string::npos);
    CHECK(fa.event("friend_request").get("account_id").as_string() == B);
    s = run_friends_session(url, "FrA", ia, {FriendsAction::Accept, B});
    CHECK(s.ok && s.action_ok && s.code == code_a && s.incoming.empty() && s.friends.size() == 2);
    bool b_online = false;
    for (const FriendInfo& f : s.friends) b_online |= f.account_id == B && f.online && f.name == "FrB";
    CHECK(b_online);
    s = run_friends_session(url, "FrA", ia, {FriendsAction::Request, unused});
    CHECK(s.ok && !s.action_ok && s.action_msg.find("not found") != std::string::npos);
    Identity wrong = ia;
    wrong.secret = secret();
    s = run_friends_session(url, "FrA", wrong, {});
    CHECK(!s.ok && s.error.find("different secret") != std::string::npos);
    s = run_friends_session("ws://127.0.0.1:1/", "FrA", ia, {});
    CHECK(!s.ok && s.error.find("cannot connect") != std::string::npos);
    fa.close(), fb.close(), fx.close();
}

// Launcher bug reports (net/bug_report.h, lobby_protocol.md 4.4): verified only, 256-char cap, 5 per account a day.
static void test_bug_report(const std::string& url) {
    std::mt19937_64 rnd{uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())};
    Identity id;
    id.ok = true, id.account_id = rnd() | 1;
    for (int i = 0; i < 4; ++i) {
        char h[17];
        std::snprintf(h, sizeof h, "%016llx", (unsigned long long)rnd());
        id.secret += h;
    }
    BugReportResult r = send_bug_report(url, "Rep", id, std::string(257, 'x'), "", "test");
    CHECK(!r.ok && r.msg.find("too long") != std::string::npos);
    for (int i = 0; i < 5; ++i) {
        r = send_bug_report(url, "Rep", id, "bug " + std::to_string(i), "log line\n", "test");
        CHECK(r.ok && r.left == 4 - i);
    }
    r = send_bug_report(url, "Rep", id, "sixth", "", "test");
    CHECK(!r.ok && r.left == 0 && r.msg.find("limit") != std::string::npos);
    Identity none;
    none.error = "x";
    CHECK(!send_bug_report(url, "Rep", none, "t", "", "test").ok);
    const char* tmp = std::getenv("TMPDIR");
    std::string p = std::string(tmp && *tmp ? tmp : "/tmp") + "/dcp_test_bug_" + std::to_string(getpid()) + ".log";
    if (std::FILE* f = std::fopen(p.c_str(), "wb")) {
        std::fputs("first line\nsecond \xc3\xa9 line\nthird line\n", f);
        std::fclose(f);
    }
    CHECK(log_tail_text(p, 1000) == "first line\nsecond ?? line\nthird line\n");
    CHECK(log_tail_text(p, 14) == "third line\n");  // starts on a whole line
    ::unlink(p.c_str());

    // Redaction: secret, long hex, IPv4 (not loopback), others' account ids; ours and short numbers stay.
    Identity me;
    me.account_id = 1234567890123456789ull, me.secret = std::string(64, 'a');
    std::string red = redact_log("hello secret=" + me.secret + " peer 203.0.113.7:3658 lan 192.168.1.20 local "
                                 "127.0.0.1:8787 friend 9876543210987654321 me 1234567890123456789 v2.75.05 "
                                 "hash 0123456789abcdef0123456789abcdef frame 5600\n", me);
    CHECK(red == "hello secret=<secret> peer <ip> lan <ip> local 127.0.0.1:8787 friend <id> me 1234567890123456789 "
                 "v2.75.05 hash <hex> frame 5600\n");
    CHECK(lobby_url_safe_for_secret("wss://lobby.example/") && lobby_url_safe_for_secret("ws://127.0.0.1:8787/") &&
          lobby_url_safe_for_secret("ws://192.168.1.5:8787/") && !lobby_url_safe_for_secret("ws://lobby.example/") &&
          !lobby_url_safe_for_secret("ws://8.8.8.8/"));
}

// identity.ini: created once, read back unchanged, a malformed file is reported and left alone.
static void test_identity() {
    const char* tmp = std::getenv("TMPDIR");
    std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/dcp_test_identity_" + std::to_string(getpid()) + "/sub";
    Identity a = load_identity(dir);
    CHECK(a.ok && (a.account_id & 1) && a.secret.size() == 64);
    Identity b = load_identity(dir);
    CHECK(b.ok && b.account_id == a.account_id && b.secret == a.secret);
    {
        std::FILE* f = std::fopen(a.path.c_str(), "wb");
        std::fputs("account_id = 12\nsecret = xyz\n", f);
        std::fclose(f);
    }
    Identity c = load_identity(dir);
    CHECK(!c.ok && !c.error.empty());
    std::FILE* f = std::fopen(a.path.c_str(), "rb");
    char buf[64] = {};
    size_t n = f ? std::fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) std::fclose(f);
    CHECK(std::string(buf, n) == "account_id = 12\nsecret = xyz\n");  // not overwritten
    ::unlink(a.path.c_str());
    ::rmdir(dir.c_str());
    ::rmdir(dir.substr(0, dir.size() - 4).c_str());
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(friend_code_normalize(" k7qf-2m9x ") == "K7QF2M9X" && friend_code_display("K7QF2M9X") == "K7QF-2M9X");
}

int main() {
    const char* ext_url = std::getenv("DCP_LOBBY_TEST_URL");
    std::unique_ptr<LobbyServer> srv;
    std::atomic<bool> stop{false};
    std::thread th;
    std::string url;
    if (ext_url && *ext_url) {
        url = ext_url;
        std::printf("test_lobby: external lobby %s\n", url.c_str());
    } else {
        LobbyServerOptions o;
        o.port = 0;
        o.verbose = std::getenv("LOBBY_VERBOSE") != nullptr;
        srv.reset(new LobbyServer(o));
        CHECK(srv->listening());
        th = std::thread([&] { srv->run(&stop); });
        url = "ws://127.0.0.1:" + std::to_string(srv->port()) + "/";
    }

    TestClient a(url), b(url);
    Json h;
    h["proto"] = kLobbyProtocol, h["version"] = kGameNetVersion, h["name"] = "Host", h["account_id"] = "1001";
    h["udp_port"] = 40001;
    Json r = a.reply(a.send("hello", h));
    CHECK(r.get("err").as_int() == 0 && r.get("account_id").as_string() == "1001");
    CHECK(r.get("worlds").size() == 1 && r.get("worlds").at(0).get("world_id").as_int() == 1);
    h["name"] = "Guest", h["account_id"] = "1002", h["udp_port"] = 40002;
    r = b.reply(b.send("hello", h));
    // Locally the lobby sees loopback; a deployed one sees our public address, so take what it reports.
    const std::string my_ip = srv ? std::string("127.0.0.1") : r.get("your_ip").as_string();
    CHECK(r.get("err").as_int() == 0 && !my_ip.empty() && r.get("your_ip").as_string() == my_ip);
    Json bad;
    bad["proto"] = 99;
    TestClient c(url);
    CHECK(c.reply(c.send("hello", bad)).get("err").as_int() != 0);
    CHECK(c.reply(c.send("worlds", Json::object())).get("err").as_int() != 0);  // no hello

    // Host creates a room like the game (net.md 3.2): 8 int attrs, 0x53 packed with version 34 in bits 13:7.
    int64_t packed = (34 << 7) | (3 << 5);
    Json cr;
    cr["world_id"] = 1, cr["max_slot"] = 4, cr["flag_attr"] = int64_t(0x86000000u);
    Json ia = Json::array();
    for (int i = 0; i < 7; ++i) ia.push(iattr(0x4c + i, i == 0 ? 5 : 0));
    ia.push(iattr(0x53, packed));
    cr["int_attrs"] = ia;
    Json bi = Json::array(), be = Json::array();
    bi.push(attr(0x57, std::string(0x78 * 2, 'a')));
    be.push(attr(0x55, "c0ffee"));
    cr["bin_internal"] = bi, cr["bin_external"] = be;
    r = a.reply(a.send("create_room", cr));
    CHECK(r.get("err").as_int() == 0 && r.get("member_id").as_int() == 0x10);
    Json room = r.get("room");
    std::string room_id = room.get("room_id").as_string(), code = room.get("join_code").as_string();
    CHECK(!room_id.empty() && code.size() == 6 && room.get("owner").as_int() == 0x10);
    CHECK(room.get("members").size() == 1 && room.get("bin_internal").at(0).get("data").as_string().size() == 0xf0);

    // Search: version filter matches, a wrong one does not; join code finds it too.
    Json s;
    s["world_id"] = 1, s["start"] = 1, s["max"] = 20, s["flag_filter"] = 0, s["flag_attr"] = int64_t(0x64000000);
    Json f = Json::array(), fo;
    fo["id"] = 0x53, fo["op"] = 1, fo["value"] = packed;
    f.push(fo);
    s["filters"] = f;
    r = b.reply(b.send("search", s));
    CHECK(r.get("err").as_int() == 0 && r.get("rooms").size() == 1);
    Json ext = r.get("rooms").at(0);
    CHECK(ext.get("room_id").as_string() == room_id && ext.get("owner_name").as_string() == "Host");
    CHECK(ext.get("bin_external").at(0).get("data").as_string() == "c0ffee" && ext.get("cur_members").as_int() == 1);
    Json f2 = Json::array(), fo2 = fo;
    fo2["value"] = packed + (1 << 7);
    f2.push(fo2);
    s["filters"] = f2;
    CHECK(b.reply(b.send("search", s)).get("rooms").size() == 0);
    Json fl = Json::array(), lo;
    lo["id"] = 0x4c, lo["op"] = 6, lo["value"] = 5;  // GE
    fl.push(lo);
    s["filters"] = fl;
    CHECK(b.reply(b.send("search", s)).get("rooms").size() == 1);
    lo["op"] = 5;  // GT
    fl = Json::array();
    fl.push(lo);
    s["filters"] = fl;
    CHECK(b.reply(b.send("search", s)).get("rooms").size() == 0);
    Json sc;
    sc["join_code"] = code;
    CHECK(b.reply(b.send("search", sc)).get("rooms").size() == 1);

    // Join by id: member 0x20, host sees member_joined, both get each other's endpoint.
    Json j;
    j["room_id"] = room_id;
    r = b.reply(b.send("join", j));
    CHECK(r.get("err").as_int() == 0 && r.get("member_id").as_int() == 0x20 && r.get("room").get("members").size() == 2);
    Json ev = a.event("member_joined");
    CHECK(ev.get("member").get("member_id").as_int() == 0x20 && ev.get("member").get("name").as_string() == "Guest");
    Json sa = a.event("signal"), sb = b.event("signal");
    CHECK(sa.get("member_id").as_int() == 0x20 && sa.get("port").as_int() == 40002 && sa.get("ip").as_string() == my_ip);
    CHECK(sb.get("member_id").as_int() == 0x10 && sb.get("port").as_int() == 40001);
    Json ep;
    ep["port"] = 40011;  // endpoint update (real bound port) re-signals the room
    CHECK(a.reply(a.send("endpoint", ep)).get("port").as_int() == 40011);
    CHECK(b.event("signal").get("port").as_int() == 40011);
    CHECK(b.reply(b.send("join", j)).get("err").as_int() != 0);  // already in a room

    // Room data internal fans out to the other members.
    Json si;
    si["room_id"] = room_id, si["flag_filter"] = 0, si["flag_attr"] = 0;
    Json bi2 = Json::array();
    bi2.push(attr(0x57, std::string(0x78 * 2, 'b')));
    si["bin_internal"] = bi2;
    CHECK(a.reply(a.send("set_room_internal", si)).get("err").as_int() == 0);
    ev = b.event("room_internal_updated");
    CHECK(ev.get("bin_internal").at(0).get("data").as_string() == std::string(0xf0, 'b'));
    Json sx;
    sx["room_id"] = room_id;
    Json ia2 = Json::array();
    ia2.push(iattr(0x4c, 9));
    sx["int_attrs"] = ia2;
    CHECK(a.reply(a.send("set_room_external", sx)).get("err").as_int() == 0);
    Json sc2;
    sc2["join_code"] = code;
    r = c.reply(c.send("search", sc2));  // c never said hello: refused
    CHECK(r.get("err").as_int() != 0);

    // Friend ops need a verified account (hello with a secret, see the friends block below).
    CHECK(uint32_t(b.reply(b.send("friend_list", Json::object())).get("err").as_int()) == 0x80550c08);

    // Owner leaves: B becomes owner (0x20) and sees owner_changed + member_left; then B leaves and the room is gone.
    CHECK(a.reply(a.send("leave", j)).get("err").as_int() == 0);
    ev = b.event("owner_changed");
    CHECK(ev.get("prev").as_int() == 0x10 && ev.get("new").as_int() == 0x20);
    CHECK(b.event("member_left").get("member_id").as_int() == 0x10);
    CHECK(b.reply(b.send("leave", j)).get("err").as_int() == 0);
    sc["join_code"] = code;
    CHECK(a.reply(a.send("search", sc)).get("rooms").size() == 0);
    r = a.reply(a.send("join", j));
    CHECK(uint32_t(r.get("err").as_int()) == 0x80550d0f);

    // Disconnect while in a room = leave.
    r = a.reply(a.send("create_room", cr));
    std::string room2 = r.get("room").get("room_id").as_string();
    j["room_id"] = room2;
    CHECK(b.reply(b.send("join", j)).get("err").as_int() == 0);
    a.close();
    ev = b.event("owner_changed");
    CHECK(ev.get("new").as_int() == 0x20);

    // Game-side client (singleton; URL from DCP_LOBBY).
    setenv("DCP_LOBBY", url.c_str(), 1);
    LobbyClient& lc = LobbyClient::instance();
    Json lh;
    lh["proto"] = kLobbyProtocol, lh["name"] = "Third", lh["account_id"] = "1003";
    uint32_t rid = lc.request("hello", lh);
    uint32_t rid2 = lc.request("worlds", Json::object());
    LobbyMessage m;
    bool got1 = false, got2 = false;
    for (int i = 0; i < 300 && !(got1 && got2); ++i) {
        while (lc.poll(&m))
            if (m.kind == LobbyMessage::Response) {
                if (m.rid == rid) got1 = m.err == 0 && m.body.get("account_id").as_string() == "1003";
                if (m.rid == rid2) got2 = m.err == 0 && m.body.get("worlds").at(0).get("rooms").as_int() == 1;
            }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(got1 && got2 && lc.connected());

    // Launcher "Test connection" against the same lobby (STUN/UPnP off: no real network in this test).
    setenv("DCP_STUN", "0", 1);
    OnlineCheck oc = run_online_check(url, "Checker", false, 40050);
    CHECK(oc.lobby_ok && !oc.your_ip.empty() && !oc.stun_ok && !oc.upnp_tried);
    OnlineCheck down = run_online_check("ws://127.0.0.1:1/", "Checker", false, 40050);
    CHECK(!down.lobby_ok && down.verdict.find("unreachable") != std::string::npos);

    test_friends(url);
    test_bug_report(url);
    test_identity();

    // JSON round trip details used by the protocol (u64 ids as strings, escapes).
    Json x;
    CHECK(Json::parse("{\"a\":\"18446744073709551615\",\"b\":[1,-2,3.5],\"c\":\"q\\\"\\u00e9\"}", &x));
    CHECK(x.get("a").as_u64() == 18446744073709551615ull && x.get("b").at(1).as_int() == -2);
    CHECK(x.get("c").as_string() == "q\"\xc3\xa9");
    CHECK(!Json::parse("{\"a\":}", &x));

    stop = true;
    if (th.joinable()) th.join();
    std::printf(g_fail ? "test_lobby: %d failure(s)\n" : "test_lobby: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
