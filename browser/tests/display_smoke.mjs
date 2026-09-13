// Display e2e smoke test: genuine VitaSDK homebrew animating a real Vita3K
// framebuffer in a browser page.
//
// Usage:
//   PLAYWRIGHT_MODULE_URL=file:///.../playwright/index.mjs node \
//     browser/tests/display_smoke.mjs build/web/dist [fixture-eboot.bin]
//
// The fixture argument is the display-fixture eboot (CPU-rendered animated
// framebuffer). Without it, build/web/dist/display-eboot.bin must be staged.
//
// Modeled on worker_smoke.mjs: an ephemeral local HTTP server serves the dist
// directory with COOP/COEP headers; a '/display-eboot.bin' request is remapped
// to the fixture path (the worker_smoke '/fixture.bin' trick). The parent's
// display.html/display.js page creates the Worker, fetches
// ./display-eboot.bin, and drives 'run-vita' (same flow as worker_smoke's
// vita mode); this test observes the worker's messages and the presented
// canvas without duplicating presentation logic.
//
// Environment overrides:
//   PLAYWRIGHT_CHROMIUM_EXECUTABLE  headless Chromium executable path
//   DISPLAY_SMOKE_TIMEOUT_MS        overall timeout, default 600000 (10 min;
//                                   the fixture runs ~600 frames at browser
//                                   speed)
//   DISPLAY_SMOKE_EXPECT_EXIT       expected fixture exit code (default 77,
//                                   the display fixture's completion code; set
//                                   42 to smoke the startup fixture instead)
//   DISPLAY_SMOKE_SCREENSHOT        PNG output, default /tmp/vita-display-smoke.png
//
// Exit codes: 0 pass, 1 failure (diagnostics on stderr), 2 usage/config.
//
// Message contract (worker -> page):
//   { type: 'vita-frame', generation, width: 960, height: 544,
//     pixelFormat: 'A8B8G8R8', data: ArrayBuffer }   // data in transfer list
// The payload is TIGHT RGBA8: byteLength === 960 * 544 * 4. The 'data' field
// is the transferred ArrayBuffer; other worker revisions may spread fields
// without a 'data' property — extraction below is defensive, but the payload
// is asserted firmly once a frame is received.
//
// Frame-counter encoding contract (browser/tests/vita_display_fixture/README.md):
// the fixture writes its frame index f into the framebuffer twice —
//   pixel (0,0)    = 0xFF000000 | (f & 0x00FFFFFF)          (direct u24)
//   pixel (1+j, 0) = white if bit j of f is set, else black (redundant strip)
// The browser test decodes both (bit strip first — it survives channel-order
// conversions) and requires the decoded counter to strictly increase.
// The fixture's completion exit code is 77 (600 frames default; override the
// frame count at fixture build time via VITA_DISPLAY_FRAME_COUNT).
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { resolve, extname, sep } from 'node:path';
import { existsSync, statSync } from 'node:fs';
import assert from 'node:assert/strict';

const TIMEOUT_MS = Number(process.env.DISPLAY_SMOKE_TIMEOUT_MS || 600000);
const SCREENSHOT_PATH = process.env.DISPLAY_SMOKE_SCREENSHOT || '/tmp/vita-display-smoke.png';
const EXPECT_EXIT = process.env.DISPLAY_SMOKE_EXPECT_EXIT !== undefined
  ? Number(process.env.DISPLAY_SMOKE_EXPECT_EXIT) : 77;
const READY_DEADLINE_MS = 60000;
const FIRST_FRAME_DEADLINE_MS = 300000;

const EXPECTED_WIDTH = 960;
const EXPECTED_HEIGHT = 544;
const EXPECTED_FORMAT = 'A8B8G8R8';
const EXPECTED_BYTES = EXPECTED_WIDTH * EXPECTED_HEIGHT * 4;
const MIN_FRAMES = 5;
const MIN_DISTINCT_CHECKSUMS = 3;

// Mirrors the decoder order defined inside the page.evaluate code below.
// Schemes 1-2 are the fixture's documented encodings (README 'Frame-encoding
// scheme'): the redundant bit strip at pixels 1..32 is channel-order-agnostic
// (any non-black pixel = set bit), so it is tried first; the direct u24 at
// pixel (0,0) assumes R,G,B byte order, with a reversed variant as a
// defensive fallback if a conversion ever swaps channels.
const COUNTER_SCHEME_NAMES = [
  'bits-lsb-pixels1-32',
  'bits-msb-pixels1-32',
  'pixel0-rgb-u24le',
  'pixel0-bgr-u24le',
];

const root = resolve(process.argv[2] || 'build/web/dist');
const fixtureArg = process.argv[3] ? resolve(process.argv[3]) : null;
const stagedFixture = resolve(root, 'display-eboot.bin');
const fixture = fixtureArg || (existsSync(stagedFixture) ? stagedFixture : null);
if (!fixture) {
  console.error(`[display-smoke] no display fixture: pass the eboot path as the second argument, or stage it at ${stagedFixture}`);
  process.exit(2);
}
if (!existsSync(fixture)) {
  console.error(`[display-smoke] fixture not found: ${fixture}`);
  process.exit(2);
}
if (!existsSync(resolve(root, 'display.html'))) {
  console.error(`[display-smoke] ${resolve(root, 'display.html')} not found — the dist directory has no display page (restage the browser build)`);
  process.exit(2);
}

const server = createServer(async (req, res) => {
  try {
    const pathname = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
    const file = pathname === '/display-eboot.bin' && fixture ? fixture : resolve(root, `.${pathname === '/' ? '/index.html' : pathname}`);
    if (file !== fixture && !file.startsWith(root + sep)) throw new Error('bad path');
    const mime = {
      '.js': 'text/javascript',
      '.html': 'text/html',
      '.wasm': 'application/wasm',
      '.bin': 'application/octet-stream',
    }[extname(file)] || 'application/octet-stream';
    res.writeHead(200, {
      'Content-Type': mime,
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
    });
    res.end(await readFile(file));
  } catch {
    res.writeHead(404);
    res.end();
  }
});
await new Promise((resolveListen) => server.listen(0, '127.0.0.1', resolveListen));

let browser;
let failed = false;
const fail = (message, extra) => {
  console.error(`[display-smoke] FAIL: ${message}`);
  if (extra !== undefined) console.error(JSON.stringify(extra, null, 2));
  process.exitCode = 1;
  failed = true;
};

try {
  const { chromium } = await import(process.env.PLAYWRIGHT_MODULE_URL || 'playwright');
  browser = await chromium.launch({
    headless: true,
    ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}),
  });
  const page = await browser.newPage();
  const pageErrors = [];
  const consoleErrors = [];
  page.on('pageerror', (error) => pageErrors.push(String(error)));
  page.on('console', (message) => { if (message.type() === 'error') consoleErrors.push(message.text()); });

  await page.goto(`http://127.0.0.1:${server.address().port}/display.html`);

  // Observes the display page's worker: collects vita-frame records (with a
  // full-payload djb2 checksum and candidate counter decodings per frame),
  // queries worker status after frames arrive, and waits for vita-exit.
  const summary = await page.evaluate(async ({
    timeoutMs, schemeNames, readyDeadlineMs, firstFrameDeadlineMs,
  }) => {
    const state = {
      frames: [],
      missingPayloadFrames: 0,
      workerErrors: [],
      statusReplies: [],
      exit: null,
      inPageErrors: [],
      logs: [],
      readySeen: false,
      schemeNames: null,
      startedAt: Date.now(),
      abort: null,
    };
    const pushLog = (message) => {
      state.logs.push(String(message));
      if (state.logs.length > 400) state.logs.splice(0, state.logs.length - 400);
    };
    self.addEventListener('error', (event) => {
      const message = String(event.message || event);
      state.inPageErrors.push(message);
      if (state.abort) state.abort(new Error(`page error: ${message}`));
    });
    self.addEventListener('unhandledrejection', (event) => {
      const message = `unhandledrejection: ${String(event.reason)}`;
      state.inPageErrors.push(message);
      if (state.abort) state.abort(new Error(message));
    });

    const djb2 = (bytes) => {
      let h = 5381;
      for (let i = 0; i < bytes.length; i++) h = ((h * 33) ^ bytes[i]) >>> 0;
      return h >>> 0;
    };
    const hexHead = (bytes, n) => Array.from(bytes.slice(0, n), (v) => v.toString(16).padStart(2, '0')).join(' ');

    // Candidate decodings for the fixture's frame counter, read from row 0 of
    // the tight-RGBA frame (pixel i occupies bytes [4i, 4i+4) as R,G,B,A).
    // Order must mirror the host-side COUNTER_SCHEME_NAMES.
    const schemes = [
      // Fixture README 3b: pixel (1+j, 0) is white when bit j of the frame
      // index is set, black otherwise. Channel-agnostic (non-black = set).
      (b) => { let v = 0; for (let j = 0; j < 32; j++) { const o = 4 * (1 + j); if ((b[o] | b[o + 1] | b[o + 2]) > 127) v = (v | (1 << j)) >>> 0; } return v >>> 0; },
      (b) => { let v = 0; for (let j = 0; j < 32; j++) { const o = 4 * (1 + j); if ((b[o] | b[o + 1] | b[o + 2]) > 127) v = (v | (1 << (31 - j))) >>> 0; } return v >>> 0; },
      // Fixture README 3a: pixel (0,0) == 0xFF000000 | (f & 0xFFFFFF); bytes
      // 0..2 hold f as R,G,B little-endian.
      (b) => (b[0] | (b[1] << 8) | (b[2] << 16)) >>> 0,
      (b) => (b[2] | (b[1] << 8) | (b[0] << 16)) >>> 0,
    ];
    state.schemeNames = schemeNames;
    if (schemes.length !== schemeNames.length) throw new Error('scheme list mismatch between host and page');

    // Defensive payload extraction: the agreed contract puts the transferred
    // ArrayBuffer at message.data, but tolerate views/nesting/alternate names
    // before the firm payload assertions run on the host.
    const extractPayload = (msg) => {
      const candidates = [msg.data, msg.pixels, msg.framebuffer, msg.buffer, msg.data?.data, msg.data?.pixels];
      for (const value of candidates) {
        if (value instanceof ArrayBuffer) return new Uint8Array(value);
        if (ArrayBuffer.isView(value)) return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
      }
      return null;
    };

    const worker = await new Promise((resolveWorker, rejectWorker) => {
      const deadline = Date.now() + 30000;
      const poll = () => {
        if (window.vita3kWeb?.worker) resolveWorker(window.vita3kWeb.worker);
        else if (Date.now() > deadline) rejectWorker(new Error('window.vita3kWeb.worker never appeared — display.js did not initialize its Worker'));
        else setTimeout(poll, 100);
      };
      poll();
    });

    return await new Promise((resolveRun, rejectRun) => {
      let settled = false;
      let exitSeen = false;
      let statusQuerySent = false;
      let exitGraceTimer = null;
      const finish = (fn, value) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        clearInterval(guardTimer);
        clearTimeout(exitGraceTimer);
        fn(value);
      };
      state.abort = (error) => finish(rejectRun, error);
      const timer = setTimeout(() => {
        finish(rejectRun, new Error(`display smoke timeout after ${Date.now() - state.startedAt} ms: ready=${state.readySeen} frames=${state.frames.length} exit=${JSON.stringify(state.exit)}`));
      }, timeoutMs);

      // Inactivity guards: catch a page that never boots the worker or never
      // presents a frame, long before the overall timeout.
      const guardTimer = setInterval(() => {
        const elapsed = Date.now() - state.startedAt;
        if (!state.readySeen && elapsed > readyDeadlineMs) {
          finish(rejectRun, new Error(`worker never posted 'ready' after ${elapsed} ms — display.js did not start the Vita runtime`));
        } else if (state.readySeen && state.frames.length === 0 && elapsed > firstFrameDeadlineMs) {
          finish(rejectRun, new Error(`no 'vita-frame' received within ${elapsed} ms despite a ready worker — presentation is not wired`));
        }
      }, 5000);

      worker.addEventListener('error', (event) => {
        state.workerErrors.push(event.message || 'worker error event');
        finish(rejectRun, new Error(`worker error: ${event.message || 'unknown'}`));
      });

      const sendStatusQuery = (phase) => {
        if (statusQuerySent) return;
        statusQuerySent = true;
        state.statusPhase = phase;
        try { worker.postMessage({ type: 'status' }); } catch { /* missing reply is reported by the host assertion */ }
      };

      worker.addEventListener('message', ({ data: msg }) => {
        if (!msg || typeof msg !== 'object') return;
        switch (msg.type) {
        case 'log':
          pushLog(msg.message);
          break;
        case 'lifecycle':
          pushLog(`<lifecycle ${msg.state}>`);
          break;
        case 'ready':
          state.readySeen = true;
          pushLog('<ready>');
          break;
        case 'error':
          state.workerErrors.push(msg.message);
          finish(rejectRun, new Error(`worker posted error: ${msg.message}`));
          break;
        case 'status':
          state.statusReplies.push({ phase: state.statusPhase || 'unsolicited', state: msg.state, framesSeen: state.frames.length, atMs: Date.now() - state.startedAt });
          if (exitSeen) finish(resolveRun, null);
          break;
        case 'vita-frame': {
          const payload = extractPayload(msg);
          if (!payload) {
            state.missingPayloadFrames++;
            state.lastMissingPayloadKeys = Object.keys(msg);
            break;
          }
          const record = {
            generation: msg.generation,
            width: msg.width,
            height: msg.height,
            pixelFormat: msg.pixelFormat,
            byteLength: payload.byteLength,
            checksum: djb2(payload),
            counters: payload.byteLength >= 128 ? schemes.map((decode) => decode(payload)) : schemes.map(() => null),
          };
          if (state.frames.length === 0) state.firstFrameHeadHex = hexHead(payload, 64);
          state.lastFrameHeadHex = hexHead(payload, 64);
          state.frames.push(record);
          if (!statusQuerySent && state.frames.length >= 5) sendStatusQuery('after-5-frames');
          break;
        }
        case 'vita-exit':
          state.exit = { exitCode: msg.exitCode, ok: msg.ok, message: msg.message, atMs: Date.now() - state.startedAt };
          if (msg.ok !== true) {
            finish(rejectRun, new Error(`vita-exit not ok after ${state.frames.length} frames: ${JSON.stringify(state.exit)} (exit -8 = guest died before clean process exit, e.g. unsupported instruction; check worker logs: ${JSON.stringify(state.logs.slice(-8))})`));
            break;
          }
          exitSeen = true;
          if (!statusQuerySent) sendStatusQuery('after-exit');
          if (state.statusReplies.length >= 1) finish(resolveRun, null);
          else exitGraceTimer = setTimeout(() => finish(resolveRun, null), 10000);
          break;
        default:
          break;
        }
      });
    }).then(() => {
      const generations = state.frames.map((f) => f.generation);
      const distinct = (values) => new Set(values).size;
      return {
        frames: state.frames,
        framesCount: state.frames.length,
        distinctGenerations: distinct(generations),
        generationsMonotonic: generations.every((g, i) => i === 0 || g > generations[i - 1]),
        distinctChecksums: distinct(state.frames.map((f) => f.checksum)),
        missingPayloadFrames: state.missingPayloadFrames,
        lastMissingPayloadKeys: state.lastMissingPayloadKeys || null,
        firstFrameHeadHex: state.firstFrameHeadHex || null,
        lastFrameHeadHex: state.lastFrameHeadHex || null,
        workerErrors: state.workerErrors,
        statusReplies: state.statusReplies,
        exit: state.exit,
        readySeen: state.readySeen,
        inPageErrors: state.inPageErrors,
        logTail: state.logs.slice(-40),
        displayPageFrames: window.vita3kWeb?.frames
          ? {
            received: window.vita3kWeb.frames.received,
            lastGeneration: window.vita3kWeb.frames.lastGeneration,
            distinctChecksums: window.vita3kWeb.frames.checksums.size,
          } : null,
        elapsedMs: Date.now() - state.startedAt,
      };
    });
  }, {
    timeoutMs: TIMEOUT_MS,
    schemeNames: COUNTER_SCHEME_NAMES,
    readyDeadlineMs: READY_DEADLINE_MS,
    firstFrameDeadlineMs: FIRST_FRAME_DEADLINE_MS,
  });

  // Screenshot of the presented canvas (falls back to the full page).
  let screenshotSaved = false;
  try {
    const canvas = await page.$('#vita-canvas');
    if (canvas) await canvas.screenshot({ path: SCREENSHOT_PATH });
    else await page.screenshot({ path: SCREENSHOT_PATH });
    screenshotSaved = existsSync(SCREENSHOT_PATH) && statSync(SCREENSHOT_PATH).size > 0;
  } catch (error) {
    console.error(`[display-smoke] screenshot failed: ${error}`);
  }

  const widths = [...new Set(summary.frames.map((f) => f.width))];
  const heights = [...new Set(summary.frames.map((f) => f.height))];
  const pixelFormats = [...new Set(summary.frames.map((f) => f.pixelFormat))];
  const byteLengths = [...new Set(summary.frames.map((f) => f.byteLength))];
  const diagnosis = {
    summary: {
      framesCount: summary.framesCount,
      distinctGenerations: summary.distinctGenerations,
      distinctChecksums: summary.distinctChecksums,
      missingPayloadFrames: summary.missingPayloadFrames,
      exit: summary.exit,
      statusReplies: summary.statusReplies,
      readySeen: summary.readySeen,
      displayPageFrames: summary.displayPageFrames,
      widths,
      heights,
      pixelFormats,
      byteLengths,
      firstFrame: summary.frames[0] || null,
      lastFrame: summary.frames.at(-1) || null,
      firstFrameHeadHex: summary.firstFrameHeadHex,
      lastFrameHeadHex: summary.lastFrameHeadHex,
      screenshot: SCREENSHOT_PATH,
      elapsedMs: summary.elapsedMs,
    },
    pageErrors,
    consoleErrors,
    inPageErrors: summary.inPageErrors,
    workerErrors: summary.workerErrors,
    logTail: summary.logTail,
  };

  // ---- Firm assertions -------------------------------------------------
  try {
    assert.deepEqual(pageErrors, [], 'page errors');
    assert.deepEqual(summary.inPageErrors, [], 'in-page errors');
    assert.deepEqual(summary.workerErrors, [], 'worker errors');
    if (summary.framesCount === 0) {
      throw new Error(`no 'vita-frame' messages received (expected >= ${MIN_FRAMES}) — presentation is not wired or the fixture never called sceDisplaySetFrameBuf`);
    }
    assert.ok(summary.framesCount >= MIN_FRAMES, `frames: ${summary.framesCount} < ${MIN_FRAMES}`);
    assert.ok(summary.distinctGenerations >= MIN_FRAMES, `distinct generations: ${summary.distinctGenerations} < ${MIN_FRAMES}`);
    assert.ok(summary.generationsMonotonic, 'generation counter is not strictly increasing');
    assert.deepEqual(widths, [EXPECTED_WIDTH], `frame widths: ${JSON.stringify(widths)}`);
    assert.deepEqual(heights, [EXPECTED_HEIGHT], `frame heights: ${JSON.stringify(heights)}`);
    assert.deepEqual(pixelFormats, [EXPECTED_FORMAT], `pixel formats: ${JSON.stringify(pixelFormats)}`);
    assert.deepEqual(byteLengths, [EXPECTED_BYTES], `frame payloads must be tight RGBA8 of ${EXPECTED_BYTES} bytes, got: ${JSON.stringify(byteLengths)}`);
    assert.equal(summary.missingPayloadFrames, 0, `'vita-frame' without pixel payload (worker must post {type:'vita-frame', ..., data: ArrayBuffer} and include that buffer in the transfer list); last such message keys: ${JSON.stringify(summary.lastMissingPayloadKeys)}`);
    assert.ok(summary.distinctChecksums >= MIN_DISTINCT_CHECKSUMS, `distinct frame checksums: ${summary.distinctChecksums} < ${MIN_DISTINCT_CHECKSUMS} — framebuffer content is not changing`);
    assert.ok(summary.statusReplies.length >= 1, 'worker did not answer the status query — worker is not alive after frames');

    // Frame counter: decode the fixture's encoded frame index and require it
    // to strictly increase across the received frames.
    const schemeIndex = COUNTER_SCHEME_NAMES.findIndex((_, i) => {
      const values = summary.frames.map((f) => f.counters[i]);
      return values.length >= MIN_FRAMES
        && values.every((v) => Number.isInteger(v) && v >= 0 && v < (1 << 20))
        && values.every((v, idx) => idx === 0 || v > values[idx - 1])
        && values[values.length - 1] - values[0] >= values.length - 1;
    });
    if (schemeIndex < 0) {
      const dump = COUNTER_SCHEME_NAMES.map((name, i) => `${name}: first=${summary.frames[0]?.counters[i]} last=${summary.frames.at(-1)?.counters[i]}`);
      throw new Error(`could not decode an incrementing frame counter from the encoded pixel region (tried: ${dump.join('; ')}) — fixture encoding contract mismatch; first frame head: ${summary.firstFrameHeadHex}; last frame head: ${summary.lastFrameHeadHex}`);
    }
    const counters = summary.frames.map((f) => f.counters[schemeIndex]);
    diagnosis.summary.counterScheme = COUNTER_SCHEME_NAMES[schemeIndex];
    diagnosis.summary.counterFirst = counters[0];
    diagnosis.summary.counterLast = counters.at(-1);

    assert.ok(summary.exit, 'no vita-exit received');
    assert.equal(summary.exit.ok, true, `vita-exit not ok: ${JSON.stringify(summary.exit)}`);
    assert.equal(summary.exit.exitCode, EXPECT_EXIT, `exit code ${summary.exit.exitCode} !== expected ${EXPECT_EXIT} (display fixture completion code is 77; override with DISPLAY_SMOKE_EXPECT_EXIT)`);
    assert.ok(screenshotSaved, `screenshot not written: ${SCREENSHOT_PATH}`);
  } catch (error) {
    fail(error.message, diagnosis);
  }

  if (!failed) console.log('[display-smoke] PASS', JSON.stringify(diagnosis.summary, null, 2));
} catch (error) {
  fail(error && error.stack ? error.stack : String(error));
} finally {
  await browser?.close();
  server.close();
}
