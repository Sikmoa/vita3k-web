// End-to-end sceMsgDialog in real Chromium: the genuine VitaSDK fixture
// (vita_msg_dialog_fixture, target vita3k_vita_msg_dialog_fixture) is staged
// as an app and runs on the retail-app path. Its Yes/No dialog is answered
//   1. by the headless probe's auto-answer (limbo_app_chromium.mjs
//      LIMBO_DIALOG=cross and =circle), which also logs the dialog, and
//   2. with keys on the limbo_serve.mjs page, which draws it over the screen.
// The fixture's exit code is 100 + the button id the guest read back.
//
// Usage (from the repository root, after building vita3k_web_jit and the fixture):
//   PLAYWRIGHT_MODULE_URL=file://$PWD/build/playwright/node_modules/playwright/index.mjs \
//     node browser/tests/msg_dialog_chromium.mjs
// MSG_DIALOG_FIXTURE overrides the fixture eboot (default: the web64 build's);
// PLAYWRIGHT_CHROMIUM_EXECUTABLE and GXM_RUNTIME_DIST pass through.
import { spawn } from 'node:child_process';
import { copyFile, mkdir, mkdtemp, rm } from 'node:fs/promises';
import { createServer } from 'node:net';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import assert from 'node:assert/strict';

const fixture = resolve(process.env.MSG_DIALOG_FIXTURE
  || 'build/web64/browser/tests/vita_msg_dialog_fixture/eboot.bin');
const title = 'MSGDLG001';
const message = 'Message dialog fixture: continue?';
const stage = await mkdtemp(join(tmpdir(), 'msg-dialog-'));
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
  // 1. Headless probe: every dialog is logged and answered with LIMBO_DIALOG.
  for (const [answer, buttonId] of [['cross', 1], ['circle', 2]]) {
    const probe = start('browser/tests/limbo_app_chromium.mjs', { LIMBO_DIALOG: answer,
      LIMBO_DEADLINE_MS: '60000', LIMBO_MAX_FRAMES: '0', LIMBO_FRAME_OUT: join(stage, 'frame') });
    const code = await probe.exited;
    const report = JSON.parse(probe.output.slice(probe.output.indexOf('{'), probe.output.lastIndexOf('}') + 1));
    assert.equal(code, 0, probe.output.slice(-4000));
    assert.deepEqual(report.exit, { exitCode: 100 + buttonId, ok: true }, `LIMBO_DIALOG=${answer}`);
    assert.deepEqual(report.dialogs.map(({ id, message, buttons, answer, buttonId, result }) =>
      ({ id, message, buttons, answer, buttonId, result })),
    [{ id: 1, message, buttons: ['Yes', 'No'], answer, buttonId, result: 0 }]);
    console.log(`probe LIMBO_DIALOG=${answer}: exit ${report.exit.exitCode}, dialog ${JSON.stringify(report.dialogs[0])}`);
  }

  // 2. The dev server page: the dialog is drawn over the screen; right arrow
  // highlights No, X (cross) answers.
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
  const page = await browser.newPage();
  const pageErrors = [];
  page.on('pageerror', (error) => pageErrors.push(String(error)));
  await page.goto(`http://127.0.0.1:${port}/?auto=1`);
  const dialog = page.locator('#dialog');
  await dialog.waitFor({ state: 'visible', timeout: 60000 });
  assert.equal(await page.locator('#dialog-message').textContent(), message);
  const buttons = page.locator('#dialog-buttons button');
  assert.deepEqual(await buttons.allTextContents(), ['Yes', 'No']);
  assert.deepEqual(await buttons.evaluateAll((all) => all.map((b) => b.className)), ['selected', '']);
  await page.keyboard.press('ArrowRight');
  assert.deepEqual(await buttons.evaluateAll((all) => all.map((b) => b.className)), ['', 'selected']);
  await page.keyboard.press('KeyX');
  await page.waitForFunction(() => document.querySelector('#status').textContent.startsWith('exit '), null, { timeout: 60000 });
  const status = await page.locator('#status').textContent();
  const log = await page.locator('#log').textContent();
  assert.equal(status, 'exit 102 (ok)', log.slice(-3000));
  assert.equal(await dialog.isHidden(), true);
  assert.deepEqual(pageErrors, []);
  console.log(`page: right arrow + X -> ${status}`);
} finally {
  await browser?.close();
  server?.kill();
  await rm(stage, { recursive: true, force: true });
}
