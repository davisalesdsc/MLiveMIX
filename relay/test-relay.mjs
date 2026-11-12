// Tests the relay running locally (npx wrangler dev --ip 127.0.0.1 --port 8787).
//   node test-relay.mjs [baseUrl]
import WebSocket from "ws";

const base = process.argv[2] || "http://127.0.0.1:8787";

// Behind an HTTPS proxy (e.g. a sandbox), route the WebSockets through it too.
// (fetch uses the proxy with NODE_USE_ENV_PROXY=1.) Optional: only if https-proxy-agent is installed.
let agent;
if (process.env.HTTPS_PROXY && base.startsWith("https")) {
  try {
    const { HttpsProxyAgent } = await import("https-proxy-agent");
    agent = new HttpsProxyAgent(process.env.HTTPS_PROXY);
  } catch (e) {}
}
const wsBase = base.replace(/^http/, "ws");
const room = "test" + Math.random().toString(36).slice(2, 14) + "abcdefgh";
const key = "k-" + Math.random().toString(36).slice(2) + "0123456789abcdef";
let failures = 0;

function check(ok, what) {
  console.log((ok ? "  [ok]   " : "  [FAIL] ") + what);
  if (!ok) failures++;
}

function open(url) {
  return new Promise((resolve) => {
    const ws = new WebSocket(url, agent ? { agent } : {});
    const texts = [], bins = [];
    ws.on("message", (data, isBinary) => (isBinary ? bins : texts).push(isBinary ? data : data.toString()));
    ws.on("open", () => resolve({ ws, texts, bins, ok: true }));
    ws.on("unexpected-response", (req, res) => resolve({ ws, ok: false, status: res.statusCode }));
    ws.on("error", () => resolve({ ws, ok: false }));
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const page = await fetch(`${base}/r/${room}`);
const html = await page.text();
check(page.status === 200 && html.includes("MLiveMIX") && html.includes("OUVIR"), "GET /r/<sala> serves the listening page");
check((await fetch(`${base}/r/x`)).status === 404, "invalid room -> 404");
check((await fetch(`${base}/nada`)).status === 404, "unknown path -> 404");

const listener = await open(`${wsBase}/r/${room}/ws`);
check(listener.ok, "listener connects before the host");
await sleep(200);
check(listener.texts.some((t) => t.includes('"type":"host"') && t.includes('"online":false')), "listener is told the host is offline");

const host = await open(`${wsBase}/r/${room}/host?key=${key}`);
check(host.ok, "host connects with its key (claims the room)");
await sleep(300);
check(host.texts.some((t) => t.includes('"listeners","count":1') || t.includes('"type":"listeners","count":1')), "host learns there is 1 listener");
check(listener.texts.some((t) => t.includes('"online":true')), "listener is told the host is online");

host.ws.send(JSON.stringify({ type: "hello", name: "MLiveMIX", version: "test", sampleRate: 48000 }));
const packet = Buffer.alloc(16 + 4800 * 4);
packet.write("MLV1", 0);
packet.writeUInt32LE(48000, 4); packet.writeUInt32LE(7, 8); packet.writeUInt16LE(2, 12);
for (let i = 16; i < packet.length; i += 2) packet.writeInt16LE(((i * 37) % 65536) - 32768, i);
host.ws.send(packet);
await sleep(500);
check(listener.texts.some((t) => t.includes('"sampleRate":48000') && t.includes('"remote":true')), "hello (sample rate) forwarded to the listener");
check(listener.bins.length === 1 && Buffer.compare(listener.bins[0], packet) === 0, "audio packet relayed byte for byte (19 KB)");

const intruder = await open(`${wsBase}/r/${room}/host?key=wrong-key-0123456789abcdef`);
check(!intruder.ok && intruder.status === 403, "another key cannot transmit into the room (403)");

const late = await open(`${wsBase}/r/${room}/ws`);
await sleep(300);
check(late.ok && late.texts.some((t) => t.includes('"sampleRate":48000')), "a late listener gets the cached hello");
check(host.texts.some((t) => t.includes('"count":2')), "host sees 2 listeners");

const status = await (await fetch(`${base}/r/${room}/status`)).json();
check(status.hostOnline === true && status.listeners === 2, "status JSON");

host.ws.close();
await sleep(400);
check(listener.texts.some((t, i) => i > 0 && t.includes('"online":false')), "listeners are told when the host stops");

const back = await open(`${wsBase}/r/${room}/host?key=${key}`);
check(back.ok, "the same key can transmit again later (fixed link)");
back.ws.close(); listener.ws.close(); late.ws.close();

console.log(failures === 0 ? "RELAY TESTS PASSED" : failures + " RELAY TEST(S) FAILED");
process.exit(failures === 0 ? 0 : 1);
