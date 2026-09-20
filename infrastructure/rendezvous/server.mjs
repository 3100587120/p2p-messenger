import { createHash, randomUUID, verify } from "node:crypto";
import http from "node:http";

const port = Number(process.env.PORT ?? 8787);
const maxFrameBytes = 64 * 1024;
const clients = new Map();

function identityFor(publicKeyPem) {
  return createHash("sha256").update(publicKeyPem).digest("hex");
}

function send(socket, value) {
  const payload = Buffer.from(JSON.stringify(value));
  if (payload.length > maxFrameBytes) return socket.destroy();
  const header = payload.length < 126
    ? Buffer.from([0x81, payload.length])
    : Buffer.from([0x81, 126, payload.length >> 8, payload.length & 0xff]);
  socket.write(Buffer.concat([header, payload]));
}

function close(socket, code = 1008) {
  socket.write(Buffer.from([0x88, 2, code >> 8, code & 0xff]));
  socket.destroy();
}

function consumeFrames(socket, state, chunk) {
  state.buffer = Buffer.concat([state.buffer, chunk]);
  while (state.buffer.length >= 2) {
    const first = state.buffer[0];
    const masked = (state.buffer[1] & 0x80) !== 0;
    let length = state.buffer[1] & 0x7f;
    let offset = 2;
    if ((first & 0x0f) === 0x8) return socket.destroy();
    if (!masked || (first & 0x0f) !== 0x1) return close(socket);
    if (length === 126) {
      if (state.buffer.length < 4) return;
      length = state.buffer.readUInt16BE(2); offset = 4;
    }
    if (length > maxFrameBytes || state.buffer.length < offset + 4 + length) return;
    const mask = state.buffer.subarray(offset, offset + 4);
    const frame = state.buffer.subarray(offset + 4, offset + 4 + length);
    for (let index = 0; index < frame.length; index++) frame[index] ^= mask[index % 4];
    state.buffer = state.buffer.subarray(offset + 4 + length);
    handleMessage(socket, state, frame);
  }
}

function handleMessage(socket, state, frame) {
  let message;
  try { message = JSON.parse(frame.toString("utf8")); } catch { return close(socket); }
  if (message.type === "register") {
    if (typeof message.publicKey !== "string" || typeof message.nonce !== "string" || typeof message.signature !== "string")
      return close(socket);
    const proof = Buffer.from(`p2p-messenger:rendezvous:register:${message.nonce}`);
    try {
      if (!verify(null, proof, message.publicKey, Buffer.from(message.signature, "base64"))) return close(socket);
    } catch { return close(socket); }
    const identity = identityFor(message.publicKey);
    const prior = clients.get(identity);
    if (prior && prior.socket !== socket) close(prior.socket, 1000);
    state.identity = identity;
    clients.set(identity, { socket, connectedAt: Date.now() });
    return send(socket, { type: "registered", identity, session: randomUUID() });
  }
  if (!state.identity) return close(socket);
  if (message.type === "presence" && typeof message.recipient === "string")
    return send(socket, { type: "presence", recipient: message.recipient, online: clients.has(message.recipient) });
  if (message.type === "deliver" && typeof message.recipient === "string" && message.envelope && typeof message.envelope === "object") {
    const recipient = clients.get(message.recipient);
    if (!recipient) return send(socket, { type: "unavailable", recipient: message.recipient });
    // The service intentionally treats the envelope as opaque ciphertext.
    send(recipient.socket, { type: "delivery", sender: state.identity, envelope: message.envelope });
    return send(socket, { type: "delivered", recipient: message.recipient });
  }
  close(socket);
}

const server = http.createServer((request, response) => {
  if (request.method === "GET" && request.url === "/health") {
    response.writeHead(200, { "content-type": "application/json", "cache-control": "no-store" });
    return response.end(JSON.stringify({ ok: true, online: clients.size }));
  }
  response.writeHead(404).end();
});

server.on("upgrade", (request, socket) => {
  if (request.url !== "/v1/rendezvous" || request.headers["sec-websocket-protocol"] !== "p2pmessenger.rendezvous.v1")
    return socket.destroy();
  const key = request.headers["sec-websocket-key"];
  if (typeof key !== "string") return socket.destroy();
  const accept = createHash("sha1").update(`${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`).digest("base64");
  socket.write(["HTTP/1.1 101 Switching Protocols", "Upgrade: websocket", "Connection: Upgrade", `Sec-WebSocket-Accept: ${accept}`, "Sec-WebSocket-Protocol: p2pmessenger.rendezvous.v1", "", ""].join("\r\n"));
  const state = { buffer: Buffer.alloc(0), identity: null };
  socket.on("data", chunk => consumeFrames(socket, state, chunk));
  socket.on("close", () => { if (state.identity && clients.get(state.identity)?.socket === socket) clients.delete(state.identity); });
  socket.on("error", () => { if (state.identity && clients.get(state.identity)?.socket === socket) clients.delete(state.identity); });
});

server.listen(port, "0.0.0.0", () => console.log(`P2P Messenger rendezvous listening on ${port}`));
