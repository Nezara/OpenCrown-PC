# Dragon's Crown Pro community lobby (Cloudflare Worker + Durable Object)

The online lobby for the PC port. It only **introduces** players: hello/login, worlds, rooms (create, search, join by
id or join code, leave, room data, kick), friends, member events and **signaling** (each member's public UDP
endpoint). Game traffic is direct P2P UDP between the players and never touches Cloudflare.

Protocol: `notes/map/lobby_protocol.md` (revision 1). Reference behaviour: the C++ mock `port/src/net/lobby_server.cpp`.
This Worker sends the same JSON (u64 ids as decimal strings, hex attrs, member ids 0x10..0x40, `rid` echo, error codes,
event order).

## Design

- `src/index.ts`: the Worker forwards every WebSocket upgrade to **one** Durable Object (`Lobby`, name `global`).
- That object holds the room index (search, join codes) **and** every room's state. One object means search, join,
  kick and owner migration are single-threaded and consistent, and no request ever costs a second (cross-object)
  billed request. A room is a few hundred bytes and a session is 2..4 players, so one object has plenty of headroom
  (thousands of concurrent sockets). If it ever needs to scale, shard by world id (`idFromName("world-N")`).
- **WebSocket Hibernation API** (`ctx.acceptWebSocket`): while players sit in a room playing P2P, the lobby is idle
  and the object is evicted from memory with no duration billed; the runtime answers WebSocket pings itself. Because
  of that, state must survive eviction:
  - rooms live in the object's **SQLite** storage (`rooms` table, one JSON row per room; `meta.next_room` counter),
  - per-connection state (name, account, UDP endpoint, current room/member) is the socket **attachment**,
  - accounts and friends (protocol 4.1 / 4.2): `accounts` (account id, SHA-256 of the secret, friend code, last
    name), `friend_links` (mutual, one row per direction), `friend_requests` (pending, sender/target/ms). The secret
    from identity.ini is checked on every verified `hello`; presence (online + room) is computed from the open
    sockets' attachments, never stored. The rev-1 one-way `friends` table is dropped on start.
  - messages and closes are handled strictly in order through one promise chain (hello awaits SHA-256, and clients
    pipeline requests right behind it).
- A closed socket is a `leave` with cause 2 (incl. owner migration). Members whose socket is gone after an object
  restart are dropped the same way when the object starts.
- `your_ip` and the default UDP endpoint IP come from `CF-Connecting-IP`. That is the public IP, not the public UDP
  port: the game must learn its mapped port (STUN / UPnP) and send `endpoint` (protocol section 8).

## Free plan

SQLite-backed Durable Objects are the only kind on the free plan (`new_sqlite_classes` in `wrangler.jsonc`). Rough
costs, all well inside the daily free allowances:
- one Worker request + one DO request per connection (the upgrade); incoming WebSocket messages are billed as DO
  requests at a 20:1 ratio. A player going online sends about 6..10 messages; nothing while playing.
- SQLite rows written: one per create/join/leave/room-data change/friend change (a 4-player session is ~10 rows).
- Duration: only while messages are being handled (hibernation); idle sockets are free.
No other resources (KV, D1, R2, queues) are used.

## Run locally

```
cd server/lobby-worker
npm ci
npm run dev           # ws://127.0.0.1:8787/   (wrangler dev --local; state in .wrangler/)
npm run dev:tls       # wss://127.0.0.1:8788/  (self-signed certificate)
npm test              # vitest unit tests inside workerd
npm run typecheck
```

Game-side tests against it (from the repo root, Git Bash, port built):

```
DCP_LOBBY_TEST_URL=ws://127.0.0.1:8787/ port/build/test_lobby.exe
DCP_LOBBY_TLS_INSECURE=1 DCP_LOBBY_TEST_URL=wss://127.0.0.1:8788/ port/build/test_lobby.exe
DCP_LOBBY=ws://127.0.0.1:8787/ port/tools/dcp_lobby/two_instances.sh <out_dir> Work/stage5/in_town_to_portal.txt
```

`test_lobby` expects an otherwise idle lobby. `DCP_LOBBY_TLS_INSECURE=1` skips certificate checks and is honoured only
for `127.0.0.1` / `localhost`.

## Deploy

Pick the Worker name first (`"name"` in `wrangler.jsonc`, now `opencrown-lobby`): it becomes part of the public URL.

```
cd server/lobby-worker
npx wrangler deploy
```

The first deploy creates the Worker and the `Lobby` Durable Object class (migration `v1`). Later deploys keep the
stored rooms/friends. Never delete or rename the `v1` migration; add a new tagged migration for class changes.

## Bug reports

The launcher's Report a Bug tab sends a short note plus the tail of the last game log through the lobby
(`bug_report`, lobby_protocol.md 4.4; verified accounts, 5 a day per account, 10 per IP, 300 in total; the newest 1000
are kept). Reading them needs a token, set once (any long random string):

```
npx wrangler secret put REPORTS_TOKEN
```

Then (PowerShell: use `curl.exe`):

```
curl -H "Authorization: Bearer <token>" https://<worker-url>/reports          # list, newest first (no logs)
curl -H "Authorization: Bearer <token>" https://<worker-url>/reports/12       # one report with its log, as text
curl -X DELETE -H "Authorization: Bearer <token>" https://<worker-url>/reports/12
```

Without the secret the `/reports` endpoints answer 403; sending reports works either way.

## URL for the launcher / game

```
DCP_LOBBY=wss://<name>.<your-subdomain>.workers.dev/
```

e.g. `wss://opencrown-lobby.example.workers.dev/` (the subdomain is the account's workers.dev subdomain shown by
`npx wrangler whoami` / the dashboard). Any path is accepted. The game's LobbyClient speaks wss:// via Windows SChannel
(`port/src/net/tls.*`) and validates the certificate against the Windows trust store.
