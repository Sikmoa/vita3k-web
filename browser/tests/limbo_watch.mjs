// Headless frame watcher for the Limbo retail app. Drives the same page a
// person would open (limbo_serve.mjs) in headless Chromium and mirrors the
// presented frames into the workspace, one PNG per frame plus `latest.png`,
// so the render can be watched from a file viewer when no port can be
// forwarded to this machine.
//
//   node browser/tests/limbo_watch.mjs
//   LIMBO_DEADLINE_MS=900000 LIMBO_WATCH_INTERVAL_MS=5000 node browser/tests/limbo_watch.mjs
//
// Environment:
//   LIMBO_WATCH_OUT         output directory (default .limbo_work/live)
//   LIMBO_DEADLINE_MS       stop after this long (default 600000)
//   LIMBO_WATCH_INTERVAL_MS poll interval (default 10000)
//   LIMBO_MEMORY            auto|w64|w32 (default auto, as the page defaults)
//   LIMBO_SERVE_PORT        port for the child dev server (default 8098)
//   PLAYWRIGHT_MODULE_URL    Playwright entry point (as in the other probes)
//   LIMBO_GPU=1             drop the SwiftShader flags (use a real GPU)
import { spawn } from 'node:child_process';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';

const out = resolve(process.env.LIMBO_WATCH_OUT || '.limbo_work/live');
const deadlineMs = Number(process.env.LIMBO_DEADLINE_MS || 600000);
const intervalMs = Number(process.env.LIMBO_WATCH_INTERVAL_MS || 10000);
const memory = process.env.LIMBO_MEMORY || 'auto';
const port = Number(process.env.LIMBO_SERVE_PORT || 8098);

await mkdir(out, { recursive: true });
const server = spawn(process.execPath, ['browser/tests/limbo_serve.mjs'], {
  env: { ...process.env, PORT: String(port), HOST: '127.0.0.1' },
  stdio: ['ignore', 'inherit', 'inherit'],
});
const shutdown = () => { if (!server.killed) server.kill('SIGTERM'); };
process.on('exit', shutdown);
process.on('SIGINT', () => { shutdown(); process.exit(130); });

// Read-back mode: a canvas transferred to the worker cannot be read from the page.
const url = `http://127.0.0.1:${port}/?auto=1&memory=${memory}&present=readback`;
for (let attempt = 0; ; ++attempt) {
  try {
    const response = await fetch(url);
    if (response.ok) break;
  } catch { /* not listening yet */ }
  if (attempt > 100) throw new Error(`dev server did not come up on ${url}`);
  await new Promise((done) => setTimeout(done, 200));
}
console.log(`[watch] serving ${url}`);

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE_URL || 'playwright');
const args = ['--enable-unsafe-webgpu', '--use-angle=swiftshader', '--enable-features=Vulkan',
  '--disable-vulkan-surface'];
const browser = await chromium.launch({
  headless: true,
  args: process.env.LIMBO_GPU === '1' ? ['--enable-unsafe-webgpu'] : args,
  ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE
    ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}),
});
const page = await browser.newPage();
const pageErrors = [];
page.on('pageerror', (error) => pageErrors.push(String(error)));
await page.goto(url);

const started = Date.now();
let saved = 0, lastFrames = 0, firstFrameAt = null;
const snapshot = () => page.evaluate(() => ({
  status: document.querySelector('#status')?.textContent ?? '',
  stats: document.querySelector('#stats')?.textContent ?? '',
  log: document.querySelector('#log')?.textContent ?? '',
  width: document.querySelector('#screen')?.width ?? 0,
  height: document.querySelector('#screen')?.height ?? 0,
}));

try {
  for (;;) {
    const state = await snapshot();
    const frames = Number(/frames=(\d+)/.exec(state.stats)?.[1] || 0);
    const elapsed = ((Date.now() - started) / 1000).toFixed(0);
    if (frames > lastFrames && state.width) {
      // Read the canvas rather than screenshotting it: the baked pixels are
      // what the guest produced, independent of page compositing.
      const dataUrl = await page.evaluate(() => document.querySelector('#screen').toDataURL('image/png'));
      const png = Buffer.from(dataUrl.slice(dataUrl.indexOf(',') + 1), 'base64');
      if (!firstFrameAt) firstFrameAt = elapsed;
      await writeFile(resolve(out, 'latest.png'), png);
      await writeFile(resolve(out, `frame_${String(frames).padStart(5, '0')}.png`), png);
      saved += 1;
      lastFrames = frames;
      console.log(`[watch] t=+${elapsed}s frame ${frames} (${state.width}x${state.height}, ${png.length} bytes) -> ${out}`);
    } else {
      console.log(`[watch] t=+${elapsed}s frames=${frames} status=${state.status || 'starting'} ${state.stats}`);
    }
    await writeFile(resolve(out, 'status.json'), JSON.stringify({
      updatedAt: new Date().toISOString(), frames, firstFrameAt, saved, ...state,
    }, null, 2));
    if (state.status.startsWith('exit') || state.status.includes('failed')) {
      console.log(`[watch] page reported ${state.status}`);
      break;
    }
    if (Date.now() - started > deadlineMs) {
      console.log(`[watch] deadline reached after ${elapsed}s with ${frames} frames`);
      break;
    }
    await new Promise((done) => setTimeout(done, intervalMs));
  }
} finally {
  await browser.close().catch(() => {});
  server.kill('SIGTERM');
}
console.log(`[watch] done: ${saved} frame images in ${out}` +
  (firstFrameAt ? `, first frame after ${firstFrameAt}s` : ', no frame presented'));
if (pageErrors.length) console.log(`[watch] page errors: ${pageErrors.slice(0, 3).join(' | ')}`);
