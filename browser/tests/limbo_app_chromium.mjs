// Retail-app e2e harness: staged Vita content -> browser JIT module -> WebGPU
// frames, in real Chromium. This is the only path that can present frames: the
// Node bench modules are NODERAWFS with no GPU, so their draws fail at the
// WebGPU boundary by construction.
//
// Usage:
//   PLAYWRIGHT_MODULE_URL=file://$PWD/build/playwright/node_modules/playwright/index.mjs \
//     node browser/tests/limbo_app_chromium.mjs
//
// The stage root must hold a Vita filesystem subtree (ux0/app/<title>/...,
// vs0/..., os0/...). SELF segments must already be decrypted offline. Content is
// uploaded into the module's MEMFS because the browser build has no NODERAWFS;
// the JIT module must export the retail-app entry points and the FS runtime
// (see browser/runtime_wasmjit.cmake).
//
// Environment overrides:
//   GXM_RUNTIME_DIST        browser module directory (default build/web64/browser)
//   LIMBO_STAGE             staged content root (default .limbo_work/stage)
//   LIMBO_TITLE             title id (default PCSE00268)
//   LIMBO_APP               app0 directory name (default the title id)
//   LIMBO_FRAME_OUT         PNG prefix (default .limbo_work/limbo_frame)
//   LIMBO_FRAME_EVERY       save every Nth generation (default 30; never 1)
//   LIMBO_MAX_FRAMES        maximum saved frames (default 8)
//   LIMBO_DEADLINE_MS       overall guest deadline (default 600000)
//   LIMBO_DISPATCHES        guest dispatch budget hint for the runtime
//   PLAYWRIGHT_CHROMIUM_EXECUTABLE  headless Chromium executable path
import { createServer } from 'node:http';
import { readFile, writeFile, readdir } from 'node:fs/promises';
import { resolve, sep, relative } from 'node:path';
import { existsSync } from 'node:fs';
import { deflateSync } from 'node:zlib';
import assert from 'node:assert/strict';

const root = resolve(process.env.GXM_RUNTIME_DIST || 'build/web64/browser');
const stage = resolve(process.env.LIMBO_STAGE || '.limbo_work/stage');
const title = process.env.LIMBO_TITLE || 'PCSE00268';
const app = process.env.LIMBO_APP || title;
const frameOut = process.env.LIMBO_FRAME_OUT || '.limbo_work/limbo_frame';
const frameEvery = Number(process.env.LIMBO_FRAME_EVERY || 30);
const maxFrames = Number(process.env.LIMBO_MAX_FRAMES || 8);
const deadlineMs = Number(process.env.LIMBO_DEADLINE_MS || 600000);

if (frameEvery === 1)
  throw new Error('LIMBO_FRAME_EVERY=1 saves no files (generations start at 1); use 2 or more');
if (!existsSync(resolve(stage, 'ux0/app', app, 'eboot.bin')))
  throw new Error(`no staged app at ${resolve(stage, 'ux0/app', app, 'eboot.bin')}`);

async function walk(directory) {
  const entries = await readdir(directory, { withFileTypes: true });
  const files = [];
  for (const entry of entries.sort((a, b) => a.name.localeCompare(b.name))) {
    const full = resolve(directory, entry.name);
    if (entry.isDirectory()) files.push(...await walk(full));
    else if (entry.isFile()) files.push({
      path: relative(stage, full).split(sep).join('/'),
      size: (await readFile(full)).byteLength,
    });
  }
  return files;
}
const staged = await walk(stage);
const manifestBytes = new TextEncoder().encode(JSON.stringify(staged));

const server = createServer(async (req, res) => {
  try {
    const path = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
    const send = (content, type) => {
      res.writeHead(200, { 'Content-Type': type, 'Content-Length': content.length });
      res.end(content);
    };
    if (path === '/') {
      send(new TextEncoder().encode('<!doctype html><title>Limbo app probe</title>'), 'text/html');
      return;
    }
    if (path === '/manifest.json') { send(manifestBytes, 'application/json'); return; }
    if (path.startsWith('/stage/')) {
      const file = resolve(stage, path.slice('/stage/'.length));
      if (!file.startsWith(stage + sep)) throw new Error('bad stage path');
      send(await readFile(file), 'application/octet-stream');
      return;
    }
    // The Worker and the GXM WebGPU bridge must be served from one directory:
    // the bridge is resolved relative to the Worker's own location.
    if (['/worker.js', '/storage.js', '/gxm_hle_bridge.js', '/gxm_renderer.js',
      '/gxp_shader_adapter.js', '/webgpu.js', '/capabilities.js', '/audio_input.js'].includes(path)) {
      send(await readFile(resolve('browser/web', path.slice(1))), 'text/javascript');
      return;
    }
    // Locally built compiler and pinned WASI dependencies; no compilation server.
    const shaderRoutes = {
      '/shaders/gxp_compiler.mjs': '.limbo_work/gxm/shader-wasm/gxp_compiler.mjs',
      '/shaders/gxp_compiler.wasm': '.limbo_work/gxm/shader-wasm/gxp_compiler.wasm',
      '/shaders/naga.wasm': '.limbo_work/gxm/node_modules/naga-wasi-cli/wasi/naga.wasm',
    };
    let shaderFile = shaderRoutes[path];
    if (path.startsWith('/shaders/wasi/')) {
      const base = resolve('.limbo_work/gxm/node_modules/@bjorn3/browser_wasi_shim/dist');
      const candidate = resolve(base, path.slice('/shaders/wasi/'.length));
      if (!candidate.startsWith(base + sep)) throw new Error('bad shader path');
      shaderFile = candidate;
    }
    if (shaderFile) {
      send(await readFile(shaderFile),
        path.endsWith('.wasm') ? 'application/wasm' : 'text/javascript');
      return;
    }
    // ?memory=w64 makes the Worker request ./wasm64/<module>.js; the module
    // directory already IS the wasm64 flavor, so strip the prefix.
    const file = resolve(root, `.${path.replace(/^\/wasm64\//, '/')}`);
    if (!file.startsWith(root + sep)) throw new Error('bad path');
    send(await readFile(file), path.endsWith('.wasm') ? 'application/wasm' : 'text/javascript');
  } catch { res.writeHead(404); res.end(); }
});
await new Promise((done) => server.listen(0, '127.0.0.1', done));

function crc32(buffer) {
  const table = crc32.table || (crc32.table = Int32Array.from({ length: 256 }, (_, n) => {
    let c = n;
    for (let k = 0; k < 8; ++k) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    return c;
  }));
  let c = -1;
  for (let i = 0; i < buffer.length; ++i) c = table[(c ^ buffer[i]) & 0xff] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
}
function png(width, height, rgba) {
  const chunk = (type, data) => {
    const head = Buffer.alloc(4);
    head.writeUInt32BE(data.length, 0);
    const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(body), 0);
    return Buffer.concat([head, body, crc]);
  };
  const raw = Buffer.alloc((width * 4 + 1) * height);
  for (let y = 0; y < height; ++y) {
    raw[y * (width * 4 + 1)] = 0; // filter 0 (None)
    Buffer.from(rgba.buffer, rgba.byteOffset + y * width * 4, width * 4)
      .copy(raw, y * (width * 4 + 1) + 1);
  }
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width, 0);
  header.writeUInt32BE(height, 4);
  header[8] = 8; header[9] = 6; // 8-bit RGBA
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk('IHDR', header), chunk('IDAT', deflateSync(raw, { level: 6 })),
    chunk('IEND', Buffer.alloc(0))]);
}

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE_URL || 'playwright');
let browser;
try {
  browser = await chromium.launch({
    headless: true,
    args: ['--enable-unsafe-webgpu', '--use-angle=swiftshader', '--enable-features=Vulkan',
      '--disable-vulkan-surface'],
    ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE
      ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}),
  });
  const page = await browser.newPage();
  const pageErrors = [];
  page.on('pageerror', (error) => pageErrors.push(String(error)));
  await page.goto(`http://127.0.0.1:${server.address().port}/`);

  const outcome = await page.evaluate(async ({ title, app, frameEvery, maxFrames, deadlineMs, dispatches }) => {
    const worker = new Worker('./worker.js?backend=jit&memory=w64', { type: 'module' });
    const state = { logs: [], logCount: 0, frames: [], saved: [], staged: null, exit: null,
      backend: null, memory: null, workerErrors: [], ready: false, timedOut: false };
    const hex = (bytes) => Array.from(bytes.slice(0, 64), (v) => v.toString(16).padStart(2, '0')).join(' ');
    const result = await new Promise((resolveRun, rejectRun) => {
      // A deadline is a diagnostic outcome, not a failure: the host needs the
      // guest logs and the exit state to name the next blocker.
      const starter = setTimeout(() => { state.timedOut = true; finish(null, state); }, deadlineMs);
      const finish = (error, value) => { clearTimeout(starter); worker.terminate();
        error ? rejectRun(error) : resolveRun(value); };
      worker.onerror = (event) => finish(rejectRun, new Error(`worker error: ${event.message}`));
      worker.onmessage = ({ data }) => {
        if (!data || typeof data !== 'object') return;
        switch (data.type) {
        case 'log':
          state.logs.push(data.message);
          state.logCount += 1;
          if (state.logs.length > 4000) state.logs.splice(0, state.logs.length - 4000);
          break;
        case 'ready':
          state.ready = true;
          state.backend = data.diagnostics.backend;
          state.memory = data.diagnostics.memoryModel;
          fetch('/manifest.json').then((response) => response.json()).then((files) =>
            worker.postMessage({ type: 'stage-files', root: '/vita',
              files: files.map((file) => ({ ...file, url: `/stage/${file.path}` })) }))
            .catch((error) => finish(rejectRun, error));
          break;
        case 'staged':
          state.staged = { files: data.files, bytes: data.bytes, root: data.root };
          worker.postMessage({ type: 'run-app', vitaFs: data.root, title, app,
            fastVblank: true, dispatches });
          break;
        case 'vita-frame': {
          const pixels = new Uint8Array(data.data);
          // performance.now() is worker-relative but monotonic across the run,
          // so the first record's `at` is the present latency after run-app.
          const record = { generation: data.generation, width: data.width, height: data.height,
            at: Math.round(performance.now()), byteLength: pixels.byteLength, head: hex(pixels),
            checksum: [...pixels].reduce((h, v) => ((h * 33) ^ v) >>> 0, 5381) };
          if (data.generation % frameEvery === 1 && state.saved.length < maxFrames) {
            let binary = '';
            for (let i = 0; i < pixels.length; i += 0x8000)
              binary += String.fromCharCode(...pixels.subarray(i, i + 0x8000));
            state.saved.push({ ...record, base64: btoa(binary) });
          }
          state.frames.push(record);
          break;
        }
        case 'vita-exit':
          state.exit = { exitCode: data.exitCode, ok: data.ok, message: data.message };
          finish(null, state);
          break;
        case 'error':
          state.workerErrors.push(data.message);
          finish(rejectRun, new Error(`worker error: ${data.message}`));
          break;
        default: break;
        }
      };
    });
    return result;
  }, { title, app, frameEvery, maxFrames, deadlineMs,
    dispatches: Number(process.env.LIMBO_DISPATCHES || 20000000) });

  const saved = [];
  for (const frame of outcome.saved) {
    const path = `${frameOut}_${String(frame.generation).padStart(6, '0')}_` +
      `${frame.width}x${frame.height}.png`;
    await writeFile(path, png(frame.width, frame.height, Buffer.from(frame.base64, 'base64')));
    saved.push(path);
  }

  const logText = outcome.logs.join('\n');
  const diagnostics = {
    backend: outcome.backend,
    memory: outcome.memory,
    timedOut: outcome.timedOut,
    staged: outcome.staged,
    manifestFiles: staged.length,
    framesPresented: outcome.frames.length,
    firstFrame: outcome.frames[0] || null,
    lastFrame: outcome.frames[outcome.frames.length - 1] || null,
    framesSaved: saved,
    exit: outcome.exit,
    workerErrors: outcome.workerErrors,
    pageErrors,
    logCount: outcome.logCount,
    gxmDraws: (logText.match(/GXM WebGPU GXP indexed draw readback completed/g) || []).length,
    gxmDrawFails: outcome.logs.filter((line) => line.includes('GXM WebGPU draw failed')).slice(-3),
    gxmRejects: outcome.logs.filter((line) => line.includes('[gxm-reject]')).slice(-6),
    moduleLoads: outcome.logs.filter((line) => line.includes('load_module')).slice(-12),
    threadErrors: outcome.logs.filter((line) => line.includes('failed:')).slice(-6),
    scheduler: outcome.logs.filter((line) => line.includes('Guest scheduler')).slice(-3),
    logTail: outcome.logs.slice(-15),
  };
  console.log(JSON.stringify(diagnostics, null, 2));

  assert.equal(outcome.workerErrors.length, 0, 'worker must not post errors');
  assert.ok(outcome.staged && outcome.staged.files === staged.length,
    `every staged file must reach MEMFS (${outcome.staged?.files} of ${staged.length})`);
  assert.ok(outcome.frames.length > 0,
    `no frames presented; timedOut=${outcome.timedOut} exit=${JSON.stringify(outcome.exit)} ` +
    `drawFails=${JSON.stringify(diagnostics.gxmDrawFails)} ` +
    `threadErrors=${JSON.stringify(diagnostics.threadErrors)} ` +
    `scheduler=${JSON.stringify(diagnostics.scheduler)} ` +
    `rejects=${JSON.stringify(diagnostics.gxmRejects)}`);
} finally {
  await browser?.close();
  server.close();
}
