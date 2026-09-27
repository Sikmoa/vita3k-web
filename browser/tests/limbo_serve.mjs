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
// ?patches=0 skips the title patches of browser/patches (e.g. Limbo's 60 FPS
// patch) otherwise staged as <vita fs>/patch/;
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
// A guest on-screen keyboard (SceIme) is a text field over the screen that
// takes the keyboard while it is open: Enter presses the keyboard's enter
// key, Escape closes it.
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
//   GXM_RUNTIME_DIST    built dist (default build/web64/dist; target vita3k_web_dist)
//   LIMBO_AOT           ahead-of-time module for the title (AOT.md), served as
//                       /aot.wasm and passed to run-app as aotUrl
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { networkInterfaces } from 'node:os';
import { resolve } from 'node:path';
import { existsSync, readdirSync } from 'node:fs';
import { readRuntimeFile, readStageFile, runtimeRoot as root, stageFiles, stageManifest } from './runtime_routes.mjs';

const port = Number(process.env.PORT || 8080);
const host = process.env.HOST || '127.0.0.1';
const stage = resolve(process.env.LIMBO_STAGE || '.limbo_work/stage');
const title = process.env.LIMBO_TITLE || 'PCSE00268';
const app = process.env.LIMBO_APP || title;
const aotPath = process.env.LIMBO_AOT ? resolve(process.env.LIMBO_AOT) : '';

if (!existsSync(resolve(stage, 'ux0/app', app, 'eboot.bin')))
  throw new Error(`no staged app at ${resolve(stage, 'ux0/app', app, 'eboot.bin')} (set LIMBO_STAGE/LIMBO_APP)`);
if (!existsSync(root) || !readdirSync(root, { recursive: true }).some((file) => file.endsWith('vita3k_web_jit.wasm')))
  throw new Error(`no built JIT module in ${root} (build vita3k_web_dist first, or set GXM_RUNTIME_DIST)`);
if (aotPath && !existsSync(aotPath)) throw new Error(`no AOT module at ${aotPath} (LIMBO_AOT)`);

const staged = await stageFiles(stage);
const manifestBytes = new TextEncoder().encode(JSON.stringify(stageManifest(staged)));

const server = createServer(async (req, res) => {
  try {
    const path = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
    const send = (content, type) => {
      res.writeHead(200, { 'Content-Type': type, 'Content-Length': content.length });
      res.end(content);
    };
    const body = (content, type) => send(content, type);
    if (path === '/') {
      const { content, type } = await readRuntimeFile('/player.html');
      return send(content, type);
    }
    if (path === '/player-config.json')
      return body(Buffer.from(JSON.stringify({ title, app, aot: Boolean(aotPath) })), 'application/json');
    if (path === '/manifest.json') return body(manifestBytes, 'application/json');
    if (path === '/favicon.ico') { res.writeHead(404); return res.end(); }
    if (path === '/aot.wasm' && aotPath) return send(await readFile(aotPath), 'application/wasm');
    if (path.startsWith('/stage/'))
      return send(await readStageFile(staged, path.slice('/stage/'.length)), 'application/octet-stream');
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
