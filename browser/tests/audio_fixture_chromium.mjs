// Web Audio audibility probe: runs the genuine VitaSDK square-wave fixture
// (browser/tests/vita_audio_fixture, exit code 7) through the production
// dist Worker and asserts the PCM arriving at the page is full-scale.
// A peak of 16000 proves the HLE->worker->page chain carries real audio;
// retail silence is then a game-state fact (loading screen, no input), not a
// routing bug. Usage: node browser/tests/audio_fixture_chromium.mjs
//   [build/web/dist] [build/vita-audio-fixture/eboot.bin]
// Environment: PLAYWRIGHT_MODULE_URL, JIT_FIXTURE_TIMEOUT_MS (default 120000).
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { dirname, extname, resolve, sep } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const repository = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const root = resolve(process.argv[2] || resolve(repository, 'build/web64/browser'));
// worker.js/storage.js/bridges ship from source (as limbo_serve.mjs does); the
// compiled module directory only holds vita3k_web*.{js,wasm}. Module files
// are requested under ./wasm64/ and stripped to the module directory.
const webDir = resolve(repository, 'browser/web');
const fixture = resolve(process.argv[3] || resolve(repository, 'build/vita-audio-fixture/eboot.bin'));
const timeoutMs = Number(process.env.JIT_FIXTURE_TIMEOUT_MS || 120000);
const fixtureBytes = await readFile(fixture);
assert.deepEqual([...fixtureBytes.subarray(0, 4)], [0x53, 0x43, 0x45, 0x00], 'expected a SELF eboot.bin');

const server = createServer(async (req, res) => {
  try {
    const pathname = decodeURIComponent(new URL(req.url, 'http://127.0.0.1').pathname);
    let type, content;
    if (pathname === '/__audio_fixture.html') {
      type = 'text/html';
      content = '<!doctype html><meta charset="utf-8"><link rel="icon" href="data:,"><title>audio fixture</title>';
    } else if (pathname === '/__audio_fixture.bin') {
      type = 'application/octet-stream';
      content = fixtureBytes;
    } else if (pathname === '/worker.js' || pathname === '/storage.js') {
      type = 'text/javascript';
      content = await readFile(resolve(webDir, `.${pathname}`));
    } else {
      const file = resolve(root, `.${pathname.replace(/^\/wasm64\//, '/')}`);
      if (!file.startsWith(root + sep)) throw new Error('Path outside dist');
      type = { '.js': 'text/javascript', '.wasm': 'application/wasm',
        '.html': 'text/html' }[extname(file)] || 'application/octet-stream';
      content = await readFile(file);
    }
    res.writeHead(200, {
      'Content-Type': type,
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
      'Cache-Control': 'no-store',
    });
    res.end(content);
  } catch { res.writeHead(404); res.end(); }
});
await new Promise((done) => server.listen(0, '127.0.0.1', done));
const origin = `http://127.0.0.1:${server.address().port}`;
const playwright = process.env.PLAYWRIGHT_MODULE_URL
  || pathToFileURL(resolve(repository, 'build/playwright/node_modules/playwright/index.mjs')).href;
const { chromium } = await import(playwright);
let browser;
try {
  browser = await chromium.launch({ headless: true, args: ['--autoplay-policy=no-user-gesture-required'] });
  const page = await browser.newPage();
  const pageErrors = [];
  page.on('pageerror', (error) => pageErrors.push(String(error)));
  await page.goto(`${origin}/__audio_fixture.html`);
  const result = await page.evaluate(async ({ timeoutMs }) => {
    const worker = new Worker('./worker.js?backend=jit&memory=w64', { type: 'module' });
    const audio = { chunks: 0, bytes: 0, peak: 0, nonzero: 0, scanned: 0, freqs: {}, channels: {} };
    const logs = [];
    return await new Promise((resolveRun, rejectRun) => {
      const timer = setTimeout(() => rejectRun(new Error('deadline exceeded')), timeoutMs);
      const finish = (value) => { clearTimeout(timer); worker.terminate(); resolveRun(value); };
      worker.onerror = (event) => finish({ error: `worker error: ${event.message}` });
      worker.onmessage = async ({ data }) => {
        if (!data || typeof data !== 'object') return;
        if (data.type === 'ready') {
          const response = await fetch('./__audio_fixture.bin');
          const bytes = await response.arrayBuffer();
          worker.postMessage({ type: 'run-vita',
            file: new File([bytes], 'eboot.bin', { type: 'application/octet-stream' }) });
        } else if (data.type === 'vita-audio') {
          const pcm = new Int16Array(data.data);
          audio.chunks += 1; audio.bytes += pcm.length * 2;
          audio.freqs[data.freq] = (audio.freqs[data.freq] || 0) + 1;
          audio.channels[data.channels] = (audio.channels[data.channels] || 0) + 1;
          for (let i = 0; i < pcm.length; i++) {
            const v = Math.abs(pcm[i]);
            if (v > audio.peak) audio.peak = v;
            if (v > 100) ++audio.nonzero;
          }
          audio.scanned += pcm.length;
        } else if (data.type === 'log') {
          logs.push(String(data.message));
          if (logs.length > 4000) logs.splice(0, logs.length - 4000);
        } else if (data.type === 'vita-exit') {
          finish({ exit: data, audio, logTail: logs.slice(-25) });
        } else if (data.type === 'error') {
          finish({ error: String(data.message), audio, logTail: logs.slice(-25) });
        }
      };
    });
  }, { timeoutMs });
  console.log(JSON.stringify(result, null, 2));
  assert.equal(pageErrors.length, 0, `page errors: ${JSON.stringify(pageErrors)}`);
  assert.equal(result.error, undefined, `worker error: ${result.error}`);
  assert.equal(result.exit?.ok, true, `vita run failed: ${JSON.stringify(result.exit)}`);
  assert.equal(result.exit?.exitCode, 7, `fixture must exit 7, got ${JSON.stringify(result.exit)}`);
  assert.equal(result.audio.chunks, 40, `expected 40 chunks, got ${result.audio.chunks}`);
  assert.ok(result.audio.peak >= 16000, `PCM not full-scale: peak=${result.audio.peak}`);
  assert.ok(result.audio.nonzero > result.audio.scanned * 0.9,
    `wave not dense: nonzero=${result.audio.nonzero}/${result.audio.scanned}`);
  console.log('AUDIO PATH AUDIBLE: 40 full-scale chunks through HLE->worker->page');
} finally {
  await browser?.close();
  server.close();
}
