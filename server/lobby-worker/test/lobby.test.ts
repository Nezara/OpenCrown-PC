/// <reference types="@cloudflare/vitest-plugin/types" />
// Protocol unit tests (notes/map/lobby_protocol.md) against the Worker + Durable Object inside workerd.
// The end-to-end checks are the game-side ones: port/tests/test_lobby.cpp with DCP_LOBBY_TEST_URL and
// port/tools/dcp_lobby/two_instances.sh with DCP_LOBBY (see README.md).
import { runInDurableObject } from "cloudflare:test";
import { env, exports } from "cloudflare:workers";
import { describe, expect, it } from "vitest";

type Msg = Record<string, any>; // eslint-disable-line @typescript-eslint/no-explicit-any

class Client {
  inbox: Msg[] = [];
  private waiters: (() => void)[] = [];
  private rid = 1;
  constructor(public ws: WebSocket) {
    ws.addEventListener("message", (e) => {
      this.inbox.push(JSON.parse(e.data as string));
      for (const w of this.waiters.splice(0)) w();
    });
  }
  // ip = the client address the lobby sees (CF-Connecting-IP); none = the lobby's fallback 127.0.0.1.
  static upgrade(ip?: string): Promise<Response> {
    const headers: Record<string, string> = { Upgrade: "websocket" };
    if (ip) headers["CF-Connecting-IP"] = ip;
    return (exports as any).default.fetch("http://lobby/", { headers });
  }
  static async open(ip?: string): Promise<Client> {
    const resp = await Client.upgrade(ip);
    const ws = resp.webSocket as WebSocket;
    expect(ws).toBeTruthy();
    ws.accept();
    return new Client(ws);
  }
  // Close and wait for the lobby's closing handshake (its webSocketClose ran: the socket no longer counts).
  close(): Promise<void> {
    const done = new Promise<void>((r) => {
      this.ws.addEventListener("close", () => r());
      setTimeout(r, 1000);
    });
    this.ws.close();
    return done;
  }
  send(op: string, body: Msg = {}): number {
    const rid = this.rid++;
    this.ws.send(JSON.stringify({ ...body, op, rid }));
    return rid;
  }
  async take(pred: (m: Msg) => boolean): Promise<Msg> {
    for (let i = 0; i < 200; ++i) {
      const k = this.inbox.findIndex(pred);
      if (k >= 0) return this.inbox.splice(k, 1)[0];
      await new Promise<void>((r) => {
        this.waiters.push(r);
        setTimeout(r, 10);
      });
    }
    throw new Error("timeout");
  }
  async waitFor(ev: string) {
    const m = await this.take((x) => x.ev === ev);
    this.inbox.push(m); // keep it (arrival order of the rest is unchanged)
  }
  reply(rid: number) {
    return this.take((m) => m.rid === rid);
  }
  event(ev: string) {
    return this.take((m) => m.ev === ev);
  }
  async req(op: string, body: Msg = {}) {
    return this.reply(this.send(op, body));
  }
  async hello(name: string, account: string, udp_port: number, secret?: string) {
    const b: Msg = { proto: 1, version: 34, title: "CUSA10454", name, account_id: account, udp_port };
    if (secret !== undefined) b.secret = secret;
    return this.req("hello", b);
  }
}

const ERR = {
  bad: 0x80550c02 | 0, auth: 0x80550c07 | 0, unverified: 0x80550c08 | 0, notFound: 0x80550c09 | 0, limit: 0x80550c0a | 0,
};
const secret = () => [...crypto.getRandomValues(new Uint8Array(32))].map((b) => b.toString(16).padStart(2, "0")).join("");

const createReq = (max_slot = 4) => ({
  world_id: 1, max_slot, flag_attr: 0x86000000,
  int_attrs: [{ id: 0x4c, value: 99 }, { id: 0x53, value: 0x1100 }],
  bin_internal: [{ id: 0x57, data: "aa".repeat(0x78) }], bin_external: [{ id: 0x55, data: "c0ffee" }],
  member_bin_internal: [],
});

describe("lobby protocol rev 1", () => {
  it("envelope: rid echo, no hello, unknown op, proto mismatch", async () => {
    const c = await Client.open();
    const r = await c.req("worlds");
    expect(r).toMatchObject({ rid: 1, err: 0x80550c03 | 0 });
    expect((await c.req("hello", { proto: 2 })).err).toBe(0x80550c04 | 0);
    const h = await c.hello("A", "0", 40001);
    expect(h.err).toBe(0);
    expect(h.account_id).toMatch(/^[1-9][0-9]*$/); // server-assigned u64 as a decimal string
    expect(h.worlds[0].world_id).toBe(1);
    expect((await c.req("nope")).err).toBe(0x80550c02 | 0);
    c.ws.close();
  });

  it("member ids, full rooms, owner migration order, kick, disconnect", async () => {
    const [a, b, c] = [await Client.open(), await Client.open(), await Client.open()];
    await a.hello("Host", "1001", 40001);
    await b.hello("Guest", "1002", 40002);
    await c.hello("Third", "1003", 40003);
    const cr = await a.req("create_room", createReq(2));
    expect(cr.member_id).toBe(0x10);
    const room = cr.room;
    expect(room.join_code).toMatch(/^[A-HJ-NP-Z2-9]{6}$/);
    expect(typeof room.room_id).toBe("string");

    const j = await b.req("join", { join_code: room.join_code });
    expect(j.member_id).toBe(0x20);
    expect(j.room.members.map((m: Msg) => m.member_id)).toEqual([0x10, 0x20]);
    expect((await a.event("member_joined")).member.name).toBe("Guest");
    expect(await b.event("signal")).toMatchObject({ member_id: 0x10, port: 40001 });
    expect(await a.event("signal")).toMatchObject({ member_id: 0x20, port: 40002 });

    // Full: not listed, join refused with 0x80550d19, FULL flag set.
    const s = await c.req("search", { world_id: 1, start: 1, max: 20, flag_filter: 0, flag_attr: 0x64000000, filters: [] });
    expect(s.rooms.find((r: Msg) => r.room_id === room.room_id)).toBeUndefined();
    expect((await c.req("join", { room_id: room.room_id })).err).toBe(0x80550d19 | 0);

    // Non-owner kick refused; owner leaves -> signal_dead, owner_changed, member_left in that order.
    expect((await b.req("kick", { room_id: room.room_id, member_id: 0x10 })).err).toBe(0x80550c05 | 0);
    b.inbox = [];
    expect((await a.req("leave", { room_id: room.room_id })).err).toBe(0);
    await b.waitFor("member_left");
    expect(b.inbox.map((m) => m.ev)).toEqual(["signal_dead", "owner_changed", "member_left"]);
    expect(b.inbox[1]).toMatchObject({ prev: 0x10, new: 0x20 });
    expect(b.inbox[2]).toMatchObject({ member_id: 0x10, cause: 0 });
    const c2 = await c.req("join", { room_id: room.room_id });
    expect(c2.member_id).toBe(0x10); // lowest free slot
    await b.event("member_joined");
    b.inbox = [];
    c.ws.close(); // disconnect = leave with cause 2; c was not owner (b is, 0x20)
    const left = await b.event("member_left");
    expect(left).toMatchObject({ member_id: 0x10, cause: 2 });

    const d = await Client.open();
    await d.hello("Fourth", "1004", 40004);
    expect((await d.req("join", { room_id: room.room_id })).member_id).toBe(0x10);
    await b.event("member_joined");
    expect((await b.req("kick", { room_id: room.room_id, member_id: 0x10 })).err).toBe(0);
    expect((await d.event("kicked")).room_id).toBe(room.room_id);
    expect(await b.event("member_left")).toMatchObject({ member_id: 0x10, cause: 1 });
    a.ws.close();
    b.ws.close();
    d.ws.close();
  });

  it("endpoint: LAN candidate relayed in signal, only private IPv4", async () => {
    const [a, b] = [await Client.open("203.0.113.7"), await Client.open("203.0.113.7")]; // same router
    await a.hello("Host", "2001", 41001);
    await b.hello("Guest", "2002", 41002);
    const room = (await a.req("create_room", createReq(2))).room;
    await b.req("join", { join_code: room.join_code });
    await b.event("signal");
    await a.event("signal");
    // Public + LAN candidate: the peer gets both.
    await a.req("endpoint", { ip: "203.0.113.7", port: 50001, local_ip: "192.168.1.20", local_port: 41001 });
    expect(await b.event("signal")).toMatchObject({ member_id: 0x10, ip: "203.0.113.7", port: 50001,
                                                    local_ip: "192.168.1.20", local_port: 41001 });
    await a.event("signal");
    // A public "local" address is not relayed (and clears the earlier one).
    await a.req("endpoint", { ip: "203.0.113.7", port: 50001, local_ip: "8.8.8.8", local_port: 41001 });
    const s2 = await b.event("signal");
    expect(s2.local_ip).toBeUndefined();
    expect(s2.local_port).toBeUndefined();
    await a.close();
    await b.close();
  });

  it("friend_add is gone (rev-1 op without users)", async () => {
    const a = await Client.open();
    await a.hello("F1", "3001", 1);
    expect((await a.req("friend_add", { account_id: "3002" })).err).toBe(ERR.bad);
    a.ws.close();
  });

  it("identity: registration, secret check, no impersonation, one hello", async () => {
    const [sa, sb] = [secret(), secret()];
    const a = await Client.open();
    const h = await a.hello("Idy", "4001", 1, sa);
    expect(h).toMatchObject({ err: 0, verified: true, account_id: "4001" });
    expect(h.friend_code).toMatch(/^[A-HJ-NP-Z2-9]{4}-[A-HJ-NP-Z2-9]{4}$/);
    expect((await a.req("friend_code")).friend_code).toBe(h.friend_code);
    expect((await a.hello("Idy", "4001", 1, sa)).err).toBe(ERR.bad); // second hello on one connection
    a.ws.close();

    const x = await Client.open();
    expect((await x.hello("Evil", "4001", 1, sb)).err).toBe(ERR.auth); // wrong secret
    expect((await x.req("worlds")).err).toBe(0x80550c03 | 0); // ... and no hello state
    expect((await x.hello("Evil", "4001", 1)).err).toBe(ERR.auth); // registered id without secret
    expect((await x.hello("Evil", "4001", 1, "abc")).err).toBe(ERR.bad); // malformed secret
    expect((await x.hello("Evil", "0", 1, sb)).err).toBe(ERR.bad); // secret needs an account id
    const old = await x.hello("Old", "4002", 1); // unknown id, no secret: old builds work, unverified
    expect(old).toMatchObject({ err: 0, verified: false });
    expect(old.friend_code).toBeUndefined();
    expect((await x.req("friend_list")).err).toBe(ERR.unverified);
    expect((await x.req("friend_request", { code: h.friend_code })).err).toBe(ERR.unverified);
    x.ws.close();

    // Same secret again (upper case): same account, same code, new name.
    const a2 = await Client.open();
    const h2 = await a2.hello("Idy2", "4001", 1, sa.toUpperCase());
    expect(h2).toMatchObject({ err: 0, verified: true, friend_code: h.friend_code });
    a2.ws.close();
  });

  it("friends: request, accept, decline, crossing, remove, presence, rooms", async () => {
    const [fa, fb, fc] = [await Client.open(), await Client.open(), await Client.open()];
    const ca = (await fa.hello("Fa", "5001", 1, secret())).friend_code as string;
    const cb = (await fb.hello("Fb", "5002", 2, secret())).friend_code as string;
    const cc = (await fc.hello("Fc", "5003", 3, secret())).friend_code as string;
    expect(new Set([ca, cb, cc]).size).toBe(3);

    // Codes: malformed / own -> bad request, unknown -> not found.
    expect((await fa.req("friend_request", { code: "K7QF-2M9" })).err).toBe(ERR.bad);
    expect((await fa.req("friend_request", { code: "O0O0-1I1I" })).err).toBe(ERR.bad);
    expect((await fa.req("friend_request", { code: ca })).err).toBe(ERR.bad);
    const unused = [ca, cb, cc].includes("ZZZZ-ZZZZ") ? "YYYY-YYYY" : "ZZZZ-ZZZZ";
    expect((await fa.req("friend_request", { code: unused })).err).toBe(ERR.notFound);

    // A -> B (lower case, no dash): pending, B told, repeat unchanged.
    const r = await fa.req("friend_request", { code: ` ${cb.replace("-", "").toLowerCase()} ` });
    expect(r).toMatchObject({ err: 0, state: "pending", account_id: "5002", name: "Fb" });
    expect(await fb.event("friend_request")).toMatchObject({ account_id: "5001", name: "Fa" });
    expect((await fa.req("friend_request", { code: cb })).state).toBe("pending");
    const ra = await fa.req("friend_requests");
    expect(ra.incoming).toEqual([]);
    expect(ra.outgoing.map((x: Msg) => x.account_id)).toEqual(["5002"]);
    const rb = await fb.req("friend_requests");
    expect(rb.incoming).toHaveLength(1);
    expect(rb.incoming[0]).toMatchObject({ account_id: "5001", name: "Fa" });
    expect(rb.incoming[0].ms).toBeGreaterThan(0);
    expect((await fa.req("friend_list")).friends).toEqual([]);

    // B accepts: A told, both list each other online, request gone.
    expect(await fb.req("friend_accept", { account_id: "5001" })).toMatchObject({ err: 0, account_id: "5001", online: true });
    expect((await fa.event("friend_added")).account_id).toBe("5002");
    expect((await fb.req("friend_accept", { account_id: "5001" })).err).toBe(ERR.notFound);
    expect((await fb.req("friend_requests")).incoming).toEqual([]);
    expect((await fa.req("friend_list")).friends).toEqual([{ account_id: "5002", name: "Fb", online: true }]);
    expect((await fb.req("friend_list")).friends.map((f: Msg) => f.account_id)).toEqual(["5001"]);
    expect((await fa.req("friend_request", { code: cb })).state).toBe("friends");

    // A friend's room shows in the list.
    const room = (await fa.req("create_room", createReq())).room;
    expect((await fb.req("friend_list")).friends[0]).toMatchObject({ room_id: room.room_id, join_code: room.join_code });
    expect((await fa.req("leave")).err).toBe(0);
    expect((await fb.req("friend_list")).friends[0].room_id).toBeUndefined();

    // C -> A declined; C -> A again + A -> C crossing = friends at once.
    expect((await fc.req("friend_request", { code: ca })).state).toBe("pending");
    expect((await fa.event("friend_request")).account_id).toBe("5003");
    expect((await fa.req("friend_decline", { account_id: "5003" })).err).toBe(0);
    expect((await fc.req("friend_requests")).outgoing).toEqual([]);
    expect((await fa.req("friend_decline", { account_id: "5003" })).err).toBe(ERR.notFound);
    expect((await fc.req("friend_request", { code: ca })).state).toBe("pending");
    await fa.event("friend_request");
    expect((await fa.req("friend_request", { code: cc })).state).toBe("friends");
    expect((await fc.event("friend_added")).account_id).toBe("5001");
    expect((await fa.req("friend_list")).friends.map((f: Msg) => f.account_id)).toEqual(["5002", "5003"]);

    // Withdraw an outgoing request.
    expect((await fb.req("friend_request", { code: cc })).state).toBe("pending");
    await fc.event("friend_request");
    expect((await fb.req("friend_decline", { account_id: "5003" })).err).toBe(0);
    expect((await fc.req("friend_requests")).incoming).toEqual([]);

    // A removes B: both sides, B told, idempotent.
    expect((await fa.req("friend_remove", { account_id: "5002" })).err).toBe(0);
    expect((await fb.event("friend_removed")).account_id).toBe("5001");
    expect((await fb.req("friend_list")).friends).toEqual([]);
    expect((await fa.req("friend_remove", { account_id: "5002" })).err).toBe(0);

    // Presence: a launcher session (presence false) does not announce; C going away -> friend_offline; back -> online.
    const sc = secret();
    const l = await Client.open();
    await l.req("hello", { proto: 1, version: 34, name: "La", account_id: "5004", udp_port: 0, secret: sc, presence: false });
    const cl = (await l.req("friend_code")).friend_code;
    expect((await l.req("friend_request", { code: ca })).state).toBe("pending");
    await fa.event("friend_request");
    expect((await fa.req("friend_accept", { account_id: "5004" })).online).toBe(false);
    await l.event("friend_added");
    l.ws.close();
    const lp = await Client.open();
    expect((await lp.req("hello", { proto: 1, version: 34, name: "La", account_id: "5004", udp_port: 0, secret: sc,
                                    presence: false })).friend_code).toBe(cl);
    expect((await fa.req("friend_list")).friends.find((f: Msg) => f.account_id === "5004").online).toBe(false);
    fa.inbox = [];
    fc.ws.close();
    expect((await fa.event("friend_offline")).account_id).toBe("5003");
    expect(fa.inbox.find((m) => m.ev === "friend_online")).toBeUndefined();
    const fl = (await fa.req("friend_list")).friends.find((f: Msg) => f.account_id === "5003");
    expect(fl).toEqual({ account_id: "5003", name: "Fc", online: false });
    lp.ws.close();
    fa.ws.close();
    fb.ws.close();
  });

  it("friends persist across connections; pipelined requests behind hello stay in order", async () => {
    const [sa, sb] = [secret(), secret()];
    const a = await Client.open();
    const ca = (await a.hello("Pa", "6001", 1, sa)).friend_code;
    const b = await Client.open();
    await b.hello("Pb", "6002", 2, sb);
    await b.req("friend_request", { code: ca });
    await a.req("friend_accept", { account_id: "6002" });
    a.ws.close();
    b.ws.close();
    // One burst like the launcher's session: hello + action + lists, without waiting for the hello reply.
    const c = await Client.open();
    const ids = [
      c.send("hello", { proto: 1, version: 34, name: "Pa", account_id: "6001", udp_port: 0, secret: sa, presence: false }),
      c.send("friend_requests"), c.send("friend_list"),
    ];
    const [h, rq, fl] = [await c.reply(ids[0]), await c.reply(ids[1]), await c.reply(ids[2])];
    expect(h).toMatchObject({ err: 0, verified: true, friend_code: ca });
    expect(rq).toMatchObject({ err: 0, incoming: [], outgoing: [] });
    expect(fl.friends).toEqual([{ account_id: "6002", name: "Pb", online: false }]);
    c.ws.close();
  });

  it("presence blob (4.3): set_presence, friend_list / friend_online carry it, cleared by \"\" and on close", async () => {
    const [sa, sb] = [secret(), secret()];
    const a = await Client.open(), b = await Client.open();
    const ca = (await a.hello("Qa", "8001", 1, sa)).friend_code;
    await b.hello("Qb", "8002", 2, sb);
    await b.req("friend_request", { code: ca });
    await a.req("friend_accept", { account_id: "8002" });
    await b.event("friend_added");
    const blob = "ab".repeat(0x78), status = "4564";
    // Unverified connections, bad hex, odd length and oversize fields are refused.
    const u = await Client.open();
    await u.hello("Qu", "8999", 3);
    expect((await u.req("set_presence", { game_data: blob })).err).toBe(ERR.unverified);
    expect((await a.req("set_presence", { game_data: "xyz0" })).err).toBe(ERR.bad);
    expect((await a.req("set_presence", { game_data: "abc" })).err).toBe(ERR.bad);
    expect((await a.req("set_presence", { game_data: "00".repeat(0x81) })).err).toBe(ERR.bad);
    expect((await a.req("set_presence", { game_data: blob, status: "00".repeat(0xc1) })).err).toBe(ERR.bad);
    expect((await a.req("friend_list")).friends[0].game_data).toBeUndefined();
    const room = (await a.req("create_room", createReq())).room;
    expect((await a.req("set_presence", { game_data: blob.toUpperCase(), status })).err).toBe(0);
    expect((await b.req("friend_list")).friends).toEqual([{ account_id: "8001", name: "Qa", online: true,
      room_id: room.room_id, join_code: room.join_code, game_data: blob, status }]);
    // A second presence connection of A (no blob) does not hide it; friend_online carries it when A comes back.
    a.ws.close();
    expect((await b.event("friend_offline")).account_id).toBe("8001");
    expect((await b.req("friend_list")).friends[0].game_data).toBeUndefined(); // gone with the socket
    const a2 = await Client.open();
    const ids = [
      a2.send("hello", { proto: 1, version: 34, name: "Qa", account_id: "8001", udp_port: 1, secret: sa }),
      a2.send("set_presence", { game_data: blob }),
    ];
    expect((await a2.reply(ids[0])).err).toBe(0);
    expect((await a2.reply(ids[1])).err).toBe(0);
    expect((await b.event("friend_online")).game_data).toBeUndefined(); // sent at hello, before the blob
    expect((await b.req("friend_list")).friends[0]).toMatchObject({ game_data: blob, online: true });
    expect((await b.req("friend_list")).friends[0].status).toBeUndefined();
    expect((await a2.req("set_presence", { game_data: "" })).err).toBe(0);
    expect((await b.req("friend_list")).friends[0].game_data).toBeUndefined();
    a2.ws.close();
    b.ws.close();
    u.ws.close();
  });

  it("limits: pending requests per account", async () => {
    const t = await Client.open("192.0.2.200");
    const ct = (await t.hello("Target", "7001", 1, secret())).friend_code;
    for (let i = 0; i < 50; ++i) {
      const s = await Client.open(`192.0.2.${i + 1}`); // one address each: new accounts are capped per IP
      await s.hello(`S${i}`, String(7101 + 2 * i), 1, secret());
      expect((await s.req("friend_request", { code: ct })).err).toBe(0);
      s.ws.close();
    }
    const s = await Client.open("192.0.2.201");
    await s.hello("Late", "7999", 1, secret());
    expect((await s.req("friend_request", { code: ct })).err).toBe(ERR.limit);
    expect((await t.req("friend_requests")).incoming).toHaveLength(50);
    s.ws.close();
    t.ws.close();
  });

  it("bug reports: verified only, text cap, per-account and per-IP limits, token-protected reading", async () => {
    const u = await Client.open();
    await u.hello("Anon", "0", 1);
    expect((await u.req("bug_report", { text: "x" })).err).toBe(ERR.unverified);
    u.ws.close();
    const a = await Client.open();
    await a.hello("Rep1", "9301", 1, secret());
    expect((await a.req("bug_report", { text: "" })).err).toBe(ERR.bad);
    expect((await a.req("bug_report", { text: "y".repeat(257) })).err).toBe(ERR.bad);
    for (let i = 0; i < 5; ++i) {
      const r = await a.req("bug_report", { text: `bug ${i}`, log: "L".repeat(300000), version: "test" });
      expect(r).toMatchObject({ err: 0, left: 4 - i });
    }
    expect(await a.req("bug_report", { text: "sixth" })).toMatchObject({ err: ERR.limit, left: 0 });
    a.ws.close();
    // Same IP (all test clients share one): a second account gets the IP's remaining 5, a third none.
    const b = await Client.open();
    await b.hello("Rep2", "9303", 1, secret());
    for (let i = 0; i < 5; ++i) expect((await b.req("bug_report", { text: `b${i}` })).err).toBe(0);
    b.ws.close();
    const c = await Client.open();
    await c.hello("Rep3", "9305", 1, secret());
    expect((await c.req("bug_report", { text: "c" })).err).toBe(ERR.limit);
    c.ws.close();

    const get = (path: string, token?: string) =>
      (exports as any).default.fetch(`http://lobby${path}`, token ? { headers: { Authorization: `Bearer ${token}` } } : {});
    expect((await get("/reports")).status).toBe(403);
    expect((await get("/reports", "wrong-token")).status).toBe(403);
    const list = await (await get("/reports", "test-token")).json();
    expect(list.length).toBe(10);
    expect(list[list.length - 1]).toMatchObject({ name: "Rep1", text: "bug 0", version: "test", log_chars: 256 * 1024 });
    const one = await (await get(`/reports/${list[list.length - 1].id}`, "test-token")).text();
    expect(one).toContain("bug 0");
    expect(one).toContain("L".repeat(1000));
  });

  // ---- hardening (lobby_protocol.md 4.5) ----
  it("chat (4.6): verified room members only, echoed to all, cleaned + capped, rate limited, never held", async () => {
    const a = await Client.open("198.51.100.70"), b = await Client.open("198.51.100.71");
    const u = await Client.open("198.51.100.72");
    await a.hello("ChatA", "9951", 40001, secret());
    const sb = secret();
    await b.hello("ChatB", "9953", 40002, sb);
    await u.hello("ChatU", "0", 40003);
    expect((await a.req("chat", { text: "hi" })).err).toBe(0x80550d0f | 0); // no room
    expect((await u.req("chat", { text: "hi" })).err).toBe(ERR.unverified);
    const room = (await a.req("create_room", createReq(3))).room;
    await b.req("join", { room_id: room.room_id });
    await a.event("member_joined");
    expect((await a.req("chat", { text: " \u0001\n " })).err).toBe(ERR.bad); // empty once cleaned
    expect((await a.req("chat", { room_id: room.room_id, text: "  hello\u0007 there  " })).err).toBe(0);
    const want = { room_id: room.room_id, member_id: 0x10, account_id: "9951", name: "ChatA", text: "hello there" };
    expect(await b.event("chat")).toMatchObject(want);
    expect(await a.event("chat")).toMatchObject(want); // the sender's echo
    await a.req("chat", { text: "\u{1f409}".repeat(200) });
    expect([...(await b.event("chat")).text].length).toBe(120);
    // Burst 5 (two used above), then ERR_LIMIT; the socket stays open and other ops still work.
    const errs = [];
    for (let i = 0; i < 4; ++i) errs.push((await a.req("chat", { text: `m${i}` })).err);
    expect(errs).toEqual([0, 0, 0, ERR.limit]);
    expect((await a.req("worlds")).err).toBe(0);
    // A seat in grace gets no chat held (resume still works after a chatty minute).
    await b.close();
    await new Promise((r) => setTimeout(r, 100));
    await runInDurableObject(lobbyStub(), async (o: unknown) => {
      (o as any).chatRl.clear(); // eslint-disable-line @typescript-eslint/no-explicit-any
    });
    for (let i = 0; i < 5; ++i) expect((await a.req("chat", { text: `x${i}` })).err).toBe(0);
    const b2 = await Client.open("198.51.100.71");
    await b2.hello("ChatB", "9953", 40012, sb);
    expect((await b2.req("resume", { room_id: room.room_id, member_id: 0x20 })).err).toBe(0);
    await new Promise((r) => setTimeout(r, 50));
    expect(b2.inbox.filter((m) => m.ev === "chat")).toEqual([]);
    await Promise.all([a.close(), b2.close(), u.close()]);
  });

  it("names: control characters stripped, 16 chars kept", async () => {
    const a = await Client.open("198.51.100.11");
    await a.hello("Ab\u0001c\u007fdefghijklmnop\nqrstu", "0", 1);
    expect((await a.req("create_room", createReq())).room.members[0].name).toBe("Abcdefghijklmnop");
    await a.close();
  });

  it("udp endpoint: claims only from the client's own address or its /16, ip fields must be IPv4", async () => {
    const a = await Client.open("203.0.113.50");
    await a.hello("U", "0", 3658, undefined);
    const ep = async (ip: unknown) => (await a.req("endpoint", { port: 3658, ip })).ip;
    expect(await ep(undefined)).toBe("203.0.113.50"); // no claim: the address the lobby sees
    expect(await ep("203.0.9.9")).toBe("203.0.9.9"); // same /16 (carrier NAT pool)
    expect(await ep("garbage")).toBe("203.0.9.9"); // not IPv4: ignored
    expect(await ep("203.000.1.1")).toBe("203.0.9.9");
    expect(await ep("1.2.3.4.5")).toBe("203.0.9.9");
    expect(await ep("8.8.8.8")).toBe("203.0.113.50"); // someone else's address
    for (const bad of ["10.0.0.5", "100.64.0.1", "127.0.0.1", "169.254.1.1", "224.0.0.1", "0.1.2.3"])
      expect(await ep(bad)).toBe("203.0.113.50");
    await a.close();
    // hello udp_ip follows the same rule.
    const b = await Client.open("203.0.113.51");
    await b.req("hello", { proto: 1, name: "V", account_id: "0", udp_port: 1, udp_ip: "203.0.77.1" });
    expect((await b.req("endpoint", { port: 2 })).ip).toBe("203.0.77.1");
    await b.close();
    const c = await Client.open("10.0.0.1"); // a private "public" address: same /16 but never trusted
    await c.req("hello", { proto: 1, name: "W", account_id: "0", udp_port: 1, udp_ip: "10.0.0.2" });
    expect((await c.req("endpoint", { port: 2 })).ip).toBe("10.0.0.1");
    await c.close();
  });

  it("signal: LAN candidate only to peers behind the same public address", async () => {
    const a = await Client.open("203.0.113.60"), b = await Client.open("203.0.113.60"), c = await Client.open("198.51.100.60");
    await a.hello("Ha", "0", 1);
    await b.hello("Hb", "0", 2);
    await c.hello("Hc", "0", 3);
    await a.req("endpoint", { port: 50001, local_ip: "192.168.1.20", local_port: 3658 });
    const room = (await a.req("create_room", createReq())).room;
    await b.req("join", { join_code: room.join_code });
    expect(await b.event("signal")).toMatchObject({ member_id: 0x10, local_ip: "192.168.1.20", local_port: 3658 });
    await c.req("join", { join_code: room.join_code });
    const sc = await c.event("signal");
    expect(sc).toMatchObject({ member_id: 0x10, ip: "203.0.113.60", port: 50001 });
    expect(sc.local_ip).toBeUndefined();
    expect(sc.local_port).toBeUndefined();
    await Promise.all([a.close(), b.close(), c.close()]);
  });

  it("room attrs: hex, sizes and counts capped, int values uint32, extra filters ignored", async () => {
    const a = await Client.open("198.51.100.70");
    await a.hello("Attr", "0", 1);
    const create = (over: Msg) => a.req("create_room", { ...createReq(), ...over });
    const bins = (n: number, bytes = 1) => Array.from({ length: n }, (_, i) => ({ id: i, data: "00".repeat(bytes) }));
    expect((await create({ bin_internal: [{ id: 0x57, data: "abc" }] })).err).toBe(ERR.bad);
    expect((await create({ bin_external: [{ id: 0x55, data: "zz" }] })).err).toBe(ERR.bad);
    expect((await create({ bin_internal: bins(1, 0x101) })).err).toBe(ERR.limit);
    expect((await create({ bin_external: bins(17) })).err).toBe(ERR.limit);
    expect((await create({ member_bin_internal: bins(17) })).err).toBe(ERR.limit);
    const ints = (n: number) => Array.from({ length: n }, (_, i) => ({ id: 0x100 + i, value: i }));
    expect((await create({ int_attrs: ints(33) })).err).toBe(ERR.limit);
    expect((await create({ int_attrs: [{ id: 0x10000, value: 1 }] })).err).toBe(ERR.bad);
    expect((await a.req("worlds")).worlds[0]).toBeTruthy();
    // Largest legal values: 16 attrs of 0x100 B, 32 int attrs; u32 values (the game's packed 0x53, -1 -> 0xffffffff).
    const cr = await create({
      bin_internal: [{ id: 0x57, data: "AB".repeat(0x78) }], bin_external: bins(16, 0x100),
      int_attrs: [...ints(30), { id: 0x53, value: 0x81102 }, { id: 0x4c, value: -1 }],
    });
    expect(cr.err).toBe(0);
    expect(cr.room.bin_internal[0].data).toBe("ab".repeat(0x78)); // stored lower-case
    expect(cr.room.int_attrs.find((x: Msg) => x.id === 0x4c).value).toBe(0xffffffff);
    expect(cr.room.int_attrs.find((x: Msg) => x.id === 0x53).value).toBe(0x81102);
    const id = cr.room.room_id;
    // Merges that would pass the per-room counts are refused and change nothing.
    expect((await a.req("set_room_external", { room_id: id, bin_external: [{ id: 0x7777, data: "00" }] })).err).toBe(ERR.limit);
    expect((await a.req("set_room_external", { room_id: id, int_attrs: [{ id: 0x7777, value: 1 }] })).err).toBe(ERR.limit);
    expect((await a.req("set_room_external", { room_id: id, int_attrs: [{ id: 0x53, value: 7 }] })).err).toBe(0);
    expect((await a.req("set_room_internal", { room_id: id, bin_internal: bins(1, 0x101) })).err).toBe(ERR.limit);
    expect((await a.req("set_room_internal", { room_id: id, bin_internal: bins(16) })).err).toBe(ERR.limit); // 1 + 16
    expect((await a.req("set_room_internal", { room_id: id, bin_internal: [{ id: 0x57, data: "cd" }] })).err).toBe(0);
    // Search: 16 conditions that hold + a 17th that would not -> the 17th is ignored; filter values are u32 too.
    const s = { world_id: 1, start: 1, max: 20, flag_filter: 0, flag_attr: 0 };
    const hold = Array.from({ length: 16 }, () => ({ id: 0x4c, op: 1, value: -1 }));
    const r1 = await a.req("search", { ...s, filters: [...hold, { id: 0x53, op: 1, value: 12345 }] });
    expect(r1.rooms.find((r: Msg) => r.room_id === id)).toMatchObject({ bin_external: bins(16, 0x100) });
    const r2 = await a.req("search", { ...s, filters: [{ id: 0x53, op: 1, value: 12345 }] });
    expect(r2.rooms.find((r: Msg) => r.room_id === id)).toBeUndefined();
    // join: member attrs checked the same way.
    const b = await Client.open("198.51.100.71");
    await b.hello("Attr2", "0", 1);
    expect((await b.req("join", { room_id: id, member_bin_internal: [{ id: 1, data: "x" }] })).err).toBe(ERR.bad);
    expect((await b.req("join", { room_id: id, member_bin_internal: bins(1, 0x101) })).err).toBe(ERR.limit);
    expect((await b.req("join", { room_id: id, member_bin_internal: bins(1) })).member_id).toBe(0x20);
    // set_user_info (kept in the attachment): 4 attrs of <= 0x20 B; the game sends one of 1 B.
    expect((await b.req("set_user_info", { bin_attrs: [{ id: 0x5f, data: "00" }] })).err).toBe(0);
    expect((await b.req("set_user_info", { bin_attrs: bins(5) })).err).toBe(ERR.limit);
    expect((await b.req("set_user_info", { bin_attrs: bins(1, 0x21) })).err).toBe(ERR.limit);
    expect((await b.req("set_user_info", { bin_attrs: [{ id: 0x5f, data: "0" }] })).err).toBe(ERR.bad);
    await Promise.all([a.close(), b.close()]);
  });

  it("attachment: every field at its cap still fits (< 2 KB)", async () => {
    const ip = "2001:0db8:85a3:0000:0000:8a2e:0370:7334";
    const a = await Client.open(ip);
    expect((await a.hello("一".repeat(20), "18446744073709551615", 65535, secret())).err).toBe(0);
    expect((await a.req("set_presence", { game_data: "ff".repeat(0x80), status: "ff".repeat(0xc0) })).err).toBe(0);
    expect((await a.req("set_user_info", { bin_attrs: Array.from({ length: 4 }, (_, i) => ({ id: 0xffff - i,
                                                                    data: "ff".repeat(0x20) })) })).err).toBe(0);
    expect((await a.req("endpoint", { port: 65535, local_ip: "192.168.100.200", local_port: 65535 })).err).toBe(0);
    const cr = await a.req("create_room", createReq());
    expect(cr.err).toBe(0);
    expect(cr.room.members[0].name).toBe("一".repeat(16));
    expect((await a.req("worlds")).err).toBe(0);
    await a.close();
  });

  it("limits: open sockets per IP (IPv6 per /64) -> 429", async () => {
    const v4: Client[] = [];
    for (let i = 0; i < 16; ++i) v4.push(await Client.open("198.51.100.90"));
    expect((await Client.upgrade("198.51.100.90")).status).toBe(429);
    expect((await Client.upgrade("198.51.100.91")).status).toBe(101); // another address is unaffected
    await v4.pop()!.close();
    const again = await Client.open("198.51.100.90"); // a closed one frees its place
    v4.push(again);
    expect((await again.hello("Sock", "0", 1)).err).toBe(0);
    const v6: Client[] = [];
    for (let i = 0; i < 16; ++i) v6.push(await Client.open(`2001:db8:5:6::${(i + 1).toString(16)}`));
    expect((await Client.upgrade("2001:db8:5:6:ffff:ffff:ffff:ffff")).status).toBe(429); // same /64
    expect((await Client.upgrade("2001:db8:5:7::1")).status).toBe(101);
    await Promise.all([...v4, ...v6].map((c) => c.close()));
  });

  it("limits: message and room-op rates per connection -> ERR_LIMIT, socket stays open", async () => {
    const a = await Client.open("198.51.100.81");
    await a.hello("Rate", "0", 1); // 1 of the 60-message burst
    const rids = Array.from({ length: 70 }, () => a.send("worlds"));
    const errs: number[] = [];
    for (const rid of rids) errs.push((await a.reply(rid)).err);
    expect(errs.slice(0, 59).every((e) => e === 0)).toBe(true);
    expect(errs.filter((e) => e === ERR.limit).length).toBeGreaterThan(0);
    expect(errs.filter((e) => e !== 0 && e !== ERR.limit)).toEqual([]);
    await new Promise((r) => setTimeout(r, 300)); // refills ~6 tokens
    expect((await a.req("worlds")).err).toBe(0);
    await a.close();

    const b = await Client.open("198.51.100.82");
    await b.hello("Rate2", "0", 1);
    const s = { world_id: 1, start: 1, max: 20, flag_filter: 0, flag_attr: 0, filters: [] };
    const sr = Array.from({ length: 35 }, () => b.send("search", s));
    const se: number[] = [];
    for (const rid of sr) se.push((await b.reply(rid)).err);
    expect(se.slice(0, 30).every((e) => e === 0)).toBe(true);
    expect(se.filter((e) => e === ERR.limit).length).toBeGreaterThan(0);
    expect((await b.req("worlds")).err).toBe(0); // other ops still go through
    await b.close();
  });

  it("limits: new verified accounts per IP and rolling day; logins and unverified hellos unaffected", async () => {
    const ip = "198.51.100.50";
    const first = secret();
    for (let i = 0; i < 20; ++i) {
      const c = await Client.open(ip);
      expect((await c.hello(`N${i}`, String(9500 + i), 1, i === 0 ? first : secret())).err).toBe(0);
      await c.close();
    }
    const x = await Client.open(ip);
    expect((await x.hello("N20", "9520", 1, secret())).err).toBe(ERR.limit);
    expect((await x.hello("Old", "9521", 1)).err).toBe(0); // no hello state after the refusal; unverified works
    await x.close();
    const back = await Client.open(ip);
    expect((await back.hello("N0", "9500", 1, first)).err).toBe(0); // an existing account logs in
    await back.close();
    const other = await Client.open("198.51.100.51");
    expect((await other.hello("N21", "9522", 1, secret())).err).toBe(0);
    await other.close();
    // A day later the address may register again.
    await runInDurableObject(lobbyStub(), (_o: unknown, st: DurableObjectState) => { st.storage.sql.exec("UPDATE account_ips SET ms = ms - ?", 25 * 3600e3); });
    const y = await Client.open(ip);
    expect((await y.hello("N20", "9520", 1, secret())).err).toBe(0);
    await y.close();
  });

  it("friends: requests expire after 14 days; numeric account ids are u64", async () => {
    const e = await Client.open("198.51.100.40"), f = await Client.open("198.51.100.41");
    await e.hello("Ex", "8192", 1, secret());
    const cf = (await f.hello("Fx", "8193", 1, secret())).friend_code;
    expect((await e.req("friend_request", { code: cf })).state).toBe("pending");
    await runInDurableObject(lobbyStub(), (_o: unknown, st: DurableObjectState) => {
      st.storage.sql.exec("UPDATE friend_requests SET ms = ms - ? WHERE sender = '8192'", 15 * 24 * 3600e3);
    });
    expect((await f.req("friend_requests")).incoming).toEqual([]);
    expect((await f.req("friend_accept", { account_id: "8192" })).err).toBe(ERR.notFound);
    expect((await e.req("friend_request", { code: cf })).state).toBe("pending");
    // 2^64 + 8192 as a JSON number wraps to 8192 (exact as a double).
    expect((await f.req("friend_accept", { account_id: 2 ** 64 + 8192 })).account_id).toBe("8192");
    await Promise.all([e.close(), f.close()]);
  });

  it("bug reports: per-IP limit by IPv6 /64, salted ip hash cleared after a day, clean fields, nosniff", async () => {
    const send = async (ip: string, acc: string, n: number, version = "v") => {
      const c = await Client.open(ip);
      await c.hello(`R${acc}`, acc, 1, secret());
      const errs: number[] = [];
      for (let i = 0; i < n; ++i) errs.push((await c.req("bug_report", { text: `t${i}`, version })).err);
      await c.close();
      return errs;
    };
    expect(await send("2001:db8:77:1::1", "9701", 5)).toEqual([0, 0, 0, 0, 0]);
    expect(await send("2001:db8:77:1:abcd::2", "9703", 5)).toEqual([0, 0, 0, 0, 0]);
    expect(await send("2001:db8:77:1:ffff::3", "9705", 1)).toEqual([ERR.limit]); // same /64: 10 used
    expect(await send("2001:db8:77:2::1", "9707", 1, "v1\u0000\u001b[2J.0\n")).toEqual([0]); // next /64
    const rows = await runInDurableObject(lobbyStub(), (_o: unknown, st: DurableObjectState) =>
      [...st.storage.sql.exec("SELECT account, ip_hash, version FROM bug_reports WHERE account IN ('9701', '9703', '9707')")]);
    const h = (a: string) => rows.filter((r) => r.account === a).map((r) => r.ip_hash);
    expect(new Set([...h("9701"), ...h("9703")]).size).toBe(1); // one /64, one key
    expect(h("9701")[0]).toMatch(/^[0-9a-f]{64}$/);
    expect(h("9701")[0]).not.toBe(h("9707")[0]);
    const plain = [...new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode("2001:db8:77:1::/64")))]
      .map((b) => b.toString(16).padStart(2, "0")).join("");
    expect(h("9701")[0]).not.toBe(plain); // keyed (salt in SQLite), not a bare hash
    expect(rows.find((r) => r.account === "9707")!.version).toBe("v1[2J.0");
    // A day later the hash is gone (cleared on the next insert).
    await runInDurableObject(lobbyStub(), (_o: unknown, st: DurableObjectState) => {
      st.storage.sql.exec("UPDATE bug_reports SET ms = ms - ? WHERE account = '9701'", 25 * 3600e3);
    });
    expect(await send("2001:db8:77:3::1", "9709", 1)).toEqual([0]);
    const after = await runInDurableObject(lobbyStub(), (_o: unknown, st: DurableObjectState) =>
      [...st.storage.sql.exec("SELECT ip_hash FROM bug_reports WHERE account = '9701'")].map((r) => r.ip_hash));
    expect(after).toEqual(["", "", "", "", ""]);
    const get = (path: string, token?: string) =>
      (exports as any).default.fetch(`http://lobby${path}`, token ? { headers: { Authorization: `Bearer ${token}` } } : {});
    expect((await get("/reports")).headers.get("X-Content-Type-Options")).toBe("nosniff");
    expect((await get("/reports", "test-token")).headers.get("X-Content-Type-Options")).toBe("nosniff");
    expect((await get("/reports/1", "test-token")).headers.get("X-Content-Type-Options")).toBe("nosniff");
  });
  it("seat grace (1.1): verified close keeps the seat, events held, resume retakes it; grace over = disconnect", async () => {
    const sa = secret(), sb = secret();
    const a = await Client.open("198.51.100.50"), b = await Client.open("198.51.100.51");
    await a.hello("GraceHost", "9901", 40001, sa);
    await b.hello("GraceGuest", "9903", 40002, sb);
    const room = (await a.req("create_room", createReq(3))).room;
    expect((await b.req("join", { room_id: room.room_id })).member_id).toBe(0x20);
    await a.event("member_joined");
    await b.close();
    // Nobody is told; the seat still counts.
    await new Promise((r) => setTimeout(r, 100));
    expect(a.inbox.filter((m) => m.ev === "member_left" || m.ev === "signal_dead")).toEqual([]);
    const c = await Client.open("198.51.100.52");
    await c.hello("GraceThird", "9905", 40003, secret());
    expect((await c.req("join", { room_id: room.room_id })).member_id).toBe(0x30);
    a.inbox = [];

    // Wrong account / unverified / wrong seat are refused; the right one resumes with the held event behind the reply.
    const x = await Client.open("198.51.100.53");
    await x.hello("Imposter", "9907", 1, secret());
    expect((await x.req("resume", { room_id: room.room_id, member_id: 0x20 })).err).toBe(0x80550d0f | 0);
    const u = await Client.open("198.51.100.54");
    await u.hello("Unverified", "0", 1);
    expect((await u.req("resume", { room_id: room.room_id, member_id: 0x20 })).err).toBe(ERR.unverified);
    const b2 = await Client.open("198.51.100.51");
    await b2.hello("GraceGuest", "9903", 40012, sb);
    const rs = await b2.req("resume", { room_id: room.room_id, member_id: 0x20 });
    expect(rs.err).toBe(0);
    expect(rs.member_id).toBe(0x20);
    expect(rs.room.members.map((m: Msg) => m.member_id)).toEqual([0x10, 0x20, 0x30]);
    expect((await b2.event("member_joined")).member.member_id).toBe(0x30);
    expect((await b2.req("resume", { room_id: room.room_id, member_id: 0x20 })).err).toBe(0x80550c06 | 0);
    // The resumed seat is live again: endpoint re-signals, a leave is an ordinary leave.
    await b2.req("endpoint", { port: 40012 });
    expect((await a.take((m) => m.ev === "signal" && m.member_id === 0x20)).port).toBe(40012);
    await b2.close();

    // Grace over: the alarm drops the seat like a disconnect, a late resume is refused.
    await runInDurableObject(lobbyStub(), async (o: unknown) => {
      const lobby = o as any; // eslint-disable-line @typescript-eslint/no-explicit-any
      lobby.rooms.get(room.room_id).members[0x20].away.until = Date.now() - 1;
      await lobby.alarm();
    });
    expect(await a.event("member_left")).toMatchObject({ member_id: 0x20, cause: 2 });
    const b3 = await Client.open("198.51.100.51");
    await b3.hello("GraceGuest", "9903", 40022, sb);
    expect((await b3.req("resume", { room_id: room.room_id, member_id: 0x20 })).err).toBe(0x80550d0f | 0);

    // Owner drop held, then resumed: no owner change; too many held events refuse the resume.
    await a.close();
    await new Promise((r) => setTimeout(r, 100));
    expect(c.inbox.filter((m) => m.ev === "owner_changed")).toEqual([]);
    for (let i = 0; i < 33; ++i) await c.req("set_room_internal", { room_id: room.room_id, flag_filter: 0, flag_attr: 0, bin_internal: [] });
    const a2 = await Client.open("198.51.100.50");
    await a2.hello("GraceHost", "9901", 40001, sa);
    expect((await a2.req("resume", { room_id: room.room_id, member_id: 0x10 })).err).toBe(0x80550d0f | 0);
    expect(await c.event("owner_changed")).toMatchObject({ prev: 0x10, new: 0x30 });
    await Promise.all([a2.close(), b3.close(), c.close(), x.close(), u.close()]);
  });

});

const lobbyStub = () => (env as any).LOBBY.get((env as any).LOBBY.idFromName("global")) as DurableObjectStub;
