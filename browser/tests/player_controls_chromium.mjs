// Exercise the real player and pointer capture with a tiny Worker fixture;
// no retail files or Wasm build are needed. This checks the input transport,
// not game compatibility. Run: node browser/tests/player_controls_chromium.mjs
// PLAYWRIGHT_MODULE_URL / PLAYWRIGHT_CHROMIUM_EXECUTABLE select local installs.
// PLAYER_SCREENSHOTS optionally saves desktop/phone layout screenshots.
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { readFile, mkdir } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { join, resolve } from 'node:path';

const web = fileURLToPath(new URL('../web/', import.meta.url));
const fixture = `
  postMessage({ type: 'ready', diagnostics: { backend: 'fixture', memoryModel: 'fixture' } });
  onmessage = ({ data }) => {
    if (data.type === 'stage-files') postMessage({ type: 'staged', files: 0, bytes: 0, root: '/vita' });
    if (data.type === 'run-app') postMessage({ type: 'vita-present' });
    if (data.type === 'fixture-message') postMessage(data.message);
  };
`;
const server = createServer(async (req, res) => {
  const path = new URL(req.url, 'http://localhost').pathname;
  try {
    if (path === '/player-config.json') { res.setHeader('Content-Type', 'application/json'); return res.end(JSON.stringify({ title: 'PCSE00268', app: 'PCSE00268', aot: true })); }
    if (path === '/manifest.json') { res.setHeader('Content-Type', 'application/json'); return res.end('[]'); }
    if (path === '/worker.js') { res.setHeader('Content-Type', 'text/javascript'); return res.end(fixture); }
    const file = path === '/' ? 'player.html' : path.slice(1);
    if (!/^[\w.-]+$/.test(file)) throw new Error('bad path');
    res.setHeader('Content-Type', file.endsWith('.css') ? 'text/css' : file.endsWith('.html') ? 'text/html' : 'text/javascript');
    res.end(await readFile(join(web, file)));
  } catch { res.writeHead(404); res.end(); }
});
await new Promise((done) => server.listen(0, '127.0.0.1', done));
const url = `http://127.0.0.1:${server.address().port}`;
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE_URL || 'playwright');
let browser;
const errors = [];
try {
  browser = await chromium.launch({ headless: true,
    args: ['--no-sandbox', '--enable-unsafe-webgpu', '--use-angle=swiftshader'],
    ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}),
  });
  async function open(options) {
    const context = await browser.newContext(options);
    await context.addInitScript(() => {
      const NativeWorker = Worker;
      window.sent = []; window.testWorkers = [];
      window.Worker = class extends NativeWorker {
        constructor(...args) { super(...args); window.testWorkers.push(this); }
        postMessage(message, ...args) {
          if (message.type !== 'attach-canvas') window.sent.push(message);
          return super.postMessage(message, ...args);
        }
      };
    });
    const page = await context.newPage();
    page.on('pageerror', (error) => errors.push(String(error)));
    await page.goto(url);
    await page.waitForFunction(() => !document.querySelector('#run').disabled);
    return page;
  }
  const lastInput = (page) => page.evaluate(() => window.sent.filter((m) => m.type === 'input').at(-1));
  async function expectPad(page, buttons, axes) {
    try { await page.waitForFunction(({ buttons, axes }) => {
      const last = window.sent.filter((m) => m.type === 'input').at(-1);
      return last?.buttons === buttons && last.axes.every((n, i) => Math.abs(n - axes[i]) < .02);
    }, { buttons, axes }, { timeout: 5000 });
    } catch (error) {
      console.error('Expected', { buttons, axes }, 'received', await lastInput(page), 'page errors', errors);
      throw error;
    }
  }
  const desktop = await open({ viewport: { width: 1280, height: 900 } });
  assert.equal(await desktop.locator('#touch-controls').isVisible(), false);
  assert.equal(await desktop.locator('#diagnostics').getAttribute('open'), null);
  await desktop.locator('#run').click();
  await expectPad(desktop, 0, [0, 0, 0, 0]);
  await desktop.keyboard.down('ArrowRight'); await desktop.keyboard.down('KeyX');
  await expectPad(desktop, 0x4020, [1, 0, 0, 0]);
  await desktop.keyboard.up('ArrowRight');
  await expectPad(desktop, 0x4000, [0, 0, 0, 0]);
  await desktop.evaluate(() => dispatchEvent(new Event('blur')));
  await expectPad(desktop, 0, [0, 0, 0, 0]);
  await desktop.keyboard.up('KeyX');
  await desktop.locator('#stop').click();
  assert.equal(await desktop.locator('#run').isEnabled(), true);

  const phone = await open({ viewport: { width: 844, height: 390 }, hasTouch: true, isMobile: true });
  assert.equal(await phone.locator('#touch-controls').isVisible(), true);
  await phone.locator('#run').click();
  await expectPad(phone, 0, [0, 0, 0, 0]);
  const cdp = await phone.context().newCDPSession(phone);
  async function point(selector, id, dx = 0, dy = 0) {
    const b = await phone.locator(selector).boundingBox();
    assert(b, selector + ' is visible');
    return { id, x: b.x + b.width / 2 + b.width * dx, y: b.y + b.height / 2 + b.height * dy };
  }
  const touch = (type, touchPoints) => cdp.send('Input.dispatchTouchEvent', { type, touchPoints });
  const stick = await point('[data-stick="0"]', 1, .4);
  const cross = await point('[data-button="cross"]', 2);
  await touch('touchStart', [stick]);
  await expectPad(phone, 0, [1, 0, 0, 0]);
  await touch('touchStart', [stick, cross]);
  await expectPad(phone, 0x4000, [1, 0, 0, 0]);
  await touch('touchEnd', [cross]);
  await expectPad(phone, 0, [1, 0, 0, 0]);
  await touch('touchEnd', []);
  await expectPad(phone, 0, [0, 0, 0, 0]);

  await phone.locator('#display').focus();
  await phone.keyboard.down('KeyX');
  await touch('touchStart', [cross]);
  await touch('touchEnd', []);
  await expectPad(phone, 0x4000, [0, 0, 0, 0]);
  await phone.keyboard.up('KeyX');
  await expectPad(phone, 0, [0, 0, 0, 0]);

  const right = await point('[data-direction="right"]', 3);
  await touch('touchStart', [right]);
  await expectPad(phone, 0x20, [1, 0, 0, 0]);
  await touch('touchMove', [await point('[data-dpad]', 3, .4, -.4)]);
  await expectPad(phone, 0x30, [1, -1, 0, 0]);
  await touch('touchCancel', []);
  await expectPad(phone, 0, [0, 0, 0, 0]);

  // Right stick, shoulders, face buttons, and Start/Select use Vita masks.
  const rightStick = await point('[data-stick="2"]', 5, 0, -.4);
  await touch('touchStart', [rightStick]);
  await expectPad(phone, 0, [0, 0, 0, -1]);
  await touch('touchEnd', []);
  for (const [name, mask] of Object.entries({ start: 8, select: 1, l: 256, r: 512, triangle: 4096, circle: 8192, square: 32768 })) {
    await touch('touchStart', [await point(`[data-button="${name}"]`, 6)]);
    await expectPad(phone, mask, [0, 0, 0, 0]);
    await touch('touchEnd', []);
    await expectPad(phone, 0, [0, 0, 0, 0]);
  }
  // Cancelling a captured button outside the control cannot leave it held.
  await touch('touchStart', [cross]);
  await touch('touchMove', [{ ...cross, x: 420, y: 150 }]);
  await touch('touchEnd', []);
  await expectPad(phone, 0, [0, 0, 0, 0]);
  await touch('touchStart', [cross]);
  await phone.locator('#touch-toggle').evaluate((e) => e.click());
  await expectPad(phone, 0, [0, 0, 0, 0]);
  await touch('touchEnd', []);
  await phone.locator('#touch-toggle').click();

  // Guest dialogs own input, remain tappable, and release previous touches.
  await touch('touchStart', [cross]);
  await phone.evaluate(() => window.testWorkers.at(-1).postMessage({ type: 'fixture-message', message: {
    type: 'vita-dialog', dialog: { id: 7, state: 'open', message: 'Fixture dialog', buttons: ['OK', 'Cancel'], progress: null, enterButton: 'cross' },
  } }));
  await phone.locator('#dialog').waitFor({ state: 'visible' });
  await expectPad(phone, 0, [0, 0, 0, 0]);
  assert.equal(await phone.locator('#touch-controls').isVisible(), false);
  await touch('touchEnd', []);
  await phone.locator('#dialog-buttons button').first().tap();
  assert.deepEqual(await phone.evaluate(() => window.sent.filter((m) => m.type === 'dialog-press').at(-1)),
    { type: 'dialog-press', id: 7, button: 0x4000, selected: 0 });
  await phone.evaluate(() => window.testWorkers.at(-1).postMessage({ type: 'fixture-message', message: { type: 'vita-dialog', dialog: { id: 7, state: 'close' } } }));
  await phone.locator('#touch-controls').waitFor({ state: 'visible' });

  await phone.evaluate(() => window.testWorkers.at(-1).postMessage({ type: 'fixture-message', message: {
    type: 'vita-ime', ime: { id: 8, state: 'open', text: '', maxLength: 20, caret: 0, enterLabel: 'Done' },
  } }));
  await phone.locator('#ime-text').fill('Phone input');
  await phone.locator('#ime-enter').tap();
  assert.equal(await phone.evaluate(() => window.sent.filter((m) => m.type === 'ime-input' && m.input.kind === 1).at(-1)?.input.text), 'Phone input');
  await phone.evaluate(() => window.testWorkers.at(-1).postMessage({ type: 'fixture-message', message: { type: 'vita-ime', ime: { id: 8, state: 'close' } } }));
  await phone.locator('#ime').waitFor({ state: 'hidden' });
  await phone.locator('#mute').click();
  assert.equal(await phone.locator('#mute').getAttribute('aria-pressed'), 'true');

  // The CSS fallback is used on phones without element fullscreen support.
  await phone.evaluate(() => Object.defineProperty(document, 'fullscreenEnabled', { value: false, configurable: true }));
  await phone.locator('#fullscreen').click();
  assert.equal(await phone.locator('#player-shell').evaluate((e) => e.classList.contains('expanded')), true);
  await phone.locator('#fullscreen').click();
  assert.equal(await phone.locator('#player-shell').evaluate((e) => e.classList.contains('expanded')), false);

  for (const size of [80, 100, 120]) for (const [width, height] of [[844, 390], [667, 375], [568, 320], [390, 844], [320, 568]]) {
    await phone.setViewportSize({ width, height });
    await phone.locator('#touch-size').evaluate((e, size) => { e.value = size; e.dispatchEvent(new Event('input')); }, size);
    const layout = await phone.evaluate(() => {
      const display = document.querySelector('#display').getBoundingClientRect();
      const controls = [...document.querySelectorAll('#touch-controls .pad-button, #touch-controls .stick')].map((e) => ({ name: e.getAttribute('aria-label'), r: e.getBoundingClientRect().toJSON() }));
      return { width: innerWidth, scrollWidth: document.documentElement.scrollWidth, display: display.toJSON(), controls };
    });
    assert(layout.scrollWidth <= layout.width + 1, `${width}: no horizontal scrolling`);
    for (const { name, r } of layout.controls) {
      assert(r.x >= layout.display.x - 1 && r.right <= layout.display.right + 1 && r.y >= layout.display.y - 1 && r.bottom <= layout.display.bottom + 1, `${width}: ${name} stays in the player`);
    }
    for (let i = 0; i < layout.controls.length; i++) for (let j = i + 1; j < layout.controls.length; j++) {
      const a = layout.controls[i], b = layout.controls[j];
      const overlapX = Math.min(a.r.right, b.r.right) - Math.max(a.r.x, b.r.x);
      const overlapY = Math.min(a.r.bottom, b.r.bottom) - Math.max(a.r.y, b.r.y);
      assert(overlapX <= 1 || overlapY <= 1, `${width}: ${a.name} must not overlap ${b.name}`);
    }
  }
  await phone.locator('#stop').click();
  assert.deepEqual((await lastInput(phone)).axes, [0, 0, 0, 0]);
  await phone.locator('#run').click();
  await expectPad(phone, 0, [0, 0, 0, 0]);
  await phone.locator('#touch-size').evaluate((e) => { e.value = 100; e.dispatchEvent(new Event('input')); });
  if (process.env.PLAYER_SCREENSHOTS) {
    const out = resolve(process.env.PLAYER_SCREENSHOTS); await mkdir(out, { recursive: true });
    await desktop.screenshot({ path: join(out, 'desktop.png'), fullPage: true });
    for (const [name, width, height] of [['landscape', 844, 390], ['portrait', 390, 844]]) {
      await phone.setViewportSize({ width, height });
      await phone.screenshot({ path: join(out, `${name}.png`), fullPage: true });
    }
  }
  assert.deepEqual(errors, []);
  console.log('PASS: keyboard, multitouch sticks/buttons, D-pad diagonals, cancellation, dialogs/IME, restart, fullscreen fallback, and five phone layouts at three control sizes; zero page errors.');
} finally {
  await browser?.close();
  await new Promise((done) => server.close(done));
}
