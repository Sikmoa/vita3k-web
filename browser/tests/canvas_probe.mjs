// Canvas-surface verification for the Vita display fixture: reads pixels
// directly from the presented Canvas2D surface after the fixture completes
// and checks them against the fixture's documented encoding.
// Usage: PLAYWRIGHT_MODULE_URL=... node canvas_probe.mjs build/web/dist
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { resolve, extname, sep } from 'node:path';
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE_URL || 'playwright');
const root = resolve(process.argv[2] || 'build/web/dist');
const server = createServer(async (req, res) => {
  try {
    const pathname = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
    const file = resolve(root, `.${pathname === '/' ? '/display.html' : pathname}`);
    if (!file.startsWith(root + sep)) throw new Error('bad path');
    const mime = { '.js': 'text/javascript', '.html': 'text/html', '.wasm': 'application/wasm' }[extname(file)] || 'application/octet-stream';
    res.writeHead(200, { 'Content-Type': mime, 'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp' });
    res.end(await readFile(file));
  } catch { res.writeHead(404); res.end(); }
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
let browser;
try {
  browser = await chromium.launch({ headless: true, ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}) });
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e)));
  await page.goto(`http://127.0.0.1:${server.address().port}/display.html`);
  // Wait for the fixture to finish all 600 frames (status text updates on exit).
  await page.waitForFunction(() => /exited with code 77/.test(document.querySelector('#status')?.textContent || ''), null, { timeout: 420000 });
  const probe = await page.evaluate(() => {
    const canvas = document.querySelector('#vita-canvas');
    const ctx = canvas.getContext('2d');
    const px = (x, y) => Array.from(ctx.getImageData(x, y, 1, 1).data);
    const last = window.vita3kWeb?.frames?.lastGeneration ?? -1;
    return { last, width: canvas.width, height: canvas.height,
      p00: px(0, 0), heartbeat: px(959, 543), rectFill: px(203, 151), bit0: px(1, 0), bit2: px(3, 0), bit8: px(9, 0), bit9: px(10, 0) };
  });
  console.log(JSON.stringify({ probe, errors }, null, 2));
  const eq = (a, b) => JSON.stringify(a) === JSON.stringify(b);
  const f = 599; // default 600 frames -> last frame index
  const ok =
    probe.width === 960 && probe.height === 544 && probe.last === 600 &&
    eq(probe.p00, [f & 255, (f >> 8) & 255, (f >> 16) & 255, 255]) && // frame index in RGB
    eq(probe.heartbeat, [255, 255, 0, 255]) && // odd frame yellow
    eq(probe.rectFill, [0, 192, 64, 255]) && // rect fill green at final position
    // 599 = 0b1001010111: pixel x=1+j holds bit j of the frame index.
    eq(probe.bit0, [255, 255, 255, 255]) && eq(probe.bit2, [255, 255, 255, 255]) &&
    eq(probe.bit8, [0, 0, 0, 255]) && eq(probe.bit9, [255, 255, 255, 255]) &&
    errors.length === 0;
  console.log(ok ? 'CANVAS-PROBE PASS' : 'CANVAS-PROBE FAIL');
  process.exitCode = ok ? 0 : 1;
} finally {
  await browser?.close();
  server.close();
}
