// Isolated M14 Worker smoke; never touches the display server on port 5173.
// Usage: node browser/tests/jit_smoke.mjs [build/web/browser]
// Optional: PLAYWRIGHT_MODULE_URL=file:///.../playwright/index.mjs
//           PLAYWRIGHT_CHROMIUM_EXECUTABLE=/path/to/chromium
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { access, readFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const repository = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const root = resolve(process.argv[2] || resolve(repository, 'build/web/browser'));
const playwright = process.env.PLAYWRIGHT_MODULE_URL
  || pathToFileURL(resolve(repository, 'build/playwright/node_modules/playwright/index.mjs')).href;
const { chromium } = await import(playwright);
// Fail before launching a browser when the parent has not built the real module.
await access(resolve(root, 'vita3k_jit_tests.js'));
await access(resolve(root, 'vita3k_jit_tests.wasm'));

const workerSource = `
import createTests from './vita3k_jit_tests.js';
const trace = [];
try {
  const module = await createTests({
    noInitialRun: true,
    print: line => { trace.push(String(line)); postMessage({ type: 'log', line: String(line) }); },
    printErr: line => { trace.push(String(line)); postMessage({ type: 'stderr', line: String(line) }); },
  });
  if (typeof module._vita3k_web_jit_tests !== 'function') throw new Error('missing real C++ test export');
  const started = performance.now();
  const exitCode = module._vita3k_web_jit_tests();
  postMessage({ type: 'result', exitCode, elapsedMs: performance.now() - started,
    inWorker: typeof WorkerGlobalScope !== 'undefined' && self instanceof WorkerGlobalScope,
    crossOriginIsolated: self.crossOriginIsolated, trace });
} catch (error) {
  postMessage({ type: 'error', message: String(error.stack || error), trace });
}
`;
const assets = new Map([
  ['/vita3k_jit_tests.js', ['text/javascript', 'vita3k_jit_tests.js']],
  ['/vita3k_jit_tests.wasm', ['application/wasm', 'vita3k_jit_tests.wasm']],
]);
const server = createServer(async (req, res) => {
  try {
    const path = new URL(req.url, 'http://localhost').pathname;
    let type, content;
    if (path === '/') {
      type = 'text/html';
      content = '<!doctype html><meta charset="utf-8"><title>M14 isolated JIT tests</title>';
    } else if (path === '/jit-test-worker.mjs') {
      type = 'text/javascript';
      content = workerSource;
    } else {
      const asset = assets.get(path);
      if (!asset) { res.writeHead(404); res.end(); return; }
      type = asset[0];
      content = await readFile(resolve(root, asset[1]));
    }
    res.writeHead(200, {
      'Content-Type': type,
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
      'Cache-Control': 'no-store',
    });
    res.end(content);
  } catch { res.writeHead(500); res.end(); }
});
await new Promise((resolve, reject) => {
  server.once('error', reject);
  server.listen(0, '127.0.0.1', resolve);
});
let browser;
try {
  browser = await chromium.launch({ headless: true,
    ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE
      ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}),
  });
  const page = await browser.newPage();
  const pageErrors = [];
  page.on('pageerror', error => pageErrors.push(String(error)));
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
  const result = await page.evaluate(async () => {
    const worker = new Worker('./jit-test-worker.mjs', { type: 'module' });
    const messages = [];
    try {
      return await new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error(`M14 Worker timeout: ${JSON.stringify(messages)}`)), 120000);
        worker.onerror = event => { clearTimeout(timer); reject(new Error(event.message)); };
        worker.onmessage = ({ data }) => {
          messages.push(data);
          if (data.type === 'error') {
            clearTimeout(timer);
            reject(new Error(JSON.stringify(data)));
          } else if (data.type === 'result') {
            clearTimeout(timer);
            resolve(data);
          }
        };
        worker.onmessageerror = () => { clearTimeout(timer); reject(new Error('Worker message deserialization failed')); };
      });
    } finally { worker.terminate(); }
  });
  console.log(JSON.stringify(result, null, 2));
  assert.deepEqual(pageErrors, []);
  assert.equal(result.inWorker, true, 'tests must run inside a real Worker');
  assert.equal(result.crossOriginIsolated, true);
  assert.equal(result.exitCode, 0, 'C++ JIT tests failed');
  assert.ok(result.trace.some(line => /^M14 SUMMARY mode=emscripten-jit passed=[1-9][0-9]* failed=0$/.test(line)),
    'missing genuine Emscripten JIT summary (native/reference-only is not sufficient)');
  console.log('M14 Worker JIT smoke passed');
} finally {
  await browser?.close();
  await new Promise(resolve => server.close(resolve));
}
