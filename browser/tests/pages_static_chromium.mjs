// The static player site (browser/pages/assemble.sh) as a visitor meets it:
// served under a project path by a plain file server that sends no
// cross-origin isolation headers (as GitHub Pages does), firmware and a game
// uploaded through the page, then a threaded boot until frames present.
//
//   PAGES_DIR=<assembled site> PAGES_FIRMWARE_ZIP=<os0+vs0 .zip> PAGES_GAME_ZIP=<game .zip> \
//     node browser/tests/pages_static_chromium.mjs
//
// PAGES_PARAMS (default threads=1) adds query parameters; PAGES_FRAMES (60)
// is how many frames count as a boot; PAGES_DEADLINE_MS (240000) bounds it.
// PLAYWRIGHT_MODULE_URL / PLAYWRIGHT_CHROMIUM_EXECUTABLE select local
// installs; PAGES_GPU=1 uses the hardware adapter. An encrypted (NoNpDrm) game
// package is decrypted on upload: the stored title must hold no sce_pfs/.
// PAGES_LOG=<file> keeps the page and worker console. PAGES_SAVES_ZIP=<zip>
// restores saves before the boot. PAGES_FIRMWARE_ZIP may list several files
// (comma-separated: .PUPs or zips, uploaded in order); a .pkg game reads its
// zRIF from PAGES_ZRIF (the string, or a file holding it). PAGES_PROFILE=<dir> uses an on-disk browser
// profile: storage of a default context lives in memory (large games).
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { createReadStream, appendFileSync, writeFileSync, existsSync, readFileSync } from 'node:fs';
import { stat } from 'node:fs/promises';
import { extname, join, resolve } from 'node:path';

const site = resolve(process.env.PAGES_DIR || 'build/pages');
const firmwareZip = process.env.PAGES_FIRMWARE_ZIP, gameZip = process.env.PAGES_GAME_ZIP;
if (!firmwareZip || !gameZip) throw new Error('set PAGES_FIRMWARE_ZIP and PAGES_GAME_ZIP');
const prefix = '/vita3k-web/';
const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css',
  '.json': 'application/json', '.wasm': 'application/wasm' };
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
const url = `http://127.0.0.1:${server.address().port}${prefix}?${process.env.PAGES_PARAMS ?? 'threads=1'}`;
const wantFrames = Number(process.env.PAGES_FRAMES || 60);
const deadline = Date.now() + Number(process.env.PAGES_DEADLINE_MS || 240000);
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE_URL || 'playwright');
const launchOptions = { headless: true,
  args: process.env.PAGES_GPU === '1' ? ['--enable-unsafe-webgpu', '--enable-gpu', '--enable-features=Vulkan']
    : ['--enable-unsafe-webgpu', '--use-angle=swiftshader', '--enable-features=Vulkan', '--disable-vulkan-surface'],
  ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}) };
const browser = process.env.PAGES_PROFILE
  ? await chromium.launchPersistentContext(process.env.PAGES_PROFILE, launchOptions)
  : await chromium.launch(launchOptions);
const errors = [];
const logTail = async (page) => page.locator('#log').textContent().then((text) => text.split('\n').slice(-25).join('\n'), () => '');
let page;
try {
  page = await (process.env.PAGES_PROFILE ? browser : await browser.newContext()).newPage();
  page.on('pageerror', (error) => errors.push(String(error)));
  if (process.env.PAGES_LOG) {
    writeFileSync(process.env.PAGES_LOG, '');
    page.on('console', (message) => appendFileSync(process.env.PAGES_LOG, message.text() + '\n'));
    // Worker log lines reach the page as messages, not console output.
    await page.context().addInitScript(() => {
      const NativeWorker = Worker;
      window.Worker = class extends NativeWorker {
        constructor(...args) {
          super(...args);
          this.addEventListener('message', ({ data }) => { if (data?.type === 'log') console.log(String(data.message ?? data.text ?? '')); });
        }
      };
    });
  }
  await page.goto(url);
  // coi.js installs the header service worker and reloads once.
  await page.waitForFunction(() => self.crossOriginIsolated, null, { timeout: 30000 });
  await page.waitForFunction(() => document.querySelector('#firmware-state')?.textContent === 'needed');
  assert.equal(await page.locator('#game-title').textContent(), 'No game yet');

  await page.locator('#run').click();
  await page.waitForFunction(() => /Upload the firmware/.test(document.querySelector('#warning')?.textContent ?? '')
    || /Upload the firmware/.test(document.body.innerText));

  for (const firmware of firmwareZip.split(',')) {
    await page.locator('#firmware-file').setInputFiles(firmware);
    await page.waitForFunction(() => !document.querySelector('#upload-firmware').disabled, null, { timeout: 300000 });
    assert.match(await page.locator('#firmware-state').textContent(), /^stored/, `${firmware}: ${await page.evaluate(() => document.body.innerText.match(/Upload failed[^\n]*/)?.[0])}`);
  }
  console.log('firmware:', await page.locator('#firmware-state').textContent());

  // The first game reloads the page into ?title=<id>.
  const uploadStarted = Date.now();
  if (process.env.PAGES_ZRIF) {
    const zrif = existsSync(process.env.PAGES_ZRIF) ? readFileSync(process.env.PAGES_ZRIF, 'utf8').trim() : process.env.PAGES_ZRIF;
    page.on('dialog', (dialog) => dialog.type() === 'prompt' ? dialog.accept(zrif) : dialog.accept());
  }
  await Promise.all([page.waitForURL(/[?&]title=/, { timeout: 300000 }), page.locator('#upload-file').setInputFiles(gameZip)]);
  await page.waitForFunction(() => self.crossOriginIsolated);
  const title = new URL(page.url()).searchParams.get('title');
  console.log('game:', title, `stored in ${((Date.now() - uploadStarted) / 1000).toFixed(0)}s`);
  assert.equal(await page.locator('#game-title').textContent() !== 'No game yet', true);
  const stored = await page.evaluate(async (title) => {
    let dir = await navigator.storage.getDirectory();
    for (const part of ['vita3k-meta', title, title]) dir = await dir.getDirectoryHandle(part);
    return JSON.parse(await (await (await dir.getFileHandle('manifest.json')).getFile()).text()).files.map((file) => file.path);
  }, title);
  assert.ok(stored.some((path) => path.endsWith('/eboot.bin')), 'the game is stored');
  assert.ok(!stored.some((path) => path.includes('/sce_pfs/')), 'an encrypted game is stored decrypted');
  console.log('stored:', stored.length, 'files');
  if (process.env.PAGES_SAVES_ZIP) {
    await page.locator('#saves-file').setInputFiles(process.env.PAGES_SAVES_ZIP);
    await page.waitForFunction(() => /Restored \d+ save file/.test(document.body.innerText));
    console.log('saves:', await page.evaluate(() => /Restored \d+ save file\(s\)/.exec(document.body.innerText)?.[0]));
  }

  await page.locator('#run').click();
  let frames = 0;
  while (Date.now() < deadline) {
    const stats = await page.locator('#stats').textContent();
    frames = Number(/frames=(\d+)/.exec(stats ?? '')?.[1] ?? 0);
    if (frames >= wantFrames) break;
    if (/failed|error/i.test(await page.locator('#status').textContent())) break;
    await page.waitForTimeout(1000);
  }
  console.log('status:', await page.locator('#status').textContent(), '| stats:', await page.locator('#stats').textContent());
  assert.ok(frames >= wantFrames, `only ${frames} frames presented\n${await logTail(page)}`);
  assert.deepEqual(errors, []);
  console.log(`PASS: static site under ${prefix}, isolated by its service worker, firmware and ${title} uploaded, ${frames} frames.`);
} catch (error) {
  if (page) console.error(await logTail(page), '\npage errors:', errors);
  throw error;
} finally {
  await browser.close();
  server.close();
}
