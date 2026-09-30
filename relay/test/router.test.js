import test from "node:test";
import assert from "node:assert/strict";
import { Router } from "../src/worker.js";

const alice = "a".repeat(64);
const bob = "b".repeat(64);

function setup() {
  const received = [];
  const sender = {
    deserializeAttachment: () => ({ id: alice }),
    send: (value) => received.push(JSON.parse(value)),
  };
  const peerFrames = [];
  const peer = { send: (value) => peerFrames.push(JSON.parse(value)) };
  let online = true;
  const router = new Router({ getWebSockets: (id) => online && id === bob ? [peer] : [] });
  return { router, sender, received, peerFrames, setOnline: (value) => { online = value; } };
}

test("forwards an opaque envelope without storing or modifying it", () => {
  const state = setup();
  const packet = { op: "send", id: "01234567-89ab-cdef", to: bob, envelope: "ciphertext" };
  state.router.webSocketMessage(state.sender, JSON.stringify(packet));
  assert.deepEqual(state.peerFrames, [{ op: "packet", id: packet.id, from: alice, envelope: packet.envelope }]);
  assert.deepEqual(state.received, [{ op: "relay", id: packet.id, status: "forwarded" }]);
});

test("offline recipient is reported, never falsely acknowledged", () => {
  const state = setup();
  state.setOnline(false);
  state.router.webSocketMessage(state.sender,
    JSON.stringify({ op: "send", id: "01234567-89ab-cdef", to: bob, envelope: "ciphertext" }));
  assert.equal(state.peerFrames.length, 0);
  assert.deepEqual(state.received, [{ op: "relay", id: "01234567-89ab-cdef", status: "recipient_offline" }]);
});

test("malformed packets are rejected", () => {
  const state = setup();
  state.router.webSocketMessage(state.sender,
    JSON.stringify({ op: "send", id: "wrong", to: bob, envelope: "ciphertext" }));
  assert.equal(state.peerFrames.length, 0);
  assert.equal(state.received[0].reason, "bad_packet");
});

test("admin wire protocol passes operation-bound proof without nesting", async () => {
  const state=setup(), id="01234567-89ab-cdef";
  state.router.directory={admin:async(action,timestamp,nonce,mac)=>{
    assert.equal(action,"backup");assert.equal(timestamp,123);assert.equal(nonce,"nonce");assert.equal(mac,"proof");
    return {accounts:0,records:{}};
  }};
  await state.router.webSocketMessage(state.sender,JSON.stringify({op:"admin_directory",id,action:"backup",timestamp:123,nonce:"nonce",mac:"proof"}));
  assert.deepEqual(state.received,[{op:"admin_directory_result",id,accounts:0,records:{}}]);
});
