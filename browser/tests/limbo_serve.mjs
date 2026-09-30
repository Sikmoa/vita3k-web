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
//   LIMBO_AOT           ahead-of-time module for the default title (AOT.md),
//                       served as /aot.wasm and passed to run-app as aotUrl
//   LIMBO_AOT_DIR       per-title AOT images (<TITLE>.aot.wasm), served as
//                       /aot/<TITLE>.wasm; AOT is on by default for every
//                       title that has an image (preferred over LIMBO_AOT)
import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
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
const aotDir = process.env.LIMBO_AOT_DIR ? resolve(process.env.LIMBO_AOT_DIR) : '';
// The AOT image belongs to the title it was built for (its code hashes).
// Prefer the per-title image; fall back to the legacy single file for the
// default app only.
const aotFileFor = (wantedTitle, wantedApp) => {
  if (aotDir && titleIdOk(wantedTitle)) {
    const candidate = resolve(aotDir, `${wantedTitle}.aot.wasm`);
    if (existsSync(candidate)) return candidate;
  }
  if (aotPath && wantedApp === app) return aotPath;
  return '';
};

// Every title staged under ux0/app with an eboot.bin is servable: the default
// one (LIMBO_TITLE/LIMBO_APP) and, without restarting the server, any other
// via ?title=<id> / ?app=<dir> on /player-config.json — useful for trying a
// homebrew next to the retail title. Each title keeps its own content cache.
const appRoot = resolve(stage, 'ux0/app');
const stagedTitles = existsSync(appRoot)
  ? readdirSync(appRoot, { withFileTypes: true })
    .filter((entry) => entry.isDirectory() && existsSync(resolve(appRoot, entry.name, 'eboot.bin')))
    .map((entry) => entry.name).sort()
  : [];
if (!stagedTitles.includes(app))
  throw new Error(`no staged app at ${resolve(appRoot, app, 'eboot.bin')} (set LIMBO_STAGE/LIMBO_APP);` +
    ` staged titles: ${stagedTitles.join(', ') || 'none'}`);
if (!existsSync(root) || !readdirSync(root, { recursive: true }).some((file) => file.endsWith('vita3k_web_jit.wasm')))
  throw new Error(`no built JIT module in ${root} (build vita3k_web_dist first, or set GXM_RUNTIME_DIST)`);
if (aotPath && !existsSync(aotPath)) throw new Error(`no AOT module at ${aotPath} (LIMBO_AOT)`);

const staged = await stageFiles(stage);
// A boot stages only its own title: firmware (os0/vs0), the app directory and
// that title's patch. Trophy data under ux0/user belongs to the retail title,
// so a homebrew does not pay for 130 MiB of another game's assets.
// A boot stages firmware (os0/vs0) and its patch always; the title's own
// files only when the server has them staged. A title the server does not
// know (an uploaded package) therefore gets a firmware-only manifest and
// takes its game files from persistent storage.
const manifestFor = (wantedApp, wantedTitle, isStaged) => new TextEncoder().encode(JSON.stringify(stageManifest(
  staged.filter((file) => {
    if (/^(os0|vs0)\//.test(file.path)) return true;
    if (file.path.startsWith('patch/')) return file.path === `patch/${wantedTitle}.txt`;
    if (!isStaged) return false;
    if (file.path.startsWith(`ux0/app/${wantedApp}/`)) return true;
    if (file.path.startsWith('ux0/user/')) return wantedApp === app;
    return false;
  }))));
// Title ids are directory names in the guest's filesystem; anything else
// would be a path traversal attempt.
const titleIdOk = (value) => /^[A-Za-z0-9_-]{1,24}$/.test(value);

const server = createServer(async (req, res) => {
  try {
    const requestUrl = new URL(req.url, 'http://localhost');
    const path = decodeURIComponent(requestUrl.pathname);
    const send = (content, type) => {
      // Dev server: the tree is edited live (player/worker/scene come from
      // source), so browsers must never cache these responses.
      res.writeHead(200, { 'Content-Type': type, 'Content-Length': content.length, 'Cache-Control': 'no-store' });
      res.end(content);
    };
    const body = (content, type) => send(content, type);
    if (path === '/') {
      const { content, type } = await readRuntimeFile('/player.html');
      return send(content, type);
    }
    if (path === '/player-config.json') {
      const wanted = requestUrl.searchParams.get('app') || requestUrl.searchParams.get('title') || app;
      if (!titleIdOk(wanted)) { res.writeHead(400, { 'Content-Type': 'text/plain' }); return res.end('bad title id'); }
      const isStaged = stagedTitles.includes(wanted);
      const wantedTitle = requestUrl.searchParams.get('title') || (wanted === app ? title : wanted);
      const aotFile = isStaged ? aotFileFor(wantedTitle, wanted) : '';
      return body(Buffer.from(JSON.stringify({ title: wantedTitle, app: wanted, staged: isStaged,
        aot: Boolean(aotFile), aotUrl: aotFile ? `/aot/${wantedTitle}.wasm` : null,
        titles: stagedTitles })), 'application/json');
    }
    if (path === '/manifest.json') {
      const wanted = requestUrl.searchParams.get('app') || requestUrl.searchParams.get('title') || app;
      if (!titleIdOk(wanted)) { res.writeHead(400, { 'Content-Type': 'text/plain' }); return res.end('bad title id'); }
      const wantedTitle = requestUrl.searchParams.get('title') || (wanted === app ? title : wanted);
      return body(manifestFor(wanted, wantedTitle, stagedTitles.includes(wanted)), 'application/json');
    }
    if (path === '/favicon.ico') { res.writeHead(404); return res.end(); }
    // The AOT image is ~100+ MB and only changes when rebuilt. Unlike the
    // live-edited web sources it is revalidatable: `no-cache` forces a cheap
    // conditional request (304, served from the browser cache) instead of a
    // full re-download on every launch, while a rebuilt image gets a new ETag
    // and is picked up immediately. Correctness never depends on the cache:
    // load_aot hash-verifies the module against the staged guest code anyway.
    const sendAot = async (file) => {
      const { size, mtimeMs } = await stat(file);
      const etag = `"${size.toString(36)}-${Math.floor(mtimeMs).toString(36)}"`;
      if (req.headers['if-none-match'] === etag) {
        res.writeHead(304, { ETag: etag, 'Cache-Control': 'no-cache' });
        return res.end();
      }
      res.writeHead(200, { 'Content-Type': 'application/wasm', 'Content-Length': size,
        'Cache-Control': 'no-cache', ETag: etag });
      return res.end(await readFile(file));
    };
    if (path === '/aot.wasm' && aotPath) return sendAot(aotPath);
    if (path.startsWith('/aot/') && path.endsWith('.wasm')) {
      const id = path.slice('/aot/'.length, -'.wasm'.length);
      if (!titleIdOk(id)) { res.writeHead(400, { 'Content-Type': 'text/plain' }); return res.end('bad title id'); }
      const file = aotFileFor(id, id);
      if (!file) { res.writeHead(404); return res.end('no AOT image for ' + id); }
      return sendAot(file);
    }
    if (path.startsWith('/stage/'))
      return send(await readStageFile(staged, path.slice('/stage/'.length)), 'application/octet-stream');
    const { content, type } = await readRuntimeFile(path);
    send(content, type);
  } catch (error) {
    res.writeHead(404); res.end(String(error?.code === 'ENOENT' ? 'not found' : error));
  }
});
server.listen(port, host, () => {
  console.log(`Limbo dev server: (title ${title}, ${staged.length} staged files)`);
  console.log(`  staged titles ${stagedTitles.join(', ')}  (open ?title=<id> for another)`);
  console.log(`  module root ${root}`);
  console.log(`  staged root ${stage}`);
  if (aotPath) console.log(`  AOT module  ${aotPath}`);
  if (aotDir) {
    const images = existsSync(aotDir)
      ? readdirSync(aotDir).filter((file) => file.endsWith('.aot.wasm')).sort() : [];
    console.log(`  AOT dir     ${aotDir} (${images.join(', ') || 'no images'})`);
  }
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
