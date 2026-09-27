// End-to-end SceIme in real Chromium: the genuine VitaSDK fixture
// (vita_ime_fixture, target vita3k_vita_ime_fixture) is staged as an app and
// opens the on-screen keyboard with the text "Vita". It is answered
//   1. by the headless probe (limbo_app_chromium.mjs LIMBO_IME=<text>, which
//      replaces the field and presses Enter), and
//   2. on the limbo_serve.mjs page, by typing into its text field and pressing
//      Enter, then (second run) Escape.
// The fixture exits 100 when its handler saw "Grüße, Vita!" and Enter, 101 when
// the keyboard was closed, 11/12 when another text arrived.
//
// Usage (from the repository root, after building vita3k_web_jit and the fixture):
//   PLAYWRIGHT_MODULE_URL=file://$PWD/build/playwright/node_modules/playwright/index.mjs \
//     node browser/tests/ime_chromium.mjs
// IME_FIXTURE overrides the fixture eboot (default: the web64 build's);
// PLAYWRIGHT_CHROMIUM_EXECUTABLE and GXM_RUNTIME_DIST pass through.
import { spawn } from 'node:child_process';
import { copyFile, mkdir, mkdtemp, rm } from 'node:fs/promises';
import { createServer } from 'node:net';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import assert from 'node:assert/strict';

const fixture = resolve(process.env.IME_FIXTURE || 'build/web64/browser/tests/vita_ime_fixture/eboot.bin');
const title = 'IMEFIX001';
const expected = 'Grüße, Vita!';
const stage = await mkdtemp(join(tmpdir(), 'ime-'));
await mkdir(join(stage, 'ux0/app', title), { recursive: true });
await copyFile(fixture, join(stage, 'ux0/app', title, 'eboot.bin'));
const env = { ...process.env, LIMBO_STAGE: stage, LIMBO_TITLE: title };

function start(script, extra) {
  const child = spawn(process.execPath, [script], { env: { ...env, ...extra }, stdio: ['ignore', 'pipe', 'pipe'] });
  child.output = '';
  child.stdout.on('data', (chunk) => { child.output += chunk; });
  child.stderr.on('data', (chunk) => { child.output += chunk; });
  child.exited = new Promise((done) => child.on('exit', done));
  return child;
}

let server, browser;
try {
  // 1. Headless probe: the keyboard is logged and answered with LIMBO_IME.
  for (const [text, exitCode] of [[expected, 100], ['something else', 11]]) {
    const probe = start('browser/tests/limbo_app_chromium.mjs', { LIMBO_IME: text,
      LIMBO_DEADLINE_MS: '60000', LIMBO_MAX_FRAMES: '0', LIMBO_FRAME_OUT: join(stage, 'frame') });
    const code = await probe.exited;
    const lines = probe.output.split('\n');
    const first = lines.indexOf('{');
    const report = JSON.parse(lines.slice(first, lines.indexOf('}', first) + 1).join('\n'));
    assert.equal(code, 0, probe.output.slice(-4000));
    assert.deepEqual(report.exit, { exitCode, ok: true }, `LIMBO_IME=${text}`);
    assert.deepEqual(report.imes.map(({ id, text: shown, maxLength, answer }) => ({ id, shown, maxLength, answer })),
      [{ id: 1, shown: 'Vita', maxLength: 64, answer: text }]);
    assert.equal(typeof report.imes[0].closedMs, 'number');
    console.log(`probe LIMBO_IME=${JSON.stringify(text)}: exit ${report.exit.exitCode}`);
  }

  // 2. The dev server page: a text field over the screen.
  const port = await new Promise((done) => {
    const probe = createServer().listen(0, '127.0.0.1', () => { const { port } = probe.address(); probe.close(() => done(port)); });
  });
  server = start('browser/tests/limbo_serve.mjs', { PORT: String(port), HOST: '127.0.0.1' });
  for (let i = 0; !server.output.includes(`:${port}/`); ++i) {
    assert.ok(i < 100 && server.exitCode === null, `limbo_serve.mjs did not start:\n${server.output}`);
    await new Promise((done) => setTimeout(done, 100));
  }
  const { chromium } = await import(process.env.PLAYWRIGHT_MODULE_URL || 'playwright');
  browser = await chromium.launch({ headless: true,
    args: ['--enable-unsafe-webgpu', '--use-angle=swiftshader', '--enable-features=Vulkan', '--disable-vulkan-surface'],
    ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}) });
  for (const [key, status] of [['Enter', 'exit 100 (ok)'], ['Escape', 'exit 101 (ok)']]) {
    const page = await browser.newPage();
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(String(error)));
    await page.goto(`http://127.0.0.1:${port}/?auto=1`);
    const field = page.locator('#ime-text');
    await field.waitFor({ state: 'visible', timeout: 60000 });
    assert.equal(await field.inputValue(), 'Vita');
    assert.equal(await page.evaluate(() => document.activeElement?.id), 'ime-text');
    // Typed keys go to the field, not to the guest pad (X is cross there).
    await field.press('End');
    await page.keyboard.type('X');
    assert.equal(await field.inputValue(), 'VitaX');
    await field.fill(expected);
    await page.keyboard.press(key);
    await page.waitForFunction(() => document.querySelector('#status').textContent.startsWith('exit '), null, { timeout: 60000 });
    const log = await page.locator('#log').textContent();
    assert.equal(await page.locator('#status').textContent(), status, log.slice(-3000));
    assert.equal(await page.locator('#ime').isHidden(), true);
    assert.deepEqual(pageErrors, []);
    console.log(`page: typed ${JSON.stringify(expected)} + ${key} -> ${status}`);
    await page.close();
  }
} finally {
  await browser?.close();
  server?.kill();
  await rm(stage, { recursive: true, force: true });
}
