import http from "node:http";
import https from "node:https";
import { readFileSync } from "node:fs";
import { pathToFileURL } from "node:url";
import WebSocket, { WebSocketServer } from "ws";
import { Router } from "../src/worker.js";

const identityPattern = /^[0-9a-f]{64}$/;

// Shares the exact opaque-message router with the Cloudflare implementation.
// No decryption keys, offline database or public Cloudflare dependency here.
export function createRelay({ tls, maxConnections = 1000, maxPerIp = 20 } = {}) {
  const handler = (request, response) => {
    if (request.method === "GET" && request.url === "/health") {
      response.writeHead(200, { "content-type": "application/json", "cache-control": "no-store" });
      response.end(JSON.stringify({ service: "shuangdianliao-relay", protocol: 1 }));
    } else response.writeHead(404).end();
  };
  const server = tls ? https.createServer(tls, handler) : http.createServer(handler);
  const sockets = new Map();
  const ipCounts = new Map();
  const webSockets = new WebSocketServer({ noServer: true, maxPayload: 65536, perMessageDeflate: false });
  const router = new Router({
    getWebSockets: (id) => [...(sockets.get(id) ?? [])].filter((socket) => socket.readyState === WebSocket.OPEN),
  });
  server.on("upgrade", (request, socket, head) => {
    let url;
    try { url = new URL(request.url, "http://localhost"); } catch { socket.destroy(); return; }
    const id = url.searchParams.get("id") ?? "";
    const ip = socket.remoteAddress ?? "unknown";
    const reject = (status) => socket.end(`HTTP/1.1 ${status}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
    if (url.pathname !== "/connect") { reject("404 Not Found"); return; }
    if (!identityPattern.test(id)) { reject("400 Bad Request"); return; }
    if (webSockets.clients.size >= maxConnections || (ipCounts.get(ip) ?? 0) >= maxPerIp) {
      reject("429 Too Many Requests"); return;
    }
    webSockets.handleUpgrade(request, socket, head, (client) => {
      const peers = sockets.get(id) ?? new Set();
      peers.add(client); sockets.set(id, peers);
      ipCounts.set(ip, (ipCounts.get(ip) ?? 0) + 1);
      client.deserializeAttachment = () => ({ id });
      client.alive = true;
      client.on("pong", () => { client.alive = true; });
      let frameWindowStart = Date.now(), frameCount = 0;
      client.on("message", (data, binary) => {
        if (Date.now() - frameWindowStart >= 1000) { frameWindowStart = Date.now(); frameCount = 0; }
        if (++frameCount > 200 || client.bufferedAmount > 4 * 1024 * 1024) {
          client.close(1008, "Rate limit"); return;
        }
        try { router.webSocketMessage(client, binary ? data : data.toString("utf8")); }
        catch { client.close(1011, "Relay error"); }
      });
      client.on("error", () => { /* Do not log packet contents or private data. */ });
      client.on("close", () => {
        peers.delete(client);
        if (!peers.size) sockets.delete(id);
        const count = (ipCounts.get(ip) ?? 1) - 1;
        if (count > 0) ipCounts.set(ip, count); else ipCounts.delete(ip);
      });
      client.send(JSON.stringify({ op: "ready", protocol: 1 }));
    });
  });
  const heartbeat = setInterval(() => {
    for (const socket of webSockets.clients) {
      if (!socket.alive) { socket.terminate(); continue; }
      socket.alive = false; socket.ping();
    }
  }, 30000);
  heartbeat.unref();
  server.on("close", () => { clearInterval(heartbeat); });
  return {
    server,
    close: async () => {
      clearInterval(heartbeat);
      for (const socket of webSockets.clients) socket.terminate();
      await new Promise((resolve) => server.close(resolve));
      webSockets.close();
    },
  };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const keyPath = process.env.RELAY_TLS_KEY, certPath = process.env.RELAY_TLS_CERT;
  if (Boolean(keyPath) !== Boolean(certPath)) throw new Error("Set both RELAY_TLS_KEY and RELAY_TLS_CERT");
  const tls = keyPath ? { key: readFileSync(keyPath), cert: readFileSync(certPath), minVersion: "TLSv1.2" } : undefined;
  const host = process.env.RELAY_HOST ?? "127.0.0.1";
  // Cleartext is deliberately loopback-only. For public access use a trusted
  // TLS certificate here or keep loopback binding behind a TLS reverse proxy.
  if (!tls && !["127.0.0.1", "::1", "localhost"].includes(host))
    throw new Error("Public listener requires TLS; use a reverse proxy for loopback HTTP");
  const port = Number(process.env.RELAY_PORT ?? 8787);
  if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error("Invalid RELAY_PORT");
  const relay = createRelay({ tls });
  relay.server.listen(port, host, () => console.log(`双点聊 encrypted relay: ${tls ? "https" : "http"}://${host}:${port}`));
  for (const signal of ["SIGINT", "SIGTERM"]) process.on(signal, () => { relay.close().then(() => process.exit(0)); });
}
