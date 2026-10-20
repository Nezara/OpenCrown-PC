# Lobby protocol (game <-> lobby), revision 1 (+ identity / friends / presence blob, 2026-10-17)

`proto` stays 1: identity, friends and the presence blob (4.1 - 4.3) only add optional `hello` fields, response fields, ops and events, so
old clients keep working unchanged against a new lobby (they send no secret: unverified, no friends) and new clients
detect an old lobby by the missing `verified` field in the `hello` response (the launcher then says the lobby needs an
update). The only op removed is the never-used `friend_add` (now `err 0x80550c02`).

The lobby only introduces players: rooms, search, join codes, friends, member lists, room events and the exchange of
each member's UDP endpoint. Game traffic never goes through it (direct P2P UDP, `port/src/net`, net.md section 9).
Reference implementation: `port/src/net/lobby_server.cpp` (mock server, `port/tools/dcp_lobby`, tested by
`port/tests/test_lobby.cpp`). Game side: `port/src/net/lobby_client.cpp` (transport) and `port/src/hle/matching2.cpp`
(maps libSceNpMatching2 calls/callbacks onto these messages). The Cloudflare Worker + Durable Object must implement
exactly this; when the two disagree, fix one and update this file.

## 1. Transport
- One WebSocket per game process, text frames, one JSON object per frame. URL from `$DCP_LOBBY` (default
  `ws://127.0.0.1:47834/`; the launcher will set it). The game connects lazily on its first lobby request, i.e. only
  when the player goes online (`sceNpMatching2ContextStart`). Offline play never connects.
- Client pings every 20 s (WebSocket ping); the server answers pongs and may ping too.
- Reconnect (1.1): if an established socket drops, the client reconnects silently for up to 25 s, sends a new `hello`
  and, when it was in a room, `resume`s its seat. Requests already sent when the link dropped fail with
  `err = 0x80550c01` (their answer is lost); requests made meanwhile wait and go out once the seat is back. Only when
  every try fails does the game get "room destroyed" + context "start over" (it then leaves online play on its own).
  A first connect that fails is not retried (lobby unreachable at launch: the game stays offline).
- The client also treats 45 s without any frame from the server as a drop (it pings every 20 s, the server pongs).
- 64-bit ids (room ids, account ids) are **decimal strings** (JSON numbers lose precision above 2^53). Binary
  attributes are lower-case **hex strings**. All other numbers are plain JSON integers.

### 1.1 Reconnect and seat grace
Game traffic is direct P2P, so a lobby-only blip need not end the match. When a socket that is in a room closes and
its connection is **verified** (4.1), the server keeps the member's seat for **30 s** instead of removing it: nothing is
sent to the others, and room events meant for that member (`member_joined`, `member_left`, `owner_changed`,
`room_internal_updated`, `signal_dead`; at most 32) are held for it. Within the grace the client sends `hello` on a new
socket, then `resume {room_id, member_id}`: the server checks the account, gives the seat to the new connection,
answers `{room: RoomInternal, member_id}` and then delivers the held events in order, as ordinary events. The client
then re-sends `endpoint`, which re-signals the room (peers keep their first endpoint, section 5).
If the grace runs out (or more than 32 events piled up), the member is removed as a disconnect (`signal_dead`,
`owner_changed`, `member_left` cause 2), exactly as before, and a late `resume` gets `0x80550d0f`. The client then
reports "room destroyed" to the game with the link up, so the game can go online again at its next stage.
If the old socket still looks open when `resume` arrives (half-open TCP), the new connection takes the seat and the old
socket is closed without a leave. Unverified connections have no grace (a close is a leave). An object restart keeps
the seats of members whose socket vanished, with a fresh grace.

## 2. Envelope
- Request (client -> server): `{"op": "<name>", "rid": <int>, ...}`. `rid` is chosen by the client, unique per
  connection; the server echoes it.
- Response: `{"rid": <int>, "err": <int>, ...}`. `err` 0 = success, otherwise a negative int32 (Matching2-style
  code, table in section 6). On error the other fields may be missing. Responses may arrive in any order relative to
  events but each request gets exactly one response.
- Event (server -> client, unsolicited): `{"ev": "<name>", ...}` (no `rid`).
- Unknown `op` -> `err 0x80550c02`. Requests before a successful `hello` -> `err 0x80550c03`.

## 3. Objects
- **Attr** (binary attribute): `{"id": <int>, "data": "<hex>"}`. Ids used by the game: 0x55 (85) room external
  (search result payload, <= 0x100 B), 0x57 (87) room internal (the 0x78-byte room info, net.md 3.4), 0x5f (95) user
  info (1 byte). Keep unknown ids as they are.
- **IntAttr**: `{"id": <int>, "value": <uint32>}`. The game sets ids 0x4c..0x53 (76..83); 0x53 packs the game
  protocol version 34 in bits 13:7 (net.md 3.4), so versions never see each other's rooms.
- **Member**: `{"member_id": <int>, "account_id": "<u64>", "name": "<online id, <= 16 chars>", "join_ms": <unix ms>,
  "nat": 2, "bin_internal": [Attr]}`. **member_id must be 0x10 * (slot + 1)**: 16, 32, 48, 64 (net.md 5.3: the game
  computes `slot = (memberId >> 4) - 1` and ignores anything outside 0x10..0x4f). Assign the lowest free one.
- **RoomInternal** (what members see): `{"room_id", "world_id", "server_id": 1, "max_slot", "flag_attr", "owner":
  <member_id>, "join_code", "members": [Member], "bin_internal": [Attr], "bin_external": [Attr], "int_attrs":
  [IntAttr]}`.
- **RoomExternal** (search results): `{"room_id", "world_id", "server_id": 1, "max_slot", "cur_members", "flag_attr",
  "join_code", "owner_account_id", "owner_name", "int_attrs": [IntAttr], "bin_external": [Attr]}`.
- `flag_attr`: PSN room flags as the game sends them (0x86000000 on create). The server adds 0x20000000 (FULL) while
  the room is full and honours 0x40000000 (CLOSED) on join. Bit meanings are PSN names, unverified on PS4.

## 4. Requests
| op | request fields | response fields | notes |
|---|---|---|---|
| `hello` | `proto` (1), `version` (34), `title` ("CUSA10454"), `name`, `account_id`, `udp_port`, optional `udp_ip`, optional `secret`, optional `presence` (default true) | `server_id`, `account_id`, `your_ip`, `worlds`, `verified`, `friend_code` (verified only) | must be first, once per connection (again -> 0x80550c02). `proto` mismatch -> 0x80550c04. `udp_ip` absent = use the address the connection comes from. `account_id` "0"/absent = server assigns one (unverified only). `secret` / `presence`: 4.1 |
| `worlds` | - | `worlds: [{world_id, rooms, members}]` | at least one world, id 1 |
| `set_user_info` | `bin_attrs: [Attr]` | - | stored per connection; Worker: <= 4 attrs of <= 0x20 B (4.5) |
| `endpoint` | `port`, optional `ip`, optional `local_ip` + `local_port` | `ip`, `port` | the client's real UDP endpoint once its P2P socket is bound (or after STUN/UPnP), plus its LAN address (RFC 1918 only, otherwise dropped and any earlier one cleared). Re-signals the current room (section 5). Worker: `ip` must be dotted IPv4 (else ignored) and is only taken when public and equal to / in the same /16 as the connection's address, else that address is used (4.5); the response says which |
| `create_room` | `world_id`, `max_slot` (<= 4), `flag_attr`, `int_attrs`, `bin_internal`, `bin_external`, `member_bin_internal` | `room` (RoomInternal), `member_id` (16) | creator becomes owner. Error if already in a room (0x80550c06). Generates a 6-char `join_code` (A-Z 2-9 without I/O/0/1) |
| `search` | `world_id`, `start` (1-based), `max` (<= 20), `flag_filter`, `flag_attr`, `filters: [{id, op, value}]`, `attr_ids`, optional `join_code` | `total`, `start`, `rooms: [RoomExternal]` | see below |
| `join` | `room_id` or `join_code`, optional `member_bin_internal` | `room`, `member_id` | errors: not found 0x80550d0f, full 0x80550d19, closed 0x80550d15, already in a room 0x80550c06 |
| `leave` | `room_id` | - | |
| `resume` | `room_id`, `member_id` | `room`, `member_id` | after a reconnect (1.1): retake a seat held in grace. Verified connections only (0x80550c08); already in a room 0x80550c06; no such room / seat, other account or grace over 0x80550d0f. Held events follow the response |
| `set_room_internal` | `room_id`, `flag_filter`, `flag_attr`, `bin_internal` | - | merges attrs by id; flags: `(old & ~filter) | (attr & filter)`; others get `room_internal_updated` |
| `set_room_external` | `room_id`, `int_attrs`, `bin_external` | - | merges by id; no event |
| `kick` | `room_id`, `member_id` | - | owner only (0x80550c05); target gets `kicked`, the rest `member_left` (cause 1) |
| `friend_code` | - | `friend_code` | own code, `XXXX-XXXX` (same as in the `hello` response). All friend ops: verified connections only (else 0x80550c08), 4.2 |
| `friend_request` | `code` | `account_id`, `name`, `state` ("pending" / "friends") | 4.2 |
| `friend_requests` | - | `incoming: [Req]`, `outgoing: [Req]` | Req = `{account_id, name, ms}` (unix ms sent), oldest first |
| `friend_accept` | `account_id` | Friend (below) | accepts an incoming request; none -> 0x80550c09 |
| `friend_decline` | `account_id` | - | declines an incoming or withdraws an outgoing request; none -> 0x80550c09 |
| `friend_remove` | `account_id` | - | both directions + any pending request between the two; idempotent |
| `friend_list` | - | `friends: [Friend]` | Friend = `{account_id, name, online, room_id?, join_code?, game_data?, status?}`, ordered by account id (numeric); `room_id`/`join_code` only while the friend is online in a room; `game_data`/`status` only while it is online with a presence blob (4.3) |
| `set_presence` | `game_data` (hex, <= 0x80 B), optional `status` (hex, <= 0xc0 B) | - | the connection's presence blob (4.3); `game_data` "" clears both. Verified connections only (0x80550c08); bad hex / odd length / oversize -> 0x80550c02 |

### 4.1 Identity
- Each install has `<user dir>/identity.ini` (`port/src/net/identity.*`): `account_id` = random odd u64, `secret` = 32
  random bytes as 64 lower-case hex chars, both from the Windows CSPRNG, created on first use and never overwritten.
  The launcher and dcp_run read the same file (the launcher passes `DCP_USER_DIR`). Deleting it = a new account.
- `hello` with `secret`: the lobby stores only SHA-256 of the secret (SHA-256 over the 64 lower-case hex characters
  as text, hex). The first verified hello for an `account_id` **registers** it (and assigns its friend code); later
  hellos must carry a secret with the same hash, else `err 0x80550c07` and the connection stays without hello. A
  secret that is not 64 hex chars, or a secret with `account_id` "0", is `0x80550c02`. The response has
  `verified: true` and `friend_code`. Worker: at most 20 registrations per client IP (IPv6: per /64) in a rolling
  24 h; the 21st is `0x80550c0a` (no hello state; logging in to an existing account is not counted).
- `hello` without `secret` (old builds, or no identity.ini): unverified, as before (`verified: false`); friend ops are
  refused with `0x80550c08`. Claiming a **registered** account id without its secret is refused (`0x80550c07`), so
  nobody can appear under someone else's account id in rooms or presence.
- `presence: false` (launcher status sessions): the connection works normally but does not count as the account
  being online (no `friend_online`, not `online` in friends' lists). Default true (the game).
- `name` of the last verified hello is the account's name in friends' lists and requests (also while offline).

### 4.2 Friends
- **Friend code**: 8 characters of the join-code alphabet (A-Z 2-9 without I/O/0/1), unique per account, shown and
  returned as `XXXX-XXXX`. `friend_request.code` accepts any case, with or without the dash, spaces ignored; anything
  else (wrong length / characters) is `0x80550c02`, own code `0x80550c02`, unknown code `0x80550c09`.
- **Request** A -> B (`friend_request`): pending; B's verified connections get `friend_request {account_id, name}`.
  Repeating it changes nothing (`state: "pending"`). Already friends: `state: "friends"`. If B already asked A, A's
  request is an accept: both become friends at once (`state: "friends"`, B gets `friend_added`).
- **Accept** (`friend_accept`, by B): mutual friends; the request is gone; A's connections get `friend_added`
  (Friend of B). **Decline / withdraw** (`friend_decline`): the request is deleted, nobody is told.
  **Remove** (`friend_remove`): both directions; the other side gets `friend_removed {account_id}` if they were friends.
- **Presence**: an account is online while it has a verified connection with presence on; in a room, `room_id` and
  `join_code` come from that connection (one in a room wins over one that is not). The account's friends get
  `friend_online {account_id, name}` when it comes online and `friend_offline {account_id}` when its last such
  connection closes. Room changes have no event: clients re-read `friend_list`.
- **Limits**: 200 friends per account; 50 pending outgoing and 50 pending incoming requests per account
  (`0x80550c0a`, also when either side's friend list is full on accept). Mock: all in memory; Worker: SQLite.
  Worker: a request unanswered for 14 days is deleted (lazily, on the next friend op that touches requests).
- Launcher side: `port/src/net/friends.*` (one short session per refresh / button: hello with `presence: false`, the
  action, `friend_requests`, `friend_list`, close).

### 4.3 Presence blob (the game's Toolkit2 presence, notes/map/friends.md 5)
- The game publishes its own presence with Toolkit2 SetPresence: `game_data` = the 0x78-byte wire RoomInfo
  (`DcNet+0xe58`, carries the room id at wire 0x68 / struct +0x70, world id, host name), `status` = the 0xc0-byte field
  before it (`DcNet+0xd98`, binary, so hex too). The game shim (hle/np.cpp -> matching2.cpp) forwards both with
  `set_presence` on the game's own lobby connection; Toolkit2 "clear presence" sends `game_data: ""`.
- The lobby keeps them **per connection, in memory only** (mock: the Client; Worker: the socket attachment, never
  SQLite); a closed connection's blob is gone. Lower-case on the way out. `status` without `game_data` is dropped.
- Friends see it as `game_data` / `status` on the Friend object (`friend_list`, `friend_accept`, `friend_added`) and on
  `friend_online` when it is already set at that moment (normally not: the game sets it after creating its room).
  The blob comes from a present connection (verified, presence on) of that account that set one. Blob changes have no
  event: clients re-read `friend_list` (the game does, every time "Join Friends Room" opens its list).
- Game side: GetPresence (Toolkit2 `7zsee5IHLis`) = one `friend_list`; each friend that is online, in a room
  (`room_id`) and has `game_data` becomes one presence row; the game itself then joins with JoinRoomA(room id from the
  blob).

**Search semantics:** a room matches when `world_id` matches (or `join_code` matches, if given - then world is
ignored), it is not full, `(flags & flag_filter) == (flag_attr & flag_filter)` (the game sends filter 0, attr
0x64000000, so this keeps everything), and every filter holds: `op` 1 EQ, 2 NE, 3 LT, 4 LE, 5 GT, 6 GE on the room's
int attr `id` (missing attr = 0). Results in creation order, `start`/`max` window. `attr_ids` is what the game asked
for ({0x4c, 0x54, 0x55}); returning all int attrs + external bin attrs is fine. The game skips rooms whose owner
account is in its own block list and its own room.

### 4.4 Bug reports (launcher "Report a Bug" tab, net/bug_report.h)

`bug_report {text, log, version}` -> `{id, left}`. Verified connections only (`ERR_UNVERIFIED` otherwise); the launcher
sends it in a one-shot session after a verified `hello` with `"presence": false`. `text`: the user's note, trimmed,
1..256 characters (else `ERR_BAD_REQUEST`). `log`: the tail of `logs/last_run.log` (the launcher sends <= 128 KB,
non-ASCII bytes as `?`; the server keeps at most the last 256 K characters). `version`: free text, <= 64 chars
(control characters removed, like in names). Limits, rolling 24 h: 5 per account, 10 per client IP (IPv6: per /64;
new identities are free to make), 300 in total; over any of them -> `ERR_LIMIT` with `left: 0`. `left` = reports this
account can still send. The Worker keeps the newest 1000 reports (SQLite `bug_reports`) and serves them over HTTPS to
whoever holds the deployment's `REPORTS_TOKEN` (README "Bug reports", responses `X-Content-Type-Options: nosniff`);
the mock only counts them (text in its verbose log). The IP is stored only as HMAC-SHA256 under a random per-deployment
salt (SQLite `secrets.ip_salt`), for the per-IP limit, and blanked on rows older than 24 h at the next report.

### 4.5 Limits (Worker)
The public lobby caps every input. All caps sit well above what the game (via `hle/matching2.cpp`), the launcher and
`port/tests/test_lobby.cpp` send; a tripped limit answers `0x80550c0a` and leaves the socket open (the client only
reconnects after a drop). Malformed input stays `0x80550c02`.
- **Names**: `hello.name` loses control characters (< 0x20, 0x7f) and is cut to 16 characters (the game's online id).
- **Bin attrs** (room `bin_internal` / `bin_external`, `member_bin_internal`): data must be hex of whole bytes
  (`0x80550c02`; stored lower-case); <= 0x100 B per attr and <= 16 attrs per array, also after a `set_room_*` merge.
  The game sends one 0x78-B attr per array (0x55 / 0x57; PSN limits 0x100 / 0xff), the shim forwards <= 16.
- **Int attrs**: <= 32 per room (also after a merge), ids 0..0xffff (`0x80550c02` otherwise), values taken as uint32
  (`>>> 0`; the game's are u32, 0x53 packed e.g. 0x81102). The game sets 8.
- **search**: the first 16 `filters` are used, more are ignored (the game sends 3); filter values are uint32 too.
- **set_user_info**: <= 4 attrs of <= 0x20 B (kept with the socket, 2 KB in all; the game sends 0x5f, 1 B).
- **Rates per connection** (token buckets): 60 messages burst, refilled 20/s; `create_room` / `join` / `leave` /
  `search` also 30 burst, refilled 1/s. The launcher's friends refresh is 4 messages per short session every 30 s;
  the game does a room op per scene change / menu choice.
- **Sockets per client IP** (IPv6: per /64): 16 open; the next upgrade gets HTTP 429.
- **UDP endpoint** (`hello.udp_ip`, `endpoint.ip`): dotted IPv4 only; taken only when public (not 0/8, RFC 1918,
  100.64/10, loopback, 169.254/16, multicast / reserved) and equal to the connection's address or in its /16 (carrier
  NAT pools), else the connection's address is used. The port is taken as claimed.
- **LAN candidate**: `signal.local_ip` / `local_port` go only to members whose WebSocket comes from the same address.

## 5. Events
| ev | fields | when | game event (matching2.cpp) |
|---|---|---|---|
| `member_joined` | `room_id`, `member` | another member joined (not sent to the joiner) | room 0x8101 |
| `member_left` | `room_id`, `member_id`, `cause` (0 leave, 1 kicked, 2 disconnected) | a member left / dropped | room 0x8102 |
| `owner_changed` | `room_id`, `prev`, `new` | the owner left and members remain: new owner = lowest member id; sent before `member_left` | room 0x1105 |
| `room_destroyed` | `room_id`, `cause` | room closed by the server (the mock never sends it; empty rooms just vanish) | room 0x1104 |
| `kicked` | `room_id` | this client was kicked | room 0x1103 |
| `room_internal_updated` | `room_id`, `flag_attr`, `bin_internal` (only the changed attrs) | owner called `set_room_internal` (not echoed to the sender) | room 0x1106 |
| `signal` | `room_id`, `member_id`, `ip`, `port`, optional `local_ip` + `local_port` | peer endpoint: on join, to the joiner for every member and to every member for the joiner; again whenever someone sends `endpoint`. Worker: `local_*` only to a member connecting from the same public address (4.5) | signaling 0x5102 Established, once per peer (see below) |
| `signal_dead` | `room_id`, `member_id` | the peer left; sent before `member_left` | signaling 0x5101 Dead |
| `friend_request` | `account_id`, `name` | someone sent us a friend request (4.2) | - (launcher) |
| `friend_added` | Friend | our request was accepted / crossed | - |
| `friend_removed` | `account_id` | a friend removed us | - |
| `friend_online` / `friend_offline` | `account_id` (+ `name`, and `game_data` / `status` if already set, when online) | a friend's presence appeared / went away | - |

Friend events go to every verified connection of the account (presence on or off).

The client raises Established for a peer only when the game can use it: the signaling callback is registered (after the
create/join response), the game's P2P socket is bound (RudpMgr_OpenSocket) and the member is in its member table. It
then calls `net::set_peer_address(member_id, ip, port)` and the game immediately `sceRudpInitiate`s to that endpoint
(both sides do, simultaneous open). Same router: when the peer's `ip` equals our own public address and it sent a
LAN candidate, the client also registers `net::set_peer_alt` and the carrier sends its SYNs to both, keeping whichever
answers (net.md 11.1). The optional fields are backward compatible: older clients and lobbies ignore them. A later `signal` with a different endpoint after Established is logged and ignored
(the game would need a Dead + Established cycle; not done yet).

## 6. Error codes
| code | meaning | game reaction |
|---|---|---|
| 0x80550c01 | lobby unreachable / link dropped (client-generated) | generic -100 |
| 0x80550c02 | bad request / unknown op | generic -100 |
| 0x80550c03 | no hello yet | generic -100 |
| 0x80550c04 | lobby protocol revision mismatch | generic -100 |
| 0x80550c05 | not the room owner | generic -100 |
| 0x80550c06 | already in a room | generic -100 |
| 0x80550c07 | registered account id, wrong / missing secret (hello refused) | generic -100 |
| 0x80550c08 | friend op on an unverified connection | - |
| 0x80550c09 | no such friend code / pending request | - |
| 0x80550c0a | a limit: friend list / pending requests full, bug reports, Worker caps and rates (4.5) | generic -100 |
| 0x80550d0f | room not found | join error -6 (b9b46) |
| 0x80550d15 | room closed | join error -7 (b9b3b) |
| 0x80550d19 | room full | join error -8 (b9b34) |
The 0x80550c0x values are ours. The three 0x80550d.. values are the ones the game special-cases (asm b9b34..b9b4b);
their PSN names are unverified.

## 7. Example (two players, from the mock lobby log of the two-instance test)
```
C1 {"op":"hello","rid":1,"proto":1,"version":34,"title":"CUSA10454","name":"HostPlayer","account_id":"1082491943426768553","udp_port":40001}
S  {"rid":1,"err":0,"server_id":1,"account_id":"1082491943426768553","your_ip":"127.0.0.1","worlds":[{"world_id":1,"rooms":0,"members":0}]}
C1 {"op":"worlds","rid":2}            -> {"rid":2,"err":0,"worlds":[...]}
C1 {"op":"set_user_info","rid":3,"bin_attrs":[{"id":95,"data":"00"}]}
C1 {"op":"create_room","rid":4,"world_id":1,"max_slot":4,"flag_attr":2248146944,
    "int_attrs":[{"id":76,"value":99},...,{"id":83,"value":4352}],"bin_internal":[{"id":87,"data":"<0x78 B>"}],
    "bin_external":[{"id":85,"data":"<0x78 B>"}],"member_bin_internal":[]}
S  {"rid":4,"err":0,"member_id":16,"room":{"room_id":"4096","owner":16,"join_code":"9M2N96","members":[...],...}}
C2 hello / worlds / set_user_info, then
C2 {"op":"search","rid":4,"world_id":1,"start":1,"max":20,"flag_filter":0,"flag_attr":1677721600,
    "filters":[{"id":83,"op":1,"value":4352}],"attr_ids":[76,84,85]}
S  {"rid":4,"err":0,"total":1,"start":1,"rooms":[{"room_id":"4096","cur_members":1,"owner_name":"HostPlayer",...}]}
C2 {"op":"join","rid":5,"room_id":"4096"}
S->C2 {"rid":5,"err":0,"member_id":32,"room":{... "members":[{"member_id":16,...},{"member_id":32,...}]}}
S->C1 {"ev":"member_joined","room_id":"4096","member":{"member_id":32,"name":"JoinPlayer",...}}
S->C1 {"ev":"signal","room_id":"4096","member_id":32,"ip":"127.0.0.1","port":40002}
S->C2 {"ev":"signal","room_id":"4096","member_id":16,"ip":"127.0.0.1","port":40001}
```

## 8. Notes for the Cloudflare Worker + Durable Object
Implemented in `server/lobby-worker` (README there): one global SQLite-backed Durable Object with the WebSocket
Hibernation API; rooms in SQLite, per-connection state in the socket attachment, friends persisted per account.
Clients reach it as `wss://<name>.<subdomain>.workers.dev/` (LobbyClient TLS: `port/src/net/tls.*`).
- One Durable Object per world (or one global) holding rooms in memory; a room's state is small (<= 4 members, a few
  hundred bytes of attrs). WebSocket hibernation is fine: the only server timer is the seat grace (1.1, a Durable
  Object alarm); when it runs out the member is dropped like a `leave` with cause 2, including owner migration.
- `your_ip` / default endpoint IP: use `CF-Connecting-IP`. That is the public IP but **not** the public UDP port; the
  client must learn its mapped UDP port (STUN to stun.cloudflare.com from the P2P socket, or UPnP) and send `endpoint`
  before peers can connect. Until then `hello.udp_port` (the local port) is only right on a LAN / with UPnP.
- Hole punching: peers start sending to each other as soon as they get `signal`; for NATs that need simultaneous
  outbound packets this works because both sides get their `signal` at (nearly) the same time. No relay in revision 1.
- `account_id`: from identity.ini and verified by its secret (4.1); without identity.ini the client falls back to
  FNV-1a of its online name (unverified). The Worker keeps `accounts (account, secret_hash, code, name)`,
  `friends (account, friend)` (one row per direction) and `friend_requests (sender, target, ms)` in SQLite; presence
  is computed from the open sockets' attachments.
- Join codes are the launcher's "join a friend" path: `$DCP_JOIN_CODE` makes the game's room search return only that
  room (`search.join_code`), so the game's normal search+join flow joins it.
- Keep `rid` echo, string u64 ids and hex attrs exactly; the client ignores unknown fields, so adding fields is
  backward compatible. Bump `proto` for anything else.
