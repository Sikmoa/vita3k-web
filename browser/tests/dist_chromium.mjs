// Deployment check: serves a built dist directory as plain static files (no
// route rewriting, as a web server would) and loads the runtime from it in
// Chromium. The Worker must load the JIT module for the dist's memory model
// without falling back.
//
//   PLAYWRIGHT_MODULE_URL=file://$PWD/build/playwright/node_modules/playwright/index.mjs \
//     node browser/tests/dist_chromium.mjs [build/web64/dist]
//
// Environment:
//   DIST_MEMORY_MODEL   expected 'ready' memoryModel (default wasm64-direct;
//                       wasm32-sparse for a wasm32 build's dist)
//   PLAYWRIGHT_CHROMIUM_EXECUTABLE, LIMBO_GPU=1, LIMBO_HEADED=1  as in limbo_app_chromium.mjs
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, resolve, sep } from 'node:path';
import assert from 'node:assert/strict';

const root = resolve(process.argv[2] || 'build/web64/dist');
const expectedModel = process.env.DIST_MEMORY_MODEL || 'wasm64-direct';
const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm' };
const requests = [];
const server = createServer(async (req, res) => {
  const path = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
  try {
    if (path === '/__dist_probe.html') {
      res.writeHead(200, { 'Content-Type': 'text/html' });
      res.end('<!doctype html><title>dist probe</title>');
      return;
    }
    const file = resolve(root, `.${path}`);
    if (!file.startsWith(root + sep)) throw new Error('bad path');
    const content = await readFile(file);
    requests.push(`200 ${path}`);
    res.writeHead(200, { 'Content-Type': types[extname(file)] || 'application/octet-stream' });
    res.end(content);
  } catch {
    requests.push(`404 ${path}`);
    res.writeHead(404); res.end();
  }
});
await new Promise((done) => server.listen(0, '127.0.0.1', done));

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE_URL || 'playwright');
let browser;
try {
  browser = await chromium.launch({
    headless: process.env.LIMBO_HEADED !== '1',
    args: process.env.LIMBO_GPU === '1' ? ['--enable-unsafe-webgpu', '--enable-features=Vulkan']
      : ['--enable-unsafe-webgpu', '--use-angle=swiftshader', '--enable-features=Vulkan', '--disable-vulkan-surface'],
    ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}),
  });
  const page = await browser.newPage();
  const pageErrors = [];
  page.on('pageerror', (error) => pageErrors.push(String(error)));
  await page.goto(`http://127.0.0.1:${server.address().port}/__dist_probe.html`);
  const ready = await page.evaluate(() => new Promise((done, fail) => {
    // Default memory selection (auto), as a deployed page uses it.
    const worker = new Worker('./worker.js?backend=jit', { type: 'module' });
    const logs = [];
    const timer = setTimeout(() => fail(new Error(`no ready: ${logs.join(' | ')}`)), 60000);
    worker.onmessage = ({ data }) => {
      if (data.type === 'log') logs.push(data.message);
      if (data.type === 'error') { clearTimeout(timer); fail(new Error(data.message)); }
      if (data.type === 'ready') { clearTimeout(timer); worker.terminate(); done({ ...data.diagnostics, logs }); }
    };
    worker.onerror = (event) => { clearTimeout(timer); fail(new Error(event.message)); };
  }));
  console.log(JSON.stringify({ root, ready, requests }, null, 2));
  assert.deepEqual(pageErrors, []);
  assert.equal(ready.memoryModel, expectedModel, 'module memory model');
  assert.equal(ready.memoryFallback, false, 'the dist must hold the module where the Worker looks first');
  console.log(`DIST OK: ${ready.module} ${ready.memoryModel} from ${root}`);
} finally {
  await browser?.close();
  server.close();
}
