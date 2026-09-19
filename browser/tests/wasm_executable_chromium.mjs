// Run a classic Emscripten test executable in real Chromium (including
// Memory64 modules that the local Node version cannot instantiate).
// Usage: PLAYWRIGHT_MODULE_URL=file://.../playwright/index.mjs \
//   node browser/tests/wasm_executable_chromium.mjs vita3k_jit_backend_test_node 'WasmJit backend:'
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { resolve, sep } from 'node:path';
import assert from 'node:assert/strict';

const target = process.argv[2];
const expected = process.argv[3];
assert.match(target || '', /^[a-zA-Z0-9_-]+$/);
assert.ok(expected, 'provide the executable\'s success marker as the second argument');
const root = resolve(process.env.WASM_TEST_DIST || 'build/web64/browser');
const timeout = Number(process.env.WASM_TEST_TIMEOUT_MS || 180000);
const options = Object.fromEntries([
  'VITA3K_JIT_INLINE_MUTEX', 'VITA3K_WASMJIT_PROMOTE_FLAGS',
  'VITA3K_WASMJIT_PROMOTE_ACCOUNTING',
].filter((key) => process.env[key] !== undefined).map((key) => [key, process.env[key]]));
const server = createServer(async (req, res) => {
  try {
    const path = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
    if (path === '/') {
      res.setHeader('Content-Type', 'text/html');
      res.end(`<!doctype html><title>Wasm test</title><script>
        window.testResult = { done: false, logs: [], errors: [], exitCode: null };
        var Module = {
          ...${JSON.stringify(options)},
          print: (v) => testResult.logs.push(String(v)),
          printErr: (v) => testResult.logs.push(String(v)),
          onAbort: (v) => { testResult.errors.push(String(v)); testResult.done = true; },
          onExit: (code) => { testResult.exitCode = code; },
          postRun: [() => { testResult.done = true; }]
        };
      </script><script src="/${target}.js"></script>`);
      return;
    }
    const file = resolve(root, `.${path}`);
    if (!file.startsWith(root + sep)) throw new Error('bad path');
    const bytes = await readFile(file);
    res.writeHead(200, { 'Content-Type': path.endsWith('.wasm') ? 'application/wasm' : 'text/javascript' });
    res.end(bytes);
  } catch { res.writeHead(404); res.end(); }
});
await new Promise((done) => server.listen(0, '127.0.0.1', done));
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE_URL || 'playwright');
let browser;
try {
  browser = await chromium.launch({ headless: true,
    ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE
      ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}) });
  const page = await browser.newPage();
  const pageErrors = [];
  page.on('pageerror', (error) => pageErrors.push(String(error)));
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
  try {
    await page.waitForFunction(() => window.testResult?.done, null, { timeout });
  } catch (error) {
    console.error(JSON.stringify(await page.evaluate(() => window.testResult), null, 2));
    throw error;
  }
  const result = await page.evaluate(() => window.testResult);
  console.log(JSON.stringify({ target, options, ...result, pageErrors }, null, 2));
  assert.equal(pageErrors.length, 0, 'browser exceptions');
  assert.equal(result.errors.length, 0, 'executable aborted');
  assert.ok(result.exitCode === null || result.exitCode === 0, 'nonzero executable exit');
  assert.ok(result.logs.some((line) => line.includes(expected)), 'success marker missing');
} finally {
  await browser?.close();
  await new Promise((done) => server.close(done));
}
