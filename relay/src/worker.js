const identity = /^[0-9a-f]{64}$/;
const packetId = /^[0-9a-f-]{16,64}$/;
const maxFrameBytes = 65536;

export default {
  fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/health")
      return Response.json({ service: "shuangdianliao-relay", protocol: 1 });
    if (url.pathname !== "/connect" || request.headers.get("Upgrade")?.toLowerCase() !== "websocket")
      return new Response("Not found", { status: 404 });
    const id = url.searchParams.get("id") || "";
    if (!identity.test(id)) return new Response("Invalid identity", { status: 400 });
    const router = env.ROUTER.get(env.ROUTER.idFromName("v1"));
    return router.fetch(request);
  },
};

// This service never decrypts or persists messages. Both peers must be online;
// a sender keeps unacknowledged packets locally and retries on reconnection.
export class Router {
  constructor(ctx) {
    this.ctx = ctx;
  }

  fetch(request) {
    const id = new URL(request.url).searchParams.get("id") || "";
    if (!identity.test(id)) return new Response("Invalid identity", { status: 400 });
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.ctx.acceptWebSocket(server, [id]);
    server.serializeAttachment({ id });
    server.send(JSON.stringify({ op: "ready", protocol: 1 }));
    return new Response(null, { status: 101, webSocket: client });
  }

  webSocketMessage(socket, data) {
    const sender = socket.deserializeAttachment()?.id;
    if (!identity.test(sender) || typeof data !== "string" || data.length > maxFrameBytes) {
      socket.close(1009, "Invalid frame");
      return;
    }
    let frame;
    try { frame = JSON.parse(data); } catch { socket.send(JSON.stringify({ op: "error", reason: "bad_json" })); return; }
    if (frame?.op !== "send" || !identity.test(frame.to) || !packetId.test(frame.id) ||
        typeof frame.envelope !== "string" || frame.envelope.length > 60000) {
      socket.send(JSON.stringify({ op: "error", id: frame?.id, reason: "bad_packet" }));
      return;
    }
    const peers = this.ctx.getWebSockets(frame.to);
    if (peers.length === 0) {
      socket.send(JSON.stringify({ op: "relay", id: frame.id, status: "recipient_offline" }));
      return;
    }
    const delivery = JSON.stringify({ op: "packet", id: frame.id, from: sender, envelope: frame.envelope });
    let forwarded = 0;
    for (const peer of peers) {
      try { peer.send(delivery); forwarded++; } catch { /* stale connection */ }
    }
    socket.send(JSON.stringify({ op: "relay", id: frame.id,
                                 status: forwarded ? "forwarded" : "recipient_offline" }));
  }
}
