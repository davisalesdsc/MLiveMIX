// End-to-end test in a real Chromium: open the listening page, press OUVIR and check that audio
// arrives, gets scheduled, reaches the output, and measure the latency (AUTO vs fixed NORMAL).
//
//   node tests/browser-e2e.js <port | full URL of a remote link> [mobile] [burstMs]
//   burstMs: the server releases audio in bursts (simulated bad Wi-Fi); AUTO must adapt.
const { chromium, devices } = require('playwright');

(async () => {
  const target = process.argv[2] || '18900';
  const mobile = process.argv[3] === 'mobile';
  const burstMs = Number(process.argv[4] || 0);
  const url = target.startsWith('http') ? target : `http://127.0.0.1:${target}/`;
  const remote = url.includes('/r/');
  const browser = await chromium.launch({ args: ['--autoplay-policy=no-user-gesture-required'] });
  const context = mobile ? await browser.newContext({ ...devices['Pixel 7'] }) : await browser.newContext();
  const page = await context.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e)));
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  const stats = () => page.evaluate(() => ({ ...window.__mlive }));

  await page.goto(url);
  const title = await page.textContent('.brand');
  await page.click('#play');               // AUTO is the default
  await page.waitForTimeout(5000);          // adaptation
  const s1 = await stats();
  await page.waitForTimeout(4000);          // steady state
  const s2 = await stats();
  const statusText = await page.textContent('#statusText');
  const meterL = await page.evaluate(() => document.getElementById('mL').style.width);

  // The fixed NORMAL mode, for comparison
  await page.click('#latSeg button:nth-child(3)');
  await page.waitForTimeout(3000);
  const sNormal = await stats();
  await page.click('#latSeg button:nth-child(1)'); // back to AUTO

  // Mono mode + stop/start again
  await page.click('#modeSeg button[data-v="mono"]');
  await page.waitForTimeout(300);
  await page.click('#play');               // PARAR
  const stopped = await page.textContent('#statusText');
  await page.click('#play');               // OUVIR again
  await page.waitForTimeout(1500);
  const s3 = await stats();

  await page.screenshot({ path: `${process.env.SHOT_DIR || '.'}/page-${remote ? 'remote-' : ''}${mobile ? 'mobile' : 'desktop'}.png`, fullPage: true });
  await browser.close();

  const steadyUnderruns = s2.underruns - s1.underruns;
  const result = {
    url, remote: s2.remote, burstMs, title, statusText, meterL, stopped,
    autoLatencyMs: Math.round(s2.latency * 1000), autoTargetMs: Math.round(s2.target * 1000),
    jitterMs: Math.round(s2.jitter * 1000), normalLatencyMs: Math.round(sNormal.latency * 1000),
    packets: s2.packets, steadyUnderruns, totalUnderruns: s2.underruns, dropped: s2.dropped,
    sampleRate: s2.sampleRate, packetsAfterRestart: s3.packets, errors: errors.concat(s2.errors),
  };
  console.log(JSON.stringify(result, null, 2));

  const minPackets = remote ? 100 : 400;   // 9 s of 40 ms (remote) or 10 ms (local) packets
  let ok = title.includes('MLive') && s2.remote === remote && s2.packets > minPackets && s2.sampleRate === 48000
        && statusText === 'Ao vivo' && parseFloat(meterL) > 50 && stopped === 'Parado'
        && s3.packets > s2.packets && result.errors.length === 0;
  if (burstMs > 0)
    ok = ok && steadyUnderruns <= 2 && s2.target >= burstMs / 1000 * 0.9;   // AUTO grew to cover the bursts
  else
    ok = ok && steadyUnderruns === 0 && s2.latency < sNormal.latency;      // AUTO is faster than NORMAL
  console.log(ok ? 'BROWSER TEST PASSED' : 'BROWSER TEST FAILED');
  process.exit(ok ? 0 : 1);
})().catch((e) => { console.error(e); process.exit(1); });
