// Diagnostic only: genuine VitaSDK SELF -> Memory64 Wasm JIT -> SceGxm NID.
// Default expectation is working HLE (exit 42). --expect-missing explicitly
// verifies the current blocker and must never be presented as renderer success.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import assert from 'node:assert/strict';
import { readRuntimeFile } from './runtime_routes.mjs';
const fixture = await readFile(process.env.GXM_PROBE_BIN || '.limbo_work/gxm/guest_probe.bin');
const server = createServer(async (req, res) => {
  try {
    const path = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
    let content, type;
    if (path === '/') { content = '<!doctype html><title>GXM guest probe</title>'; type = 'text/html'; }
    else if (path === '/probe.bin') { content = fixture; type = 'application/octet-stream'; }
    else ({ content, type } = await readRuntimeFile(path));
    res.writeHead(200, { 'Content-Type': type }); res.end(content);
  } catch { res.writeHead(404); res.end(); }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE_URL || 'playwright');
let browser;
try {
  browser = await chromium.launch({ headless: true, args: [
    '--enable-unsafe-webgpu', '--use-angle=swiftshader', '--enable-features=Vulkan', '--disable-vulkan-surface'
  ] });
  const page = await browser.newPage();
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
  const outcome = await page.evaluate(() => new Promise((resolveRun, reject) => {
    const worker = new Worker('./worker.js?backend=jit&memory=w64', { type: 'module' });
    const logs = []; let ready;
    const done = (error, value) => { clearTimeout(timer); worker.terminate(); error ? reject(error) : resolveRun(value); };
    const timer = setTimeout(() => done(new Error('probe deadline')), 40000);
    worker.onmessage = ({ data }) => {
      if (data.type === 'error') done(new Error(data.message));
      else if (data.type === 'ready') {
        ready = data.diagnostics;
        fetch('/probe.bin').then(r => r.arrayBuffer()).then(bytes => worker.postMessage({ type: 'run-vita', bytes }))
          .catch(error => done(error));
      } else if (data.type === 'log') logs.push(data.message);
      else if (data.type === 'vita-exit') done(null, { ready, exit: data.exitCode, logs });
    };
    worker.onerror = e => done(new Error(e.message));
  }));
  assert.equal(outcome.ready.backend, 'jit');
  assert.equal(outcome.ready.memoryModel, 'wasm64-direct');
  assert.equal(outcome.ready.memoryFallback, false);
  const logs = outcome.logs.join('\n');
  assert.match(logs, /CPU backend: WasmJitCPU \(no fallback\)/);
  assert.match(logs, /sceGxmInitialize NID=b0f1e4ec/i);
  const missing = /Import function for NID 0xB0F1E4EC not found/i.test(logs);
  console.log(JSON.stringify({ probe: 'guest GXP color and LINEAR ABGR8 texture draws and pixel readback',
    expectedExit: process.argv.includes('--expect-missing') ? -8 : 42,
    actualExit: outcome.exit, missingSceGxmInitializeBridge: missing,
    backend: outcome.ready.backend, memoryModel: outcome.ready.memoryModel }));
  console.log(logs);
  if (process.argv.includes('--expect-missing')) {
    assert.equal(missing, true); assert.equal(outcome.exit, -8);
  } else {
    assert.equal(missing, false, 'sceGxmInitialize bridge must be registered (see HLE log above)');
    assert.equal(outcome.exit, 42, 'guest GXP pixel assertions and lifecycle operations must succeed');
    assert.equal((logs.match(/GXM WebGPU GXP indexed draw readback completed/g) || []).length, 9,
      'two color, six textured and final untextured guest draws must complete GPU readback');
    assert.match(logs, /missing_nids=0/);
    assert.match(logs, /GXM finish entered/);
    assert.match(logs, /GXM WebGPU queue fence completed/);
  }
  console.log(JSON.stringify({ backend: 'WasmJitCPU', memory: 'wasm64-direct',
    guestSceGxmCallReached: true, missingSceGxmBridge: missing, exit: outcome.exit, rendererComplete: false }));
} finally { await browser?.close(); server.close(); }
