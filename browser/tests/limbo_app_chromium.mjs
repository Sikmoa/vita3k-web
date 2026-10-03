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
//   GXM_RUNTIME_DIST        built dist (default build/web64/dist)
//   LIMBO_STAGE             staged content root (default .limbo_work/stage)
//   LIMBO_TITLE             title id (default PCSE00268)
//   LIMBO_APP               app0 directory name (default the title id)
//   LIMBO_FRAME_OUT         PNG prefix (default .limbo_work/limbo_frame)
//   LIMBO_FRAME_EVERY       save every Nth generation (default 30; never 1)
//   LIMBO_MAX_FRAMES        maximum saved frames (default 8)
//   LIMBO_DEADLINE_MS       overall guest deadline (default 600000)
//   LIMBO_INLINE_MUTEX      0 disables inline lock/unlock for a matched baseline
//   PLAYWRIGHT_CHROMIUM_EXECUTABLE  headless Chromium executable path
//   LIMBO_GPU=1             drop the SwiftShader flags (use a real GPU)
//   LIMBO_HEADED=1          headed browser (headless Chrome only has SwiftShader WebGPU)
//   LIMBO_GUEST_CORES=N     emulated guest CPU cores (default 3; 1 = single-core scheduler)
//   LIMBO_FPS_HACK=1        Vita3K fps-hack: display waits use one vblank
//   LIMBO_TEXTURE_VERIFY=1  verify cached textures against guest memory
//   LIMBO_PATCHES=0         skip the title patches of browser/patches (staged as patch/)
//   LIMBO_SCALE=N           internal resolution multiplier (default 2)
//   LIMBO_SURFACE_SYNC=1    read rendered surfaces back into guest memory
//   LIMBO_GXM_TRACE=N       trace passes, draws, textures and presents after N frames
//   LIMBO_GXM_TRACE_LINES=N trace line budget (default 600)
//   LIMBO_CPU_PROFILE=S:D   Worker CPU profile from S ms for D ms (LIMBO_PROFILE_OUT.cpu.cpuprofile)
//   LIMBO_DIALOG            answer to every guest message dialog (sceMsgDialog):
//                           cross (default; the highlighted first button), circle
//                           (the last of several buttons) or none (leave it open)
//   LIMBO_IME               text typed into every guest on-screen keyboard (SceIme),
//                           then Enter; unset leaves the keyboard open
//   LIMBO_AOT               ahead-of-time module to supply to the run (AOT.md)
//   LIMBO_AOT_DIR           per-title images (<TITLE>.aot.wasm); preferred,
//                           on by default for the run's title when present
//   LIMBO_LOG_OUT           write the retained worker log tail (4000 lines) to this file
//                           (and translated programs, with LIMBO_GXM_TRACE, to <file>.programs)
//   LIMBO_WORKER_PARAMS     extra raw query parameters appended to the worker URL
import { createServer } from 'node:http';
import { readFile, stat, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { existsSync, readFileSync } from 'node:fs';
import { deflateSync } from 'node:zlib';
import assert from 'node:assert/strict';
import { readRuntimeFile, runtimeRoot, sendStageFile, stageFiles, stageManifest } from './runtime_routes.mjs';

const stage = resolve(process.env.LIMBO_STAGE || '.limbo_work/stage');
const title = process.env.LIMBO_TITLE || 'PCSE00268';
const app = process.env.LIMBO_APP || title;
const frameOut = process.env.LIMBO_FRAME_OUT || '.limbo_work/limbo_frame';
const frameEvery = Number(process.env.LIMBO_FRAME_EVERY || 30);
const maxFrames = Number(process.env.LIMBO_MAX_FRAMES || 8);
// Uncapped vblank measures headroom; LIMBO_FAST_VBLANK=0 paces at 60 Hz.
const fastVblank = process.env.LIMBO_FAST_VBLANK !== '0';
const hleProfile = process.env.LIMBO_HLE_PROFILE === '1';
// LIMBO_MEASURE=1: fixed measurement scenario. The page tracks the game phase
// (loading screen, then gameplay: mostly mid-grey fog instead of white text
// on black), profiles the Worker's CPU for 3 progress reports (~15 s) on each,
// presses cross every 5 s after the loading window and holds the left stick
// right once gameplay starts. The run ends after the gameplay window.
const measure = process.env.LIMBO_MEASURE === '1';
// LIMBO_CPU_PROFILE=<start ms>:<duration ms>: one Worker CPU profile of that
// window after the page loads, written to LIMBO_PROFILE_OUT.cpu.cpuprofile.
const cpuWindow = (process.env.LIMBO_CPU_PROFILE || '').split(':').map(Number);
// LIMBO_PROFILE=0 keeps the measurement windows but skips the CPU profiler,
// which itself costs guest throughput.
const profileWindows = process.env.LIMBO_PROFILE !== '0';
// LIMBO_PROFILE=alloc samples allocations (HeapProfiler) instead of CPU time;
// LIMBO_PROFILE_OUT then receives <name>.heapprofile.
const allocationProfile = process.env.LIMBO_PROFILE === 'alloc';
// Scripted pad input: LIMBO_INPUT="<ms>:<input>[+<input>]:<hold ms>,..." with
// times relative to run-app, e.g. "30000:cross:200,32000:lstick-right:5000".
const ctrlButtons = { select: 0x1, start: 0x8, up: 0x10, right: 0x20, down: 0x40, left: 0x80,
  l: 0x100, r: 0x200, triangle: 0x1000, circle: 0x2000, cross: 0x4000, square: 0x8000 };
const stickAxes = { 'lstick-left': [0, -1], 'lstick-right': [0, 1], 'lstick-up': [1, -1], 'lstick-down': [1, 1] };
const inputScript = (process.env.LIMBO_INPUT || '').split(',').filter(Boolean).map((entry) => {
  const [at, names, hold] = entry.split(':');
  let mask = 0;
  const axes = [0, 0, 0, 0];
  for (const name of names.split('+')) {
    if (name in stickAxes) { const [axis, value] = stickAxes[name]; axes[axis] = value; continue; }
    assert(name in ctrlButtons, `unknown input ${name} in LIMBO_INPUT`);
    mask |= ctrlButtons[name];
  }
  return { at: Number(at), mask, axes, hold: Number(hold || 200) };
});
const deadlineMs = Number(process.env.LIMBO_DEADLINE_MS || 600000);
const inlineMutex = process.env.LIMBO_INLINE_MUTEX !== '0';
// A/B switch for the code-page write observer: default ON (load-bearing for
// correctness). LIMBO_WRITE_OBSERVER=0 disables the page-walk/epoch bump and is
// a diagnostic state used to isolate the versioning build's run_js/emit
// regression; it is NOT a valid shipping config.
const writeObserver = process.env.LIMBO_WRITE_OBSERVER !== '0';
// Region-cache size A/B: unset keeps the built-in default, a number overrides.
const regionCache = process.env.LIMBO_REGION_CACHE || '';
const aotPath = process.env.LIMBO_AOT ? resolve(process.env.LIMBO_AOT) : '';
const aotDir = process.env.LIMBO_AOT_DIR ? resolve(process.env.LIMBO_AOT_DIR) : '';
const titleIdOk = (value) => /^[A-Za-z0-9_-]{1,24}$/.test(value);
const aotFileForProbe = () => {
  if (aotDir && titleIdOk(title)) {
    const candidate = resolve(aotDir, `${title}.aot.wasm`);
    if (existsSync(candidate)) return candidate;
  }
  return aotPath;
};
const aotProbePath = aotFileForProbe();
if (aotProbePath && !existsSync(aotProbePath)) throw new Error(`no AOT module at ${aotProbePath}`);
const aotProbeUrl = aotProbePath ? `/aot/${title}.wasm` : null;
const dialogAnswer = process.env.LIMBO_DIALOG || 'cross';
assert(['cross', 'circle', 'none'].includes(dialogAnswer), `LIMBO_DIALOG=${dialogAnswer}: use cross, circle or none`);
const imeAnswer = process.env.LIMBO_IME ?? null;

if (frameEvery === 1)
  throw new Error('LIMBO_FRAME_EVERY=1 saves no files (generations start at 1); use 2 or more');
if (!existsSync(resolve(stage, 'ux0/app', app, 'eboot.bin')))
  throw new Error(`no staged app at ${resolve(stage, 'ux0/app', app, 'eboot.bin')}`);

const staged = (await stageFiles(stage)).filter((file) => process.env.LIMBO_PATCHES !== '0' || !file.path.startsWith('patch/'));
const manifestBytes = new TextEncoder().encode(JSON.stringify(stageManifest(staged)));

const server = createServer(async (req, res) => {
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  try {
    const path = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
    const send = (content, type) => {
      res.writeHead(200, { 'Content-Type': type, 'Content-Length': content.length, 'Cache-Control': 'no-store' });
      res.end(content);
    };
    if (path === '/') {
      send(new TextEncoder().encode('<!doctype html><title>Limbo app probe</title>'), 'text/html');
      return;
    }
    if (path === '/manifest.json') { send(manifestBytes, 'application/json'); return; }
    const sendAot = async (file) => {
      const { size, mtimeMs } = await stat(file);
      const etag = `"${size.toString(36)}-${Math.floor(mtimeMs).toString(36)}"`;
      if (req.headers['if-none-match'] === etag) {
        res.writeHead(304, { ETag: etag, 'Cache-Control': 'no-cache' });
        res.end();
        return;
      }
      res.writeHead(200, { 'Content-Type': 'application/wasm', 'Content-Length': size,
        'Cache-Control': 'no-cache', ETag: etag });
      res.end(await readFile(file));
    };
    if (path === '/aot.wasm' && aotPath) { await sendAot(aotPath); return; }
    if (aotProbeUrl && path === aotProbeUrl) { await sendAot(aotProbePath); return; }
    if (path.startsWith('/stage/')) {
      await sendStageFile(req, res, staged, path.slice('/stage/'.length));
      return;
    }
    const { content, type } = await readRuntimeFile(path);
    send(content, type);
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
// CPU profile of the Worker -> milliseconds per category. Each sample goes to
// the innermost frame with a category, so libc called from HLE counts as HLE.
// Wasm frames of a module built without a name section (no --profiling-funcs)
// arrive as wasm-function[N]; the linker's symbol map next to the build
// (vita3k_web_jit.js.symbols, not staged into dist) names them.
const symbolMaps = new Map();
function wasmFunctionName(url, name) {
  const index = /^wasm-function\[(\d+)\]$/.exec(name)?.[1];
  const module = /\/(vita3k_web(?:_jit)?)\.wasm$/.exec(url)?.[1];
  if (index === undefined || !module) return name;
  if (!symbolMaps.has(module)) {
    const names = new Map();
    const file = resolve(runtimeRoot, '../browser', `${module}.js.symbols`);
    if (existsSync(file)) {
      for (const line of readFileSync(file, 'utf8').split('\n')) {
        const colon = line.indexOf(':');
        if (colon > 0) names.set(line.slice(0, colon),
          line.slice(colon + 1).replace(/\\([0-9a-f]{2})/g, (_, hex) => String.fromCharCode(parseInt(hex, 16))));
      }
    }
    symbolMaps.set(module, names);
  }
  return symbolMaps.get(module).get(index) ?? name;
}
function categorize(frame) {
  const { url } = frame;
  const name = wasmFunctionName(url, frame.functionName);
  if (name === '(idle)') return 'idle';
  if (name === '(garbage collector)') return 'gc';
  if (name === '(program)' || name === '(root)') return null;
  if (url.endsWith('/aot.wasm')) return 'guest-aot';
  if (url.startsWith('wasm://')) return 'guest-lazy-jit';
  if (url.endsWith('.wasm')) {
    if (/asyncify|emscripten_fiber|GuestFiberScheduler|GuestThreadRuntime|KernelExecutionHost/i.test(name)) return 'host-scheduler';
    if (/renderer::|gxm_webgpu|scene::|decode_texture|bind_texture|consume_|submit_command_list|XXH|swizzled_texture|tiled_texture|gxp|GXM|Gxm/.test(name)) return 'host-scene';
    if (/WasmJitCPU|wasmjit|Dynarmic|execute_regions|run_slice|AotRuntime/i.test(name)) return 'host-jit';
    if (/export_|call_import|^sce|::sce|_sce|Kernel|kernel|ThreadState|SceAudio|audio|io::|open_file|read_file|ctrl|display|Display|Semaphore|Mutex|EventFlag|lwmutex|vblank/.test(name)) return 'host-hle';
    return null; // libc and helpers: attributed to the caller
  }
  if (url.endsWith('gxm_scene.js')) return 'js-webgpu';
  if (/vita3k_web_jit\.js|vita3k_web\.js/.test(url)) {
    if (/^invoke_/.test(name)) return 'js-invoke';
    if (/Asyncify|handleSleep|handleAsync|fiber|Rewind|Unwind|trampoline|maybeStopUnwind/i.test(name)) return 'js-asyncify';
    return 'js-glue';
  }
  if (url.endsWith('worker.js')) return 'js-worker';
  return url ? 'js-other' : null;
}
function summarizeProfile(profile) {
  const nodes = new Map(profile.nodes.map((node) => [node.id, node]));
  const parent = new Map();
  for (const node of profile.nodes) for (const child of node.children ?? []) parent.set(child, node.id);
  const categoryOf = new Map();
  // JS frames own only their self time: a JS frame between Wasm frames (an
  // invoke_* exception wrapper, an EM_JS call) is transparent for its callees.
  const isJs = (category) => category?.startsWith('js-');
  const ancestor = (id) => {
    if (categoryOf.has(id)) return categoryOf.get(id);
    const own = categorize(nodes.get(id).callFrame);
    const category = own && !isJs(own) ? own : parent.has(id) ? ancestor(parent.get(id)) : 'host-other';
    categoryOf.set(id, category);
    return category;
  };
  const resolve = (id) => {
    const own = categorize(nodes.get(id).callFrame);
    return own && isJs(own) ? own : ancestor(id);
  };
  const totals = {};
  let totalMs = 0;
  profile.samples.forEach((id, i) => {
    const ms = (profile.timeDeltas[i + 1] ?? 0) / 1000;
    totals[resolve(id)] = (totals[resolve(id)] ?? 0) + ms;
    totalMs += ms;
  });
  const top = {};
  const self = new Map();
  profile.samples.forEach((id, i) => self.set(id, (self.get(id) ?? 0) + (profile.timeDeltas[i + 1] ?? 0) / 1000));
  for (const [id, ms] of [...self].sort((a, b) => b[1] - a[1]).slice(0, 25)) {
    const frame = nodes.get(id).callFrame;
    top[`${wasmFunctionName(frame.url, frame.functionName) || '(anonymous)'} @${frame.url.split('/').pop()}`] = Math.round(ms);
  }
  return { totalMs: Math.round(totalMs),
    categories: Object.fromEntries(Object.entries(totals).sort((a, b) => b[1] - a[1]).map(([k, v]) => [k, Math.round(v)])),
    topSelf: top };
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
    // --enable-gpu permits the hardware adapter in headless Chrome too;
    // LIMBO_HEADED=1 opens a window for interactive debugging.
    headless: process.env.LIMBO_HEADED !== '1',
    args: process.env.LIMBO_GPU === '1' ? ['--enable-unsafe-webgpu', '--enable-gpu', '--enable-features=Vulkan']
      : ['--enable-unsafe-webgpu', '--use-angle=swiftshader', '--enable-features=Vulkan',
        '--disable-vulkan-surface'],
    ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE
      ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}),
  });
  const page = await browser.newPage();
  const pageErrors = [];
  page.on('pageerror', (error) => pageErrors.push(String(error)));
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
  const measurements = {};
  if (measure || cpuWindow.length === 2) {
    // Playwright has no CDP session for a Worker: use the page session's
    // legacy (non-flat) auto-attach and message relay to reach it.
    const cdp = await page.context().newCDPSession(page);
    let workerSession = null, nextId = 0;
    const pending = new Map();
    cdp.on('Target.attachedToTarget', (event) => {
      if (event.targetInfo.type === 'worker') workerSession = event.sessionId;
    });
    cdp.on('Target.receivedMessageFromTarget', (event) => {
      const message = JSON.parse(event.message);
      pending.get(message.id)?.(message);
      pending.delete(message.id);
    });
    await cdp.send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: false });
    const workerCall = (method, params = {}) => new Promise((resolveCall, rejectCall) => {
      if (!workerSession) { rejectCall(new Error('worker target not attached')); return; }
      const id = ++nextId;
      pending.set(id, (message) => message.error ? rejectCall(new Error(message.error.message)) : resolveCall(message.result));
      cdp.send('Target.sendMessageToTarget', { sessionId: workerSession, message: JSON.stringify({ id, method, params }) })
        .catch(rejectCall);
    });
    if (cpuWindow.length === 2) {
      setTimeout(async () => {
        try {
          await workerCall('Profiler.enable');
          await workerCall('Profiler.setSamplingInterval', { interval: 500 });
          await workerCall('Profiler.start');
          setTimeout(async () => {
            const { profile } = await workerCall('Profiler.stop');
            await writeFile(`${process.env.LIMBO_PROFILE_OUT || 'limbo'}.cpu.cpuprofile`, JSON.stringify(profile));
          }, cpuWindow[1]);
        } catch (error) { console.error('cpu profile:', error); }
      }, cpuWindow[0]);
    }
    if (measure) await page.exposeFunction('limboMeasure', async ({ kind, name }) => {
      if (!profileWindows) return;
      if (allocationProfile) {
        if (kind === 'start') {
          await workerCall('HeapProfiler.enable');
          await workerCall('HeapProfiler.startSampling', { samplingInterval: 16384,
            includeObjectsCollectedByMajorGC: true, includeObjectsCollectedByMinorGC: true });
        } else {
          const { profile } = await workerCall('HeapProfiler.stopSampling');
          if (process.env.LIMBO_PROFILE_OUT)
            await writeFile(`${process.env.LIMBO_PROFILE_OUT}.${name}.heapprofile`, JSON.stringify(profile));
        }
        return;
      }
      if (kind === 'start') {
        await workerCall('Profiler.enable');
        await workerCall('Profiler.setSamplingInterval', { interval: 500 });
        await workerCall('Profiler.start');
      } else {
        const { profile } = await workerCall('Profiler.stop');
        measurements[name] = summarizeProfile(profile);
        if (process.env.LIMBO_PROFILE_OUT)
          await writeFile(`${process.env.LIMBO_PROFILE_OUT}.${name}.cpuprofile`, JSON.stringify(profile));
      }
    });
  }

  const outcome = await page.evaluate(async ({ title, app, frameEvery, maxFrames, deadlineMs, inlineMutex, regionCache, writeObserver, useAot, aotUrl, fastVblank, hleProfile, inputScript, measure, guestCores, fpsHack, textureVerify, scale, surfaceSync, gxmTrace, gxmTraceLines, pcSample, workerParams, dialogAnswer, ctrlButtons, imeAnswer }) => {
    const worker = new Worker(`./worker.js?backend=jit&memory=w64&inlineMutex=${inlineMutex ? '1' : '0'}&regionCache=${encodeURIComponent(regionCache)}&writeObserver=${writeObserver ? '1' : '0'}&readback=${frameEvery}${hleProfile ? '&hleProfile=1' : ''}${guestCores ? `&cores=${guestCores}` : ''}${fpsHack ? '&fpsHack=1' : ''}${textureVerify ? '&textureVerify=1' : ''}${scale ? '&scale=' + scale : ''}${surfaceSync ? '&surfaceSync=1' : ''}${gxmTrace ? '&gxmTrace=' + gxmTrace : ''}${gxmTraceLines ? '&gxmTraceLines=' + gxmTraceLines : ''}${pcSample ? '&pcSample=' + pcSample : ''}${workerParams ? '&' + workerParams : ''}`, { type: 'module' });
    const state = { logs: [], programs: [], logCount: 0, frames: [], saved: [], staged: null, exit: null,
      backend: null, memory: null, workerErrors: [], ready: false, timedOut: false,
      gxmSceneStats: null, gxmFailures: [], gxmSkips: [],
      latestProgress: null, profiles: {}, jitThreads: {}, runStartedAt: null, dialogs: [], imes: [],
      phase: 'boot', phaseTrace: [], windows: {}, lastElapsed: 0, latestScene: null,
      audio: { chunks: 0, bytes: 0, peak: 0, nonzero: 0, scanned: 0, freqs: {}, channels: {}, first: null } };
    const hex = (bytes) => Array.from(bytes.slice(0, 64), (v) => v.toString(16).padStart(2, '0')).join(' ');
    // Measurement scenario (LIMBO_MEASURE=1); see the option's comment.
    let phaseSince = 0, grey = 0, pressTimer = null, finishRun = null, framesAtReport = 0, steadySince = null;
    const post = (message) => worker.postMessage(message);
    const setPhase = (phase) => {
      state.phase = phase;
      phaseSince = state.lastElapsed;
      state.phaseTrace.push({ phase, elapsed: state.lastElapsed, sinceRunMs: Math.round(performance.now() - state.runStartedAt) });
      if (phase === 'gameplay') {
        clearInterval(pressTimer);
        setTimeout(() => post({ type: 'input', buttons: 0, axes: [1, 0, 0, 0] }), 3000);
      }
    };
    const observeFrame = (pixels) => {
      if (state.phase === 'boot') setPhase('loading');
      if (!pixels || state.phase !== 'loading') return;
      let mid = 0, count = 0;
      for (let i = 0; i < pixels.length; i += 4 * 37, ++count)
        if (pixels[i] > 24 && pixels[i] < 200) ++mid;
      grey = mid / count > 0.35 ? grey + 1 : 0;
      if (grey >= 2) setPhase('gameplay');
    };
    const measureStep = async (elapsed, line) => {
      // The loading screen counts once it animates (>= 1 frame/s): before
      // that the main thread is still booting the title.
      const framesNow = state.frames.length;
      if (state.phase === 'loading' && steadySince === null && framesNow - framesAtReport >= 5)
        steadySince = elapsed;
      framesAtReport = framesNow;
      for (const name of ['loading', 'gameplay']) {
        const window_ = state.windows[name] ??= { reports: 0 };
        if (window_.done) continue;
        if (!window_.start) {
          const since = name === 'loading' ? steadySince : phaseSince;
          const settle = name === 'loading' ? 5 : 10;
          if (state.phase !== name || since === null || elapsed < since + settle) continue;
          Object.assign(window_, { start: line, startScene: state.latestScene, startProfile: state.latestJitProfile,
            startAt: performance.now(), startFrames: state.frames.length });
          await window.limboMeasure({ kind: 'start', name });
          continue;
        }
        if (++window_.reports < 3) continue;
        Object.assign(window_, { end: line, endScene: state.latestScene, endProfile: state.latestJitProfile,
          endAt: performance.now(), endFrames: state.frames.length, done: true });
        window_.fps = (window_.endFrames - window_.startFrames) * 1000 / (window_.endAt - window_.startAt);
        await window.limboMeasure({ kind: 'stop', name });
        if (name === 'loading')
          pressTimer = setInterval(() => {
            post({ type: 'input', buttons: 0x4000 });
            setTimeout(() => post({ type: 'input', buttons: 0 }), 300);
          }, 5000);
        else
          finishRun();
      }
    };
    const result = await new Promise((resolveRun, rejectRun) => {
      // A deadline is a diagnostic outcome, not a failure: the host needs the
      // guest logs and the exit state to name the next blocker.
      const starter = setTimeout(() => { state.timedOut = true; finish(null, state); }, deadlineMs);
      const finish = (error, value) => { clearTimeout(starter); worker.terminate();
        error ? rejectRun(error) : resolveRun(value); };
      finishRun = () => finish(null, state);
      worker.onerror = (event) => finish(new Error(`worker error: ${event.message} at ${event.filename}:${event.lineno}:${event.colno}; last logs: ${state.logs.slice(-15).join(" | ")}`));
      worker.onmessage = ({ data }) => {
        if (!data || typeof data !== 'object') return;
        switch (data.type) {
        case 'log': {
          const message = String(data.message);
          state.logs.push(message);
          // Translated programs and pipelines (?gxmTrace) arrive long before the traced frames.
          if (/^\[gxm-trace\] (program|pipeline) /.test(message)) state.programs.push(message);
          state.logCount += 1;
          // Renderer diagnostics are tracked here: the log keeps only its tail.
          if (message.startsWith('[gxm-scene] stats '))
            state.gxmSceneStats = JSON.parse(message.slice('[gxm-scene] stats '.length));
          if (/^\[vita3k-web\] GX[MP] .* failed|^\[gxm-scene\] WebGPU validation:/.test(message))
            state.gxmFailures = [...state.gxmFailures.slice(-2), message];
          if (message.startsWith('[gxm-skip]')) state.gxmSkips.push(message);
          if (message.includes('[gxm] scene_ms')) state.latestScene = message;
          if (message.includes('jit profile')) state.latestJitProfile = message;
          if (message.includes('jit[progress]')) {
            state.latestProgress = message;
            const elapsed = Number(/elapsed=([\d.]+)s/.exec(message)?.[1] ?? 0);
            state.lastElapsed = elapsed;
            if (measure) measureStep(elapsed, message);
          }
          const profile = message.match(/jit profile (\d+):/);
          if (profile) state.profiles[profile[1]] = message;
          const thread = message.match(/jit thread=(\d+) /);
          if (thread) state.jitThreads[thread[1]] = message;
          if (state.logs.length > 4000) state.logs.splice(0, state.logs.length - 4000);
          break;
        }
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
          state.runStartedAt = performance.now();
          worker.postMessage({ type: 'run-app', vitaFs: data.root, title, app,
            fastVblank, ...(useAot && aotUrl ? { aotUrl } : {}) });
          // At every press/release boundary send the combined state of all
          // inputs held then, so releasing one input keeps the others held.
          const boundaries = [...new Set(inputScript.flatMap(({ at, hold }) => [at, at + hold]))];
          for (const time of boundaries) {
            let buttons = 0;
            const axes = [0, 0, 0, 0];
            for (const input of inputScript) {
              if (time < input.at || time >= input.at + input.hold) continue;
              buttons |= input.mask;
              input.axes.forEach((value, axis) => { if (value) axes[axis] = value; });
            }
            setTimeout(() => worker.postMessage({ type: 'input', buttons, axes }), time);
          }
          break;
        case 'vita-present':
          if (measure) observeFrame(null);
          // GPU-presented frame without a pixel read-back (gxm_scene.js).
          state.frames.push({ generation: data.generation, width: data.width, height: data.height,
            sinceRunMs: Math.round(performance.now() - state.runStartedAt), at: Math.round(performance.now()) });
          break;
        case 'vita-frame': {
          const pixels = new Uint8Array(data.data);
          if (measure) observeFrame(pixels);
          // Retain page-relative time for compatibility and report the actual
          // elapsed time since run-app separately (excludes staging).
          const record = { generation: data.generation, width: data.width, height: data.height,
            sinceRunMs: Math.round(performance.now() - state.runStartedAt),
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
        case 'vita-audio': {
          // PCM content audit: the page cannot prove audibility headless, but
          // peak/nonzero over the run separates "guest sends silence" from
          // "page drops sound". Transferable buffer; scan and release.
          const pcm = new Int16Array(data.data);
          const a = state.audio;
          a.chunks += 1; a.bytes += pcm.length * 2;
          a.freqs[data.freq] = (a.freqs[data.freq] || 0) + 1;
          a.channels[data.channels] = (a.channels[data.channels] || 0) + 1;
          let peak = 0, nonzero = 0;
          for (let i = 0; i < pcm.length; i++) {
            const v = Math.abs(pcm[i]);
            if (v > peak) peak = v;
            if (v > 100) ++nonzero;
          }
          if (peak > a.peak) a.peak = peak;
          a.nonzero += nonzero; a.scanned += pcm.length;
          if (!a.first) a.first = { freq: data.freq, channels: data.channels,
            frames: data.frames, samples: pcm.length, peak, nonzero };
          break;
        }
        case 'vita-dialog': {
          // Every dialog is logged; LIMBO_DIALOG answers it as a pad press.
          const dialog = data.dialog;
          const sinceRunMs = Math.round(performance.now() - state.runStartedAt);
          if (dialog.state === 'close') {
            const entry = state.dialogs.find((d) => d.id === dialog.id);
            if (entry) Object.assign(entry, { closedMs: sinceRunMs, buttonId: dialog.buttonId, result: dialog.result });
            state.logs.push(`[limbo-probe] dialog ${dialog.id} closed: buttonId=${dialog.buttonId} result=${dialog.result}`);
            break;
          }
          if (dialog.state !== 'open') break;
          state.dialogs.push({ id: dialog.id, message: dialog.message, buttons: dialog.buttons,
            progress: dialog.progress, openedMs: sinceRunMs, answer: dialogAnswer });
          state.logs.push(`[limbo-probe] dialog ${dialog.id}: ${JSON.stringify(dialog.message)} [${dialog.buttons.join(', ')}] answer=${dialogAnswer}`);
          if (dialogAnswer !== 'none')
            worker.postMessage({ type: 'dialog-press', id: dialog.id, button: ctrlButtons[dialogAnswer], selected: 0 });
          break;
        }
        case 'vita-ime': {
          // Every keyboard is logged; LIMBO_IME types its text and presses Enter.
          const ime = data.ime;
          const sinceRunMs = Math.round(performance.now() - state.runStartedAt);
          if (ime.state === 'close') {
            const entry = state.imes.find((i) => i.id === ime.id);
            if (entry) entry.closedMs = sinceRunMs;
            state.logs.push(`[limbo-probe] ime ${ime.id} closed`);
            break;
          }
          state.imes.push({ id: ime.id, text: ime.text, maxLength: ime.maxLength, type: ime.type,
            enterLabel: ime.enterLabel, openedMs: sinceRunMs, answer: imeAnswer });
          state.logs.push(`[limbo-probe] ime ${ime.id}: ${JSON.stringify(ime.text)} max=${ime.maxLength} answer=${JSON.stringify(imeAnswer)}`);
          if (imeAnswer !== null) {
            worker.postMessage({ type: 'ime-input', input: { id: ime.id, kind: 0, text: imeAnswer, caret: imeAnswer.length } });
            worker.postMessage({ type: 'ime-input', input: { id: ime.id, kind: 1 } });
          }
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
  }, { title, app, frameEvery, maxFrames, deadlineMs, inlineMutex, regionCache, writeObserver, useAot: Boolean(aotProbePath), aotUrl: aotProbeUrl, fastVblank, hleProfile, inputScript, measure, guestCores: process.env.LIMBO_GUEST_CORES || '', fpsHack: process.env.LIMBO_FPS_HACK === '1', textureVerify: process.env.LIMBO_TEXTURE_VERIFY === '1', scale: process.env.LIMBO_SCALE || '', surfaceSync: process.env.LIMBO_SURFACE_SYNC === '1', gxmTrace: process.env.LIMBO_GXM_TRACE || '', gxmTraceLines: process.env.LIMBO_GXM_TRACE_LINES || '', pcSample: process.env.LIMBO_PC_SAMPLE || '', workerParams: process.env.LIMBO_WORKER_PARAMS || '', dialogAnswer, ctrlButtons, imeAnswer });

  const saved = [];
  for (const frame of outcome.saved) {
    const path = `${frameOut}_${String(frame.generation).padStart(6, '0')}_` +
      `${frame.width}x${frame.height}.png`;
    await writeFile(path, png(frame.width, frame.height, Buffer.from(frame.base64, 'base64')));
    saved.push({ path, sinceRunMs: frame.sinceRunMs });
  }

  const diagnostics = {
    backend: outcome.backend,
    memory: outcome.memory,
    inlineMutex,
    writeObserver,
    regionCache: regionCache || '(default)',
    latestProgress: outcome.latestProgress,
    jitThreads: outcome.jitThreads,
    profiles: outcome.profiles,
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
    gxmSceneStats: outcome.gxmSceneStats,
    audio: outcome.audio,
    gxmFailures: outcome.gxmFailures,
    gxmSkips: outcome.gxmSkips,
    dialogs: outcome.dialogs,
    imes: outcome.imes,
    moduleLoads: outcome.logs.filter((line) => line.includes('load_module')).slice(-12),
    threadErrors: outcome.logs.filter((line) => line.includes('failed:')).slice(-6),
    scheduler: outcome.logs.filter((line) => line.includes('Guest scheduler')).slice(-3),
    logTail: outcome.logs.slice(-15),
    ...(measure ? { phases: outcome.phaseTrace, windows: outcome.windows, measurements } : {}),
  };
  if (process.env.LIMBO_LOG_OUT)
    await writeFile(process.env.LIMBO_LOG_OUT, outcome.logs.join('\n') + '\n');
  if (process.env.LIMBO_LOG_OUT && outcome.programs.length)
    await writeFile(process.env.LIMBO_LOG_OUT + '.programs', outcome.programs.join('\n') + '\n');
  console.log(JSON.stringify(diagnostics, null, 2));

  assert.equal(outcome.workerErrors.length, 0, 'worker must not post errors');
  assert.ok(outcome.staged && outcome.staged.files === staged.length,
    `every staged file must reach MEMFS (${outcome.staged?.files} of ${staged.length})`);
  assert.ok(outcome.frames.length > 0,
    `no frames presented; timedOut=${outcome.timedOut} exit=${JSON.stringify(outcome.exit)} ` +
    `gxmFailures=${JSON.stringify(diagnostics.gxmFailures)} ` +
    `threadErrors=${JSON.stringify(diagnostics.threadErrors)} ` +
    `scheduler=${JSON.stringify(diagnostics.scheduler)} ` +
    `gxmSkips=${JSON.stringify(diagnostics.gxmSkips)}`);
} finally {
  await browser?.close();
  server.close();
}
