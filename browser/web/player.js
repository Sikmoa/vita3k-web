import { SCE_CTRL, keyMap, createPadState, createTouchControls, createGamepadInput } from './pad_input.js';
// Persistent content helpers live in content_cache.js but are imported
// lazily at each use site (never statically): late dynamic imports resolve
// completely and a missing file (old server) disables the feature cleanly.
// Only the pure key helpers are inlined so they need no module at all.
const sanitizeSegment = (value) =>
  String(value ?? '').replace(/[^A-Za-z0-9._-]+/g, '_').slice(0, 64) || '_';
const storageSupported = () => {
  try {
    return typeof navigator !== 'undefined' && typeof navigator.storage?.getDirectory === 'function';
  } catch {
    return false;
  }
};
// Module namespace, resolved once and shared: the uploader's successful
// import is reused by the boot so both paths always see one instance.
let cacheNSPromise = null;
function cacheAPI() {
  if (!cacheNSPromise) cacheNSPromise = import('./content_cache.js').then((m) => m, () => null);
  return cacheNSPromise;
}
// Namespace of the current run's staging session, resolved in the ready
// handler and shared by the stage-need/store/staged handlers below.
let cacheNS = null;
// Save batches from the worker are stored one after another (vita-saves).
let saveChain = Promise.resolve();

const params = new URLSearchParams(location.search);
const backend = params.get('backend') === 'interp' ? 'interp' : 'jit';
const memory = ['w64', 'w32'].includes(params.get('memory')) ? params.get('memory') : 'auto';
// ?title=<id>&app=<dir> boots another title the server has staged (utilities
// like homebrew next to the retail title); without them the server's default
// applies. The title also selects its own persistent content cache.
const titleParam = params.get('title') || '', appParam = params.get('app') || '';
const configQuery = (() => {
  const query = new URLSearchParams();
  if (titleParam) query.set('title', titleParam);
  if (appParam) query.set('app', appParam);
  return query.size ? '?' + query : '';
})();
let config;
try {
  const response = await fetch('./player-config.json' + configQuery);
  if (!response.ok) throw new Error(await response.text() || `Player configuration: HTTP ${response.status}`);
  config = await response.json();
} catch (error) {
  document.querySelector('#status').textContent = 'Unable to load player';
  const warning = document.querySelector('#warning');
  warning.textContent = error.message; warning.style.display = 'block';
  throw error;
}
// A static host (GitHub Pages, browser/pages/assemble.sh) has no game or firmware of its
// own: its player-config.json says {"static": true}, and everything boots
// from what this browser holds — the title from ?title= or the first stored
// package, firmware from its own upload.
if (config.static) {
  const cache = storageSupported() ? await cacheAPI() : null;
  let stored = [];
  try { stored = cache ? await cache.listCachedTitles() : []; } catch {}
  const title = titleParam || stored[0]?.title || '';
  config = { ...config, title, app: appParam || title, staged: false, aot: false, aotUrl: null, aotMtUrl: null, titles: [] };
}
// ?source=package boots this browser's package of a title the server also
// stages: only firmware and patches come from the server then.
const stagedTitles = Array.isArray(config.titles) ? config.titles : [];
if (params.get('source') === 'package' && config.staged)
  config = { ...config, staged: false, aot: false, aotUrl: null, aotMtUrl: null };
const fromServer = config.staged !== false;
const { title: TITLE, app: APP, aot: AOT, aotUrl: AOT_URL, aotMtUrl: AOT_MT_URL } = config;
const aotUrl = typeof AOT_URL === 'string' && AOT_URL ? AOT_URL : (AOT ? '/aot.wasm' : null);
// Uploaded games need no server-built image or recorded execution seeds:
// build from their loaded code at launch. An explicit query overrides this.
const buildAot = params.get('buildAot') ?? (backend === 'jit' && config.staged === false ? '1' : null);
// Persistent content (OPFS) namespaces: an uploaded package at
// <title>/<app>, the server's copy of a staged title at <title>/<app>.server
// (content_cache.js) so that neither overwrites the other. Staging reads
// through cacheKey and stores what it downloads there. A server copy stored
// at the package key before the split stays in use there.
const packageKey = `${sanitizeSegment(TITLE)}/${sanitizeSegment(APP)}`;
const cacheKey = await (async () => {
  if (!fromServer) return packageKey;
  const cache = storageSupported() ? await cacheAPI() : null;
  if (!cache) return packageKey + '.server';
  const serverKey = packageKey + cache.SERVER_SUFFIX;
  if (await cache.cacheReadManifest(serverKey)) return serverKey;
  return cache.isServerManifest(await cache.cacheReadManifest(packageKey)) ? packageKey : serverKey;
})();
// Page-side content cache state: the page owns all OPFS I/O (the worker
// only asks and hands back). stageCacheActive gates reads and write-back;
// stageNeeded is the manifest the worker is currently staging.
let stageCacheActive = false;
let stageNeeded = null;
let stageCacheIndex = null;
// Storage key of each staged file read from another title's key (firmware).
let stageKeys = new Map();
const stageSources = { storage: 0, downloaded: 0 };
// A transferred canvas can no longer be drawn or read from this page, so GPU
// frames get their own element and pixel frames the 2D canvas over it.
const presentToCanvas = params.get('present') !== 'readback';
const fastVblank = params.get('fastvblank') === '1';
const screen = document.querySelector('#screen'), ctx = screen.getContext('2d');
const status = document.querySelector('#status'), stats = document.querySelector('#stats');
const logBox = document.querySelector('#log'), runButton = document.querySelector('#run');
const stopButton = document.querySelector('#stop'), warningBox = document.querySelector('#warning');
const beepButton = document.querySelector('#beep');
// Saves of the selected title as a .zip laid out like the Vita filesystem
// (ux0/user/00/savedata/<title>/...), for backups or moving them elsewhere.
document.querySelector('#download-saves').onclick = async () => {
  const saveAPI = storageSupported() ? await cacheAPI() : null;
  if (!saveAPI) { notice('This browser has no persistent storage, so there are no saves to download.'); return; }
  const saves = await saveAPI.readSaves(TITLE).catch(() => []);
  if (!saves.length) { notice(`No saves for ${TITLE} in this browser yet.`); return; }
  const { createZip } = await import('./zip.js');
  const zip = createZip(saves.map(({ path, bytes }) => ({ path: `ux0/user/00/savedata/${TITLE}/${path}`, bytes })));
  const link = document.createElement('a');
  link.href = URL.createObjectURL(zip);
  link.download = `${TITLE}-saves.zip`;
  link.click();
  setTimeout(() => URL.revokeObjectURL(link.href), 10000);
  log(`saves: downloaded ${saves.length} file(s) for ${TITLE}`);
};
// Restores a title's saves from a .zip (the Download button's, or a save
// folder zipped); they replace the ones this browser holds and reach the
// game at its next launch.
const savesInput = document.querySelector('#saves-file');
document.querySelector('#upload-saves').onclick = () => {
  if (!TITLE) { notice('Upload a game first: saves belong to a title.'); return; }
  if (running || !stopButton.disabled) { notice('Stop the game before restoring saves.'); return; }
  if (!storageSupported()) { notice('This browser has no persistent storage for saves.'); return; }
  savesInput.click();
};
savesInput.onchange = async () => {
  const file = savesInput.files?.[0];
  savesInput.value = '';
  if (!file) return;
  try {
    const saveAPI = await cacheAPI();
    const { listZipEntries, extractZipEntry } = await import('./zip.js');
    const saves = saveAPI.savesInArchive(await listZipEntries(file), TITLE);
    const existing = await saveAPI.readSaves(TITLE).catch(() => []);
    if (existing.length && !confirm(`Replace the ${existing.length} save file(s) of ${TITLE} in this browser with ${saves.length} from ${file.name}?`))
      return;
    await saveAPI.clearSaves(TITLE);
    for (const save of saves) await saveAPI.writeSave(TITLE, save.path, await extractZipEntry(file, save.entry));
    log(`saves: restored ${saves.length} file(s) for ${TITLE} from ${file.name}`);
    notice(`Restored ${saves.length} save file(s) for ${TITLE}. Press Play.`);
  } catch (error) {
    log('saves upload failed: ' + (error?.message ?? error));
    notice('Saves upload failed: ' + (error?.message ?? error));
  }
};
const display = document.querySelector('#display'), shell = document.querySelector('#player-shell');
const welcome = document.querySelector('#welcome'), fpsLabel = document.querySelector('#fps');
// Launch status under the "Starting your game…" overlay: the phase, a
// byte-progress bar and counts/percentage/elapsed time while content is
// staged into the worker (see the worker's 'stage-progress' message).
const launchStatus = document.querySelector('#launch-status'), launchPhase = document.querySelector('#launch-phase');
const launchBar = document.querySelector('#launch-bar'), launchFill = document.querySelector('#launch-fill');
const launchDetail = document.querySelector('#launch-detail');
let launchStartedAt = 0, stageTotals = { files: 0, bytes: 0 };
const mib = (bytes) => (bytes / 1048576).toFixed(1);
const elapsed = () => (launchStartedAt ? Math.max(0, Math.round((performance.now() - launchStartedAt) / 1000)) : 0) + 's';
function launch(phase, detail, fraction) {
  launchStatus.hidden = !phase;
  if (!phase) return;
  launchPhase.textContent = phase;
  launchDetail.textContent = detail || '';
  const measured = Number.isFinite(fraction);
  launchBar.hidden = !measured;
  if (measured) launchFill.style.width = Math.round(Math.min(1, Math.max(0, fraction)) * 100) + '%';
}
// Progress of staging. The byte fraction counts every staged file (read from
// storage or downloaded), so `stageTotals.bytes` is the whole title, not the
// amount fetched: the read/download counters say which is which.
function stagingDetail(index, bytes) {
  const parts = [];
  if (stageTotals.files) parts.push(`${Math.min(index, stageTotals.files)}/${stageTotals.files} files`);
  if (stageTotals.bytes) parts.push(`${mib(bytes)}/${mib(stageTotals.bytes)} MiB`,
    `${Math.floor(Math.min(1, bytes / stageTotals.bytes) * 100)}%`);
  if (stageSources.storage) parts.push(`${stageSources.storage} from storage`);
  if (stageSources.downloaded) parts.push(`${stageSources.downloaded} downloaded`);
  parts.push(elapsed());
  return parts.join(' · ');
}
document.querySelector('#game-title').textContent = TITLE === 'PCSE00268' ? 'Limbo' : TITLE || 'No game yet';
document.title = 'Vita3K Web — ' + document.querySelector('#game-title').textContent;
document.querySelector('#runtime-info').textContent = TITLE + ' · ' + backend.toUpperCase() + ' · ' + memory +
  (aotUrl ? ' · AOT' : buildAot === '1' || buildAot === 'only' ? ' · AOT at launch' : '') +
  (config.staged === false ? ' · package' : '');
function notice(message) {
  const element = document.querySelector('#player-notice');
  element.textContent = message; element.hidden = !message;
}

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
// navigator.gpu can still come without an adapter, or with only the CPU
// fallback (SwiftShader), on which the game runs at a few frames per second.
if (!webgpuBlocked) navigator.gpu.requestAdapter().then((adapter) => {
  const info = adapter?.info ?? {};
  const flags = 'Chrome on Linux needs --enable-unsafe-webgpu --enable-features=Vulkan, and --disable-gpu-sandbox on NixOS.';
  const problem = !adapter ? 'WebGPU has no adapter: the browser blocks this GPU. ' + flags
    : adapter.isFallbackAdapter || info.isFallbackAdapter || /swiftshader/i.test(info.architecture + ' ' + info.description)
      ? 'WebGPU runs on the CPU (' + info.vendor + ' ' + info.architecture + '), so the game will be very slow. ' + flags
      : null;
  if (problem) { warningBox.textContent = problem; warningBox.style.display = 'block'; }
}).catch((error) => { warningBox.textContent = 'WebGPU adapter check failed: ' + error.message; warningBox.style.display = 'block'; });
let worker = null, running = false, frames = 0, gpuFrames = 0, pixelFrames = 0, firstFrameAt = 0, startedAt = 0;
// Present watchdog: a run whose worker stays alive but stops presenting is
// the frozen-frame failure (device loss, wedged queue). Armed only after
// the first presented frame so slow boot/AOT compile never trips it.
let lastPresentAt = 0, presentedOnce = false, watchdogWarned = false;
let fps = 0, fpsSince = 0, fpsFrames = 0;
// Web Audio sink: guest PCM (int16 interleaved; 48 kHz stereo on the MAIN
// port) arrives as transferred ArrayBuffers from the worker (see
// browser/src/hle_audio_null.cpp). AudioBuffers are chained on the context
// clock; when the unpaced guest submits ahead of realtime (bursts while
// loading) the chain is resynced instead of scheduling seconds of latency.
let audioCtx = null, audioGain = null, muted = false;
// Next start time per guest audio port: ports that are open together (a
// movie's audio next to the game's mixer) are separate streams Web Audio
// mixes. One shared timeline played their chunks one after another, so audio
// fell behind whenever a second port was open.
const audioNext = new Map();
const audioSources = new Set();
let audioChunks = 0, audioBytes = 0, audioLogged = false, audioFirstAt = 0, audioPeak = 0;
function ensureAudio() {
  if (!audioCtx) {
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return;
    audioCtx = new AC();
    audioGain = audioCtx.createGain();
    audioGain.gain.value = muted ? 0 : 1;
    audioGain.connect(audioCtx.destination);
    audioNext.clear();
  }
  if (audioCtx.state === 'suspended') audioCtx.resume().catch((error) => notice('Tap Sound on to enable audio: ' + error.message));
}
function playAudioPCM(freq, channels, frameCount, buffer, port = 0) {
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
  src.connect(audioGain);
  audioSources.add(src);
  src.onended = () => { audioSources.delete(src); src.disconnect(); };
  const now = audioCtx.currentTime;
  let next = audioNext.get(port) ?? 0;
  // A stream that ran dry (or a first chunk) restarts slightly ahead so the
  // next few small chunks can arrive late without a gap.
  if (next < now || next > now + 1.0) next = now + 0.04;
  src.start(next);
  audioNext.set(port, next + audio.duration);
}
// Threaded runtime: one AudioWorklet node per guest port plays the port's PCM
// ring straight from shared memory (browser/web/audio_ring_worklet.js). Its
// consumption is what paces the guest audio thread.
const audioRings = new Map();
let audioWorkletReady = null;
async function playAudioRing(ring) {
  ensureAudio();
  if (!audioCtx?.audioWorklet) { log('audio: no AudioWorklet in this browser; threaded audio is silent'); return; }
  audioWorkletReady ??= audioCtx.audioWorklet.addModule('./audio_ring_worklet.js');
  try { await audioWorkletReady; } catch (error) { log('audio: ring worklet failed: ' + error.message); return; }
  stopAudioRing(ring.port);
  const node = new AudioWorkletNode(audioCtx, 'vita3k-audio-ring', { numberOfInputs: 0, outputChannelCount: [2],
    processorOptions: { buffer: ring.buffer, offset: ring.offset, generation: ring.generation } });
  node.connect(audioGain);
  audioRings.set(ring.port, node);
  node.port.onmessage = ({ data }) => log(`audio: ring port=${ring.port} played=${data.played} frames underruns=${data.underruns} peak=${data.peak}/32768`);
  log(`audio: ring port=${ring.port} ${ring.channels}ch ${ring.freq} Hz capacity=${ring.capacity} frames ctx=${audioCtx.state}`);
}
function stopAudioRing(port) {
  const node = audioRings.get(port);
  if (!node) return;
  node.port.postMessage('stop');
  node.disconnect();
  audioRings.delete(port);
}
const logLines = [];
let logDirty = false;
const log = (text) => {
  logLines.push(String(text));
  if (logLines.length > 200) logLines.splice(0, logLines.length - 200);
  logDirty = true;
};
// Batch diagnostics instead of rebuilding/scrolling the log on every HLE message.
setInterval(() => {
  if (worker) showStats();
  if (running && presentedOnce && !watchdogWarned && performance.now() - lastPresentAt > 15000) {
    watchdogWarned = true;
    const idle = Math.round((performance.now() - lastPresentAt) / 1000);
    log(`watchdog: no presented frames for ${idle}s while the run continues (device loss or wedged queue?)`);
    notice(`No new frames for ${idle}s — the graphics device may be stuck. Stop and press Play to restart.`);
  }
  if (logDirty) {
    logBox.textContent = logLines.join('\n'); logDirty = false;
    if (document.querySelector('#diagnostics').open) logBox.scrollTop = logBox.scrollHeight;
  }
}, 500);
const showStats = () => {
  fpsLabel.textContent = frames ? fps.toFixed(0) + ' FPS' : '— FPS';
  const elapsed = (performance.now() - startedAt) / 1000;
  const audio = audioCtx ? ' audio=chunks=' + audioChunks + ' ' + (audioBytes / 1048576).toFixed(1) + 'MiB peak=' + audioPeak + ' ctx=' + audioCtx.state : ' audio=off';
  if (!frames) { stats.textContent = 'elapsed=' + elapsed.toFixed(1) + 's' + audio; return; }
  stats.textContent = 'frames=' + frames + ' (gpu=' + gpuFrames + ' pixels=' + pixelFrames + ') fps=' + fps.toFixed(1) +
    ' elapsed=' + elapsed.toFixed(1) + 's first=' + (firstFrameAt / 1000).toFixed(1) + 's' + audio;
};
function countFrame() {
  const now = performance.now();
  lastPresentAt = now; presentedOnce = true; watchdogWarned = false;
  // The launch overlay (staging / AOT download / compile status) has served
  // its purpose once frames present; otherwise a stale "Compiling the AOT
  // module…" phase lingers over gameplay even when AOT loaded fine.
  if (!launchStatus.hidden) launch(null);
  frames += 1;
  if (!firstFrameAt) { firstFrameAt = now - startedAt; fpsSince = now; fpsFrames = 0; }
  fpsFrames += 1;
  if (now - fpsSince >= 1000) { fps = fpsFrames * 1000 / (now - fpsSince); fpsSince = now; fpsFrames = 0; }
  welcome.hidden = true;
}
// An OffscreenCanvas lives only as long as its worker and a canvas element can
// transfer control once, so every run gets a fresh GPU canvas.
function attachCanvas() {
  const gpuScreen = document.createElement('canvas');
  gpuScreen.setAttribute('aria-label', 'Game video');
  gpuScreen.id = 'gpu-screen'; gpuScreen.width = 960; gpuScreen.height = 544;
  document.querySelector('#gpu-screen').replaceWith(gpuScreen);
  const canvas = gpuScreen.transferControlToOffscreen();
  worker.postMessage({ type: 'attach-canvas', canvas }, [canvas]);
}

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
    log(`dialog ${message.id} closed: buttonId=${message.buttonId} result=${message.result}`);
    if (dialog?.id === message.id) { dialog = null; dialogBox.hidden = true; updateTouchVisibility(); display.focus({ preventScroll: true }); }
    return;
  }
  if (message.state === 'open') {
    log(`dialog ${message.id}: ${JSON.stringify(message.message)} [${message.buttons.join(', ')}]`);
    // The dialog takes the pad, as the Vita's does: release what the guest holds.
    clearInputs();
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
  updateTouchVisibility();
  if (message.state === 'open') dialogBox.querySelector('button')?.focus({ preventScroll: true });
}
function dialogKey(event) {
  const { buttons: button = 0 } = keyMap[event.code];
  if (event.type !== 'keydown' || event.repeat) return;
  dialogButton(button);
}
function dialogButton(button) {
  if (button === SCE_CTRL.left || button === SCE_CTRL.right) {
    const last = Math.max(0, dialog.buttons.length - 1);
    dialog.selected = Math.max(0, Math.min(last, dialog.selected + (button === SCE_CTRL.right ? 1 : -1)));
    renderDialogButtons();
    dialogBox.querySelector('button.selected')?.focus({ preventScroll: true });
  } else if (button === SCE_CTRL.cross || button === SCE_CTRL.circle) {
    pressDialog(button, dialog.selected);
  }
}
// Guest on-screen keyboard (worker 'vita-ime'); null when none is shown.
let ime = null;
const imeBox = document.querySelector('#ime');
const imeText = document.querySelector('#ime-text');
// kind 0: the field's text and caret, 1: enter, 2: close (ime_bridge.cpp).
function sendIme(kind) {
  worker.postMessage({ type: 'ime-input', input: { id: ime.id, kind, text: imeText.value,
    caret: imeText.selectionStart ?? imeText.value.length } });
}
function onIme(message) {
  if (message.state === 'close') {
    log(`ime ${message.id} closed`);
    if (ime?.id === message.id) { ime = null; imeBox.hidden = true; imeText.blur(); updateTouchVisibility(); display.focus({ preventScroll: true }); }
    return;
  }
  log(`ime ${message.id}: ${JSON.stringify(message.text)} max=${message.maxLength}`);
  // The keyboard takes the keys, as the Vita's does: release what the guest holds.
  clearInputs();
  ime = { id: message.id };
  imeText.value = message.text;
  imeText.maxLength = message.maxLength;
  document.querySelector('#ime-enter').textContent = message.enterLabel || 'Enter';
  imeBox.hidden = false;
  updateTouchVisibility();
  imeText.focus();
  imeText.setSelectionRange(message.caret, message.caret);
}
imeText.addEventListener('input', () => { if (ime) sendIme(0); });
imeText.addEventListener('keyup', (event) => { if (ime && event.key.startsWith('Arrow')) sendIme(0); });
imeBox.addEventListener('submit', (event) => { event.preventDefault(); if (ime) { sendIme(0); sendIme(1); } });
document.querySelector('#ime-close').onclick = () => { if (ime) sendIme(2); };
const pad = createPadState((state) => {
  if (running && worker) worker.postMessage({ type: 'input', ...state });
});
const touchRoot = document.querySelector('#touch-controls');
// Touch controls take presses while the game loads too: what is held when
// it starts reaches it at once (sendPad on launch).
const touch = createTouchControls(touchRoot, pad, {
  enabled: () => !dialog && !ime,
  onGesture: ensureAudio,
});
const gamepads = createGamepadInput(pad, {
  enabled: () => !dialog && !ime,
  onPress: (button) => { if (running && dialog && !ime) dialogButton(button); },
  onGesture: ensureAudio,
  onConnect: (gamepad, connected) => {
    log(`gamepad ${connected ? 'connected' : 'disconnected'}: ${gamepad.id} (${gamepad.mapping || 'no standard mapping'})`);
    updateTouchVisibility();
  },
});
function clearInputs() { touch.clear(); gamepads.clear(); pad.clear(); }
function sendPad() { pad.flush(true); }
function onKey(event) {
  if (event.type === 'keyup') pad.release('key:' + event.code);
  if (ime) {
    if (event.type === 'keydown' && event.code === 'Escape') { event.preventDefault(); sendIme(2); }
    return;
  }
  if (!running || !(event.code in keyMap)) return;
  if (dialog) {
    if (event.code === 'Enter') return; // activate the focused dialog button
    event.preventDefault(); dialogKey(event); return;
  }
  if (event.target.closest?.('input, textarea, select, button, summary, a, [contenteditable="true"]')) return;
  if (event.ctrlKey || event.metaKey || event.altKey) return;
  event.preventDefault();
  if (event.type === 'keydown' && !event.repeat) { ensureAudio(); pad.set('key:' + event.code, keyMap[event.code]); }
}
addEventListener('keydown', onKey);
addEventListener('keyup', onKey);
addEventListener('blur', clearInputs);
addEventListener('pagehide', clearInputs);
document.addEventListener('visibilitychange', () => { if (document.hidden) clearInputs(); });
addEventListener('resize', clearInputs);

const touchToggle = document.querySelector('#touch-toggle');
const touchMode = document.querySelector('#touch-mode');
const coarsePointer = matchMedia('(any-pointer: coarse)');
function readPreference(key, fallback) {
  try { return localStorage.getItem('vita3k.' + key) ?? fallback; } catch { return fallback; }
}
function writePreference(key, value) {
  try { localStorage.setItem('vita3k.' + key, value); } catch { /* private browsing */ }
}
touchMode.value = readPreference('touch', 'auto');
if (!touchMode.value) touchMode.value = 'auto';
// auto: touch screens show the controls, unless a gamepad is connected.
function wantsTouch() { return touchMode.value === 'on' || (touchMode.value === 'auto' && !gamepads.connected() && (coarsePointer.matches || navigator.maxTouchPoints > 0)); }
function updateTouchVisibility() {
  touch.clear();
  const visible = wantsTouch() && !dialog && !ime;
  touchRoot.hidden = !visible;
  display.classList.toggle('touch-visible', visible);
  touchToggle.setAttribute('aria-pressed', String(wantsTouch()));
}
touchMode.onchange = () => { writePreference('touch', touchMode.value); updateTouchVisibility(); };
touchToggle.onclick = () => { touchMode.value = wantsTouch() ? 'off' : 'on'; touchMode.onchange(); };
coarsePointer.addEventListener('change', updateTouchVisibility);
for (const [id, property, fallback] of [['touch-opacity', '--control-opacity', 65], ['touch-size', '--preferred-control-scale', 100]]) {
  const input = document.getElementById(id);
  const stored = Number(readPreference(id, fallback));
  input.value = Number.isFinite(stored) ? Math.max(Number(input.min), Math.min(Number(input.max), stored)) : fallback;
  const apply = () => { touch.clear(); document.documentElement.style.setProperty(property, Number(input.value) / 100); };
  input.oninput = apply;
  input.onchange = () => writePreference(id, input.value);
  apply();
}
updateTouchVisibility();
const muteButton = document.querySelector('#mute');
muteButton.onclick = () => {
  muted = !muted; ensureAudio();
  if (audioGain) audioGain.gain.value = muted ? 0 : 1;
  muteButton.textContent = muted ? 'Sound off' : 'Sound on';
  muteButton.setAttribute('aria-pressed', String(muted));
  muteButton.setAttribute('aria-label', muted ? 'Unmute sound' : 'Mute sound');
};
const fullscreenButton = document.querySelector('#fullscreen');
function updateFullscreen() {
  clearInputs();
  const expanded = Boolean(document.fullscreenElement) || shell.classList.contains('expanded');
  fullscreenButton.textContent = expanded ? 'Exit full' : 'Fullscreen';
  fullscreenButton.setAttribute('aria-label', expanded ? 'Exit fullscreen' : 'Enter fullscreen');
}
fullscreenButton.onclick = async () => {
  notice(''); clearInputs();
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else if (shell.classList.contains('expanded')) { shell.classList.remove('expanded'); document.body.classList.remove('player-expanded'); }
    else if (shell.requestFullscreen && document.fullscreenEnabled) await shell.requestFullscreen();
    else { shell.classList.add('expanded'); document.body.classList.add('player-expanded'); }
  } catch (error) { notice('Fullscreen is unavailable: ' + error.message); }
  updateFullscreen();
};
document.addEventListener('fullscreenchange', updateFullscreen);
addEventListener('keydown', (event) => {
  if (event.code === 'Escape' && shell.classList.contains('expanded')) {
    shell.classList.remove('expanded'); document.body.classList.remove('player-expanded'); updateFullscreen();
  }
});
// Game package upload: a .zip of the staged tree is unpacked into persistent
// storage (OPFS) once, and later boots stage from there instead of
// re-downloading. Uploads are serialized against runs via the Stop button:
// a run is active exactly while Stop is enabled.
// The firmware button takes a .zip of os0/ and vs0/ (stored once for every
// title); the package button takes a game, which may carry firmware too.
const uploadRow = document.querySelector('#upload-row');
const firmwareRow = document.querySelector('#firmware-row');
const uploadButton = document.querySelector('#upload');
const uploadInput = document.querySelector('#upload-file');
const firmwareButton = document.querySelector('#upload-firmware');
const firmwareInput = document.querySelector('#firmware-file');
if (!storageSupported()) {
  uploadRow.hidden = true; firmwareRow.hidden = true;
} else {
  for (const [button, input] of [[uploadButton, uploadInput], [firmwareButton, firmwareInput]]) {
    button.onclick = () => {
      if (running || !stopButton.disabled) { notice('Stop the game before uploading.'); return; }
      input.click();
    };
    input.onchange = () => {
      const file = input.files?.[0];
      input.value = '';
      if (file) uploadPackage(file, input === firmwareInput);
    };
  }
}
async function uploadPackage(file, firmware) {
  const wasDisabled = runButton.disabled;
  runButton.disabled = true; uploadButton.disabled = true; firmwareButton.disabled = true;
  const started = performance.now();
  let lastNotice = 0;
  const throttled = (text) => {
    const now = performance.now();
    if (now - lastNotice < 200) return;
    lastNotice = now;
    notice(text);
  };
  try {
    // A .vpk is a zip under another name; the reader only cares about the container.
    const kind = /\.pup$/i.test(file.name) ? 'pup' : /\.pkg$/i.test(file.name) ? 'pkg'
      : /\.(zip|vpk)$/i.test(file.name) ? 'zip' : null;
    if (!kind || (firmware && kind === 'pkg'))
      throw new Error(firmware ? 'firmware comes as a .PUP or a .zip of its folders' : 'games come as a .zip/.vpk, or a .pkg with its zRIF');
    if (!firmware && kind === 'pup') throw new Error('this is firmware: use the Firmware Upload button');
    const cache = await cacheAPI();
    if (!cache) throw new Error('content cache module unavailable');
    if (kind === 'pup') {
      // Sony's firmware files (system software, font package): installed and
      // decrypted here, as desktop Vita3K does (decrypt_worker.js).
      const done = await runDecryptWorker({ type: 'pup', archive: file },
        (bytes, path) => throttled(`Installing ${file.name}: ${path} — ${mib(bytes)} MiB stored`));
      try { await navigator.storage.persist?.(); } catch {}
      log(`firmware ${done.version} installed from ${file.name}: ${done.roots.join(', ')}` +
        (done.skipped.length ? ` (skipped ${done.skipped.join(', ')}: not needed by games)` : '') +
        ` — ${done.files} files in storage, ${Math.round((performance.now() - started) / 1000)}s`);
      notice(`Firmware ${done.version}: ${done.roots.join(', ')} installed.` + (TITLE ? ' Press Play.' : ' Now upload a game.'));
      await refreshFirmwareStatus();
      return;
    }
    let result, packageKey, isFirmware = false;
    if (kind === 'pkg') {
      // A PSN download: its license comes separately, as a zRIF string.
      const zrif = prompt(`${file.name} needs its license: paste its zRIF (a long string that starts with KO5i, as NoPayStation lists it).`);
      if (!zrif?.trim()) throw new Error('a .pkg needs its zRIF license');
      const done = await runDecryptWorker({ type: 'pkg', archive: file, zrif },
        (bytes, path) => throttled(`${path} — ${mib(bytes)} MiB`));
      log(`decrypted ${done.title} (${done.contentId}): ${done.decrypted} PFS files, ${done.selfs} SELFs`);
      result = { title: done.title, app: done.app, files: done.files.length, bytes: done.bytes };
      packageKey = `${sanitizeSegment(done.title)}/${sanitizeSegment(done.app)}`;
    } else {
      // The package names its own title (its ux0/app/<id> directory), so an
      // upload is all a game needs to become bootable besides firmware (the
      // server's or its own upload). Files are stored under that title's own cache.
      const contents = await cache.packageContents(file);
      if (firmware && !contents.firmware)
        throw new Error(`this is a game package (${contents.title}), not firmware: use the game Upload button`);
      if (!firmware && contents.firmware)
        throw new Error('this archive holds firmware only: use the Firmware Upload button');
      isFirmware = Boolean(contents.firmware);
      packageKey = `${sanitizeSegment(contents.title)}/${sanitizeSegment(contents.app)}`;
      const totalBytes = contents.files.reduce((sum, entry) => sum + entry.size, 0);
      // An encrypted dump (NoNpDrm: a PFS image and its license) is decrypted
      // into storage as it unpacks (decrypt_worker.js).
      const appRoot = `ux0/app/${contents.app}/`;
      const encrypted = !contents.firmware && contents.files.some((entry) => entry.path === appRoot + 'sce_pfs/files.db')
        && contents.files.some((entry) => entry.path === appRoot + 'sce_sys/package/work.bin');
      if (encrypted) {
        const done = await runDecryptWorker({ type: 'decrypt', archive: file, key: packageKey, appRoot,
          files: contents.files.map(({ path, size, entry }) => ({ path, size, entry })) },
        (bytes, path) => throttled(`Decrypting ${path} — ${mib(bytes)}/${mib(totalBytes)} MiB`));
        log(`decrypted ${contents.title}: ${done.decrypted} PFS files, ${done.selfs} SELFs`);
        result = { title: contents.title, app: contents.app, files: done.files.length, bytes: done.bytes };
      } else {
        result = await cache.unpackPackageToCache(file, packageKey, (done, total, path, doneBytes) =>
          (done < total ? throttled : notice)(`Unpacking ${path} — ${done}/${total} files · ${mib(doneBytes)}/${mib(totalBytes)} MiB`));
      }
    }
    try { await navigator.storage.persist?.(); } catch {}
    const secs = Math.round((performance.now() - started) / 1000);
    log(`package stored: ${result.title} — ${result.files} files · ${mib(result.bytes)} MiB` +
      ` in ${secs}s (persistent storage, key ${packageKey})`);
    if (isFirmware) {
      notice(`Firmware ready: ${result.files} files · ${mib(result.bytes)} MiB.` + (TITLE ? ' Press Play.' : ' Now upload a game.'));
      await refreshFirmwareStatus();
      return;
    }
    // A static host's first game: reload into it.
    if (!TITLE) { location.search = new URLSearchParams({ ...Object.fromEntries(params), title: result.title }).toString(); return; }
    await refreshTitlePicker();
    titlePicker.value = `package:${result.title}`;
    notice(result.title === TITLE && !fromServer
      ? `Package ready: ${result.files} files · ${mib(result.bytes)} MiB. Press Play.`
      : `Package ready: ${result.title} — ${result.files} files · ${mib(result.bytes)} MiB. Pick it in Title, then Play.`);
  } catch (error) {
    log('package upload failed: ' + (error?.message ?? error));
    notice('Upload failed: ' + (error?.message ?? error));
  } finally {
    runButton.disabled = wasDisabled; uploadButton.disabled = false; firmwareButton.disabled = false;
  }
}
// One job of decrypt_worker.js (an encrypted dump, a .pkg or a firmware
// .PUP); resolves with its result. onProgress(bytes done, path).
function runDecryptWorker(message, onProgress) {
  return new Promise((resolve, reject) => {
    const worker = new Worker('./decrypt_worker.js', { type: 'module' });
    worker.onerror = (event) => { worker.terminate(); reject(new Error('decryption worker: ' + event.message)); };
    worker.onmessage = ({ data }) => {
      if (data.type === 'progress') { onProgress(data.done, data.path); return; }
      worker.terminate();
      if (data.type === 'error') reject(new Error(data.message));
      else resolve(data);
    };
    worker.postMessage(message);
  });
}
// What firmware this browser holds: shown next to its upload button.
async function refreshFirmwareStatus() {
  const cache = storageSupported() ? await cacheAPI() : null;
  const files = (cache ? await cache.cacheReadManifest(cache.FIRMWARE_KEY) : null)?.files ?? [];
  const roots = new Set(files.map((file) => file.path.split('/')[0]));
  const parts = [roots.has('vs0') ? 'system software' : null, roots.has('sa0') ? 'fonts' : null].filter(Boolean);
  document.querySelector('#firmware-state').textContent = files.length
    ? `stored: ${parts.join(' + ')} (${files.length} files)${roots.has('vs0') && !roots.has('sa0') ? ' · font package missing' : ''}`
    : config.static ? 'needed' : 'from the server';
}
refreshFirmwareStatus();
// Title picker: everything bootable — the server's staged titles plus the
// packages this browser holds in persistent storage (uploaded, no server
// work). Switching reloads with ?title=<id>, which is also the shareable link.
const titlePicker = document.querySelector('#title-picker');
// A title both staged and uploaded is listed twice (value <source>:<id>).
const currentChoice = `${fromServer ? 'server' : 'package'}:${TITLE}`;
async function refreshTitlePicker() {
  const showTitles = (uploaded) => {
    const counts = new Map(uploaded.map((entry) => [entry.title, entry.files]));
    const choices = new Map();
    for (const id of stagedTitles) choices.set(`server:${id}`, `${id} · server`);
    for (const [id, files] of counts) choices.set(`package:${id}`, `${id} · package (${files})`);
    if (TITLE && !choices.has(currentChoice)) choices.set(currentChoice, `${TITLE} · ${fromServer ? 'server' : 'package'}`);
    titlePicker.replaceChildren(...[...choices].sort(([a], [b]) =>
      a.slice(a.indexOf(':') + 1).localeCompare(b.slice(b.indexOf(':') + 1)) || b.localeCompare(a)).map(([value, label]) => {
      const option = document.createElement('option');
      option.value = value;
      option.textContent = label;
      return option;
    }));
    titlePicker.value = currentChoice;
    titlePicker.disabled = false;
  };
  // Keep the picker visible even with one title, and make the server's
  // titles available while persistent storage is being read.
  showTitles([]);
  const cache = storageSupported() ? await cacheAPI() : null;
  let uploaded = [];
  try { uploaded = cache?.listCachedTitles ? await cache.listCachedTitles() : []; } catch { uploaded = []; }
  showTitles(uploaded);
}
titlePicker.onchange = () => {
  const next = new URLSearchParams(location.search.replace(/^\?/, ''));
  const [source, id] = titlePicker.value.split(':');
  next.set('title', id);
  next.delete('app');
  if (source === 'package' && stagedTitles.includes(id)) next.set('source', 'package');
  else next.delete('source');
  location.search = next.toString();
};
refreshTitlePicker();

function stop(keepsStatus) {
  clearInputs();
  if (worker) showStats();
  worker?.terminate(); worker = null; running = false;
  for (const source of audioSources) { source.stop(); source.disconnect(); }
  audioSources.clear(); audioNext.clear();
  for (const port of [...audioRings.keys()]) stopAudioRing(port);
  dialog = null; dialogBox.hidden = true;
  ime = null; imeBox.hidden = true;
  runButton.disabled = false; stopButton.disabled = true;
  updateTouchVisibility();
  launch(null);
  if (!frames) {
    welcome.querySelector('h2').textContent = 'Ready when you are.';
    welcome.querySelector('p').textContent = 'Press Play to launch the game.';
  }
  if (!keepsStatus) status.textContent = 'Stopped';
}
beepButton.onclick = () => {
  ensureAudio();
  if (!audioCtx) { log('audio: no AudioContext in this browser'); return; }
  log('audio: beep ctx=' + audioCtx.state);
  const osc = audioCtx.createOscillator(), gain = audioCtx.createGain();
  osc.frequency.value = 660; gain.gain.value = 0.2;
  osc.connect(gain); gain.connect(audioGain);
  osc.start(); osc.stop(audioCtx.currentTime + 0.25);
};
async function run() {
  if (!TITLE) { notice('Upload the firmware and a game package first (below the player).'); return; }
  stop(); notice('');
  frames = 0; gpuFrames = 0; pixelFrames = 0; fps = 0; firstFrameAt = 0; logBox.textContent = ''; logLines.length = 0; logDirty = false; startedAt = performance.now();
  lastPresentAt = 0; presentedOnce = false; watchdogWarned = false;
  welcome.hidden = false;
  welcome.querySelector('h2').textContent = 'Starting your game…';
  welcome.querySelector('p').textContent = 'The first launch can take a little while.';
  launchStartedAt = performance.now();
  stageTotals = { files: 0, bytes: 0 };
  launch('Loading the WebAssembly runtime…', `${backend === 'jit' ? 'JIT' : 'interpreter'} backend`, undefined);
  display.focus({ preventScroll: true });
  screen.hidden = true;
  document.querySelector('#gpu-screen').hidden = !presentToCanvas;
  audioChunks = 0; audioBytes = 0; audioLogged = false; audioFirstAt = 0; audioPeak = 0;
  status.textContent = 'loading module…';
  if (webgpuBlocked) log('warning: ' + webgpuBlocked);
  runButton.disabled = true; stopButton.disabled = false;
  ensureAudio();
  const workerParams = new URLSearchParams({ backend, memory, inlineMutex: params.get('inlineMutex') === '0' ? '0' : '1' });
  if (buildAot !== null) workerParams.set('buildAot', buildAot);
  for (const name of ['fpsHack', 'scale', 'surfaceSync', 'maxInFlight', 'cores', 'hleProfile', 'gles',
    'readback', 'stampLru', 'writeObserver', 'regionCache', 'textureVerify', 'threads', 'asyncScene', 'hleIntrinsics']) {
    if (params.has(name)) workerParams.set(name, params.get(name));
  }
  const currentWorker = worker = new Worker(`./worker.js?${workerParams}`, { type: 'module' });
  worker.onerror = (event) => { if (worker !== currentWorker) return; log('worker error: ' + event.message); status.textContent = 'Worker error'; notice(event.message); stop(true); };
  worker.onmessage = async ({ data }) => {
    if (worker !== currentWorker || !data || typeof data !== 'object') return;
    switch (data.type) {
      case 'lifecycle': log('lifecycle: ' + data.state); break;
      case 'ready':
        status.textContent = 'staging content…';
        launch('Reading the file manifest…', `${data.diagnostics?.backend || backend} · ${data.diagnostics?.memoryModel || memory}`, undefined);
        log(`ready (backend=${data.diagnostics?.backend} memory=${data.diagnostics?.memoryModel} inlineMutex=${data.diagnostics?.inlineMutex})`);
        if (presentToCanvas) {
          try { attachCanvas(); } catch (error) {
            log('canvas transfer failed: ' + error); status.textContent = 'canvas transfer failed'; stop(true); break;
          }
        }
        try {
          // Same title query as the config: the server scopes its manifest to
          // this title (firmware, its patch, and the app directory only when
          // the server has it staged). The title's own files may instead live
          // in persistent storage (an uploaded package), so merge those in,
          // marked storage-only: a miss there is an error, not a doomed fetch.
          const response = await fetch('./manifest.json' + configQuery);
          if (!response.ok) throw new Error('HTTP ' + response.status);
          // A package boot of a server-staged title takes firmware and
          // patches from the server, not its copy of the game.
          const appPrefix = `ux0/app/${APP}/`;
          const serverFiles = (await response.json())
            .filter((file) => params.get('patches') !== '0' || !file.path.startsWith('patch/'))
            .filter((file) => fromServer || !(file.path.startsWith(appPrefix) || file.path.startsWith('ux0/user/')));
          if (worker !== currentWorker) return;
          const cache = await cacheAPI();
          // Shared with the stage-need/stage-store handlers below.
          cacheNS = cache;
          const storedManifest = cache ? await cache.cacheReadManifest(cacheKey) : null;
          const serverPaths = new Set(serverFiles.map((file) => file.path));
          // When the server stages this title's app directory, that directory
          // is the server's: a stored copy (an older staging) adds nothing to it.
          const serverHasApp = serverFiles.some((file) => file.path.startsWith(appPrefix));
          const storedOnly = fromServer ? [] : (storedManifest?.files ?? []).filter((file) => !serverPaths.has(file.path)
            && !(serverHasApp && file.path.startsWith(appPrefix)));
          // Firmware uploaded on its own fills what neither the server nor
          // the package has (os0/vs0), read from its shared key.
          const firmwareManifest = cache ? await cache.cacheReadManifest(cache.FIRMWARE_KEY) : null;
          const known = new Set([...serverPaths, ...storedOnly.map((file) => file.path)]);
          const firmwareOnly = (firmwareManifest?.files ?? []).filter((file) => !known.has(file.path));
          stageKeys = new Map(firmwareOnly.map((file) => [file.path, cache.FIRMWARE_KEY]));
          const staged = [...serverFiles, ...storedOnly, ...firmwareOnly];
          if (!staged.some((file) => file.path.startsWith('os0/')))
            notice('No firmware: upload it (a .zip of its os0 and vs0 folders) below the player.');
          // An empty manifest is allowed (a fixture, or a title with nothing
          // staged yet): say what to do, then let the launch report the rest.
          if (!staged.length)
            notice(`No content for ${TITLE}: upload its .zip package (or stage it on the server) first.`);
          stageTotals = { files: staged.length, bytes: staged.reduce((sum, file) => sum + (file.size || 0), 0) };
          stageNeeded = staged.map((file) => ({ path: file.path, size: file.size,
            ...(Number.isSafeInteger(file.version) ? { version: file.version } : {}) }));
          stageCacheActive = storageSupported() && !!cache;
          stageCacheIndex = stageCacheActive ? cache.cacheIndexFor(storedManifest?.files, stageNeeded) : null;
          if (stageCacheIndex) for (const [path, size] of cache.cacheIndexFor(firmwareOnly, stageNeeded)) stageCacheIndex.set(path, size);
          const useContentCache = (stageCacheIndex?.size ?? 0) > 0;
          document.body.dataset.cacheVerdict = stageCacheActive
            ? `${stageCacheIndex.size} of ${stageNeeded.length} files in storage`
              + (storedOnly.length ? `, ${storedOnly.length} package-only` : '')
            : 'unsupported';
          log(`[vita3k-web] ${staged.length} files to stage (${storedOnly.length} from the package, ` +
            `${stageCacheIndex?.size ?? 0} in storage)`);
          stageSources.storage = 0;
          stageSources.downloaded = 0;
          const toFetch = stageTotals.files - (stageCacheIndex?.size ?? 0);
          const fetchBytes = staged.filter((file) => !stageCacheIndex?.has(file.path))
            .reduce((sum, file) => sum + (file.size || 0), 0);
          launch('Staging game files…', `${toFetch} of ${stageTotals.files} files to download` +
            (toFetch ? ` (${mib(fetchBytes)} MiB)` : '') + (toFetch ? `; ${stageCacheIndex?.size ?? 0} from storage` : ''),
            stageTotals.bytes ? 0 : undefined);
          worker.postMessage({ type: 'stage-files', root: '/vita', useContentCache,
            files: staged.map((file) => ({ ...file,
              url: serverPaths.has(file.path) ? `stage/${file.path}` : null })) });
        } catch (error) { if (worker !== currentWorker) return; log('manifest failed: ' + error); status.textContent = 'Staging failed'; notice(error.message); stop(true); }
        break;
      case 'stage-progress':
        // Live download status while the overlay reads "Starting your game…".
        // phase 'runtime' is the runtime .wasm (byte counts from the worker),
        // 'aot' the AOT download and 'aot-compiling' its device-side compile
        // (compile() exposes no progress events, so the bar holds full).
        if (data.phase === 'runtime' || data.phase === 'aot' || data.phase === 'aot-compiling') {
          const name = String(data.path || '').replace(/^.*\//, '') || 'WebAssembly runtime';
          const done = data.pathBytes || 0, total = data.pathSize || 0;
          if (data.phase === 'aot-compiling') {
            const have = data.pathSize ? `${mib(done)}${data.pathBytes ? ' downloaded' : ''}` : `${mib(done)} downloaded`;
            launch('Compiling the AOT module…', `${name} · ${have} · compiling on this device…`, 1);
          } else {
            const verb = data.phase === 'aot' ? 'Downloading' : 'Loading';
            const filePct = total > 0 && done > 0 ? Math.min(99, Math.floor(done / total * 100)) : null;
            launch(`${verb} ${name}` + (filePct === null ? '' : ` — ${filePct}%`),
              `${mib(done)}/${total ? mib(total) + ' ' : ''}MiB · ${elapsed()}`,
              total ? done / total : undefined);
          }
          break;
        }
        if (data.total) stageTotals.files = data.total;
        if (data.totalBytes) stageTotals.bytes = data.totalBytes;
        {
          const bytes = data.bytes || 0;
          const filePct = data.pathSize >= 1048576 && data.pathBytes > 0
            ? Math.min(99, Math.floor(data.pathBytes / data.pathSize * 100)) : null;
          if (data.source === 'cache') stageSources.storage += 1;
          else if (data.path) stageSources.downloaded += 1;
          const phase = data.source === 'cache' && data.path ? `Reading ${data.path} from storage`
            : (data.path ? `Downloading ${data.path}` : 'Staging game files…');
          launch(phase + (filePct === null ? '' : ` — ${filePct}%`),
            stagingDetail(data.index || 0, bytes), stageTotals.bytes ? bytes / stageTotals.bytes : undefined);
        }
        break;
      case 'stage-need': {
        // OPFS read-through for the worker's staging loop. A miss (or any
        // error) answers null and the worker downloads instead.
        let bytes = null;
        if (stageCacheActive && cacheNS && stageCacheIndex?.get(data.path) === data.size
            && Number.isSafeInteger(data.size) && data.size >= 0 && typeof data.path === 'string') {
          try {
            const hit = await cacheNS.cacheReadFile(stageKeys.get(data.path) ?? cacheKey, data.path);
            if (hit && hit.byteLength === data.size) bytes = hit;
          } catch { bytes = null; }
        }
        worker.postMessage({ type: 'stage-data', path: data.path, bytes: bytes ? bytes.buffer : null },
          bytes ? [bytes.buffer] : []);
        break;
      }
      case 'stage-store': {
        if (stageCacheActive && cacheNS && typeof data.path === 'string' && data.bytes instanceof ArrayBuffer) {
          try {
            await cacheNS.cacheWriteFile(cacheKey, data.path, new Uint8Array(data.bytes));
          } catch {
            stageCacheActive = false;
            log('[vita3k-web] content cache write failed; continuing without cache');
          }
        }
        break;
      }
      case 'aot-compiled':
        // Device-side compile finished; the worker now boots the title and the
        // runtime reports `AOT on / off / REJECTED` in the log. Do not leave the
        // overlay on "Compiling…" while the game runs (it used to stick there
        // forever, with a 0.0 MiB byte count, even on successful AOT runs).
        launch('Launching the game…', `${mib(data.bytes || 0)} MiB AOT ready` +
          ` (download ${(data.downloadMs / 1000).toFixed(1)}s, compile ${(data.compileMs / 1000).toFixed(1)}s) · ${elapsed()}`, 1);
        break;
      case 'staged':
        status.textContent = 'running';
        launch('Launching the game…', `${data.files} files` +
          (data.cachedFiles ? ` (${data.cachedFiles} from storage)` : '') +
          ` · ${mib(data.bytes)} MiB staged · ${elapsed()}`, 1);
        if (stageCacheActive && stageNeeded && cacheNS)
          cacheNS.cacheWriteManifest(cacheKey, stageNeeded).catch(() => {});
        log(`staged ${data.files} files (${(data.bytes / 1048576).toFixed(1)} MiB) — launching`);
        // Saves this browser kept for the title replace the staged ones.
        const saveAPI = storageSupported() ? await cacheAPI() : null;
        const saves = saveAPI ? await saveAPI.readSaves(TITLE).catch(() => []) : [];
        if (worker !== currentWorker) return;
        if (saves.length) log(`restoring ${saves.length} saved file(s) from this browser`);
        worker.postMessage({ type: 'run-app', vitaFs: data.root, title: TITLE, app: APP, fastVblank, saves,
          ...(aotUrl ? { aotUrl } : {}),
          ...(AOT_MT_URL ? { aotMtUrl: AOT_MT_URL } : {}) }, saves.map((file) => file.bytes.buffer));
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
        playAudioPCM(data.freq, data.channels, data.frames, data.data, data.port ?? 0);
        break;
      case 'vita-audio-ring': playAudioRing(data); break;
      case 'vita-saves': {
        // The game saved: keep the files in this browser (save_sync.js), one
        // batch after another.
        const batch = data;
        saveChain = saveChain.then(async () => {
          const saveAPI = storageSupported() ? await cacheAPI() : null;
          if (!saveAPI) { log('saves: no persistent storage in this browser; the save lasts until reload'); return; }
          const data = batch;
          try {
            for (const file of data.files) await saveAPI.writeSave(data.title, file.path, new Uint8Array(file.bytes));
            for (const path of data.removed) await saveAPI.removeSave(data.title, path);
            log(`saves: stored ${data.files.length} file(s)${data.removed.length ? `, removed ${data.removed.length}` : ''} for ${data.title}`);
          } catch (error) {
            log('saves: storing failed: ' + (error?.message || error));
            notice('Could not store the save in this browser: ' + (error?.message || error));
          }
        });
        break;
      }
      case 'vita-dialog': onDialog(data.dialog); break;
      case 'vita-ime': onIme(data.ime); break;
      case 'vita-gxm-throttle': {
        const info = data.throttle ?? {};
        log(`[gxm-throttle] Waiting: ${info.inFlight} submissions queued (max ${info.maxInFlight}); ` +
          `${info.throttledScenes} scene retries, no scene data discarded`);
        notice('Rendering is waiting for graphics work to finish. A lower resolution may help.');
        break;
      }
      case 'vita-gxm-device': {
        const info = data.device ?? {};
        log(`[gxm-device] lost reason=${info.reason ?? 'unknown'} message=${info.message ?? ''}`);
        status.textContent = info.reason === 'queue-error' ? 'Graphics queue stalled' : 'Graphics device lost';
        notice(`${status.textContent} (${info.reason ?? 'unknown'}). Stop and press Play to restart.`);
        break;
      }
      case 'vita-exit':
        // Report first, then release the worker: stop() must not overwrite the
        // outcome the viewer is waiting to read.
        status.textContent = `exit ${data.exitCode} (${data.ok ? 'ok' : 'failed'})`;
        log('exit: ' + JSON.stringify(data));
        stop(true);
        break;
      case 'log': {
        log(data.message);
        // A rejected/stale AOT image otherwise fails silent (the run just
        // continues on the fallback JIT with aot=0). Surface the runtime's
        // own verdict so a version/content mismatch is visible in the UI.
        if (typeof data.message === 'string' && data.message.includes('AOT REJECTED')) {
          const reason = data.message.replace(/^.*AOT REJECTED:\s*/, '');
          notice(`AOT module rejected (${reason}). Running on the JIT fallback — rebuild the AOT image.`);
        }
        break;
      }
      case 'error': log('ERROR ' + data.message); status.textContent = 'Runtime error'; notice(data.message); stop(true); break;
      default: break;
    }
  };
}
runButton.disabled = false;
runButton.onclick = () => run().catch((error) => { log('ERROR ' + error.message); status.textContent = 'Launch failed'; notice(error.message); stop(true); });
stopButton.onclick = () => stop(false);
if (params.get('auto') === '1') runButton.click();
