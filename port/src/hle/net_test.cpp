// Online test driver (debug only, $DCP_NET_TEST): pushes the game's own online code into host / join without the
// menus, for the two-instance milestone when the save has no online access yet. It only calls the game's entry points
// that its UI would reach (addresses from notes/map/net.md section 3 and the asm cited below); everything after that
// (sign-in machine, Matching2 requests, Rudp) is the game's code running against our shims.
//   DCP_NET_TEST=host|join|friend   DCP_NET_TEST_FRAME=<vblank to start, default 1200>
//   friend: hosts like `host` (going online always self-hosts, net.md 12), then once in its room does what "Join Friends
//   Room" does after its list window (notes/map/friends.md 4.4): DcNetIf slot 0x1b0 = FUN_000bade0(matching, filter)
//   (command 0xa -> Signin command 4 -> Toolkit2 GetPresence -> vector P), waits for FUN_000bc620(matching, id)
//   (slot 0x228 CmdFinished), reads the row count like slot 0x288 (FUN_0009c120: Signin vector P, 0x180-B rows), and
//   with a row joins it like slot 0x178 = FUN_000baae0(matching, 0) (command 6 -> mode 6: leave own room, JoinRoomA).
//   DCP_NET_TEST_FILTER = the round filter (default -1 = any; the menu passes its own, see friends.md 4.3).
// Steps (each from sceNpCheckCallback = render-thread tail, a guest thread, holding the object's own mutex):
//   1. DcNetSignin (game+0xb8) SetMode(1) via vtable +0xa8 (f860 -> 58da30): mode 1 = FUN_000bfe10, the sign-in
//      machine (c6b60 / c74f0 / c7050 / c71f0, ends with signin+0x10c = 2, +0x157c = 1 at c0099 / c003a).
//   2. wait for Signin_IsOnline FUN_000bece0 (also needs NetCtl state 3).
//   3. host: DcNet (game+0xb0) FUN_0009c5b0 sets the "host" bit (+0x210 |= bit 4); DcNet_Update -> FUN_0009a8c0 then
//      queues matching command 3 (FUN_000ba1c0) = mode 3 FUN_000b6b30: init, context, world, user info, CreateJoinRoom.
//      join: FUN_000ba6e0(matching, no conditions) = command 4 = mode 4 FUN_000b6cb0 (init + SearchRoom), then when the
//      browse list (matching+0x108..0x110, 0x178-B entries) is non-empty FUN_000baa20(matching, 0) = command 5 = mode 5
//      FUN_000b6e60 (JoinRoom by list index).
//   4. host: the game ends the online session on some scene transitions (title / data load -> inn: FUN_0053ce80 step
//      at 0x53cffe calls the online interface vtable 0xc62a10 slot +0x188 = thunk b4e60 -> FUN_0009c6c0, which sets
//      DcNet+0xedc, queues leave (command 7, FUN_000baba0) and command 2 (FUN_000ba620)). On PS4 the online menu is
//      only reachable in town, so that never hits a live room; here we start at a fixed vblank and may still be in the
//      title / prologue. While no member has joined, a room lost that way is re-hosted (steps 1-3 again) once the
//      matching machine is idle; the new room logs a new "created room ... code", which the launcher picks up.
#include <cstdlib>
#include <cstring>

#include "common.h"
#include "net/rudp.h"

namespace dcp::hle {

void net_test_tick();

namespace {

enum class Step { Off, Wait, SignIn, WaitSignIn, Act, WaitList, Join, FriendRefresh, FriendWait, FriendJoin, Done };
Step g_step = Step::Wait;
bool g_host = false, g_friend = false;
uint64_t g_cmd = 0, g_in_room_at = 0;
int32_t g_filter = -1;
uint64_t g_start = 1200, g_t = 0;
int g_logged_inroom = -1;
bool g_had_room = false, g_had_member = false, g_ac_done = false;

uint64_t eb(uint64_t off) { return config().eboot_base + off; }
template <class T> T rd(uint64_t a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof v);
    return v;
}

using Fn1 = DCP_SYSV uint64_t (*)(uint64_t);
using Fn2 = DCP_SYSV uint64_t (*)(uint64_t, uint64_t);
using Fn3 = DCP_SYSV uint64_t (*)(uint64_t, uint64_t, uint64_t);

uint64_t game() { return rd<uint64_t>(eb(0xc17758)); }  // DAT_00c17758, getter FUN_00016b10
void lock(uint64_t m) { reinterpret_cast<Fn1>(eb(0x63a690))(m); }    // eboot PLT: scePthreadMutexLock
void unlock(uint64_t m) { reinterpret_cast<Fn1>(eb(0x63a6a0))(m); }  // eboot PLT: scePthreadMutexUnlock

}  // namespace

void net_test_tick() {
    if (g_step == Step::Off || g_step == Step::Done) return;
    if (g_step == Step::Wait) {
        const char* m = std::getenv("DCP_NET_TEST");
        if (!m || (std::strcmp(m, "host") && std::strcmp(m, "join") && std::strcmp(m, "friend")) || !config().eboot_base) {
            g_step = Step::Off;
            return;
        }
        g_friend = !std::strcmp(m, "friend");
        g_host = g_friend || !std::strcmp(m, "host");
        if (const char* f = std::getenv("DCP_NET_TEST_FILTER")) g_filter = int32_t(std::strtol(f, nullptr, 0));
        if (const char* f = std::getenv("DCP_NET_TEST_FRAME")) g_start = std::strtoull(f, nullptr, 10);
        g_step = Step::SignIn;
        std::fprintf(stderr, "NETTEST %s: starting at vblank %llu\n", m, (unsigned long long)g_start);
    }
    uint64_t now = vblank_count();
    uint64_t g = game();
    if (now < g_start || !g) return;
    uint64_t dcnet = rd<uint64_t>(g + 0xb0), signin = rd<uint64_t>(g + 0xb8), matching = rd<uint64_t>(g + 0xc8);
    if (!dcnet || !signin || !matching) return;
    if (g_step == Step::WaitList || g_step == Step::Join || g_step == Step::FriendJoin) {
        if (now % 600 == 0) {
            char tag[48];
            std::snprintf(tag, sizeof tag, "vblank %llu", (unsigned long long)now);
            net::rudp::log_stats(tag);
        }
    }
    switch (g_step) {
    case Step::SignIn: {
        lock(signin + 0x220);
        uint64_t vt = rd<uint64_t>(signin);
        reinterpret_cast<Fn2>(rd<uint64_t>(vt + 0xa8))(signin, 1);  // SetMode(1)
        unlock(signin + 0x220);
        std::fprintf(stderr, "NETTEST vblank %llu: DcNetSignin SetMode(1)\n", (unsigned long long)now);
        g_step = Step::WaitSignIn, g_t = now;
        break;
    }
    case Step::WaitSignIn:
        if (reinterpret_cast<Fn1>(eb(0xbece0))(signin) & 0xff) {
            std::fprintf(stderr, "NETTEST vblank %llu: signed in (signin+0x10c = %d, mode %u)\n", (unsigned long long)now,
                         rd<int32_t>(signin + 0x10c), rd<uint8_t>(signin + 0x5b));
            g_step = Step::Act;
        } else if (now - g_t == 600) {
            std::fprintf(stderr, "NETTEST: still not signed in after 600 vblanks (signin mode %u step %u err %d)\n",
                         rd<uint8_t>(signin + 0x5b), rd<uint8_t>(signin + 0x5c), rd<int32_t>(signin + 0x25c));
        }
        break;
    case Step::Act:
        if (g_host) {
            // The quest-departure UI normally hands DcNet the room info (DcNetInfo vtable thunk b5170 ->
            // FUN_0009c3a0(dcnet, info)); without it the room is created with empty attrs and no 0x78-B info, so
            // nobody can find (0x53 version filter) or join it (b5c31 size check). Defaults from RoomInfo_Init
            // FUN_00098da0 (version 0x22 @0x58, save-derived @0x5a), then the fields a default search packs as 0
            // (@0x50, @0x52, @0x56; @0x54 = FUN_00098f00 like the search side) and the level cap @0x40 that the
            // joiner compares its own value against (b9a8f..b9a9a) set high.
            static uint8_t info[0x100];
            reinterpret_cast<Fn1>(eb(0x98da0))(reinterpret_cast<uint64_t>(info));
            uint16_t cap = 99, zero = 0, fa = uint16_t(reinterpret_cast<Fn1>(eb(0x98f00))(0));
            std::memcpy(info + 0x40, &cap, 2);
            std::memcpy(info + 0x50, &zero, 2);
            std::memcpy(info + 0x52, &zero, 2);
            std::memcpy(info + 0x54, &fa, 2);
            std::memcpy(info + 0x56, &zero, 2);
            lock(dcnet + 0x208);
            reinterpret_cast<Fn2>(eb(0x9c3a0))(dcnet, reinterpret_cast<uint64_t>(info));
            unlock(dcnet + 0x208);
            std::fprintf(stderr, "NETTEST vblank %llu: room info set (%llu bytes)\n", (unsigned long long)now,
                         (unsigned long long)reinterpret_cast<Fn1>(eb(0x9d550))(dcnet + 0xd98));
            reinterpret_cast<Fn1>(eb(0x9c5b0))(dcnet);  // DcNet: host bit
            std::fprintf(stderr, "NETTEST vblank %llu: DcNet host bit set (+0x210 = 0x%x)\n", (unsigned long long)now,
                         rd<uint32_t>(dcnet + 0x210));
            g_step = Step::WaitList;  // reuse: just watch the room state
        } else {
            lock(matching + 0x6b0);
            uint64_t id = reinterpret_cast<Fn3>(eb(0xba6e0))(matching, 0, 0);  // command 4: search, no conditions
            unlock(matching + 0x6b0);
            std::fprintf(stderr, "NETTEST vblank %llu: matching search command %llu\n", (unsigned long long)now,
                         (unsigned long long)id);
            g_step = Step::WaitList;
        }
        g_t = now;
        break;
    case Step::WaitList: {
        int inroom = int(reinterpret_cast<Fn1>(eb(0xb8700))(matching) & 0xff);
        if (inroom != g_logged_inroom) {
            std::fprintf(stderr, "NETTEST vblank %llu: in room %d (matching mode %u, err %d / 0x%x)\n", (unsigned long long)now,
                         inroom, rd<uint8_t>(matching + 0x5b), rd<int32_t>(matching + 0x5c4), rd<uint32_t>(matching + 0x510));
            g_logged_inroom = inroom;
        }
        if (g_host) {
            if (inroom) {
                if (!g_had_room) g_in_room_at = now;
                g_had_room = true;
                if (g_friend && now - g_in_room_at >= 300) {  // settled in our own room: look for friends' rooms
                    g_step = Step::FriendRefresh;
                    break;
                }
                if ((reinterpret_cast<Fn1>(eb(0xbbd40))(matching) & 0xffffffff) > 1) g_had_member = true;  // member count
                // Step 5: the same go-offline call also clears the host bit when the room survives it (inn -> town);
                // FUN_0009a8c0 then closes + hides the room (command 9 -> mode 9 b76f0: SetRoomDataInternal flags
                // 0x54000000, CLOSED|HIDDEN) and the lobby refuses joins (0x80550d15). With the bit set again it
                // reopens it (b9dd0 "closed" && info@0x40 > 0 -> command 8 -> mode 8 b7530, flags 0x04000000).
                uint32_t mask = rd<uint32_t>(eb(0xc15b20) + 0x10);
                if (!(rd<uint32_t>(dcnet + 0x210) & mask) && rd<uint8_t>(dcnet + 0xedc) == 0) {
                    reinterpret_cast<Fn1>(eb(0x9c5b0))(dcnet);
                    std::fprintf(stderr, "NETTEST vblank %llu: host bit was cleared in a scene change; set again "
                                 "(+0x210 = 0x%x)\n", (unsigned long long)now, rd<uint32_t>(dcnet + 0x210));
                }
            }
            if (!inroom && g_had_room && !g_had_member && rd<uint8_t>(matching + 0x5b) == 0 &&
                       rd<uint8_t>(dcnet + 0xedc) == 0) {
                // the game left the room on its own (scene change, see step 4) and DcNet finished its teardown
                // (FUN_0009a390 clears +0xedc): host again after a short settle.
                std::fprintf(stderr, "NETTEST vblank %llu: game ended the session before anyone joined; re-hosting\n",
                             (unsigned long long)now);
                g_had_room = false, g_logged_inroom = -1;
                g_start = now + 120;
                g_step = Step::SignIn;
            }
            break;
        }
        uint64_t b = rd<uint64_t>(matching + 0x108), e = rd<uint64_t>(matching + 0x110);
        if (e > b && rd<uint8_t>(matching + 0x5b) == 0) {
            lock(matching + 0x6b0);
            uint64_t id = reinterpret_cast<Fn2>(eb(0xbaa20))(matching, 0);  // command 5: join list entry 0
            unlock(matching + 0x6b0);
            std::fprintf(stderr, "NETTEST vblank %llu: %llu room(s) listed, join command %llu\n", (unsigned long long)now,
                         (unsigned long long)((e - b) / 0x178), (unsigned long long)id);
            g_step = Step::Join, g_t = now;
        } else if (rd<uint8_t>(matching + 0x5b) == 0 && now - g_t > 300 && (now - g_t) % 300 == 0) {
            lock(matching + 0x6b0);  // nothing found yet: search again
            reinterpret_cast<Fn3>(eb(0xba6e0))(matching, 0, 0);
            unlock(matching + 0x6b0);
            std::fprintf(stderr, "NETTEST vblank %llu: no rooms yet, searching again\n", (unsigned long long)now);
        }
        break;
    }
    case Step::Join: {
        int inroom = int(reinterpret_cast<Fn1>(eb(0xb8700))(matching) & 0xff);
        if (inroom != g_logged_inroom) {
            std::fprintf(stderr, "NETTEST vblank %llu: in room %d (matching mode %u, err %d / 0x%x)\n", (unsigned long long)now,
                         inroom, rd<uint8_t>(matching + 0x5b), rd<int32_t>(matching + 0x5c4), rd<uint32_t>(matching + 0x510));
            g_logged_inroom = inroom;
        }
        if (!inroom && rd<uint8_t>(matching + 0x5b) == 0 && now - g_t >= 300) {
            // join refused (room closed / full / gone): search again, the host may reopen or re-create it
            lock(matching + 0x6b0);
            reinterpret_cast<Fn3>(eb(0xba6e0))(matching, 0, 0);
            unlock(matching + 0x6b0);
            std::fprintf(stderr, "NETTEST vblank %llu: not in a room after the join; searching again\n",
                         (unsigned long long)now);
            g_step = Step::WaitList, g_t = now, g_logged_inroom = -1;
        }
        break;
    }
    case Step::FriendRefresh: {
        lock(matching + 0x6b0);
        g_cmd = reinterpret_cast<Fn2>(eb(0xbade0))(matching, uint64_t(uint32_t(g_filter)));  // slot 0x1b0, command 0xa
        unlock(matching + 0x6b0);
        std::fprintf(stderr, "NETTEST vblank %llu: refresh friend rooms (filter %d), command %llu\n",
                     (unsigned long long)now, g_filter, (unsigned long long)g_cmd);
        g_step = Step::FriendWait, g_t = now;
        break;
    }
    case Step::FriendWait: {
        if (!(reinterpret_cast<Fn2>(eb(0xbc620))(matching, g_cmd) & 0xff)) {
            if (now - g_t == 1200) std::fprintf(stderr, "NETTEST: friend refresh not finished after 1200 vblanks\n");
            break;
        }
        uint64_t b = rd<uint64_t>(signin + 0x80), e = rd<uint64_t>(signin + 0x88);
        size_t n = e > b ? size_t((e - b) / 0x180) : 0;
        std::fprintf(stderr, "NETTEST vblank %llu: friend refresh done, %zu friend room(s) (Signin+0xa94 = %d)\n",
                     (unsigned long long)now, n, rd<int32_t>(signin + 0xa94));
        for (size_t i = 0; i < n; ++i) {
            uint64_t r = b + i * 0x180;
            char id[17] = {};
            std::memcpy(id, reinterpret_cast<const void*>(r), 16);
            uint32_t len = rd<uint32_t>(r + 0x164);
            std::fprintf(stderr, "NETTEST   row %zu: %s account %llu state %u type %u data %u B, room id in blob %llu\n", i,
                         id, (unsigned long long)rd<uint64_t>(r + 0x170), rd<uint32_t>(r + 0x168), rd<uint32_t>(r + 0x178),
                         len, (unsigned long long)(len >= 0x6c ? rd<uint64_t>(r + 0xe4 + 0x64) : 0));
        }
        if (n) {
            lock(matching + 0x6b0);
            g_cmd = reinterpret_cast<Fn2>(eb(0xbaae0))(matching, 0);  // slot 0x178: join friend room 0 (command 6)
            unlock(matching + 0x6b0);
            std::fprintf(stderr, "NETTEST vblank %llu: join friend room 0, command %llu\n", (unsigned long long)now,
                         (unsigned long long)g_cmd);
            g_step = Step::FriendJoin, g_t = now, g_logged_inroom = -1;
        } else {
            g_step = Step::WaitList, g_in_room_at = now;  // try again in 300 vblanks (host may not be up yet)
        }
        break;
    }
    case Step::FriendJoin: {
        int inroom = int(reinterpret_cast<Fn1>(eb(0xb8700))(matching) & 0xff);
        uint32_t members = uint32_t(reinterpret_cast<Fn1>(eb(0xbbd40))(matching));
        bool done = reinterpret_cast<Fn2>(eb(0xbc620))(matching, g_cmd) & 0xff;
        int key = inroom * 16 + int(std::min<uint32_t>(members, 15)) + (done ? 256 : 0);
        if (key != g_logged_inroom) {
            std::fprintf(stderr, "NETTEST vblank %llu: friend join: in room %d, %u member(s), command %s (matching mode "
                         "%u, err %d / 0x%x)\n", (unsigned long long)now, inroom, members, done ? "finished" : "running",
                         rd<uint8_t>(matching + 0x5b), rd<int32_t>(matching + 0x5c4), rd<uint32_t>(matching + 0x510));
            g_logged_inroom = key;
        }
        // What the menu's FriendsRoomState does next (50fc90 -> FUN_00514380 first frame): FUN_000a0200(platform sys)
        // = if DcNetIf slot 0x128 (live link to the room owner) and sys mode != 3: sys SetMode(3) = the connect check
        // FUN_0009f550, which then puts the Act into AcceptConnect (load of the host's scene). Without this call the
        // headless joiner just idles in the room. DCP_NET_TEST_AC=0 skips it (the old driver behaviour). Called only
        // once the game's own first check (sys mode 2 = FUN_0009f1e0, query 0x3ec -> answer 0x3ed) has stored the
        // owner's slot at sys+0x168f4: earlier, 9f550 fails at once with error 0xc (slot ids 0xff) and leaves.
        if (done && inroom && members >= 2 && !g_ac_done) {
            const char* ac = std::getenv("DCP_NET_TEST_AC");
            uint64_t gsys = rd<uint64_t>(eb(0xc1dc40));
            uint64_t ps = gsys ? rd<uint64_t>(gsys + 0x78) : 0;
            if (ac && !std::strcmp(ac, "0")) {
                g_ac_done = true;
            } else if (ps && rd<int32_t>(ps + 0x168f4) != 0xff) {
                bool ok = reinterpret_cast<Fn1>(eb(0xa0200))(ps) & 0xff;
                if (ok || now - g_t > 900) {
                    std::fprintf(stderr, "NETTEST vblank %llu: EnterAcceptConnectIfGuest(a0200) -> %d (ps mode %u)\n",
                                 (unsigned long long)now, int(ok), rd<uint8_t>(ps + 0x5b));
                    g_ac_done = true;
                }
            }
        }
        break;
    }
    default: break;
    }
}

}  // namespace dcp::hle
