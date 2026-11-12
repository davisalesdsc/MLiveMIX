// End-to-end test in a real Chromium: open the page served by MLiveMixTests --serve,
// press OUVIR and check that audio packets arrive, get scheduled and reach the output.
//
//   node tests/browser-e2e.js <port> [mobile]
const { chromium, devices } = require('playwright');

(async () => {
  const port = process.argv[2] || '18900';
  const mobile = process.argv[3] === 'mobile';
  const browser = await chromium.launch({ args: ['--autoplay-policy=no-user-gesture-required'] });
  const context = mobile ? await browser.newContext({ ...devices['Pixel 7'] }) : await browser.newContext();
  const page = await context.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e)));
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });

  await page.goto(`http://127.0.0.1:${port}/`);
  const title = await page.textContent('.brand');
  await page.click('#play');
  await page.waitForTimeout(4000);

  const s = await page.evaluate(() => ({ ...window.__mlive }));
  const statusText = await page.textContent('#statusText');
  const rate = await page.textContent('#rate');
  const meterL = await page.evaluate(() => document.getElementById('mL').style.width);

  // Mono mode + stop/start again
  await page.click('#modeSeg button[data-v="mono"]');
  await page.waitForTimeout(500);
  await page.click('#play');               // PARAR
  const stopped = await page.textContent('#statusText');
  await page.click('#play');               // OUVIR again
  await page.waitForTimeout(1500);
  const s2 = await page.evaluate(() => ({ ...window.__mlive }));

  await page.screenshot({ path: `${process.env.SHOT_DIR || '.'}/page-${mobile ? 'mobile' : 'desktop'}.png`, fullPage: true });
  await browser.close();

  const result = {
    title, statusText, rate, meterL, stopped,
    packets: s.packets, scheduled: s.scheduled, underruns: s.underruns, dropped: s.dropped,
    sampleRate: s.sampleRate, latencyMs: Math.round(s.latency * 1000), peak: s.lastPeak,
    packetsAfterRestart: s2.packets, errors: errors.concat(s.errors),
  };
  console.log(JSON.stringify(result, null, 2));

  const ok = title.includes('MLive') && s.packets > 150 && s.scheduled > 150 && s.sampleRate === 48000
          && statusText === 'Ao vivo' && parseFloat(meterL) > 50 && stopped === 'Parado'
          && s2.packets > s.packets && result.errors.length === 0;
  console.log(ok ? 'BROWSER TEST PASSED' : 'BROWSER TEST FAILED');
  process.exit(ok ? 0 : 1);
})().catch((e) => { console.error(e); process.exit(1); });
