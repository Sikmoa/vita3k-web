// Deployment check: serves a built dist directory as plain static files (no
// route rewriting, as a web server would) and loads the runtime from it in
// Chromium. The Worker, asked for the dist's memory model, must load the JIT
// module from where it looks for that model, and gxm_scene.js must initialise
// from the dist's shaders/ and translate repository GXP programs, as
// sceGxmInitialize does.
//
//   PLAYWRIGHT_MODULE_URL=file://$PWD/build/playwright/node_modules/playwright/index.mjs \
//     node browser/tests/dist_chromium.mjs [build/web64/dist]
//
// Environment:
//   DIST_MEMORY         w64 (default: a Memory64 build's dist) or w32
//   PLAYWRIGHT_CHROMIUM_EXECUTABLE, LIMBO_GPU=1, LIMBO_HEADED=1  as in limbo_app_chromium.mjs
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, resolve, sep } from 'node:path';
import assert from 'node:assert/strict';

const root = resolve(process.argv[2] || 'build/web64/dist');
const gxpRoot = resolve('tools/native-tool/src/shaders');
const memory = process.env.DIST_MEMORY || 'w64';
const expectedModel = { w64: 'wasm64-direct', w32: 'wasm32-sparse' }[memory];
assert(expectedModel, `DIST_MEMORY=${memory}: expected w64 or w32`);
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
    if (path.startsWith('/__gxp/')) {
      res.writeHead(200, { 'Content-Type': 'application/octet-stream' });
      res.end(await readFile(resolve(gxpRoot, path.slice('/__gxp/'.length).replace(/[^\w.]/g, ''))));
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
  const ready = await page.evaluate((memory) => new Promise((done, fail) => {
    // A forced model loads only from that model's directory: a misplaced
    // module fails here instead of being found by the other model's attempt.
    const worker = new Worker(`./worker.js?backend=jit&memory=${memory}`, { type: 'module' });
    const logs = [];
    const timer = setTimeout(() => fail(new Error(`no ready: ${logs.join(' | ')}`)), 60000);
    worker.onmessage = ({ data }) => {
      if (data.type === 'log') logs.push(data.message);
      if (data.type === 'error') { clearTimeout(timer); fail(new Error(data.message)); }
      if (data.type === 'ready') { clearTimeout(timer); worker.terminate(); done({ ...data.diagnostics, logs }); }
    };
    worker.onerror = (event) => { clearTimeout(timer); fail(new Error(event.message)); };
  }), memory);
  // The URLs web_gxm_init (browser/src/gxm_webgpu_bridge.cpp) builds relative
  // to the Worker, which sits next to this page.
  const gxm = await page.evaluate(async () => {
    const logs = [];
    const scene = await import('./gxm_scene.js');
    await scene.init({
      submissionProtocol: 1,
      compilerURL: new URL('shaders/gxp_compiler.mjs', location.href).href,
      nagaURL: new URL('shaders/naga.wasm', location.href).href,
      wasiShimURL: new URL('shaders/wasi/index.js', location.href).href,
      logger: (message) => logs.push(message),
    });
    let id = 0;
    for (const [name, fragment] of [['color_v.gxp', false], ['color_f.gxp', true]]) {
      const gxp = new Uint8Array(await (await fetch(`/__gxp/${name}`)).arrayBuffer());
      await scene.registerProgram(++id, gxp, fragment);
    }
    return { programs: id, logs };
  });
  console.log(JSON.stringify({ root, ready, gxm, requests }, null, 2));
  assert.deepEqual(pageErrors, []);
  assert.equal(ready.memoryModel, expectedModel, 'module memory model');
  assert.equal(gxm.programs, 2, 'GXM initialised and translated both programs');
  console.log(`DIST OK: ${ready.module} ${ready.memoryModel}, GXM initialised from ${root}`);
} finally {
  await browser?.close();
  server.close();
}
