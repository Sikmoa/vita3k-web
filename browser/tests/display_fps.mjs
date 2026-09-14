// M14c browser FPS benchmark: steady-state frame rate of the real display
// page (browser/web/display.html) on both CPU backends, plus the runtime's
// own final "JIT stats" / "JIT profile" console lines. Headless Chromium via
// the Playwright installation under build/playwright; Firefox runs too when
// that installation provides it, otherwise its absence is reported.
//
// Usage:
//   node browser/tests/display_fps.mjs [dist-dir] [fixture-eboot.bin]
//
//   [dist-dir]       staged browser dist (default build/web/dist) containing
//                    display.html, display.js, worker.js and both
//                    vita3k_web{,_jit}.{js,wasm} modules
//   [fixture-eboot]  eboot served as ./display-eboot.bin (default: the short
//                    60-frame build fixture when present, else the staged
//                    dist fixture). The fixture choice is printed and hashed.
//
// Environment:
//   PLAYWRIGHT_MODULE_URL         Playwright module (default build/playwright)
//   PLAYWRIGHT_CHROMIUM_EXECUTABLE  override Chromium executable
//   PLAYWRIGHT_FIREFOX_EXECUTABLE   override Firefox executable
//   DISPLAY_FPS_TIMEOUT_MS        per-run deadline (default 900000)
//   DISPLAY_FPS_WARMUP_MS         warm-up window in ms after the first frame
//                                 (default 15000)
//   DISPLAY_FPS_SKIP_FRAMES       warm-up window in frames (default 20)
//   DISPLAY_FPS_RUNS              runs per backend per browser (default 1)
//
// Steady-state policy (printed with the results): the warm-up ends at
// whichever of {first DISPLAY_FPS_SKIP_FRAMES frames, first
// DISPLAY_FPS_WARMUP_MS after the first frame} is reached first; FPS is
// intervals over the steady window. The default fixture is the SHORT variant
// so both backends run to completion (exit 77) and the runtime's final JIT
// stats/profile lines are captured; pass the staged dist fixture for a
// 600-frame run.
//
// Constraints honored: ephemeral HTTP server bound to 127.0.0.1 on a random
// port (never 5173 - that is the user's demo server), no process is ever
// killed by name (only this script's own browser instances are closed).
import { createHash } from 'node:crypto';
import { access, readFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { createServer } from 'node:http';
import { dirname, extname, resolve, sep } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const repository = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const root = resolve(process.argv[2] || resolve(repository, 'build/web/dist'));
const shortFixture = resolve(repository, 'build/web/browser/tests/vita_display_fixture/eboot-short.bin');
const stagedFixture = resolve(root, 'display-eboot.bin');
const fixture = resolve(process.argv[3]
  || (existsSync(shortFixture) ? shortFixture : stagedFixture));
const timeoutMs = Number(process.env.DISPLAY_FPS_TIMEOUT_MS || 900000);
const warmupMs = Number(process.env.DISPLAY_FPS_WARMUP_MS || 15000);
const skipFrames = Number(process.env.DISPLAY_FPS_SKIP_FRAMES || 20);
const runs = Number(process.env.DISPLAY_FPS_RUNS || 1);
if (!Number.isSafeInteger(timeoutMs) && timeoutMs > 0) throw new Error('bad DISPLAY_FPS_TIMEOUT_MS');
if (!Number.isSafeInteger(warmupMs) && warmupMs >= 0) throw new Error('bad DISPLAY_FPS_WARMUP_MS');
if (!Number.isSafeInteger(skipFrames) && skipFrames >= 0) throw new Error('bad DISPLAY_FPS_SKIP_FRAMES');
if (!Number.isSafeInteger(runs) && runs >= 1) throw new Error('bad DISPLAY_FPS_RUNS');

const required = ['display.html', 'display.js', 'worker.js', 'storage.js',
  'vita3k_web.js', 'vita3k_web.wasm', 'vita3k_web_jit.js', 'vita3k_web_jit.wasm'];
for (const file of [...required.map((file) => resolve(root, file)), fixture]) {
  if (!existsSync(file)) {
    console.error(`[display-fps] missing ${file} (stage the browser build and fixture first)`);
    process.exit(2);
  }
}
const fixtureBytes = await readFile(fixture);
const fixtureSha256 = createHash('sha256').update(fixtureBytes).digest('hex');

const playwrightUrl = process.env.PLAYWRIGHT_MODULE_URL
  || pathToFileURL(resolve(repository, 'build/playwright/node_modules/playwright/index.mjs')).href;
const { chromium, firefox } = await import(playwrightUrl);

// Ephemeral loopback server: serves the dist directory with COOP/COEP and
// remaps /display-eboot.bin to the chosen fixture (same trick as
// display_smoke.mjs). Random port; never 5173.
const server = createServer(async (req, res) => {
  try {
    const pathname = decodeURIComponent(new URL(req.url, 'http://127.0.0.1').pathname);
    const file = pathname === '/display-eboot.bin'
      ? fixture
      : resolve(root, `.${pathname === '/' ? '/index.html' : pathname}`);
    if (file !== fixture && !file.startsWith(root + sep)) throw new Error('path outside dist');
    const type = {
      '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm',
      '.html': 'text/html', '.json': 'application/json', '.bin': 'application/octet-stream',
    }[extname(file)] || 'application/octet-stream';
    res.writeHead(200, {
      'Content-Type': type,
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
      'Cache-Control': 'no-store',
    });
    res.end(await readFile(file));
  } catch {
    res.writeHead(404);
    res.end();
  }
});
await new Promise((resolveListen, rejectListen) => {
  server.once('error', rejectListen);
  server.listen(0, '127.0.0.1', resolveListen);
});
const origin = `http://127.0.0.1:${server.address().port}`;

// One measured run: loads the real display page for a backend, records frame
// timestamps from the page's own Worker messages and the console lines the
// page relays (worker 'log' messages, including the runtime's final JIT
// stats/profile), and waits for the fixture to complete (vita-exit).
async function measureRun(browser, backend) {
  const context = await browser.newContext();
  const consoleLines = [];
  try {
    const page = await context.newPage();
    page.on('pageerror', (error) => consoleLines.push(`pageerror: ${String(error)}`));
    page.on('crash', () => consoleLines.push('page crashed'));
    page.on('console', (message) => consoleLines.push(message.text()));
    await page.goto(`${origin}/display.html${backend === 'jit' ? '?backend=jit' : ''}`, { timeout: 60000 });
    const result = await page.evaluate(async ({ timeoutMs }) => {
      const startedAt = performance.now();
      const state = { frames: [], readyAt: null, exit: null, inPageErrors: [] };
      self.addEventListener('error', (event) => state.inPageErrors.push(String(event.message || event)));
      self.addEventListener('unhandledrejection', (event) => state.inPageErrors.push(`unhandledrejection: ${String(event.reason)}`));
      const worker = await new Promise((resolveWorker, rejectWorker) => {
        const deadline = Date.now() + 30000;
        const poll = () => {
          if (window.vita3kWeb?.worker) resolveWorker(window.vita3kWeb.worker);
          else if (Date.now() > deadline) rejectWorker(new Error('window.vita3kWeb.worker never appeared'));
          else setTimeout(poll, 100);
        };
        poll();
      });
      await new Promise((resolveRun, rejectRun) => {
        const timer = setTimeout(() => rejectRun(new Error(
          `display fps run timeout after ${performance.now() - startedAt} ms: frames=${state.frames.length}`)), timeoutMs);
        worker.addEventListener('error', (event) => {
          clearTimeout(timer);
          rejectRun(new Error(`worker error: ${event.message || 'unknown'}`));
        });
        worker.addEventListener('message', ({ data: msg }) => {
          if (!msg || typeof msg !== 'object') return;
          if (msg.type === 'ready') state.readyAt = performance.now();
          else if (msg.type === 'vita-frame') state.frames.push(performance.now());
          else if (msg.type === 'vita-exit') {
            state.exit = { exitCode: msg.exitCode, ok: msg.ok, atMs: performance.now() - startedAt };
            clearTimeout(timer);
            resolveRun();
          } else if (msg.type === 'error') {
            clearTimeout(timer);
            rejectRun(new Error(`worker posted error: ${msg.message}`));
          }
        });
      }).catch((error) => { state.failure = String(error); });
      return {
        frames: state.frames,
        readyAt: state.readyAt === null ? null : state.readyAt - startedAt,
        exit: state.exit,
        failure: state.failure || null,
        inPageErrors: state.inPageErrors,
        pageFramesReceived: window.vita3kWeb?.frames?.received ?? null,
        elapsedMs: performance.now() - startedAt,
      };
    }, { timeoutMs });
    return { backend, ...result, consoleLines };
  } finally {
    await context.close();
  }
}

// Steady-state window per the printed policy: warm-up ends at whichever of
// {skipFrames, warmupMs since the first frame} is reached first.
function steadyStats(frames) {
  if (frames.length < 2) return null;
  const first = frames[0];
  let start = 0; // first steady frame index
  while (start < frames.length - 1
    && start < skipFrames && frames[start] - first < warmupMs) start++;
  const steady = frames.slice(start);
  if (steady.length < 2) return null;
  const spanMs = steady[steady.length - 1] - steady[0];
  const deltas = [];
  for (let i = 1; i < steady.length; i++) deltas.push(steady[i] - steady[i - 1]);
  deltas.sort((a, b) => a - b);
  const mid = deltas.length >> 1;
  return {
    skipped: start,
    steadyFrames: steady.length,
    windowMs: spanMs,
    fps: spanMs > 0 ? (steady.length - 1) / (spanMs / 1000) : null,
    medianFrameMs: deltas.length % 2 ? deltas[mid] : (deltas[mid - 1] + deltas[mid]) / 2,
  };
}

const pick = (lines, needle) => lines.filter((line) => line.includes(needle));

function summarize(label, run) {
  const stats = steadyStats(run.frames);
  const jitStats = pick(run.consoleLines, 'JIT stats:');
  const jitProfile = pick(run.consoleLines, 'JIT profile:');
  const benchmark = pick(run.consoleLines, 'Vita benchmark:');
  return {
    label,
    backend: run.backend,
    ok: !run.failure && run.exit?.ok === true && stats !== null,
    failure: run.failure || null,
    exitCode: run.exit?.exitCode ?? null,
    frames: run.frames.length,
    pageFramesReceived: run.pageFramesReceived,
    readyAtMs: run.readyAt === null ? null : Number(run.readyAt.toFixed(0)),
    elapsedMs: Number(run.elapsedMs.toFixed(0)),
    steady: stats === null ? null : {
      skipped: stats.skipped,
      frames: stats.steadyFrames,
      windowMs: Number(stats.windowMs.toFixed(0)),
      fps: stats.fps === null ? null : Number(stats.fps.toFixed(3)),
      medianFrameMs: Number(stats.medianFrameMs.toFixed(1)),
    },
    inPageErrors: run.inPageErrors,
    runtimeLines: {
      jitStats: jitStats.length ? jitStats[jitStats.length - 1] : null,
      jitProfile: jitProfile.length ? jitProfile[jitProfile.length - 1] : null,
      benchmark: benchmark.length ? benchmark[benchmark.length - 1] : null,
    },
  };
}

const median = (values) => {
  const sorted = [...values].sort((a, b) => a - b);
  const mid = sorted.length >> 1;
  return sorted.length % 2 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2;
};

let exitStatus = 0;
const allResults = [];
let chromiumBrowser = null;
let firefoxBrowser = null;
const browserLaunchNotes = [];
try {
  chromiumBrowser = await chromium.launch({
    headless: true, timeout: 30000,
    ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE
      ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}),
  });

  // Firefox participation is decided by an actual installed executable under
  // this Playwright installation (or an explicit override) - not assumed.
  let firefoxExecutable = process.env.PLAYWRIGHT_FIREFOX_EXECUTABLE || null;
  if (!firefoxExecutable) {
    try { firefoxExecutable = firefox.executablePath(); } catch { firefoxExecutable = null; }
  }
  const firefoxInstalled = firefoxExecutable !== null && existsSync(firefoxExecutable);
  if (!firefoxInstalled) {
    browserLaunchNotes.push(
      `Playwright Firefox is not installed under build/playwright (expected ${firefoxExecutable || 'no executable path'}); skipping Firefox runs.`);
  } else {
    try {
      firefoxBrowser = await firefox.launch({ headless: true, timeout: 30000 });
    } catch (error) {
      browserLaunchNotes.push(`Playwright Firefox failed to launch (${String(error)}); skipping Firefox runs.`);
      firefoxBrowser = null;
    }
  }

  for (const [browserName, browser] of [['chromium', chromiumBrowser], ['firefox', firefoxBrowser]]) {
    if (!browser) continue;
    for (const backend of ['interpreter', 'jit']) {
      const runSummaries = [];
      for (let run = 1; run <= runs; run++) {
        const measured = await measureRun(browser, backend);
        const summary = summarize(`${browserName}/${backend} run ${run}`, measured);
        runSummaries.push(summary);
        allResults.push(summary);
        if (!summary.ok) exitStatus = 1;
      }
      const fpsValues = runSummaries.map((s) => s.steady?.fps).filter((v) => typeof v === 'number');
      console.log(`[display-fps] ${browserName}/${backend}: `
        + runSummaries.map((s) => `fps=${s.steady?.fps} (exit=${s.exitCode}, ${s.frames} frames, steady ${s.steady?.frames} frames / ${s.steady?.windowMs} ms, median ${s.steady?.medianFrameMs} ms/frame)${s.ok ? '' : ` FAILURE: ${s.failure}`}`).join(' | ')
        + (fpsValues.length > 1 ? ` | median fps=${Number(median(fpsValues).toFixed(3))}` : ''));
      const jit = runSummaries.find((s) => s.runtimeLines.jitStats);
      if (jit) {
        console.log(`[display-fps]   ${jit.runtimeLines.jitStats}`);
        if (jit.runtimeLines.jitProfile) console.log(`[display-fps]   ${jit.runtimeLines.jitProfile}`);
      }
      const bench = runSummaries.find((s) => s.runtimeLines.benchmark);
      if (bench) console.log(`[display-fps]   ${bench.runtimeLines.benchmark}`);
    }
  }

  for (const note of browserLaunchNotes) console.log(`[display-fps] ${note}`);

  // Final comparison line per browser (median across runs when repeated).
  for (const browserName of ['chromium', 'firefox']) {
    const perBackend = ['interpreter', 'jit'].map((backend) => {
      const values = allResults
        .filter((s) => s.label.startsWith(`${browserName}/${backend} `) && s.steady?.fps != null)
        .map((s) => s.steady.fps);
      return { backend, fps: values.length ? median(values) : null };
    });
    if (perBackend.some((entry) => entry.fps !== null)) {
      const interp = perBackend[0].fps, jit = perBackend[1].fps;
      console.log(`[display-fps] ${browserName} steady-state FPS: interpreter=${interp === null ? 'n/a' : Number(interp.toFixed(3))} jit=${jit === null ? 'n/a' : Number(jit.toFixed(3))}`
        + (interp && jit ? ` (jit/interpreter = ${Number((jit / interp).toFixed(3))})` : ''));
    }
  }
  console.log(`[display-fps] fixture=${fixture} sha256=${fixtureSha256} warmup=${warmupMs}ms|${skipFrames}frames dist=${root}`);
} catch (error) {
  console.error(`[display-fps] FAIL: ${error && error.stack ? error.stack : String(error)}`);
  exitStatus = 1;
} finally {
  try { await chromiumBrowser?.close(); }
  catch { /* best effort */ }
  try { await firefoxBrowser?.close(); }
  catch { /* best effort */ }
  finally { await new Promise((resolveClose) => server.close(resolveClose)); }
}
console.log(`[display-fps] JSON ${JSON.stringify({ fixture, fixtureSha256, warmupMs, skipFrames, runs, results: allResults })}`);
process.exit(exitStatus);
