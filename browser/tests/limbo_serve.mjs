// Standalone Limbo dev server: serves the browser runtime plus a staged retail
// app to a real browser, and renders the presented frames to a canvas. This is
// the interactive counterpart of limbo_app_chromium.mjs (which drives the same
// routes headlessly); the routes and asset locations are intentionally
// identical so a live session and a probe run exercise the same files.
//
//   node browser/tests/limbo_serve.mjs                  # http://127.0.0.1:8080/
//   PORT=9000 LIMBO_MEMORY=w32 node browser/tests/limbo_serve.mjs
//
// Query parameters: ?backend=jit|interp  ?memory=auto|w64|w32
// ?inlineMutex=0 disables the inline-mutex optimization for A/B testing; ?auto=1 (start
// immediately). Requires a WebGPU browser **on a secure origin**: WebGPU is
// exposed only to secure contexts, so a page served over plain HTTP from a
// non-loopback address has no navigator.gpu and the first draw fails. Serve it
// through a TLS reverse proxy (e.g. Caddy) and open https://<name>/, or forward
// the port and open http://localhost:<PORT>/ (loopback is secure). Both the page
// and the bridge name this reason explicitly instead of failing late.
// A Chromium without a usable GPU needs
// --enable-unsafe-webgpu --enable-unsafe-swiftshader. The page probes
// Memory64 and the worker falls back to the wasm32 module when it is missing,
// but the wasm32 module must have been built from the same tree (the wasm64
// build is the primary one).
//
// Environment:
//   PORT                listen port (default 8080)
//   HOST                listen address (default 127.0.0.1; use 0.0.0.0 when the
//                       port is forwarded or published from outside)
//   LIMBO_STAGE         staged content root (default .limbo_work/stage)
//   LIMBO_TITLE         title id (default PCSE00268)
//   LIMBO_APP           app directory under ux0/app (default: LIMBO_TITLE)
//   GXM_RUNTIME_DIST    built module directory (default build/web64/browser)
//   GXM_SHADER_ASSETS   GXP compiler/Naga/WASI assets (default .limbo_work/gxm)
import { createServer } from 'node:http';
import { readFile, readdir } from 'node:fs/promises';
import { networkInterfaces } from 'node:os';
import { resolve, sep, relative } from 'node:path';
import { existsSync } from 'node:fs';

const port = Number(process.env.PORT || 8080);
const host = process.env.HOST || '127.0.0.1';
const root = resolve(process.env.GXM_RUNTIME_DIST || 'build/web64/browser');
const shaderRoot = resolve(process.env.GXM_SHADER_ASSETS || '.limbo_work/gxm');
const stage = resolve(process.env.LIMBO_STAGE || '.limbo_work/stage');
const title = process.env.LIMBO_TITLE || 'PCSE00268';
const app = process.env.LIMBO_APP || title;

if (!existsSync(resolve(stage, 'ux0/app', app, 'eboot.bin')))
  throw new Error(`no staged app at ${resolve(stage, 'ux0/app', app, 'eboot.bin')} (set LIMBO_STAGE/LIMBO_APP)`);
if (!existsSync(resolve(root, 'vita3k_web_jit.wasm')))
  throw new Error(`no built module at ${root} (build vita3k_web_jit first, or set GXM_RUNTIME_DIST)`);

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
const manifestBytes = new TextEncoder().encode(JSON.stringify(await walk(stage)));

// The host files (Worker and bridges) must all come from one directory: the
// bridges are resolved relative to the Worker's own location.
const hostFiles = ['/worker.js', '/storage.js', '/gxm_hle_bridge.js', '/gxm_renderer.js',
  '/gxp_shader_adapter.js', '/webgpu.js', '/capabilities.js', '/audio_input.js'];

const shaderFiles = {
  '/shaders/gxp_compiler.mjs': resolve(shaderRoot, 'shader-wasm/gxp_compiler.mjs'),
  '/shaders/gxp_compiler.wasm': resolve(shaderRoot, 'shader-wasm/gxp_compiler.wasm'),
  '/shaders/naga.wasm': resolve(shaderRoot, 'node_modules/naga-wasi-cli/wasi/naga.wasm'),
};

const page = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Vita3K Web — Limbo (${title})</title>
<style>
  body { margin: 0; background: #111; color: #ddd; font: 13px/1.45 ui-monospace, monospace; }
  header { padding: 10px 14px; display: flex; gap: 14px; align-items: center; flex-wrap: wrap; }
  canvas { display: block; margin: 0 auto; max-width: 100%; background: #000; image-rendering: pixelated; }
  #log { margin: 0; padding: 10px 14px; height: 30vh; overflow: auto; white-space: pre-wrap; color: #9c9; }
  #warning { display: none; margin: 8px 14px; padding: 8px 10px; background: #4a2222; color: #fdd;
    border-left: 3px solid #f66; max-width: 70ch; }
  button { font: inherit; padding: 4px 12px; }
  #status { color: #fc6; }
</style>
</head>
<body>
<header>
  <strong>Limbo ${title}</strong>
  <button id="run">Run</button>
  <button id="stop" disabled>Stop</button>
  <button id="beep">Beep</button>
  <span id="status">idle</span>
  <span id="stats"></span>
</header>
<div id="warning"></div>
<canvas id="screen" width="960" height="544"></canvas>
<pre id="log"></pre>
<script type="module">
const params = new URLSearchParams(location.search);
const backend = params.get('backend') === 'interp' ? 'interp' : 'jit';
const memory = ['w64', 'w32'].includes(params.get('memory')) ? params.get('memory') : 'auto';
const TITLE = ${JSON.stringify(title)}, APP = ${JSON.stringify(app)};
const screen = document.querySelector('#screen'), ctx = screen.getContext('2d');
const status = document.querySelector('#status'), stats = document.querySelector('#stats');
const logBox = document.querySelector('#log'), runButton = document.querySelector('#run');
const stopButton = document.querySelector('#stop'), warningBox = document.querySelector('#warning');
const beepButton = document.querySelector('#beep');
// WebGPU is exposed only in a secure context; say so before the run instead of
// letting the first draw fail with a device error.
function webgpuProblem() {
  if ('gpu' in navigator) return null;
  if (isSecureContext)
    return 'navigator.gpu is missing: this browser does not expose WebGPU (Chrome: --enable-unsafe-webgpu or chrome://flags; Firefox: dom.webgpu.enabled).';
  return 'navigator.gpu is missing because ' + location.origin + ' is not a secure context. Open this page over https:// (a TLS reverse proxy in front of this server) or as http://localhost:' + location.port + '/ with the port forwarded.';
}
const webgpuBlocked = webgpuProblem();
if (webgpuBlocked) { warningBox.textContent = webgpuBlocked; warningBox.style.display = 'block'; }
let worker = null, frames = 0, firstFrameAt = 0, startedAt = 0;
// Web Audio sink: guest PCM (int16 interleaved; 48 kHz stereo on the MAIN
// port) arrives as transferred ArrayBuffers from the worker (see
// browser/src/hle_audio_null.cpp). AudioBuffers are chained on the context
// clock; when the unpaced guest submits ahead of realtime (bursts while
// loading) the chain is resynced instead of scheduling seconds of latency.
let audioCtx = null, audioNext = 0;
let audioChunks = 0, audioBytes = 0, audioLogged = false, audioFirstAt = 0, audioPeak = 0;
function ensureAudio() {
  if (!audioCtx) {
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return;
    audioCtx = new AC();
    audioNext = 0;
  }
  if (audioCtx.state === 'suspended') audioCtx.resume();
}
function playAudioPCM(freq, channels, frameCount, buffer) {
  if (!audioCtx || !buffer) return;
  const pcm = new Int16Array(buffer);
  if (frameCount <= 0 || channels < 1 || channels > 2 || pcm.length < frameCount * channels) return;
  audioChunks += 1; audioBytes += pcm.length * 2;
  // Rolling peak over the whole run: separates "guest sends silence" (peak
  // stays 0) from "page drops sound" (peak > 0 but nothing audible).
  const m = Math.min(pcm.length, frameCount * channels);
  let chunkPeak = 0;
  for (let i = 0; i < m; i++) { const v = Math.abs(pcm[i]); if (v > chunkPeak) chunkPeak = v; }
  if (chunkPeak > audioPeak) audioPeak = chunkPeak;
  if (audioChunks % 100 === 0) log('audio: chunks=' + audioChunks + ' peak=' + audioPeak + '/32768 ctx=' + audioCtx.state);
  if (!audioLogged) {
    audioLogged = true; audioFirstAt = (performance.now() - startedAt) / 1000;
    const n = Math.min(pcm.length, frameCount * channels);
    let peak = 0, nonzero = 0;
    for (let i = 0; i < n; i++) { const v = Math.abs(pcm[i]); if (v > peak) peak = v; if (v > 100) nonzero++; }
    log('audio: first chunk ch=' + channels + ' freq=' + freq + ' frames=' + frameCount + ' nonzero=' + nonzero + '/' + n + ' peak=' + peak + '/32768 ctx=' + audioCtx.state + ' at=' + audioFirstAt.toFixed(1) + 's');
  }
  const audio = audioCtx.createBuffer(channels, frameCount, freq > 0 ? freq : 48000);
  for (let ch = 0; ch < channels; ch++) {
    const out = audio.getChannelData(ch);
    if (channels === 1) {
      for (let i = 0; i < frameCount; i++) out[i] = pcm[i] / 32768;
    } else {
      for (let i = 0; i < frameCount; i++) out[i] = pcm[i * 2 + ch] / 32768;
    }
  }
  const src = audioCtx.createBufferSource();
  src.buffer = audio;
  src.connect(audioCtx.destination);
  const now = audioCtx.currentTime;
  if (audioNext < now) audioNext = now;
  if (audioNext > now + 1.0) audioNext = now;
  src.start(audioNext);
  audioNext += audio.duration;
}
const log = (text) => {
  const lines = logBox.textContent.split('\\n');
  lines.push(text);
  logBox.textContent = lines.slice(-200).join('\\n');
  logBox.scrollTop = logBox.scrollHeight;
};
const showStats = () => {
  const elapsed = (performance.now() - startedAt) / 1000;
  const audio = audioCtx ? ' audio=chunks=' + audioChunks + ' ' + (audioBytes / 1048576).toFixed(1) + 'MiB peak=' + audioPeak + ' ctx=' + audioCtx.state : ' audio=off';
  if (!frames) { stats.textContent = 'elapsed=' + elapsed.toFixed(1) + 's' + audio; return; }
  stats.textContent = 'frames=' + frames + ' elapsed=' + elapsed.toFixed(1) + 's first=' + (firstFrameAt / 1000).toFixed(1) + 's' + audio;
};
function stop(keepsStatus) {
  worker?.terminate(); worker = null;
  runButton.disabled = false; stopButton.disabled = true;
  if (!keepsStatus) status.textContent = 'stopped';
}
beepButton.onclick = () => {
  ensureAudio();
  if (!audioCtx) { log('audio: no AudioContext in this browser'); return; }
  log('audio: beep ctx=' + audioCtx.state);
  const osc = audioCtx.createOscillator(), gain = audioCtx.createGain();
  osc.frequency.value = 660; gain.gain.value = 0.2;
  osc.connect(gain); gain.connect(audioCtx.destination);
  osc.start(); osc.stop(audioCtx.currentTime + 0.25);
};
async function run() {
  stop();
  frames = 0; firstFrameAt = 0; logBox.textContent = ''; startedAt = performance.now();
  audioChunks = 0; audioBytes = 0; audioLogged = false; audioFirstAt = 0;
  status.textContent = 'loading module…';
  if (webgpuBlocked) log('warning: ' + webgpuBlocked);
  runButton.disabled = true; stopButton.disabled = false;
  ensureAudio();
  worker = new Worker(\`./worker.js?backend=\${backend}&memory=\${memory}&inlineMutex=\${params.get('inlineMutex') === '0' ? '0' : '1'}\`, { type: 'module' });
  worker.onerror = (event) => { log('worker error: ' + event.message); status.textContent = 'worker error'; };
  worker.onmessage = async ({ data }) => {
    if (!data || typeof data !== 'object') return;
    switch (data.type) {
      case 'lifecycle': log('lifecycle: ' + data.state); break;
      case 'ready':
        status.textContent = 'staging content…';
        log(\`ready (backend=\${data.diagnostics?.backend} memory=\${data.diagnostics?.memoryModel} inlineMutex=\${data.diagnostics?.inlineMutex})\`);
        try {
          const files = await (await fetch('/manifest.json')).json();
          worker.postMessage({ type: 'stage-files', root: '/vita',
            files: files.map((file) => ({ ...file, url: \`/stage/\${file.path}\` })) });
        } catch (error) { log('manifest failed: ' + error); status.textContent = 'staging failed'; }
        break;
      case 'staged':
        status.textContent = 'running';
        log(\`staged \${data.files} files (\${(data.bytes / 1048576).toFixed(1)} MiB) — launching\`);
        worker.postMessage({ type: 'run-app', vitaFs: data.root, title: TITLE, app: APP, fastVblank: true });
        break;
      case 'vita-frame': {
        const pixels = new Uint8Array(data.data);
        if (screen.width !== data.width || screen.height !== data.height) {
          screen.width = data.width; screen.height = data.height;
        }
        ctx.putImageData(new ImageData(new Uint8ClampedArray(pixels.buffer, pixels.byteOffset, pixels.byteLength),
          data.width, data.height), 0, 0);
        frames += 1;
        if (!firstFrameAt) firstFrameAt = performance.now() - startedAt;
        showStats();
        break;
      }
      case 'vita-audio':
        playAudioPCM(data.freq, data.channels, data.frames, data.data);
        break;
      case 'vita-exit':
        // Report first, then release the worker: stop() must not overwrite the
        // outcome the viewer is waiting to read.
        status.textContent = \`exit \${data.exitCode} (\${data.ok ? 'ok' : 'failed'})\`;
        log('exit: ' + JSON.stringify(data));
        stop(true);
        break;
      case 'log': log(data.message); break;
      case 'error': log('ERROR ' + data.message); status.textContent = 'error'; break;
      default: break;
    }
  };
}
runButton.onclick = run;
stopButton.onclick = () => stop(false);
if (params.get('auto') === '1') run();
</script>
</body>
</html>`;

const server = createServer(async (req, res) => {
  try {
    const path = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
    const send = (content, type) => {
      res.writeHead(200, { 'Content-Type': type, 'Content-Length': content.length });
      res.end(content);
    };
    const body = (content, type) => send(content, type);
    if (path === '/') return body(new TextEncoder().encode(page), 'text/html');
    if (path === '/manifest.json') return body(manifestBytes, 'application/json');
    if (path === '/favicon.ico') { res.writeHead(404); return res.end(); }
    if (path.startsWith('/stage/')) {
      const file = resolve(stage, path.slice('/stage/'.length));
      if (!file.startsWith(stage + sep)) throw new Error('bad stage path');
      return send(await readFile(file), 'application/octet-stream');
    }
    if (hostFiles.includes(path))
      return body(await readFile(resolve('browser/web', path.slice(1))), 'text/javascript');
    let shaderFile = shaderFiles[path];
    if (path.startsWith('/shaders/wasi/')) {
      const base = resolve(shaderRoot, 'node_modules/@bjorn3/browser_wasi_shim/dist');
      const candidate = resolve(base, path.slice('/shaders/wasi/'.length));
      if (!candidate.startsWith(base + sep)) throw new Error('bad shader path');
      shaderFile = candidate;
    }
    if (shaderFile) return send(await readFile(shaderFile), path.endsWith('.wasm') ? 'application/wasm' : 'text/javascript');
    // ?memory=w64 makes the Worker request ./wasm64/<module>.js; the module
    // directory already IS the wasm64 flavor, so strip the prefix.
    const file = resolve(root, `.${path.replace(/^\/wasm64\//, '/')}`);
    if (!file.startsWith(root + sep)) throw new Error('bad path');
    send(await readFile(file), path.endsWith('.wasm') ? 'application/wasm' : 'text/javascript');
  } catch (error) {
    res.writeHead(404); res.end(String(error?.code === 'ENOENT' ? 'not found' : error));
  }
});
server.listen(port, host, () => {
  const files = JSON.parse(new TextDecoder().decode(manifestBytes));
  console.log(`Limbo dev server: (title ${title}, ${files.length} staged files)`);
  console.log(`  module root ${root}`);
  console.log(`  staged root ${stage}`);
  // Name every address the server actually answers on, so a browser on another
  // host does not have to guess which one to open.
  const addresses = host === '0.0.0.0' || host === '::'
    ? Object.values(networkInterfaces()).flat()
      .filter((entry) => entry?.family === 'IPv4' && !entry.internal).map((entry) => entry.address)
    : [host];
  for (const address of addresses.length ? addresses : [host])
    console.log(`  http://${address}:${port}/        (add ?auto=1 to start immediately)`);
  if (host === '127.0.0.1')
    console.log('  note: loopback only; set HOST=0.0.0.0 to accept connections from other hosts');
});
