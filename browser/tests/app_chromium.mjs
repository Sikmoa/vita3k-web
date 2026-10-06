// The app (browser/app) on the static site, as a visitor uses it: served
// under a project path by a plain file server (as GitHub Pages serves it),
// firmware and a game imported through the Import dialog, the game played
// until frames present, then back to the library.
//
//   APP_SITE=<assembled site with the app> APP_FIRMWARE=<.PUP or .zip>[,…] APP_GAME=<game file> \
//     node browser/tests/app_chromium.mjs
//
// APP_ZRIF answers a .pkg game's license (the string or a file); APP_FRAMES
// (60) frames count as a boot; APP_SCREENSHOTS=<dir> keeps desktop and phone
// screenshots of each page. PLAYWRIGHT_MODULE_URL /
// PLAYWRIGHT_CHROMIUM_EXECUTABLE select local installs; APP_GPU=1 uses the
// hardware adapter. APP_SWIPE=x1,y1,x2,y2[;…] (0-1 across the game picture)
// drags the mouse over the game once it plays, as front-touchscreen swipes
// APP_SWIPE_WAIT ms (4000) apart, keeps an after-swipe screenshot and checks
// the game still presents frames afterwards.
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { createReadStream, existsSync, readFileSync, mkdirSync } from 'node:fs';
import { stat } from 'node:fs/promises';
import { extname, join, resolve } from 'node:path';

const site = resolve(process.env.APP_SITE || 'build/pages');
const firmware = (process.env.APP_FIRMWARE || '').split(',').filter(Boolean);
const gameFile = process.env.APP_GAME;
if (!firmware.length || !gameFile) throw new Error('set APP_FIRMWARE and APP_GAME');
const shots = process.env.APP_SCREENSHOTS;
if (shots) mkdirSync(shots, { recursive: true });
const prefix = '/vita3k-web/';
const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css',
  '.json': 'application/json', '.wasm': 'application/wasm', '.png': 'image/png', '.svg': 'image/svg+xml' };
const server = createServer(async (req, res) => {
  const path = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
  if (!path.startsWith(prefix) || path.includes('..')) { res.writeHead(404); return res.end(); }
  let file = join(site, path.slice(prefix.length));
  try {
    if ((await stat(file)).isDirectory()) file = join(file, 'index.html');
    const { size } = await stat(file);
    res.writeHead(200, { 'Content-Type': types[extname(file)] || 'application/octet-stream', 'Content-Length': size });
    createReadStream(file).pipe(res);
  } catch { res.writeHead(404); res.end(); }
});
await new Promise((done) => server.listen(0, '127.0.0.1', done));
const root = `http://127.0.0.1:${server.address().port}${prefix}`;
const wantFrames = Number(process.env.APP_FRAMES || 60);
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE_URL || 'playwright');
const browser = await chromium.launch({ headless: true,
  args: process.env.APP_GPU === '1' ? ['--enable-unsafe-webgpu', '--enable-gpu', '--enable-features=Vulkan']
    : ['--enable-unsafe-webgpu', '--use-angle=swiftshader', '--enable-features=Vulkan', '--disable-vulkan-surface'],
  ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}) });
const errors = [];
const shot = async (page, name) => { if (shots) await page.screenshot({ path: join(shots, `${name}.png`) }); };
try {
  const context = await browser.newContext({ viewport: { width: 1280, height: 800 } });
  const page = await context.newPage();
  page.on('pageerror', (error) => errors.push(String(error)));
  await page.goto(root);
  await page.waitForFunction(() => self.crossOriginIsolated, null, { timeout: 30000 });
  await page.getByText('Your library is empty').waitFor();
  await page.getByText('Firmware needed').waitFor();
  await shot(page, 'library-empty');

  const importThrough = async (file, kind, zrif) => {
    await page.getByRole('button', { name: 'Import a game or firmware' }).click();
    const dialog = page.getByRole('dialog', { name: 'Import' });
    await dialog.waitFor();
    await dialog.getByRole('radio', { name: kind === 'game' ? 'Game' : 'Firmware' }).click();
    await dialog.locator('input[type=file]').setInputFiles(file);
    if (zrif) await dialog.getByPlaceholder(/KO5i/).fill(zrif);
    if (kind === 'firmware') await shot(page, 'import-firmware');
    await dialog.getByRole('button', { name: 'Import', exact: true }).click();
    await dialog.getByText(/is ready|installed/).waitFor({ timeout: 600000 });
    if (kind === 'game') await shot(page, 'import-done');
    return dialog;
  };
  for (const file of firmware) {
    const dialog = await importThrough(file, 'firmware');
    await dialog.getByRole('button', { name: 'Done', exact: true }).click();
  }
  const zrifEnv = process.env.APP_ZRIF;
  const zrif = zrifEnv ? (existsSync(zrifEnv) ? readFileSync(zrifEnv, 'utf8').trim() : zrifEnv) : '';
  const dialog = await importThrough(gameFile, 'game', /\.pkg$/i.test(gameFile) ? zrif : '');
  await dialog.getByRole('button', { name: 'Close', exact: true }).click();
  await dialog.waitFor({ state: 'hidden' });
  await page.locator('article').first().waitFor();
  await shot(page, 'library');
  const name = await page.locator('article h3').first().textContent();
  console.log('library:', await page.locator('article').count(), 'game(s), first:', name);

  // Settings and About render.
  await page.getByRole('link', { name: 'Settings' }).click();
  await page.getByRole('heading', { name: 'Settings' }).waitFor();
  await shot(page, 'settings');
  // Screenshots show the touch controls too.
  if (shots) await page.getByRole('radio', { name: 'Always' }).click();
  // The game's log goes to a file too.
  await page.getByRole('switch', { name: 'Save logs to files' }).click();
  await page.getByRole('link', { name: 'About' }).click();
  await page.getByText('This device').waitFor();
  await page.waitForFunction(() => !document.body.innerText.includes('Checking…'));
  await shot(page, 'about');

  // Files: the storage root, then a folder made, filled, renamed and removed.
  await page.getByRole('link', { name: 'Files' }).click();
  await page.getByText('vita3k-content').waitFor();
  await shot(page, 'files');
  const folderDialog = page.getByRole('dialog', { name: 'New folder' });
  await page.getByRole('button', { name: 'New folder' }).click();
  await folderDialog.getByLabel('Name').fill('test folder');
  await folderDialog.getByRole('button', { name: 'Create' }).click();
  await page.getByRole('link', { name: /^test folder/ }).click();
  await page.getByText('This folder is empty').waitFor();
  await page.locator('input[type=file][multiple]').setInputFiles({ name: 'notes.txt', mimeType: 'text/plain', buffer: Buffer.from('hello from the files page') });
  await page.getByText('notes.txt').waitFor();
  await page.getByRole('button', { name: 'Actions for notes.txt' }).click();
  await page.getByRole('menuitem', { name: 'Rename…' }).click();
  const renameDialog = page.getByRole('dialog', { name: 'Rename notes.txt' });
  await renameDialog.getByLabel('Name').fill('renamed.txt');
  await renameDialog.getByRole('button', { name: 'Rename', exact: true }).click();
  await page.getByText('renamed.txt').waitFor();
  const stored = await page.evaluate(async () => {
    let dir = await navigator.storage.getDirectory();
    dir = await dir.getDirectoryHandle('test folder');
    return (await (await dir.getFileHandle('renamed.txt')).getFile()).text();
  });
  assert.equal(stored, 'hello from the files page');
  await shot(page, 'files-folder');
  await page.getByRole('link', { name: 'Storage' }).click();
  await page.getByRole('button', { name: 'Actions for test folder' }).click();
  await page.getByRole('menuitem', { name: 'Remove…' }).click();
  await page.getByRole('dialog', { name: 'Remove test folder?' }).getByRole('button', { name: 'Remove', exact: true }).click();
  await page.getByRole('link', { name: /^test folder/ }).waitFor({ state: 'detached' });
  console.log('files: folder created, file uploaded, renamed and removed');
  await page.getByRole('link', { name: 'Library' }).click();

  // Play.
  await page.locator('article a').first().click();
  await page.waitForFunction(() => location.hash.startsWith('#/play/'));
  await page.waitForTimeout(3000);
  await shot(page, 'starting');
  const deadline = Date.now() + Number(process.env.APP_DEADLINE_MS || 300000);
  let frames = 0;
  while (Date.now() < deadline) {
    frames = Number(await page.locator('.stage').getAttribute('data-frames') ?? 0);
    if (frames >= wantFrames || (await page.locator('.stage').getAttribute('data-phase')) === 'error') break;
    await page.waitForTimeout(1000);
  }
  await shot(page, 'playing');
  if (shots) {
    await page.setViewportSize({ width: 844, height: 390 });
    await page.waitForTimeout(500);
    await shot(page, 'phone-landscape-playing');
    await page.setViewportSize({ width: 1280, height: 800 });
    await page.getByRole('button', { name: 'Fullscreen' }).click();
    await page.waitForTimeout(800);
    await shot(page, 'fullscreen');
    await page.getByRole('button', { name: 'Exit fullscreen' }).click();
    await page.waitForTimeout(500);
  }
  if (process.env.APP_SWIPE) {
    const box = await page.locator('canvas.game-canvas').boundingBox();
    const scale = Math.min(box.width / 960, box.height / 544);
    const left = box.x + (box.width - 960 * scale) / 2, top = box.y + (box.height - 544 * scale) / 2;
    const at = (x, y) => [left + x * 960 * scale, top + y * 544 * scale];
    for (const swipe of process.env.APP_SWIPE.split(';')) {
      const [x1, y1, x2, y2] = swipe.split(',').map(Number);
      await page.mouse.move(...at(x1, y1));
      await page.mouse.down();
      for (let step = 1; step <= 12; ++step) {
        await page.mouse.move(...at(x1 + (x2 - x1) * step / 12, y1 + (y2 - y1) * step / 12));
        await page.waitForTimeout(16);
      }
      await page.mouse.up();
      await page.waitForTimeout(Number(process.env.APP_SWIPE_WAIT || 4000));
    }
    await shot(page, 'after-swipe');
    const before = Number(await page.locator('.stage').getAttribute('data-frames'));
    await page.waitForTimeout(3000);
    const after = Number(await page.locator('.stage').getAttribute('data-frames'));
    console.log('swiped', process.env.APP_SWIPE, '; frames in 3 s afterwards:', after - before);
    assert.ok(after > before, 'the game still presents frames after the swipes');
  }
  const phase = await page.locator('.stage').getAttribute('data-phase');
  // A run that did not reach its frames (or APP_LOG_TAIL=<lines>): the end of its saved log.
  if (phase !== 'running' || frames < wantFrames || process.env.APP_LOG_TAIL) console.log(await page.evaluate(async (lines) => {
    const dir = await (await navigator.storage.getDirectory()).getDirectoryHandle('vita3k-logs');
    let last = null;
    for await (const handle of dir.values()) if (!last || handle.name > last.name) last = handle;
    return last ? (await last.getFile()).text().then((text) => text.split('\n').slice(-lines).join('\n')) : 'no log';
  }, Number(process.env.APP_LOG_TAIL) || 60).catch((error) => 'no log: ' + error));
  assert.equal(phase, 'running', 'the player shows the running game');
  assert.ok(frames >= wantFrames, `only ${frames} frames (phase ${phase}): ${await page.locator('.stage').innerText()}`);
  console.log('played:', frames, 'frames, phase', phase);

  // Phone layout of the library and the player with touch controls.
  if (shots) {
    const phone = await browser.newContext({ viewport: { width: 390, height: 844 }, hasTouch: true, isMobile: true });
    const mobile = await phone.newPage();
    await mobile.goto(root);
    await mobile.getByText('Your library is empty').or(mobile.locator('article').first()).waitFor();
    await shot(mobile, 'phone-library');
    await mobile.getByRole('button', { name: 'Open navigation' }).click();
    await shot(mobile, 'phone-menu');
    await phone.close();
  }

  // The logs dialog shows the run's log and where it is saved.
  await page.getByRole('button', { name: 'Logs' }).click();
  const logs = page.getByRole('dialog', { name: 'Logs' });
  await logs.getByText(/saved to vita3k-logs\//).waitFor();
  assert.ok((await logs.locator('pre').innerText()).split('\n').length > 5, 'the log has lines');
  await shot(page, 'logs');
  await logs.getByRole('button', { name: 'Close dialog' }).click();

  // Leaving a running game asks first: Cancel keeps it running.
  await page.getByRole('link', { name: 'Back to the library' }).click();
  const leave = page.getByRole('dialog', { name: 'Leave the game?' });
  await shot(page, 'leave-confirm');
  await leave.getByRole('button', { name: 'Cancel' }).click();
  assert.equal(await page.locator('.stage').getAttribute('data-phase'), 'running');
  await page.getByRole('link', { name: 'Back to the library' }).click();
  await leave.getByRole('button', { name: 'Leave' }).click();
  await page.getByRole('heading', { name: 'Library' }).waitFor();

  // The saved log, on the Files page.
  await page.getByRole('link', { name: 'Files' }).click();
  await page.getByText('vita3k-logs').click();
  await page.getByText(/\.log$/).first().waitFor();
  await page.goto(root + '#/library');
  await page.getByRole('heading', { name: 'Library' }).waitFor();
  assert.deepEqual(errors, []);
  console.log(`PASS: the app under ${prefix}: firmware and ${name} imported through the dialog, ${frames} frames played.`);
} finally {
  await browser.close();
  server.close();
}
