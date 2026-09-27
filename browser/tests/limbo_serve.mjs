// Standalone Limbo dev server: serves the browser runtime plus a staged retail
// app to a real browser, and shows the presented frames on the page. This is
// the interactive counterpart of limbo_app_chromium.mjs (which drives the same
// routes headlessly); both serve the runtime through runtime_routes.mjs.
//
//   node browser/tests/limbo_serve.mjs                  # http://127.0.0.1:8080/
//   PORT=9000 LIMBO_MEMORY=w32 node browser/tests/limbo_serve.mjs
//
// Query parameters: ?backend=jit|interp  ?memory=auto|w64|w32
// ?fastvblank=1 free-runs the vblank clock (measures headroom; frame-locked
// titles such as Limbo then run faster than real time); ?fpsHack=1 is Vita3K's
// fps-hack (display waits use one vblank; Limbo's logic is frame-locked, so it
// also runs up to twice as fast);
// ?scale=N renders at N times the Vita resolution (1-4; default 2 = 1920x1088);
// ?surfaceSync=1 reads rendered surfaces back into guest memory after each scene;
// ?inlineMutex=0 disables the inline-mutex optimization for A/B testing; ?auto=1 (start
// immediately); ?present=readback keeps the canvas on the page and has the worker
// read every GPU frame back instead (slower; for tools that read the page canvas,
// e.g. limbo_watch.mjs). By default the page transfers an OffscreenCanvas to the
// worker, gxm_scene.js presents GPU frames into it directly, and the page only
// counts them ('vita-present'); frames that arrive with pixels (CPU-presented
// guest memory) are drawn on a 2D canvas stacked over it.
//
// Keyboard (shown on the page): arrows = d-pad + left stick, X cross, C circle,
// Z square, V triangle, Q/E = L/R, Enter start, Right Shift select, I/J/K/L =
// right stick. A guest message dialog (sceMsgDialog) is drawn over the screen
// and takes the keys while it is open: left/right select a button, cross and
// circle answer it as on the Vita (the runtime applies the enter-button rule).
//
// Requires a WebGPU browser **on a secure origin**: WebGPU is
// exposed only to secure contexts, so a page served over plain HTTP from a
// non-loopback address has no navigator.gpu and the first draw fails. Serve it
// through a TLS reverse proxy (e.g. Caddy) and open https://<name>/, or forward
// the port and open http://localhost:<PORT>/ (loopback is secure). Both the page
// and gxm_scene.js name this reason explicitly instead of failing late.
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
//   LIMBO_AOT           ahead-of-time module for the title (AOT.md), served as
//                       /aot.wasm and passed to run-app as aotUrl
import { createServer } from 'node:http';
import { readFile, readdir } from 'node:fs/promises';
import { networkInterfaces } from 'node:os';
import { resolve, sep, relative } from 'node:path';
import { existsSync } from 'node:fs';
import { readRuntimeFile, runtimeRoot as root } from './runtime_routes.mjs';

const port = Number(process.env.PORT || 8080);
const host = process.env.HOST || '127.0.0.1';
const stage = resolve(process.env.LIMBO_STAGE || '.limbo_work/stage');
const title = process.env.LIMBO_TITLE || 'PCSE00268';
const app = process.env.LIMBO_APP || title;
const aotPath = process.env.LIMBO_AOT ? resolve(process.env.LIMBO_AOT) : '';

if (!existsSync(resolve(stage, 'ux0/app', app, 'eboot.bin')))
  throw new Error(`no staged app at ${resolve(stage, 'ux0/app', app, 'eboot.bin')} (set LIMBO_STAGE/LIMBO_APP)`);
if (!existsSync(resolve(root, 'vita3k_web_jit.wasm')))
  throw new Error(`no built module at ${root} (build vita3k_web_jit first, or set GXM_RUNTIME_DIST)`);
if (aotPath && !existsSync(aotPath)) throw new Error(`no AOT module at ${aotPath} (LIMBO_AOT)`);

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

const page = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Vita3K Web — Limbo (${title})</title>
<style>
  body { margin: 0; background: #111; color: #ddd; font: 13px/1.45 ui-monospace, monospace; }
  header { padding: 10px 14px; display: flex; gap: 14px; align-items: center; flex-wrap: wrap; }
  #display { display: grid; justify-content: center; }
  #display > canvas { grid-area: 1 / 1; width: min(100vw, calc(85vh * 960 / 544)); height: auto; background: #000; }
  #display > canvas[hidden] { display: none; }
  #keys { margin: 0; padding: 0 14px 6px; color: #999; }
  #log { margin: 0; padding: 10px 14px; height: 30vh; overflow: auto; white-space: pre-wrap; color: #9c9; }
  #warning { display: none; margin: 8px 14px; padding: 8px 10px; background: #4a2222; color: #fdd;
    border-left: 3px solid #f66; max-width: 70ch; }
  button { font: inherit; padding: 4px 12px; }
  #status { color: #fc6; }
  #dialog { grid-area: 1 / 1; align-self: center; justify-self: center; z-index: 1; min-width: 40ch;
    max-width: 70%; padding: 18px 22px; background: #eee; color: #111; border-radius: 8px;
    font: 15px/1.4 system-ui, sans-serif; text-align: center; box-shadow: 0 4px 24px #000a; }
  #dialog[hidden] { display: none; }
  #dialog-message { margin: 0 0 14px; white-space: pre-wrap; }
  #dialog progress { width: 100%; }
  #dialog-buttons { display: flex; gap: 10px; justify-content: center; }
  #dialog-buttons button { min-width: 10ch; }
  #dialog-buttons button.selected { outline: 3px solid #36f; }
  #dialog-hint { display: block; margin-top: 10px; color: #666; font-size: 12px; }
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
<p id="keys">keys: arrows = d-pad + left stick · X cross · C circle · Z square · V triangle ·
  Q/E = L/R · Enter start · Right Shift select · I/J/K/L right stick</p>
<div id="warning"></div>
<div id="display">
  <canvas id="gpu-screen" width="960" height="544"></canvas>
  <canvas id="screen" width="960" height="544" hidden></canvas>
  <div id="dialog" role="dialog" hidden>
    <p id="dialog-message"></p>
    <progress id="dialog-progress" max="100" hidden></progress>
    <div id="dialog-buttons"></div>
    <small id="dialog-hint"></small>
  </div>
</div>
<pre id="log"></pre>
<script type="module">
const params = new URLSearchParams(location.search);
const backend = params.get('backend') === 'interp' ? 'interp' : 'jit';
const memory = ['w64', 'w32'].includes(params.get('memory')) ? params.get('memory') : 'auto';
const TITLE = ${JSON.stringify(title)}, APP = ${JSON.stringify(app)}, AOT = ${JSON.stringify(Boolean(aotPath))};
// A transferred canvas can no longer be drawn or read from this page, so GPU
// frames get their own element and pixel frames the 2D canvas over it.
const presentToCanvas = params.get('present') !== 'readback';
const fastVblank = params.get('fastvblank') === '1';
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
let worker = null, running = false, frames = 0, gpuFrames = 0, pixelFrames = 0, firstFrameAt = 0, startedAt = 0;
let fps = 0, fpsSince = 0, fpsFrames = 0;
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
  stats.textContent = 'frames=' + frames + ' (gpu=' + gpuFrames + ' pixels=' + pixelFrames + ') fps=' + fps.toFixed(1) +
    ' elapsed=' + elapsed.toFixed(1) + 's first=' + (firstFrameAt / 1000).toFixed(1) + 's' + audio;
};
function countFrame() {
  const now = performance.now();
  frames += 1;
  if (!firstFrameAt) { firstFrameAt = now - startedAt; fpsSince = now; fpsFrames = 0; }
  fpsFrames += 1;
  if (now - fpsSince >= 1000) { fps = fpsFrames * 1000 / (now - fpsSince); fpsSince = now; fpsFrames = 0; }
  showStats();
}
// An OffscreenCanvas lives only as long as its worker and a canvas element can
// transfer control once, so every run gets a fresh GPU canvas.
function attachCanvas() {
  const gpuScreen = document.createElement('canvas');
  gpuScreen.id = 'gpu-screen'; gpuScreen.width = 960; gpuScreen.height = 544;
  document.querySelector('#gpu-screen').replaceWith(gpuScreen);
  const canvas = gpuScreen.transferControlToOffscreen();
  worker.postMessage({ type: 'attach-canvas', canvas }, [canvas]);
}

// Keyboard -> SCE_CTRL button mask and stick axes [lx, ly, rx, ry] in [-1, 1].
const SCE_CTRL = { select: 0x1, start: 0x8, up: 0x10, right: 0x20, down: 0x40, left: 0x80,
  l: 0x100, r: 0x200, triangle: 0x1000, circle: 0x2000, cross: 0x4000, square: 0x8000 };
const keyMap = {
  ArrowUp: { button: SCE_CTRL.up, axis: [1, -1] }, ArrowDown: { button: SCE_CTRL.down, axis: [1, 1] },
  ArrowLeft: { button: SCE_CTRL.left, axis: [0, -1] }, ArrowRight: { button: SCE_CTRL.right, axis: [0, 1] },
  KeyX: { button: SCE_CTRL.cross }, KeyC: { button: SCE_CTRL.circle },
  KeyZ: { button: SCE_CTRL.square }, KeyV: { button: SCE_CTRL.triangle },
  KeyQ: { button: SCE_CTRL.l }, KeyE: { button: SCE_CTRL.r },
  Enter: { button: SCE_CTRL.start }, ShiftRight: { button: SCE_CTRL.select },
  KeyI: { axis: [3, -1] }, KeyK: { axis: [3, 1] }, KeyJ: { axis: [2, -1] }, KeyL: { axis: [2, 1] },
};
const held = new Set();
// Guest message dialog (worker 'vita-dialog'); null when none is shown.
let dialog = null;
const dialogBox = document.querySelector('#dialog');
function pressDialog(button, selected) {
  worker.postMessage({ type: 'dialog-press', id: dialog.id, button, selected });
}
function renderDialogButtons() {
  const box = document.querySelector('#dialog-buttons');
  box.replaceChildren(...dialog.buttons.map((label, index) => {
    const element = document.createElement('button');
    element.textContent = label;
    element.className = index === dialog.selected ? 'selected' : '';
    element.onclick = () => pressDialog(dialog.enter, index);
    return element;
  }));
}
function onDialog(message) {
  if (message.state === 'close') {
    log(\`dialog \${message.id} closed: buttonId=\${message.buttonId} result=\${message.result}\`);
    if (dialog?.id === message.id) { dialog = null; dialogBox.hidden = true; }
    return;
  }
  if (message.state === 'open') {
    log(\`dialog \${message.id}: \${JSON.stringify(message.message)} [\${message.buttons.join(', ')}]\`);
    // The dialog takes the pad, as the Vita's does: release what the guest holds.
    held.clear(); sendPad();
    dialog = { id: message.id, selected: 0 };
  }
  if (!dialog || dialog.id !== message.id) return;
  dialog.buttons = message.buttons;
  dialog.enter = message.enterButton === 'circle' ? SCE_CTRL.circle : SCE_CTRL.cross;
  dialog.selected = Math.min(dialog.selected, Math.max(0, message.buttons.length - 1));
  document.querySelector('#dialog-message').textContent = message.message;
  const progress = document.querySelector('#dialog-progress');
  progress.hidden = message.progress === null;
  if (message.progress !== null) progress.value = message.progress;
  renderDialogButtons();
  document.querySelector('#dialog-hint').textContent = message.buttons.length
    ? (message.enterButton === 'circle' ? 'C (circle) selects · X (cross) backs out' : 'X (cross) selects · C (circle) backs out') : '';
  dialogBox.hidden = false;
}
function dialogKey(event) {
  const { button = 0 } = keyMap[event.code];
  if (event.type !== 'keydown' || event.repeat) return;
  if (button === SCE_CTRL.left || button === SCE_CTRL.right) {
    const last = Math.max(0, dialog.buttons.length - 1);
    dialog.selected = Math.max(0, Math.min(last, dialog.selected + (button === SCE_CTRL.right ? 1 : -1)));
    renderDialogButtons();
  } else if (button === SCE_CTRL.cross || button === SCE_CTRL.circle) {
    pressDialog(button, dialog.selected);
  }
}
function sendPad() {
  if (!running) return;
  let buttons = 0;
  const axes = [0, 0, 0, 0];
  for (const code of held) {
    const { button = 0, axis } = keyMap[code];
    buttons |= button;
    if (axis) axes[axis[0]] += axis[1];
  }
  worker.postMessage({ type: 'input', buttons, axes: axes.map((value) => Math.max(-1, Math.min(1, value))) });
}
function onKey(event) {
  if (!running || !(event.code in keyMap)) return;
  // While the guest runs, mapped keys are its input: no scrolling or button activation.
  event.preventDefault();
  if (dialog) { dialogKey(event); return; }
  const down = event.type === 'keydown';
  if (down === held.has(event.code)) return; // auto-repeat
  if (down) held.add(event.code); else held.delete(event.code);
  sendPad();
}
addEventListener('keydown', onKey);
addEventListener('keyup', onKey);
addEventListener('blur', () => { held.clear(); sendPad(); });

function stop(keepsStatus) {
  worker?.terminate(); worker = null; running = false; held.clear();
  dialog = null; dialogBox.hidden = true;
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
  frames = 0; gpuFrames = 0; pixelFrames = 0; fps = 0; firstFrameAt = 0; logBox.textContent = ''; startedAt = performance.now();
  screen.hidden = true;
  document.querySelector('#gpu-screen').hidden = !presentToCanvas;
  audioChunks = 0; audioBytes = 0; audioLogged = false; audioFirstAt = 0;
  status.textContent = 'loading module…';
  if (webgpuBlocked) log('warning: ' + webgpuBlocked);
  runButton.disabled = true; stopButton.disabled = false;
  ensureAudio();
  worker = new Worker(\`./worker.js?backend=\${backend}&memory=\${memory}&inlineMutex=\${params.get('inlineMutex') === '0' ? '0' : '1'}\${params.get('fpsHack') === '1' ? '&fpsHack=1' : ''}\${params.get('scale') ? '&scale=' + params.get('scale') : ''}\${params.get('surfaceSync') === '1' ? '&surfaceSync=1' : ''}\`, { type: 'module' });
  worker.onerror = (event) => { log('worker error: ' + event.message); status.textContent = 'worker error'; };
  worker.onmessage = async ({ data }) => {
    if (!data || typeof data !== 'object') return;
    switch (data.type) {
      case 'lifecycle': log('lifecycle: ' + data.state); break;
      case 'ready':
        status.textContent = 'staging content…';
        log(\`ready (backend=\${data.diagnostics?.backend} memory=\${data.diagnostics?.memoryModel} inlineMutex=\${data.diagnostics?.inlineMutex})\`);
        if (presentToCanvas) {
          try { attachCanvas(); } catch (error) {
            log('canvas transfer failed: ' + error); status.textContent = 'canvas transfer failed'; stop(true); break;
          }
        }
        try {
          const files = await (await fetch('/manifest.json')).json();
          worker.postMessage({ type: 'stage-files', root: '/vita',
            files: files.map((file) => ({ ...file, url: \`/stage/\${file.path}\` })) });
        } catch (error) { log('manifest failed: ' + error); status.textContent = 'staging failed'; }
        break;
      case 'staged':
        status.textContent = 'running';
        log(\`staged \${data.files} files (\${(data.bytes / 1048576).toFixed(1)} MiB) — launching\`);
        worker.postMessage({ type: 'run-app', vitaFs: data.root, title: TITLE, app: APP, fastVblank,
          ...(AOT ? { aotUrl: '/aot.wasm' } : {}) });
        running = true;
        sendPad();
        break;
      case 'vita-present':
        // Already on the GPU canvas; nothing to draw here.
        screen.hidden = true;
        gpuFrames += 1;
        countFrame();
        break;
      case 'vita-frame': {
        const pixels = new Uint8Array(data.data);
        if (screen.width !== data.width || screen.height !== data.height) {
          screen.width = data.width; screen.height = data.height;
        }
        ctx.putImageData(new ImageData(new Uint8ClampedArray(pixels.buffer, pixels.byteOffset, pixels.byteLength),
          data.width, data.height), 0, 0);
        screen.hidden = false;
        pixelFrames += 1;
        countFrame();
        break;
      }
      case 'vita-audio':
        playAudioPCM(data.freq, data.channels, data.frames, data.data);
        break;
      case 'vita-dialog': onDialog(data.dialog); break;
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
    if (path === '/aot.wasm' && aotPath) return send(await readFile(aotPath), 'application/wasm');
    if (path.startsWith('/stage/')) {
      const file = resolve(stage, path.slice('/stage/'.length));
      if (!file.startsWith(stage + sep)) throw new Error('bad stage path');
      return send(await readFile(file), 'application/octet-stream');
    }
    const { content, type } = await readRuntimeFile(path);
    send(content, type);
  } catch (error) {
    res.writeHead(404); res.end(String(error?.code === 'ENOENT' ? 'not found' : error));
  }
});
server.listen(port, host, () => {
  const files = JSON.parse(new TextDecoder().decode(manifestBytes));
  console.log(`Limbo dev server: (title ${title}, ${files.length} staged files)`);
  console.log(`  module root ${root}`);
  console.log(`  staged root ${stage}`);
  if (aotPath) console.log(`  AOT module  ${aotPath}`);
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
