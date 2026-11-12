// MLiveMIX relay: lets someone outside the studio's network listen to the mix.
//
//   GET /r/<room>          -> the listening page (same player as on the local network)
//   GET /r/<room>/ws       -> listener WebSocket (receives the audio)
//   GET /r/<room>/host     -> the plugin's WebSocket (?key=<secret>; sends the audio)
//   GET /r/<room>/status   -> { hostOnline, listeners }
//
// One Durable Object per room fans the plugin's packets out to every listener. The first host
// connection claims the room with its secret key (only its hash is stored); afterwards only that
// key can broadcast into the room, so a leaked listening link cannot be used to transmit.

import { PAGE_HTML } from "./page.js";

const ROOM_RE = /^[A-Za-z0-9_-]{16,64}$/;
const MAX_LISTENERS = 25;
const MAX_MESSAGE_BYTES = 256 * 1024;
const ROOM_EXPIRY_MS = 60 * 24 * 3600 * 1000; // forget a room 60 days after its last broadcast

const textHeaders = { "content-type": "text/plain; charset=utf-8", "cache-control": "no-store" };

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const parts = url.pathname.split("/").filter(Boolean);

    if (parts.length === 0)
      return new Response("MLiveMIX — servidor de escuta remota", { headers: textHeaders });

    if (parts[0] !== "r" || !parts[1] || !ROOM_RE.test(parts[1]) || parts.length > 3)
      return new Response("Não encontrado", { status: 404, headers: textHeaders });

    const action = parts[2] || "";
    if (action === "")
      return new Response(PAGE_HTML, {
        headers: { "content-type": "text/html; charset=utf-8", "cache-control": "no-store",
                   "x-robots-tag": "noindex", "referrer-policy": "no-referrer" },
      });

    if (action === "ws" || action === "host" || action === "status") {
      const stub = env.ROOMS.get(env.ROOMS.idFromName(parts[1]));
      return stub.fetch(request);
    }

    return new Response("Não encontrado", { status: 404, headers: textHeaders });
  },
};

async function sha256Hex(text) {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

export class Room {
  constructor(state) {
    this.state = state;
    this.host = null;
    this.listeners = new Set();
    this.hello = null;
  }

  async fetch(request) {
    const url = new URL(request.url);
    const action = url.pathname.split("/").filter(Boolean)[2];

    if (action === "status")
      return Response.json({ hostOnline: this.host !== null, listeners: this.listeners.size },
                           { headers: { "cache-control": "no-store", "access-control-allow-origin": "*" } });

    if (request.headers.get("Upgrade") !== "websocket")
      return new Response("WebSocket esperado", { status: 426, headers: textHeaders });

    return action === "host" ? this.acceptHost(url) : this.acceptListener();
  }

  async acceptHost(url) {
    const key = url.searchParams.get("key") || "";
    if (key.length < 16 || key.length > 128)
      return new Response("Chave inválida", { status: 403, headers: textHeaders });

    const hash = await sha256Hex(key);
    const stored = await this.state.storage.get("hostKeyHash");
    if (stored && stored !== hash)
      return new Response("Esta sala pertence a outro transmissor", { status: 403, headers: textHeaders });
    if (!stored)
      await this.state.storage.put("hostKeyHash", hash);
    await this.state.storage.setAlarm(Date.now() + ROOM_EXPIRY_MS);

    const [client, server] = Object.values(new WebSocketPair());
    server.accept();

    if (this.host) {
      try { this.host.close(4000, "substituído por outra conexão"); } catch (e) {}
    }
    this.host = server;

    const dropHost = () => {
      if (this.host !== server) return;
      this.host = null;
      this.broadcastText({ type: "host", online: false });
    };
    server.addEventListener("message", (ev) => this.onHostMessage(ev));
    server.addEventListener("close", dropHost);
    server.addEventListener("error", dropHost);

    this.broadcastText({ type: "host", online: true });
    this.sendListenerCount();
    return new Response(null, { status: 101, webSocket: client });
  }

  acceptListener() {
    if (this.listeners.size >= MAX_LISTENERS)
      return new Response("Sala cheia", { status: 503, headers: textHeaders });

    const [client, server] = Object.values(new WebSocketPair());
    server.accept();
    this.listeners.add(server);

    try {
      server.send(this.hello || JSON.stringify({ type: "hello", name: "MLiveMIX", remote: true, sampleRate: 0 }));
      server.send(JSON.stringify({ type: "host", online: this.host !== null }));
    } catch (e) {}

    const drop = () => {
      if (this.listeners.delete(server))
        this.sendListenerCount();
    };
    server.addEventListener("close", drop);
    server.addEventListener("error", drop);

    this.sendListenerCount();
    return new Response(null, { status: 101, webSocket: client });
  }

  onHostMessage(ev) {
    if (typeof ev.data === "string") {
      try {
        const m = JSON.parse(ev.data);
        if (m && m.type === "hello") {
          m.remote = true;
          this.hello = JSON.stringify(m);
          this.broadcastText(m);
        }
      } catch (e) {}
      return;
    }

    if (ev.data.byteLength > MAX_MESSAGE_BYTES)
      return;

    for (const l of this.listeners) {
      try { l.send(ev.data); } catch (e) { this.listeners.delete(l); }
    }
  }

  broadcastText(obj) {
    const text = JSON.stringify(obj);
    for (const l of this.listeners) {
      try { l.send(text); } catch (e) { this.listeners.delete(l); }
    }
  }

  sendListenerCount() {
    if (!this.host) return;
    try { this.host.send(JSON.stringify({ type: "listeners", count: this.listeners.size })); } catch (e) {}
  }

  async alarm() {
    if (this.host === null)
      await this.state.storage.deleteAll();
  }
}
