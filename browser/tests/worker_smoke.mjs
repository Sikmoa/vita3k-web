// Usage: PLAYWRIGHT_MODULE_URL=file:///.../playwright/index.mjs node
// browser/tests/worker_smoke.mjs build/web/dist [build/.../eboot.bin]
// PLAYWRIGHT_CHROMIUM_EXECUTABLE optionally selects an installed Chromium.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { resolve, extname, sep } from 'node:path';
import assert from 'node:assert/strict';
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE_URL || 'playwright');
const root = resolve(process.argv[2] || 'build/web/dist');
const fixture = process.argv[3] && resolve(process.argv[3]);
const server = createServer(async (req, res) => {
  try {
    const pathname = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
    const file = pathname === '/fixture.bin' && fixture ? fixture : resolve(root, `.${pathname === '/' ? '/index.html' : pathname}`);
    if (file !== fixture && !file.startsWith(root + sep)) throw new Error('bad path');
    const mime = { '.js': 'text/javascript', '.html': 'text/html', '.wasm': 'application/wasm' }[extname(file)] || 'application/octet-stream';
    res.writeHead(200, { 'Content-Type': mime, 'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp' });
    res.end(await readFile(file));
  } catch { res.writeHead(404); res.end(); }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
let browser;
try {
  browser = await chromium.launch({ headless: true, ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}) });
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
  const result = await page.evaluate(async ({ vita }) => {
    const worker = new Worker('./worker.js', { type: 'module' });
    const trace = [];
    try {
      return await new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error(`Worker timeout: ${JSON.stringify(trace)}`)), 300000);
        worker.onerror = event => { clearTimeout(timer); reject(new Error(event.message)); };
        worker.onmessage = async ({ data }) => {
          trace.push(data);
          if (data.type === 'error') { clearTimeout(timer); reject(new Error(data.message)); }
          if (data.type === 'ready') {
            const bytes = await (await fetch(vita ? './fixture.bin' : './guest.elf')).arrayBuffer();
            worker.postMessage({ type: vita ? 'run-vita' : 'run-guest', file: new File([bytes], vita ? 'eboot.bin' : 'guest.elf') });
          }
          if (data.type === (vita ? 'vita-exit' : 'guest-exit')) {
            clearTimeout(timer); resolve({ outcome: data, trace });
          }
        };
      });
    } finally { worker.terminate(); }
  }, { vita: !!fixture });
  assert.deepEqual(errors, []);
  assert.equal(result.outcome.ok, true, JSON.stringify(result));
  assert.equal(result.outcome.exitCode, 42, JSON.stringify(result));
  console.log(JSON.stringify(result, null, 2));
} finally {
  await browser?.close();
  server.close();
}
