import test from "node:test";
import assert from "node:assert/strict";
import { once } from "node:events";
import WebSocket from "ws";
import { createRelay, listenerConfig } from "../server.js";

test("managed hosting binding requires explicit TLS proxy trust and uses PORT", () => {
  assert.throws(() => listenerConfig({ RELAY_HOST: "0.0.0.0", PORT: "10000" }), /requires TLS/);
  const managed = listenerConfig({ RELAY_HOST: "0.0.0.0", PORT: "10000", RELAY_BEHIND_TLS_PROXY: "1" });
  assert.equal(managed.host, "0.0.0.0");
  assert.equal(managed.port, 10000);
  assert.equal(managed.behindTlsProxy, true);
  assert.throws(() => listenerConfig({ PORT: "not-a-port" }), /Invalid/);
  assert.throws(() => listenerConfig({ RELAY_TLS_KEY: "without-cert" }), /Set both/);
});

async function withRelay(t, options) {
  const relay = createRelay(options);
  relay.server.listen(0, "127.0.0.1");
  await once(relay.server, "listening");
  t.after(() => relay.close());
  return `http://127.0.0.1:${relay.server.address().port}`;
}

async function connect(base, id) {
  const socket = new WebSocket(`${base.replace("http:", "ws:")}/connect?id=${id}`);
  const ready = once(socket, "message");
  await once(socket, "open");
  assert.equal(JSON.parse((await ready)[0]).op, "ready");
  return socket;
}

test("health and invalid handshake", async (t) => {
  const base = await withRelay(t);
  assert.deepEqual(await (await fetch(`${base}/health`)).json(), { service: "shuangdianliao-relay", protocol: 1 });
  const invalid = new WebSocket(`${base.replace("http:", "ws:")}/connect?id=bad`);
  await assert.rejects(once(invalid, "open"), /400/);
});

test("real WebSocket forwards opaque frames, handles ping, and reports offline", async (t) => {
  const base = await withRelay(t);
  const alice = await connect(base, "a".repeat(64)), bob = await connect(base, "b".repeat(64));
  const pong = once(alice, "pong"); alice.ping("SD1"); await pong;
  const packet = { op: "send", id: "01234567-89ab-cdef", to: "b".repeat(64), envelope: "opaque ciphertext" };
  const delivered = once(bob, "message"), forwarded = once(alice, "message");
  alice.send(JSON.stringify(packet));
  assert.deepEqual(JSON.parse((await delivered)[0]), { op: "packet", id: packet.id, from: "a".repeat(64), envelope: packet.envelope });
  assert.deepEqual(JSON.parse((await forwarded)[0]), { op: "relay", id: packet.id, status: "forwarded" });
  const closed = once(bob, "close"); bob.close(); await closed;
  const offline = once(alice, "message"); alice.send(JSON.stringify(packet));
  assert.equal(JSON.parse((await offline)[0]).status, "recipient_offline");
  const binaryClosed = once(alice, "close"); alice.send(Buffer.from([1, 2]));
  assert.equal((await binaryClosed)[0], 1009);
});

test("per-IP connection cap is enforced", async (t) => {
  const base = await withRelay(t, { maxPerIp: 1 });
  await connect(base, "a".repeat(64));
  const extra = new WebSocket(`${base.replace("http:", "ws:")}/connect?id=${"b".repeat(64)}`);
  await assert.rejects(once(extra, "open"), /429/);
});
