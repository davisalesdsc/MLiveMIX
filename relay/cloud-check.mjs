// Listens to a remote link on the REAL relay and checks the audio the plugin sends there.
//   node relay/cloud-check.mjs <relayBase> <room> [seconds]
// Run it while `MLiveMixTests --serve-remote <sec> <relayBase> <room> <key>` transmits (uses the
// platform's secure client: NSURLSession on macOS, WinHTTP on Windows).
import WebSocket from "ws";

const [base, room, seconds = "10"] = process.argv.slice(2);
const url = base.replace(/^http/, "ws") + "/r/" + room + "/ws";
const ws = new WebSocket(url);
let hello = null, hostOnline = false, packets = 0, badPackets = 0, lastSeq = -1, gaps = 0, peak = 0;

ws.on("message", (data, isBinary) => {
  if (!isBinary) {
    const m = JSON.parse(data.toString());
    if (m.type === "hello" && m.sampleRate > 0) hello = m;
    if (m.type === "host") hostOnline = m.online;
    return;
  }
  const b = Buffer.from(data);
  const ok = b.toString("ascii", 0, 4) === "MLV1" && b.readUInt32LE(4) === 48000 && b.readUInt16LE(12) === 2
          && (b.length - 16) / 4 === 1920;
  if (!ok) { badPackets++; return; }
  const seq = b.readUInt32LE(8);
  if (lastSeq >= 0 && seq !== lastSeq + 1) gaps++;
  lastSeq = seq;
  for (let i = 16; i < b.length; i += 2) peak = Math.max(peak, Math.abs(b.readInt16LE(i)));
  packets++;
});
ws.on("error", (e) => { console.log("connection error:", e.message); process.exit(1); });

setTimeout(() => {
  ws.close();
  const result = { url, hello, hostOnline, packets, badPackets, gaps, peak };
  console.log(JSON.stringify(result));
  // 25 packets/s (40 ms); allow for connection time
  const ok = hello && hello.remote === true && packets >= Number(seconds) * 25 * 0.6 && badPackets === 0 && gaps === 0
          && peak > 16000 && peak <= 16384;
  console.log(ok ? "CLOUD REMOTE CHECK PASSED" : "CLOUD REMOTE CHECK FAILED");
  process.exit(ok ? 0 : 1);
}, Number(seconds) * 1000);
