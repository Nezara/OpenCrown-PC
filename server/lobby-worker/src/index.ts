// Dragon's Crown Pro community lobby: Cloudflare Worker + one SQLite-backed Durable Object.
// Implements notes/map/lobby_protocol.md revision 1 exactly like the C++ mock (port/src/net/lobby_server.cpp).
// The lobby only introduces players (rooms, search, join codes, friends, member events, endpoint exchange); game
// traffic is direct P2P UDP and never reaches Cloudflare.
//
// Layout: every WebSocket goes to ONE Durable Object ("global"). It holds the room index (search, join codes) and all
// per-room state, so search/join/kick/owner migration are single-threaded and consistent with no cross-object calls
// (each cross-DO call would be another billed request). Connections use the WebSocket Hibernation API: an idle lobby
// (players sitting in a room, playing P2P) costs no duration; the runtime answers protocol pings without waking us.
// Because the object can be evicted while sockets stay open, rooms live in SQLite and per-connection state lives in
// the socket attachment; both survive hibernation.
//
// The lobby is public, so every input is capped (lobby_protocol.md 4.5): sizes and counts well above what the game,
// the launcher and port/tests/test_lobby.cpp send, message / room-op rate per connection, sockets and new accounts
// per client IP. A tripped limit answers ERR_LIMIT; the socket stays open.
//
// Seat grace (lobby_protocol.md 1.1): a verified member whose socket closes keeps its seat for AWAY_GRACE_MS (a DO
// alarm), with the room events meant for it held, so a client that reconnects and sends `resume` carries on as if
// nothing happened; its P2P links to the others never depended on us.

export interface Env {
  LOBBY: DurableObjectNamespace;
  REPORTS_TOKEN?: string; // `wrangler secret put REPORTS_TOKEN`: enables GET/DELETE /reports (bug reports, 4.4)
  SEAT_GRACE_MS?: string; // wrangler.jsonc "vars": seat grace in ms (1.1), default AWAY_GRACE_MS; "0" = PSN-like, a
  // closed socket leaves its room at once
}

export default {
  async fetch(req: Request, env: Env): Promise<Response> {
    const path = new URL(req.url).pathname;
    if (path === "/reports" || path.startsWith("/reports/")) {
      // Reviewing bug reports: only with the deployment's token (unset = disabled).
      const tok = env.REPORTS_TOKEN ?? "";
      const got = req.headers.get("Authorization") ?? "";
      if (!tok || got.length !== tok.length + 7 || !timingSafeEqual(got, `Bearer ${tok}`))
        return new Response("forbidden\n", { status: 403, headers: { "X-Content-Type-Options": "nosniff" } });
      return env.LOBBY.get(env.LOBBY.idFromName("global")).fetch(req);
    }
    if ((req.headers.get("Upgrade") ?? "").toLowerCase() !== "websocket")
      return new Response("Dragon's Crown Pro lobby: connect with a WebSocket (lobby protocol rev 1)\n", { status: 426 });
    return env.LOBBY.get(env.LOBBY.idFromName("global")).fetch(req);
  },
};

function timingSafeEqual(a: string, b: string): boolean {
  let d = a.length ^ b.length;
  for (let i = 0; i < Math.min(a.length, b.length); ++i) d |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return d === 0;
}

// ---- addresses ----
// Dotted-quad IPv4 (canonical decimal, <= 15 chars) -> its four octets, anything else -> null.
function ipv4(s: string): number[] | null {
  if (s.length > 15) return null;
  const m = /^(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\.(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\.(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\.(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)$/.exec(s);
  return m ? m.slice(1).map(Number) : null;
}
// LAN candidates (endpoint local_ip) must be RFC 1918: a client can never make peers send to an arbitrary host.
function isPrivateIpv4(s: string): boolean {
  const o = ipv4(s);
  if (!o) return false;
  const [a, b] = o;
  return a === 10 || (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168);
}
// A routable unicast address: not 0/8, private, CGNAT 100.64/10, loopback, link-local, multicast or reserved (224+).
function isPublicIpv4(s: string): boolean {
  const o = ipv4(s);
  if (!o) return false;
  const [a, b] = o;
  return !(a === 0 || a === 127 || a >= 224 || isPrivateIpv4(s) || (a === 100 && b >= 64 && b <= 127) ||
           (a === 169 && b === 254));
}
// The UDP address peers are told to send to. A client may claim one (hello udp_ip, endpoint ip: its STUN / UPnP
// mapping), but only its own: the address its WebSocket comes from, or one in the same /16 (carrier NATs map TCP and
// UDP from one pool). Anything else would let a client aim every peer's hole punching at a third party.
function trustedUdpIp(claim: string, peer: string): string {
  if (!isPublicIpv4(claim) || claim === peer) return peer;
  const a = ipv4(claim)!, b = ipv4(peer);
  return b && a[0] === b[0] && a[1] === b[1] ? claim : peer;
}
// Key for per-IP limits: an IPv4 address as it is; IPv6 by its /64 (one subscriber gets a whole /64 and can rotate
// through it freely).
function ipKey(ip: string): string {
  if (!ip.includes(":")) return ip;
  const s = ip.toLowerCase().split("%")[0];
  const groups = (x: string) => (x ? x.split(":").flatMap((g) => (g.includes(".") ? ["0", "0"] : [g])) : []);
  const k = s.indexOf("::");
  let g: string[];
  if (k < 0) g = groups(s);
  else {
    const head = groups(s.slice(0, k)), tail = groups(s.slice(k + 2));
    g = [...head, ...Array(Math.max(0, 8 - head.length - tail.length)).fill("0"), ...tail];
  }
  return g.slice(0, 4).map((x) => (parseInt(x, 16) || 0).toString(16)).join(":") + "::/64";
}

// ---- protocol constants (lobby_protocol.md section 6) ----
const PROTO = 1;
const ERR_BAD_REQUEST = 0x80550c02 | 0;
const ERR_NO_HELLO = 0x80550c03 | 0;
const ERR_PROTO = 0x80550c04 | 0;
const ERR_NOT_OWNER = 0x80550c05 | 0;
const ERR_IN_ROOM = 0x80550c06 | 0;
const ERR_AUTH = 0x80550c07 | 0; // registered account id, wrong / missing secret (hello refused)
const ERR_UNVERIFIED = 0x80550c08 | 0; // friend op on a connection without a verified account
const ERR_NOT_FOUND = 0x80550c09 | 0; // no such friend code / pending request
const ERR_LIMIT = 0x80550c0a | 0; // a limit tripped: friends / pending full, sizes, rates, new accounts (4.5)
const ERR_NO_ROOM = 0x80550d0f | 0;
const ERR_CLOSED = 0x80550d15 | 0;
const ERR_FULL = 0x80550d19 | 0;
const FLAG_FULL = 0x20000000;
const FLAG_CLOSED = 0x40000000;
const CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // join codes and friend codes (no I O 0 1)
const DAY_MS = 24 * 3600 * 1000;
const MAX_FRIENDS = 200; // per account
const MAX_PENDING = 50; // per account, incoming and outgoing separately
const FRIEND_REQUEST_TTL_MS = 14 * DAY_MS; // unanswered requests expire (deleted lazily on friend ops)
const MAX_GAME_DATA = 0x80; // set_presence limits in bytes (Toolkit2 SetPresence fields, lobby_protocol.md 4.3)
const MAX_STATUS = 0xc0;
const MAX_NAME = 16; // online id: the game keeps 16 chars (matching2.cpp member onlineId, 20 B with NUL + pad)
// Bug reports (lobby_protocol.md 4.4): verified accounts only; rolling 24 h limits per account, per IP (new identities
// are free to make) and in total, and only the newest MAX_REPORTS are kept, so storage stays bounded.
const MAX_REPORT_TEXT = 256; // characters
const MAX_REPORT_LOG = 256 * 1024; // characters; the tail is kept
const REPORTS_PER_ACCOUNT = 5;
const REPORTS_PER_IP = 10;
const REPORTS_PER_DAY = 300;
const MAX_REPORTS = 1000;
// Room attributes (lobby_protocol.md 4.5). The game sends bin attrs 0x55 / 0x57 of 0x78 B and 0x5f of 1 B (PSN's own
// limits: 0x55 <= 0x100, 0x57 <= 0xff, net.md 3.4), one attr per array; 8 int attrs 0x4c..0x53 with u32 values (0x53
// packed, e.g. 0x81102); 3 search conditions. The shim (hle/matching2.cpp) forwards at most 16 entries per array.
const MAX_ATTR_BYTES = 0x100; // per bin attr
const MAX_BIN_ATTRS = 16; // per array (request and stored room/member array)
const MAX_INT_ATTRS = 32; // per room, ids 0..0xffff
const MAX_FILTERS = 16; // search conditions; more are ignored
// set_user_info lands in the socket attachment (2 KB in all): the game sends one 1-byte attr (0x5f).
const MAX_USER_ATTRS = 4;
const MAX_USER_ATTR_BYTES = 0x20;
// Worst case of everything the attachment holds with the caps above is ~1.4 KB of JSON; the runtime refuses > 2048 B.
const MAX_ATTACHMENT = 1900;
// Abuse limits (4.5). Normal use: the game keeps one socket, the launcher opens a short one per friends refresh
// (every 30 s, 4 messages at once) / bug report / connection test; a room op every few seconds at most (re-hosting
// on scene changes = leave + create_room, a search per "Join Random Room").
const MAX_SOCKETS_PER_IP = 16; // open sockets per client IP (IPv6: per /64); more -> HTTP 429 on the upgrade
const MSG_BURST = 60; // per-connection token bucket over all messages
const MSG_PER_S = 20;
const ROOM_OP_BURST = 30; // and over create_room / join / leave / search
const ROOM_OP_PER_S = 1;
const NEW_ACCOUNTS_PER_IP = 20; // verified accounts registered per client IP, rolling 24 h
const ROOM_OPS = new Set(["create_room", "join", "leave", "search", "resume"]);
// Seat grace (1.1): longer than the client's 25 s of reconnect tries; at most MAX_HELD_EVENTS events held per seat.
// Env SEAT_GRACE_MS overrides it ("0" = off: a close is a leave, as before).
const AWAY_GRACE_MS = 30000;
const MAX_HELD_EVENTS = 32;
// Room chat (4.6): relayed to the room, never stored. Own bucket per connection, kept in memory only (a hibernation
// wake-up refills it, which is harmless at these rates).
const MAX_CHAT = 120; // characters (code points) after control characters are stripped
const CHAT_BURST = 5;
const CHAT_PER_S = 1;
const FRIEND_OPS = new Set(["friend_code", "friend_request", "friend_requests", "friend_accept", "friend_decline",
  "friend_remove", "friend_list"]);

// Friend codes (lobby_protocol.md 4.2): stored as 8 chars, shown XXXX-XXXX; input in any case, dashes/spaces ignored.
function friendCodeNormalize(s: string): string {
  const r = s.replace(/[-\s]/g, "").toUpperCase();
  if (r.length !== 8) return "";
  for (const ch of r) if (!CODE_ALPHABET.includes(ch)) return "";
  return r;
}
const friendCodeDisplay = (c: string) => (c.length === 8 ? `${c.slice(0, 4)}-${c.slice(4)}` : c);
const toHex = (b: ArrayBuffer | Uint8Array) =>
  [...new Uint8Array(b)].map((x) => x.toString(16).padStart(2, "0")).join("");
async function sha256Hex(s: string): Promise<string> {
  return toHex(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s)));
}
const byU64 = (x: string, y: string) => (BigInt(x) < BigInt(y) ? -1 : BigInt(x) > BigInt(y) ? 1 : 0);
// Names and versions end up in other players' UIs and in the report viewer: no control characters.
const clean = (s: string, max: number) => s.replace(/[\u0000-\u001f\u007f]/g, "").substring(0, max);

// Thrown when the attachment would outgrow its 2 KB; the request gets ERR_LIMIT and nothing is half-applied.
class LimitError extends Error {}

type Json = any; // eslint-disable-line @typescript-eslint/no-explicit-any
interface Attr {
  id: number;
  data: string;
}
// Per-connection state (WebSocket attachment, survives hibernation; must stay < 2 KB serialized, MAX_ATTACHMENT).
interface Conn {
  cid: string;
  hello: boolean;
  name: string;
  account: string; // u64 decimal
  verified?: boolean; // hello carried the account's secret (friend ops allowed; lobby_protocol.md 4.1)
  presence?: boolean; // hello "presence" (default true); false = launcher status session, not shown online
  game_data?: string; // set_presence (4.3): hex presence blob (the game's 0x78-B room info); kept with the socket only
  status?: string; // set_presence: hex 0xc0-B status field
  peer_ip: string; // CF-Connecting-IP
  udp_ip: string; // trustedUdpIp: peer_ip or the client's claim in the same /16
  udp_port: number;
  local_ip?: string; // LAN candidate (private IPv4) for players behind the same NAT; optional, see signalBody
  local_port?: number;
  user_bin: Attr[];
  room: string; // "" = none
  member: number;
  rl?: [number, number, number]; // rate limit: message tokens, room-op tokens, last refill (unix ms)
}
interface Member {
  cid: string;
  account: string;
  name: string;
  join_ms: number;
  bin: Attr[];
  // Seat held after the socket closed (1.1): until = unix ms when it is dropped, held = room events for it in order,
  // over = more than MAX_HELD_EVENTS arrived (resume refused).
  away?: { until: number; held: Json[]; over?: boolean };
}
interface Room {
  id: string;
  world: number;
  max_slot: number;
  flag_attr: number; // uint32, FULL never stored
  owner: number;
  code: string;
  members: Record<string, Member>; // member id -> member
  int_attrs: Record<string, number>; // attr id -> uint32 value
  bin_internal: Attr[];
  bin_external: Attr[];
}

// ---- JSON accessors with the mock's defaults (net/json.h: wrong type -> default) ----
const int = (v: Json, def = 0): number => (typeof v === "number" && isFinite(v) ? Math.trunc(v) : def);
const str = (v: Json): string => (typeof v === "string" ? v : "");
const arr = (v: Json): Json[] => (Array.isArray(v) ? v : []);
function u64(v: Json): bigint {
  try {
    if (typeof v === "string" && v.length) return BigInt.asUintN(64, BigInt(v.trim()));
    if (typeof v === "number" && isFinite(v) && v >= 0) return BigInt.asUintN(64, BigInt(Math.trunc(v)));
  } catch {
    /* not a number */
  }
  return 0n;
}
// Bin attrs from a request: ids u16, data hex of whole bytes (stored lower-case). Too many / too long -> ERR_LIMIT,
// malformed hex -> ERR_BAD_REQUEST (returned as a number instead of the array).
function attrsFrom(v: Json, maxN = MAX_BIN_ATTRS, maxBytes = MAX_ATTR_BYTES): Attr[] | number {
  const a = arr(v);
  if (a.length > maxN) return ERR_LIMIT;
  const out: Attr[] = [];
  for (const e of a) {
    const data = str(e?.data).toLowerCase();
    if (data.length > 2 * maxBytes) return ERR_LIMIT;
    if (!/^([0-9a-f]{2})*$/.test(data)) return ERR_BAD_REQUEST;
    out.push({ id: int(e?.id) & 0xffff, data });
  }
  return out;
}
// attrs merged into dst by id, as a new array; null when the result would hold more than MAX_BIN_ATTRS.
function attrsMerged(dst: Attr[], src: Attr[]): Attr[] | null {
  const out = dst.map((x) => ({ ...x }));
  for (const s of src) {
    const d = out.find((x) => x.id === s.id);
    if (d) d.data = s.data;
    else out.push({ ...s });
  }
  return out.length > MAX_BIN_ATTRS ? null : out;
}
// Int attrs from a request: ids 0..0xffff, values uint32 (the game's IntAttr value is a u32; >>> 0 like the shim).
function intAttrsFrom(v: Json): Record<string, number> | number {
  const a = arr(v);
  if (a.length > MAX_INT_ATTRS) return ERR_LIMIT;
  const out: Record<string, number> = {};
  for (const e of a) {
    const id = int(e?.id, -1);
    if (id < 0 || id > 0xffff) return ERR_BAD_REQUEST;
    out[id] = int(e?.value) >>> 0;
  }
  return out;
}
function intMatch(v: number, op: number, x: number): boolean {
  switch (op) {
    case 1: return v === x;
    case 2: return v !== x;
    case 3: return v < x;
    case 4: return v <= x;
    case 5: return v > x;
    case 6: return v >= x;
    default: return false;
  }
}
const mids = (r: Room): number[] => Object.keys(r.members).map(Number).sort((a, b) => a - b);

export class Lobby implements DurableObject {
  private sql: SqlStorage;
  private rooms = new Map<string, Room>();
  private chatRl = new Map<string, [number, number]>(); // cid -> chat tokens, last refill (unix ms)
  private ipSalt: string;
  private ipHmac: Promise<CryptoKey> | null = null;
  private graceMs: number;

  constructor(private ctx: DurableObjectState, env: Env) {
    const g = Number(env.SEAT_GRACE_MS ?? AWAY_GRACE_MS);
    this.graceMs = Number.isFinite(g) && g > 0 ? Math.min(g, 300000) : 0;
    this.sql = ctx.storage.sql;
    this.sql.exec("CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v INTEGER)");
    // Text values (meta.v is INTEGER: a digits-only text would be converted). ip_salt: the HMAC key of every stored
    // IP hash, random per deployment, so the hashes cannot be reversed by hashing all 2^32 IPv4 addresses.
    this.sql.exec("CREATE TABLE IF NOT EXISTS secrets (k TEXT PRIMARY KEY, v TEXT NOT NULL)");
    this.sql.exec("INSERT OR IGNORE INTO secrets (k, v) VALUES ('ip_salt', ?)", toHex(crypto.getRandomValues(new Uint8Array(32))));
    this.ipSalt = [...this.sql.exec("SELECT v FROM secrets WHERE k = 'ip_salt'")][0].v as string;
    this.sql.exec("CREATE TABLE IF NOT EXISTS rooms (id INTEGER PRIMARY KEY, data TEXT)");
    // Identity + friends (lobby_protocol.md 4.1 / 4.2). The rev-1 one-way `friends` table (friend_add, never used by
    // the game) is dropped: friendships are mutual now, one row per direction in friend_links.
    this.sql.exec("DROP TABLE IF EXISTS friends");
    this.sql.exec(
      "CREATE TABLE IF NOT EXISTS accounts (account TEXT PRIMARY KEY, secret_hash TEXT NOT NULL, code TEXT NOT NULL UNIQUE, " +
        "name TEXT NOT NULL, created_ms INTEGER NOT NULL) WITHOUT ROWID");
    this.sql.exec("CREATE TABLE IF NOT EXISTS friend_links (account TEXT, friend TEXT, PRIMARY KEY (account, friend)) WITHOUT ROWID");
    this.sql.exec(
      "CREATE TABLE IF NOT EXISTS friend_requests (sender TEXT, target TEXT, ms INTEGER NOT NULL, PRIMARY KEY (sender, target)) WITHOUT ROWID");
    this.sql.exec("CREATE INDEX IF NOT EXISTS friend_requests_target ON friend_requests (target)");
    this.sql.exec("CREATE INDEX IF NOT EXISTS friend_requests_ms ON friend_requests (ms)");
    // Registrations per client IP (NEW_ACCOUNTS_PER_IP); rows older than a day are deleted on the next insert.
    this.sql.exec("CREATE TABLE IF NOT EXISTS account_ips (ip_hash TEXT NOT NULL, ms INTEGER NOT NULL)");
    this.sql.exec("CREATE INDEX IF NOT EXISTS account_ips_ip ON account_ips (ip_hash, ms)");
    // Bug reports (4.4). ip_hash = ipHash() of the client IP, only for the per-IP limit; cleared after a day.
    this.sql.exec(
      "CREATE TABLE IF NOT EXISTS bug_reports (id INTEGER PRIMARY KEY AUTOINCREMENT, ms INTEGER NOT NULL, account TEXT NOT NULL, " +
        "name TEXT NOT NULL, ip_hash TEXT NOT NULL, version TEXT NOT NULL, text TEXT NOT NULL, log TEXT NOT NULL)");
    this.sql.exec("CREATE INDEX IF NOT EXISTS bug_reports_ms ON bug_reports (ms)");
    for (const row of this.sql.exec("SELECT data FROM rooms ORDER BY id")) {
      const r = JSON.parse(row.data as string) as Room;
      this.rooms.set(r.id, r);
    }
    // Members whose socket vanished while we were evicted (object restart) keep their seat for a fresh grace, like a
    // close (1.1): a restart usually drops every socket at once, and their clients reconnect within seconds.
    // Grace off: dropped like a disconnect at once.
    for (const r of [...this.rooms.values()])
      for (const mid of mids(r)) {
        const m = r.members[mid];
        if (this.conn(m.cid)) continue;
        if (!this.graceMs) this.removeMember(r, mid, 2);
        else if (!m.away) {
          m.away = { until: Date.now() + this.graceMs, held: [] };
          this.persist(r);
        }
      }
    this.scheduleAlarm();
  }

  // HMAC-SHA256(ip_salt, ipKey(ip)) as hex: what SQLite keeps instead of an address.
  private async ipHash(ip: string): Promise<string> {
    this.ipHmac ??= crypto.subtle.importKey("raw", new TextEncoder().encode(this.ipSalt), { name: "HMAC", hash: "SHA-256" },
                                            false, ["sign"]);
    return toHex(await crypto.subtle.sign("HMAC", await this.ipHmac, new TextEncoder().encode(ipKey(ip))));
  }

  // ---- connection plumbing ----
  async fetch(req: Request): Promise<Response> {
    const path = new URL(req.url).pathname;
    if (path === "/reports" || path.startsWith("/reports/")) { // token checked by the Worker
      const res = this.reportsHttp(req, path);
      res.headers.set("X-Content-Type-Options", "nosniff");
      return res;
    }
    const peer = req.headers.get("CF-Connecting-IP") ?? "127.0.0.1";
    const ipTag = `ip:${ipKey(peer)}`;
    let open = 0;
    for (const ws of this.ctx.getWebSockets(ipTag))
      if (ws.readyState <= 1 && (ws.deserializeAttachment() as Conn | null)?.cid) ++open;
    if (open >= MAX_SOCKETS_PER_IP) return new Response("too many connections from this address\n", { status: 429 });
    const pair = new WebSocketPair();
    const cid = crypto.randomUUID();
    this.ctx.acceptWebSocket(pair[1], [cid, ipTag]);
    const c: Conn = {
      cid, hello: false, name: "", account: "0", udp_ip: "", udp_port: 0, user_bin: [], room: "", member: 0,
      peer_ip: peer, rl: [MSG_BURST, ROOM_OP_BURST, Date.now()],
    };
    pair[1].serializeAttachment(c);
    return new Response(null, { status: 101, webSocket: pair[0] });
  }

  private conn(cid: string): { ws: WebSocket; c: Conn } | null {
    const ws = this.ctx.getWebSockets(cid)[0];
    if (!ws || ws.readyState > 1) return null;
    return { ws, c: ws.deserializeAttachment() as Conn };
  }
  private save(ws: WebSocket, c: Conn) {
    if (JSON.stringify(c).length > MAX_ATTACHMENT) throw new LimitError();
    ws.serializeAttachment(c);
  }
  private send(ws: WebSocket, j: Json) {
    try {
      ws.send(JSON.stringify(j));
    } catch {
      /* closing; webSocketClose cleans up */
    }
  }
  private event(ws: WebSocket, ev: string, body: Json) {
    this.send(ws, { ...body, ev });
  }
  private roomEvent(r: Room, ev: string, body: Json, exceptCid = "") {
    let held = false;
    for (const mid of mids(r)) {
      const m = r.members[mid];
      if (m.cid === exceptCid) continue;
      if (m.away) { // seat in grace: kept for its resume (1.1)
        if (m.away.held.length < MAX_HELD_EVENTS) m.away.held.push({ ...body, ev });
        else m.away.over = true;
        held = true;
        continue;
      }
      const o = this.conn(m.cid);
      if (o) this.event(o.ws, ev, body);
    }
    if (held && this.rooms.has(r.id)) this.persist(r);
  }

  // ---- persistence ----
  private persist(r: Room) {
    this.sql.exec("INSERT OR REPLACE INTO rooms (id, data) VALUES (?, ?)", Number(r.id), JSON.stringify(r));
  }
  private nextRoomId(): string {
    const row = [...this.sql.exec("SELECT v FROM meta WHERE k = 'next_room'")][0];
    const id = row ? Number(row.v) : 0x1000;
    this.sql.exec("INSERT OR REPLACE INTO meta (k, v) VALUES ('next_room', ?)", id + 1);
    return String(id);
  }

  // ---- room views (lobby_protocol.md section 3) ----
  private roomFlags(r: Room): number {
    return (r.flag_attr | (mids(r).length >= r.max_slot ? FLAG_FULL : 0)) >>> 0;
  }
  private memberJson(r: Room, mid: number): Json {
    const m = r.members[mid];
    return { member_id: mid, account_id: m.account, name: m.name, join_ms: m.join_ms, nat: 2, bin_internal: m.bin };
  }
  private intAttrsJson(r: Room): Json[] {
    return Object.keys(r.int_attrs).map(Number).sort((a, b) => a - b).map((id) => ({ id, value: r.int_attrs[id] }));
  }
  private roomInternal(r: Room): Json {
    return {
      room_id: r.id, world_id: r.world, server_id: 1, max_slot: r.max_slot, flag_attr: this.roomFlags(r),
      owner: r.owner, join_code: r.code, members: mids(r).map((m) => this.memberJson(r, m)),
      bin_internal: r.bin_internal, bin_external: r.bin_external, int_attrs: this.intAttrsJson(r),
    };
  }
  private roomExternal(r: Room): Json {
    const o = r.members[r.owner];
    return {
      room_id: r.id, world_id: r.world, server_id: 1, max_slot: r.max_slot, cur_members: mids(r).length,
      flag_attr: this.roomFlags(r), join_code: r.code, owner_account_id: o ? o.account : "0",
      owner_name: o ? o.name : "", int_attrs: this.intAttrsJson(r), bin_external: r.bin_external,
    };
  }
  private worldsJson(): Json[] {
    let members = 0;
    for (const r of this.rooms.values()) members += mids(r).length;
    return [{ world_id: 1, rooms: this.rooms.size, members }];
  }
  private newCode(): string {
    const used = new Set([...this.rooms.values()].map((r) => r.code));
    for (;;) {
      const b = crypto.getRandomValues(new Uint8Array(6));
      const s = [...b].map((x) => CODE_ALPHABET[x & 31]).join("");
      if (!used.has(s)) return s;
    }
  }

  // ---- signaling: tell a and b each other's UDP endpoint (when both have one) ----
  // ip/port is o's trusted public endpoint (trustedUdpIp). o's LAN candidate goes only to a peer whose WebSocket comes
  // from the same public address (same router): only then does the client use it (matching2.cpp, Established: peer ip
  // == our own public ip -> set_peer_alt; the public path needs NAT hairpinning there). Anyone else never learns
  // o's private address.
  private signalBody(r: Room, o: Conn, to: Conn): Json {
    const b: Json = { room_id: r.id, member_id: o.member, ip: o.udp_ip, port: o.udp_port };
    if (o.local_ip && o.local_port && o.peer_ip === to.peer_ip) b.local_ip = o.local_ip, b.local_port = o.local_port;
    return b;
  }
  private signalPair(r: Room, a: { ws: WebSocket; c: Conn }, b: { ws: WebSocket; c: Conn }) {
    if (!a.c.udp_port || !b.c.udp_port) return;
    this.event(a.ws, "signal", this.signalBody(r, b.c, a.c));
    this.event(b.ws, "signal", this.signalBody(r, a.c, b.c));
  }
  private signalAll(r: Room, me: { ws: WebSocket; c: Conn }) {
    for (const mid of mids(r)) {
      const cid = r.members[mid].cid;
      if (cid === me.c.cid) continue;
      const o = this.conn(cid);
      if (o) this.signalPair(r, me, o);
    }
  }

  // Removes member mid (its connection state is the caller's business): signal_dead, owner_changed, member_left.
  private removeMember(r: Room, mid: number, cause: number) {
    delete r.members[mid];
    if (mids(r).length === 0) {
      this.rooms.delete(r.id);
      this.sql.exec("DELETE FROM rooms WHERE id = ?", Number(r.id));
      return;
    }
    this.roomEvent(r, "signal_dead", { room_id: r.id, member_id: mid });
    if (mid === r.owner) {
      const prev = r.owner;
      r.owner = mids(r)[0]; // lowest remaining member id
      this.roomEvent(r, "owner_changed", { room_id: r.id, prev, new: r.owner });
    }
    this.roomEvent(r, "member_left", { room_id: r.id, member_id: mid, cause });
    this.persist(r);
  }
  private leaveRoom(ws: WebSocket, c: Conn, cause: number) {
    const r = this.rooms.get(c.room);
    const mid = c.member;
    c.room = "";
    c.member = 0;
    this.save(ws, c);
    if (r && r.members[mid]) this.removeMember(r, mid, cause);
  }
  // Seat grace (1.1): c's socket closed while in a room; the seat stays, nobody is told, events are held for it.
  private holdSeat(ws: WebSocket, c: Conn) {
    const r = this.rooms.get(c.room);
    const m = r?.members[c.member];
    c.room = "";
    c.member = 0;
    this.save(ws, c);
    if (!r || !m) return;
    m.away = { until: Date.now() + this.graceMs, held: [] };
    this.persist(r);
    this.scheduleAlarm();
  }
  // The alarm fires at the earliest seat grace end; none left = no alarm.
  private scheduleAlarm() {
    let next = Infinity;
    for (const r of this.rooms.values())
      for (const mid of mids(r)) {
        const a = r.members[mid].away;
        if (a) next = Math.min(next, a.until);
      }
    if (next === Infinity) void this.ctx.storage.deleteAlarm();
    else void this.ctx.storage.setAlarm(next);
  }
  async alarm() {
    return this.enqueue(() => {
      const now = Date.now();
      for (const r of [...this.rooms.values()])
        for (const mid of mids(r)) {
          const a = r.members[mid]?.away;
          if (a && a.until <= now) this.removeMember(r, mid, 2); // grace over: a disconnect after all
        }
      this.scheduleAlarm();
    });
  }
  // `resume` (1.1): a reconnected client retakes its seat; the held events follow the response.
  private resume(ws: WebSocket, c: Conn, j: Json, reply: (body: Json, err?: number) => void) {
    if (!c.verified) return reply({}, ERR_UNVERIFIED);
    if (c.room) return reply({}, ERR_IN_ROOM);
    const r = this.rooms.get(u64(j.room_id).toString());
    const mid = int(j.member_id) & 0xffff;
    const m = r?.members[mid];
    if (!r || !m || m.account !== c.account) return reply({}, ERR_NO_ROOM);
    if (m.away?.over) { // missed too much to carry on: drop it now
      this.removeMember(r, mid, 2);
      this.scheduleAlarm();
      return reply({}, ERR_NO_ROOM);
    }
    if (!m.away) { // the old socket still looks open (half-open TCP): the new one takes over, no leave
      const old = this.conn(m.cid);
      if (old && old.ws !== ws) {
        old.c.room = "";
        old.c.member = 0;
        this.save(old.ws, old.c);
        try {
          old.ws.close(1000, "resumed elsewhere");
        } catch {
          /* closed already */
        }
      }
    }
    const held = m.away?.held ?? [];
    c.room = r.id;
    c.member = mid;
    this.save(ws, c);
    m.cid = c.cid;
    m.name = c.name;
    delete m.away;
    this.persist(r);
    this.scheduleAlarm();
    reply({ room: this.roomInternal(r), member_id: mid });
    for (const e of held) this.send(ws, e);
  }
  // Puts c into r. The attachment is saved first: if that fails nothing changed, so no member without a connection
  // that knows it is in the room (a ghost nobody can remove until the object restarts).
  // The creator always gets in (a CLOSED flag on create only keeps others out).
  private joinRoom(ws: WebSocket, c: Conn, r: Room, bin: Attr[], creating = false): number {
    if (mids(r).length >= r.max_slot) return ERR_FULL;
    if (!creating && r.flag_attr & FLAG_CLOSED) return ERR_CLOSED;
    let mid = 0;
    for (let s = 1; s <= r.max_slot && !mid; ++s) if (!r.members[s << 4]) mid = s << 4; // 0x10 * (slot + 1)
    if (!mid) return ERR_FULL;
    c.room = r.id;
    c.member = mid;
    try {
      this.save(ws, c);
    } catch (e) {
      c.room = "";
      c.member = 0;
      throw e;
    }
    r.members[mid] = { cid: c.cid, account: c.account, name: c.name, join_ms: Date.now(), bin };
    return 0;
  }
  // joinRoom undone (persisting the room failed): member and attachment back to "not in a room".
  private unjoinRoom(ws: WebSocket, c: Conn, r: Room) {
    delete r.members[c.member];
    c.room = "";
    c.member = 0;
    this.save(ws, c);
  }

  // ---- hibernation handlers ----
  // Every message and close runs through one chain, in arrival order: hello awaits SHA-256 (crypto.subtle), and a
  // client may pipeline requests right behind it (the launcher's friends session does), so nothing may overtake it.
  private chain: Promise<void> = Promise.resolve();
  private enqueue(f: () => void | Promise<void>): Promise<void> {
    this.chain = this.chain.then(f).catch((e) => console.error("lobby handler", e));
    return this.chain;
  }
  async webSocketMessage(ws: WebSocket, msg: string | ArrayBuffer) {
    let j: Json = null;
    if (typeof msg === "string") {
      try {
        j = JSON.parse(msg);
      } catch {
        /* counted, not answered */
      }
    }
    if (!j || typeof j !== "object" || Array.isArray(j)) j = null;
    return this.enqueue(() => {
      const c = ws.deserializeAttachment() as Conn | null;
      if (!c || !c.cid) return; // connection already dropped
      const over = this.rateLimited(c, j ? str(j.op) : "");
      this.save(ws, c);
      if (!j) return;
      if (over) return this.send(ws, { rid: j.rid ?? null, err: ERR_LIMIT });
      return this.handle(ws, c, j);
    });
  }
  // Token buckets kept in the attachment (so they survive hibernation): every message costs one of MSG_BURST
  // (refilled MSG_PER_S per second), room ops also one of ROOM_OP_BURST (ROOM_OP_PER_S). Garbage frames count too.
  private rateLimited(c: Conn, op: string): boolean {
    const now = Date.now();
    const [t0, r0, last] = c.rl ?? [MSG_BURST, ROOM_OP_BURST, now];
    const dt = Math.max(0, now - last) / 1000;
    let t = Math.min(MSG_BURST, t0 + dt * MSG_PER_S), r = Math.min(ROOM_OP_BURST, r0 + dt * ROOM_OP_PER_S);
    let over = t < 1;
    if (!over) t -= 1;
    if (!over && ROOM_OPS.has(op)) {
      if (r < 1) over = true;
      else r -= 1;
    }
    c.rl = [Math.round(t * 1000) / 1000, Math.round(r * 1000) / 1000, now];
    return over;
  }
  async webSocketClose(ws: WebSocket, code: number, reason: string) {
    await this.enqueue(() => this.drop(ws));
    try {
      ws.close(code === 1005 || code === 1006 ? 1000 : code, reason);
    } catch {
      /* already closed */
    }
  }
  async webSocketError(ws: WebSocket) {
    return this.enqueue(() => this.drop(ws));
  }
  private drop(ws: WebSocket) {
    const c = ws.deserializeAttachment() as Conn | null;
    if (!c || c.cid === "") return; // dropped already (close after error)
    if (c.room) {
      if (c.verified && this.graceMs) this.holdSeat(ws, c); // 1.1: the client may resume within the grace
      else this.leaveRoom(ws, c, 2);
    }
    const wasPresent = c.hello && c.verified && c.presence !== false;
    this.chatRl.delete(c.cid);
    c.cid = ""; // marks the attachment as gone: no presence, no events
    this.save(ws, c);
    if (wasPresent && !this.present(c.account)) this.presenceEvent(c.account, false);
  }

  // ---- identity + friends (lobby_protocol.md 4.1 / 4.2) ----
  // Open, hello'd connections (attachment + socket), optionally only those of one account.
  private liveConns(account?: string): { ws: WebSocket; c: Conn }[] {
    const out: { ws: WebSocket; c: Conn }[] = [];
    for (const ws of this.ctx.getWebSockets()) {
      if (ws.readyState > 1) continue;
      const c = ws.deserializeAttachment() as Conn | null;
      if (c && c.cid && c.hello && (account === undefined || c.account === account)) out.push({ ws, c });
    }
    return out;
  }
  // The connection friends see for account a: verified, presence on; one in a room wins.
  private present(a: string, conns = this.liveConns(a)): Conn | null {
    let r: Conn | null = null;
    for (const { c } of conns)
      if (c.account === a && c.verified && c.presence !== false && (!r || (!r.room && c.room))) r = c;
    return r;
  }
  private accountEvent(a: string, ev: string, body: Json) {
    for (const o of this.liveConns(a)) if (o.c.verified) this.event(o.ws, ev, body);
  }
  private account(a: string): { secret_hash: string; code: string; name: string } | null {
    const row = [...this.sql.exec("SELECT secret_hash, code, name FROM accounts WHERE account = ?", a)][0];
    return row ? { secret_hash: row.secret_hash as string, code: row.code as string, name: row.name as string } : null;
  }
  private accountName(a: string): string {
    return this.account(a)?.name ?? "";
  }
  private newFriendCode(): string {
    for (;;) {
      const s = [...crypto.getRandomValues(new Uint8Array(8))].map((x) => CODE_ALPHABET[x & 31]).join("");
      if (![...this.sql.exec("SELECT 1 FROM accounts WHERE code = ?", s)].length) return s;
    }
  }
  private friendsOf(a: string): string[] {
    return [...this.sql.exec("SELECT friend FROM friend_links WHERE account = ?", a)].map((r) => r.friend as string);
  }
  private isFriend(a: string, b: string): boolean {
    return [...this.sql.exec("SELECT 1 FROM friend_links WHERE account = ? AND friend = ?", a, b)].length > 0;
  }
  private friendCount(a: string): number {
    return Number([...this.sql.exec("SELECT COUNT(*) AS n FROM friend_links WHERE account = ?", a)][0].n);
  }
  private pendingCount(a: string, incoming: boolean): number {
    const q = incoming ? "SELECT COUNT(*) AS n FROM friend_requests WHERE target = ?"
                       : "SELECT COUNT(*) AS n FROM friend_requests WHERE sender = ?";
    return Number([...this.sql.exec(q, a)][0].n);
  }
  private hasRequest(from: string, to: string): boolean {
    return [...this.sql.exec("SELECT 1 FROM friend_requests WHERE sender = ? AND target = ?", from, to)].length > 0;
  }
  private deleteRequest(from: string, to: string): boolean {
    const had = this.hasRequest(from, to);
    if (had) this.sql.exec("DELETE FROM friend_requests WHERE sender = ? AND target = ?", from, to);
    return had;
  }
  // The presence blob friends see for account a: from a present connection that set one (4.3).
  private putPresence(f: Json, a: string, conns = this.liveConns(a)) {
    for (const { c } of conns)
      if (c.account === a && c.verified && c.presence !== false && c.game_data) {
        f.game_data = c.game_data;
        if (c.status) f.status = c.status;
        return;
      }
  }
  private friendJson(a: string, conns?: { ws: WebSocket; c: Conn }[]): Json {
    const all = conns ?? this.liveConns(a);
    const o = this.present(a, all);
    const f: Json = { account_id: a, name: o ? o.name : this.accountName(a), online: !!o };
    if (o && o.room) {
      f.room_id = o.room;
      f.join_code = this.rooms.get(o.room)?.code ?? "";
    }
    this.putPresence(f, a, all);
    return f;
  }
  // friend_online / friend_offline to a's friends when a's presence appears / goes away.
  private presenceEvent(a: string, on: boolean) {
    const body: Json = { account_id: a };
    if (on) {
      body.name = this.accountName(a);
      this.putPresence(body, a);
    }
    const fr = new Set(this.friendsOf(a));
    if (!fr.size) return;
    for (const o of this.liveConns())
      if (o.c.verified && fr.has(o.c.account)) this.event(o.ws, on ? "friend_online" : "friend_offline", body);
  }
  // me and b become mutual friends (accept, or crossing requests); b's connections get friend_added.
  private makeFriends(me: string, b: string) {
    this.sql.exec("INSERT OR IGNORE INTO friend_links (account, friend) VALUES (?, ?), (?, ?)", me, b, b, me);
    this.deleteRequest(me, b);
    this.deleteRequest(b, me);
    this.accountEvent(b, "friend_added", this.friendJson(me));
  }
  private friendsFull(a: string, b: string): boolean {
    return this.friendCount(a) >= MAX_FRIENDS || this.friendCount(b) >= MAX_FRIENDS;
  }
  private pendingJson(me: string, incoming: boolean): Json[] {
    const q = incoming
      ? "SELECT r.sender AS a, r.ms AS ms, a.name AS name FROM friend_requests r LEFT JOIN accounts a ON a.account = r.sender WHERE r.target = ?"
      : "SELECT r.target AS a, r.ms AS ms, a.name AS name FROM friend_requests r LEFT JOIN accounts a ON a.account = r.target WHERE r.sender = ?";
    return [...this.sql.exec(q, me)]
      .map((r) => ({ account_id: r.a as string, name: (r.name as string) ?? "", ms: Number(r.ms) }))
      .sort((x, y) => x.ms - y.ms || byU64(x.account_id, y.account_id)); // oldest first
  }
  private handleFriend(c: Conn, op: string, j: Json, reply: (body: Json, err?: number) => void) {
    const me = c.account;
    if (op === "friend_code") return reply({ friend_code: friendCodeDisplay(this.account(me)?.code ?? "") });
    if (op === "friend_list") {
      const conns = this.liveConns();
      return reply({ friends: this.friendsOf(me).sort(byU64).map((f) => this.friendJson(f, conns)) });
    }
    // Every op below reads or writes pending requests: expire the stale ones first (indexed on ms; usually none).
    this.sql.exec("DELETE FROM friend_requests WHERE ms < ?", Date.now() - FRIEND_REQUEST_TTL_MS);
    if (op === "friend_request") {
      const code = friendCodeNormalize(str(j.code));
      if (!code) return reply({}, ERR_BAD_REQUEST);
      const row = [...this.sql.exec("SELECT account, name FROM accounts WHERE code = ?", code)][0];
      if (!row) return reply({}, ERR_NOT_FOUND);
      const t = row.account as string;
      if (t === me) return reply({}, ERR_BAD_REQUEST);
      const b: Json = { account_id: t, name: row.name as string, state: "friends" };
      if (this.isFriend(me, t)) return reply(b);
      if (this.hasRequest(t, me)) { // they asked us already: this is an accept
        if (this.friendsFull(me, t)) return reply({}, ERR_LIMIT);
        this.makeFriends(me, t);
        return reply(b);
      }
      b.state = "pending";
      if (this.hasRequest(me, t)) return reply(b); // sent before: unchanged
      if (this.friendCount(me) >= MAX_FRIENDS || this.pendingCount(me, false) >= MAX_PENDING ||
          this.pendingCount(t, true) >= MAX_PENDING)
        return reply({}, ERR_LIMIT);
      this.sql.exec("INSERT INTO friend_requests (sender, target, ms) VALUES (?, ?, ?)", me, t, Date.now());
      reply(b);
      this.accountEvent(t, "friend_request", { account_id: me, name: this.accountName(me) });
      return;
    }
    if (op === "friend_requests") return reply({ incoming: this.pendingJson(me, true), outgoing: this.pendingJson(me, false) });
    const a = u64(j.account_id).toString();
    if (op === "friend_accept") {
      if (!this.hasRequest(a, me)) return reply({}, ERR_NOT_FOUND);
      if (this.friendsFull(me, a)) return reply({}, ERR_LIMIT);
      this.makeFriends(me, a);
      return reply(this.friendJson(a));
    }
    if (op === "friend_decline") { // declines an incoming request or withdraws an outgoing one; no event
      if (!this.deleteRequest(a, me) && !this.deleteRequest(me, a)) return reply({}, ERR_NOT_FOUND);
      return reply({});
    }
    // friend_remove: both directions plus any pending request between the two; idempotent
    const was = this.isFriend(me, a);
    this.sql.exec("DELETE FROM friend_links WHERE (account = ? AND friend = ?) OR (account = ? AND friend = ?)", me, a, a, me);
    this.deleteRequest(me, a);
    this.deleteRequest(a, me);
    reply({});
    if (was) this.accountEvent(a, "friend_removed", { account_id: me });
  }

  private async hello(ws: WebSocket, c: Conn, j: Json, reply: (body: Json, err?: number) => void) {
    if (int(j.proto) !== PROTO) return reply({}, ERR_PROTO);
    if (c.hello) return reply({}, ERR_BAD_REQUEST); // one identity per connection
    const name = clean(str(j.name), MAX_NAME);
    let acc = u64(j.account_id);
    const secret = str(j.secret).toLowerCase();
    const verified = secret !== "";
    let code = "";
    if (verified) { // first verified hello registers the account, later ones must match (4.1)
      if (!/^[0-9a-f]{64}$/.test(secret) || !acc) return reply({}, ERR_BAD_REQUEST);
      const h = await sha256Hex(secret);
      const a = this.account(acc.toString());
      if (a && a.secret_hash !== h) return reply({}, ERR_AUTH);
      if (a) {
        code = a.code;
        this.sql.exec("UPDATE accounts SET name = ? WHERE account = ?", name, acc.toString());
      } else {
        // New identities are free to make (a new identity.ini), so registrations are capped per client IP.
        const now = Date.now(), ipHash = await this.ipHash(c.peer_ip);
        const n = Number([...this.sql.exec("SELECT COUNT(*) AS n FROM account_ips WHERE ip_hash = ? AND ms > ?",
                                           ipHash, now - DAY_MS)][0].n);
        if (n >= NEW_ACCOUNTS_PER_IP) return reply({}, ERR_LIMIT);
        code = this.newFriendCode();
        this.sql.exec("INSERT INTO accounts (account, secret_hash, code, name, created_ms) VALUES (?, ?, ?, ?, ?)",
                      acc.toString(), h, code, name, now);
        this.sql.exec("DELETE FROM account_ips WHERE ms <= ?", now - DAY_MS);
        this.sql.exec("INSERT INTO account_ips (ip_hash, ms) VALUES (?, ?)", ipHash, now);
      }
    } else if (acc && this.account(acc.toString())) {
      return reply({}, ERR_AUTH); // a registered account id needs its secret (no impersonation)
    }
    if (!acc) acc = BigInt.asUintN(64, crypto.getRandomValues(new BigUint64Array(1))[0] | 1n);
    const account = acc.toString();
    const wasPresent = verified && !!this.present(account);
    c.hello = true;
    c.name = name;
    c.account = account;
    c.verified = verified;
    c.presence = j.presence !== false;
    c.udp_port = int(j.udp_port) & 0xffff;
    c.udp_ip = trustedUdpIp(str(j.udp_ip), c.peer_ip);
    this.save(ws, c);
    const body: Json = { server_id: 1, account_id: c.account, your_ip: c.peer_ip, worlds: this.worldsJson(), verified };
    if (verified) body.friend_code = friendCodeDisplay(code);
    reply(body);
    if (verified && c.presence && !wasPresent) this.presenceEvent(account, true);
  }

  // ---- requests (lobby_protocol.md section 4) ----
  private async handle(ws: WebSocket, c: Conn, j: Json) {
    const reply = (body: Json, err = 0) => this.send(ws, { ...body, rid: j.rid ?? null, err });
    try {
      return await this.handleOp(ws, c, j, reply);
    } catch (e) {
      if (e instanceof LimitError) return reply({}, ERR_LIMIT);
      throw e;
    }
  }
  private async handleOp(ws: WebSocket, c: Conn, j: Json, reply: (body: Json, err?: number) => void) {
    const op = str(j.op);
    if (op === "hello") return this.hello(ws, c, j, reply);
    if (!c.hello) return reply({}, ERR_NO_HELLO);
    if (op === "bug_report") {
      if (!c.verified) return reply({}, ERR_UNVERIFIED);
      return this.bugReport(c, j, reply);
    }
    if (FRIEND_OPS.has(op)) {
      if (!c.verified) return reply({}, ERR_UNVERIFIED);
      return this.handleFriend(c, op, j, reply);
    }
    switch (op) {
      case "set_presence": { // 4.3: held in the socket attachment only (never in SQLite); cleared by "" or on close
        if (!c.verified) return reply({}, ERR_UNVERIFIED);
        const gd = str(j.game_data).toLowerCase(), st = str(j.status).toLowerCase();
        const hex = (x: string, max: number) => x.length <= 2 * max && /^([0-9a-f]{2})*$/.test(x);
        if (!hex(gd, MAX_GAME_DATA) || !hex(st, MAX_STATUS)) return reply({}, ERR_BAD_REQUEST);
        const old = { game_data: c.game_data, status: c.status };
        if (gd) {
          c.game_data = gd;
          if (st) c.status = st;
          else delete c.status;
        } else {
          delete c.game_data;
          delete c.status;
        }
        try {
          this.save(ws, c);
        } catch (e) {
          Object.assign(c, old);
          throw e;
        }
        return reply({});
      }
      case "worlds":
        return reply({ worlds: this.worldsJson() });
      case "endpoint": {
        c.udp_port = int(j.port) & 0xffff;
        if (ipv4(str(j.ip))) c.udp_ip = trustedUdpIp(str(j.ip), c.peer_ip); // malformed: ignored
        const lip = str(j.local_ip), lport = int(j.local_port) & 0xffff;
        if (isPrivateIpv4(lip) && lport) c.local_ip = lip, c.local_port = lport;
        else delete c.local_ip, delete c.local_port;
        this.save(ws, c);
        reply({ ip: c.udp_ip, port: c.udp_port });
        const r = this.rooms.get(c.room);
        if (r) this.signalAll(r, { ws, c });
        return;
      }
      case "set_user_info": {
        const a = attrsFrom(j.bin_attrs, MAX_USER_ATTRS, MAX_USER_ATTR_BYTES);
        if (typeof a === "number") return reply({}, a);
        const old = c.user_bin;
        c.user_bin = a;
        try {
          this.save(ws, c);
        } catch (e) {
          c.user_bin = old;
          throw e;
        }
        return reply({});
      }
      case "create_room": {
        if (c.room) return reply({}, ERR_IN_ROOM);
        const bi = attrsFrom(j.bin_internal), be = attrsFrom(j.bin_external), mb = attrsFrom(j.member_bin_internal);
        const ia = intAttrsFrom(j.int_attrs);
        for (const x of [bi, be, mb, ia]) if (typeof x === "number") return reply({}, x);
        const r: Room = {
          id: this.nextRoomId(), world: int(j.world_id, 1) >>> 0,
          max_slot: Math.min(Math.max(int(j.max_slot, 4), 1), 4),
          flag_attr: (int(j.flag_attr) & ~FLAG_FULL) >>> 0, owner: 0, code: this.newCode(), members: {},
          int_attrs: ia as Record<string, number>, bin_internal: bi as Attr[], bin_external: be as Attr[],
        };
        this.joinRoom(ws, c, r, mb as Attr[], true); // empty room, creator: cannot refuse (only throw)
        r.owner = c.member;
        try {
          this.persist(r);
        } catch (e) {
          this.unjoinRoom(ws, c, r);
          throw e;
        }
        this.rooms.set(r.id, r); // only once it is stored: search / join never see an unpersisted room
        return reply({ room: this.roomInternal(r), member_id: c.member });
      }
      case "search": {
        const world = int(j.world_id, 1) >>> 0;
        const ff = int(j.flag_filter) >>> 0, fa = int(j.flag_attr) >>> 0;
        const code = str(j.join_code);
        const filters = arr(j.filters).slice(0, MAX_FILTERS)
          .map((f) => ({ id: int(f?.id), op: int(f?.op), value: int(f?.value) >>> 0 }));
        const hits: Room[] = [];
        for (const r of this.rooms.values()) {
          if (code ? r.code !== code : r.world !== world) continue;
          if (mids(r).length >= r.max_slot) continue; // full rooms are not listed (server policy)
          if (((this.roomFlags(r) & ff) >>> 0) !== ((fa & ff) >>> 0)) continue;
          if (filters.every((f) => intMatch(r.int_attrs[f.id] ?? 0, f.op, f.value))) hits.push(r);
        }
        hits.sort((a, b) => Number(a.id) - Number(b.id)); // creation order
        const start = Math.max(int(j.start, 1), 1), max = Math.min(int(j.max, 20), 20);
        const rooms: Json[] = [];
        for (let i = start - 1; i < hits.length && rooms.length < max; ++i) rooms.push(this.roomExternal(hits[i]));
        return reply({ total: hits.length, start, rooms });
      }
      case "join": {
        if (c.room) return reply({}, ERR_IN_ROOM);
        const bin = attrsFrom(j.member_bin_internal);
        if (typeof bin === "number") return reply({}, bin);
        const code = str(j.join_code);
        let r: Room | undefined;
        if (code) r = [...this.rooms.values()].find((x) => x.code === code);
        else r = this.rooms.get(u64(j.room_id).toString());
        if (!r) return reply({}, ERR_NO_ROOM);
        const e = this.joinRoom(ws, c, r, bin);
        if (e) return reply({}, e);
        try {
          this.persist(r);
        } catch (err) {
          this.unjoinRoom(ws, c, r);
          throw err;
        }
        reply({ room: this.roomInternal(r), member_id: c.member });
        this.roomEvent(r, "member_joined", { room_id: r.id, member: this.memberJson(r, c.member) }, c.cid);
        this.signalAll(r, { ws, c });
        return;
      }
      case "resume":
        return this.resume(ws, c, j, reply);
      case "leave":
        if (!c.room || ("room_id" in j && u64(j.room_id).toString() !== c.room)) return reply({}, ERR_NO_ROOM);
        this.leaveRoom(ws, c, 0);
        return reply({});
      case "kick": {
        const r = this.rooms.get(c.room);
        if (!r) return reply({}, ERR_NO_ROOM);
        if (r.owner !== c.member) return reply({}, ERR_NOT_OWNER);
        const mid = int(j.member_id) & 0xffff;
        const m = r.members[mid];
        if (!m || mid === c.member) return reply({}, ERR_BAD_REQUEST);
        const t = this.conn(m.cid);
        if (t) {
          this.event(t.ws, "kicked", { room_id: r.id });
          this.leaveRoom(t.ws, t.c, 1);
        } else {
          this.removeMember(r, mid, 1);
        }
        return reply({});
      }
      case "chat":
        return this.chat(c, j, reply);
      case "set_room_internal":
      case "set_room_external": {
        const r = this.rooms.get(c.room);
        if (!r || ("room_id" in j && u64(j.room_id).toString() !== c.room)) return reply({}, ERR_NO_ROOM);
        if (op === "set_room_internal") {
          const a = attrsFrom(j.bin_internal);
          if (typeof a === "number") return reply({}, a);
          const merged = attrsMerged(r.bin_internal, a);
          if (!merged) return reply({}, ERR_LIMIT);
          r.bin_internal = merged;
          const ff = int(j.flag_filter) >>> 0;
          r.flag_attr = (((r.flag_attr & ~ff) | (int(j.flag_attr) & ff)) & ~FLAG_FULL) >>> 0;
          this.persist(r);
          reply({});
          this.roomEvent(r, "room_internal_updated", { room_id: r.id, flag_attr: this.roomFlags(r), bin_internal: a }, c.cid);
          return;
        }
        const ia = intAttrsFrom(j.int_attrs), be = attrsFrom(j.bin_external);
        if (typeof ia === "number") return reply({}, ia);
        if (typeof be === "number") return reply({}, be);
        const ints = { ...r.int_attrs, ...ia }, merged = attrsMerged(r.bin_external, be);
        if (!merged || Object.keys(ints).length > MAX_INT_ATTRS) return reply({}, ERR_LIMIT);
        r.int_attrs = ints;
        r.bin_external = merged;
        this.persist(r);
        return reply({});
      }
      default:
        return reply({}, ERR_BAD_REQUEST);
    }
  }

  // ---- room chat (lobby_protocol.md 4.6) ----
  // Verified members only (mutes are per account). Sent to every live member, the sender included (its echo is the
  // confirmation); never held for a seat in grace, so chat cannot push a reconnecting player over MAX_HELD_EVENTS.
  private chat(c: Conn, j: Json, reply: (body: Json, err?: number) => void) {
    if (!c.verified) return reply({}, ERR_UNVERIFIED);
    const r = this.rooms.get(c.room);
    if (!r || ("room_id" in j && u64(j.room_id).toString() !== c.room)) return reply({}, ERR_NO_ROOM);
    const text = [...clean(str(j.text), 4 * MAX_CHAT).trim()].slice(0, MAX_CHAT).join("");
    if (!text) return reply({}, ERR_BAD_REQUEST);
    const now = Date.now();
    const [t0, last] = this.chatRl.get(c.cid) ?? [CHAT_BURST, now];
    const t = Math.min(CHAT_BURST, t0 + (Math.max(0, now - last) / 1000) * CHAT_PER_S);
    if (t < 1) {
      this.chatRl.set(c.cid, [t, now]);
      return reply({}, ERR_LIMIT);
    }
    this.chatRl.set(c.cid, [t - 1, now]);
    reply({});
    const body = { room_id: r.id, member_id: c.member, account_id: c.account, name: c.name, text };
    for (const mid of mids(r)) {
      const m = r.members[mid];
      if (m.away) continue;
      const o = this.conn(m.cid);
      if (o) this.event(o.ws, "chat", body);
    }
  }

  // ---- bug reports (lobby_protocol.md 4.4) ----
  private async bugReport(c: Conn, j: Json, reply: (body: Json, err?: number) => void) {
    const text = str(j.text).trim();
    if (!text || [...text].length > MAX_REPORT_TEXT) return reply({}, ERR_BAD_REQUEST);
    let log = str(j.log);
    if (log.length > MAX_REPORT_LOG) log = log.slice(log.length - MAX_REPORT_LOG);
    const now = Date.now(), since = now - DAY_MS;
    const ipHash = await this.ipHash(c.peer_ip);
    const count = (where: string, ...args: (string | number)[]) =>
      Number([...this.sql.exec(`SELECT COUNT(*) AS n FROM bug_reports WHERE ms > ? ${where}`, since, ...args)][0].n);
    const left = Math.min(REPORTS_PER_ACCOUNT - count("AND account = ?", c.account),
                          REPORTS_PER_IP - count("AND ip_hash = ?", ipHash), REPORTS_PER_DAY - count(""));
    if (left <= 0) return reply({ left: 0 }, ERR_LIMIT);
    // The hash is only needed for the rolling day: older rows drop it.
    this.sql.exec("UPDATE bug_reports SET ip_hash = '' WHERE ms <= ? AND ip_hash != ''", since);
    this.sql.exec("INSERT INTO bug_reports (ms, account, name, ip_hash, version, text, log) VALUES (?, ?, ?, ?, ?, ?, ?)",
                  now, c.account, clean(c.name, MAX_NAME), ipHash, clean(str(j.version), 64), text, log);
    const id = Number([...this.sql.exec("SELECT last_insert_rowid() AS id")][0].id);
    this.sql.exec("DELETE FROM bug_reports WHERE id <= ?", id - MAX_REPORTS);
    reply({ id, left: left - 1 });
  }
  // GET /reports: list (newest first, no logs); GET /reports/<id>: one report as text; DELETE /reports/<id>.
  private reportsHttp(req: Request, path: string): Response {
    if (path === "/reports" && req.method === "GET") {
      const rows = [...this.sql.exec(
        "SELECT id, ms, account, name, version, text, length(log) AS log_chars FROM bug_reports ORDER BY id DESC")];
      return Response.json(rows.map((r) => ({ ...r, time: new Date(Number(r.ms)).toISOString() })));
    }
    const id = Number(path.slice("/reports/".length));
    if (!Number.isInteger(id) || id <= 0) return new Response("bad id\n", { status: 400 });
    if (req.method === "DELETE") {
      this.sql.exec("DELETE FROM bug_reports WHERE id = ?", id);
      return new Response("deleted\n");
    }
    if (req.method !== "GET") return new Response("method not allowed\n", { status: 405 });
    const r = [...this.sql.exec("SELECT * FROM bug_reports WHERE id = ?", id)][0];
    if (!r) return new Response("not found\n", { status: 404 });
    const head = `Report ${r.id}  ${new Date(Number(r.ms)).toISOString()}  ${r.name} (account ${r.account})  ${r.version}\n\n` +
      `${r.text}\n\n---- last_run.log (tail, ${String(r.log).length} chars) ----\n`;
    return new Response(head + r.log, { headers: { "Content-Type": "text/plain; charset=utf-8" } });
  }
}
