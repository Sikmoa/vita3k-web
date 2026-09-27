// End-to-end test of the "package defines the title" flow: a .zip whose
// ux0/app/<id> directory names a title the server does NOT have is uploaded,
// then booted with every game download blocked. Staging must complete from
// persistent storage, with only firmware coming over the network — no
// server-side staging, no restart.
// Run: node browser/tests/content_cache_chromium.mjs
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { createServer } from 'node:http';
import { mkdirSync, writeFileSync } from 'node:fs';
import { randomBytes } from 'node:crypto';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

// The server stages one title (and the firmware file every boot needs).
const stage = join(tmpdir(), 'vita3k-cache-stage');
const stageFiles = {
  'ux0/app/TEST00001/eboot.bin': randomBytes(1024),
  'os0/kd/b.bin': randomBytes(131072),
};
for (const [path, bytes] of Object.entries(stageFiles)) {
  const full = join(stage, path);
  mkdirSync(join(full, '..'), { recursive: true });
  writeFileSync(full, bytes);
}
// The package holds a different title: PACK00001, laid out without a device
// root (app/…), exactly like a dump.
const packageBytes = randomBytes(2048);
const zipPath = join(tmpdir(), 'vita3k-cache-pkg.zip');
const zip = spawnSync('python3', ['-c', `
import zipfile
with zipfile.ZipFile(${JSON.stringify(zipPath)}, 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('app/PACK00001/eboot.bin', ${JSON.stringify(packageBytes.toString('latin1'))}.encode('latin1'))
    z.writestr('app/PACK00001/data/a.bin', b'x' * 4096)
`], { encoding: 'utf8' });
if (zip.status !== 0) throw new Error('zip fixture failed: ' + zip.stderr);

const probe = createServer();
await new Promise((done) => probe.listen(0, '127.0.0.1', done));
const port = probe.address().port;
await new Promise((done) => probe.close(done));
const server = spawn('node', ['browser/tests/limbo_serve.mjs'], {
  cwd: '/home/user/__vita-3k-port',
  env: { ...process.env, PORT: String(port), HOST: '127.0.0.1', LIMBO_STAGE: stage,
    LIMBO_TITLE: 'TEST00001', GXM_RUNTIME_DIST: 'deploy' },
  stdio: ['ignore', 'pipe', 'inherit'],
});
const base = `http://127.0.0.1:${port}/`;
await new Promise((resolve, reject) => {
  const timer = setTimeout(() => reject(new Error('server did not start')), 15000);
  server.stdout.on('data', (chunk) => {
    if (String(chunk).includes('Limbo dev server')) { clearTimeout(timer); resolve(); }
  });
  server.on('error', reject);
  server.on('exit', (code) => reject(new Error('server exited: ' + code)));
});
console.log('server:', base);

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE_URL || 'playwright');
const errors = [];
const browser = await chromium.launch({ headless: true, args: ['--no-sandbox', '--use-angle=swiftshader'] });
let ux0Hits = 0, os0Hits = 0, manifestHits = 0;
try {
  const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
  page.on('pageerror', (error) => errors.push(String(error)));
  // Game content is unreachable; firmware stays available (it is machine
  // infrastructure, not part of any package).
  await page.route('**/stage/**', (route) => {
    const path = new URL(route.request().url()).pathname.replace(/^\/stage/, '');
    if (path.startsWith('/ux0/')) { ux0Hits++; return route.abort(); }
    if (path.startsWith('/os0/')) os0Hits++;
    return route.continue();
  });
  await page.route('**/manifest.json*', (route) => { manifestHits++; return route.continue(); });

  await page.goto(base);
  await page.waitForFunction(() => !document.querySelector('#run').disabled);
  assert.equal(await page.locator('#title-row').isVisible(), false, 'one title: no picker yet');

  // The package names its own title and is stored under it.
  await page.locator('#upload-file').setInputFiles(zipPath);
  await page.waitForFunction(
    () => (document.querySelector('#player-notice').textContent || '').includes('Package ready'),
    null, { timeout: 60000 });
  const stored = await page.locator('#player-notice').textContent();
  console.log('upload:', stored);
  assert.match(stored, /PACK00001/);

  // The picker now offers both titles (server-staged and uploaded).
  await page.waitForFunction(() => !document.querySelector('#title-row').hidden);
  const options = await page.locator('#title-picker option').allTextContents();
  console.log('picker:', options);
  assert.ok(options.some((text) => text.includes('PACK00001') && text.includes('package')));
  assert.ok(options.some((text) => text.includes('TEST00001') && text.includes('server')));

  // Boot the uploaded title: its file can only come from persistent storage.
  const config = await (await page.request.get(`${base}player-config.json?title=PACK00001`)).json();
  assert.equal(config.title, 'PACK00001');
  assert.equal(config.staged, false);
  assert.equal(config.aot, false);
  await page.goto(`${base}?title=PACK00001`);
  await page.waitForFunction(() => !document.querySelector('#run').disabled);
  ux0Hits = 0; os0Hits = 0;
  await page.locator('#run').click();
  let terminal = '';
  const deadline = Date.now() + 150000;
  for (;;) {
    terminal = await page.evaluate(() => document.querySelector('#status').textContent);
    if (/^(exit|Runtime error|Launch failed|Worker error)/.test(terminal)) break;
    if (Date.now() > deadline) throw new Error('timed out (status=' + terminal + ')');
    await new Promise((done) => setTimeout(done, 100));
  }
  console.log('terminal status:', terminal,
    '| storage:', await page.evaluate(() => document.body.dataset.cacheVerdict));
  if (!/^exit /.test(terminal)) {
    await new Promise((done) => setTimeout(done, 1500));
    console.log('notice:', await page.evaluate(() => document.querySelector('#player-notice').textContent));
    console.log('logtail:', await page.evaluate(() => document.querySelector('#log').textContent.slice(-800)));
  }
  assert.match(terminal, /^exit /, 'the uploaded title did not reach run-app');
  assert.equal(ux0Hits, 0, `${ux0Hits} game files were fetched instead of read from storage`);
  assert.ok(os0Hits >= 1, 'firmware should still come from the server');
  assert.match(await page.evaluate(() => document.body.dataset.cacheVerdict), /package-only/);
  assert.ok(manifestHits >= 1, 'the manifest was never read');

  // Persistent storage and the module graph survive a reload.
  await page.reload();
  await page.waitForFunction(() => !document.querySelector('#run').disabled);
  const bindings = await page.evaluate(async () => {
    const m = await import('./content_cache.js');
    const titles = await m.listCachedTitles();
    return { cacheReadManifest: typeof m.cacheReadManifest, titles: titles.map((entry) => entry.title) };
  });
  assert.equal(bindings.cacheReadManifest, 'function');
  assert.deepEqual(bindings.titles, ['PACK00001']);
  console.log('after reload, stored titles:', bindings.titles);
  assert.deepEqual(errors, []);
  console.log('page errors: none');
} finally {
  await browser.close();
  server.kill();
}
console.log('PASS: an uploaded package names and boots its own title with zero game downloads');
