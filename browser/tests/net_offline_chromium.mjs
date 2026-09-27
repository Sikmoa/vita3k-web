// Offline network in real Chromium: the genuine VitaSDK fixture
// (vita_net_offline_fixture, target vita3k_vita_net_offline_fixture) is staged
// as an app and runs through the headless probe on the retail-app path. It
// checks what a Vita without a connection answers (net/offline_socket.h) and
// exits with 100, or with the number of the first failed check.
//
// Usage (from the repository root, after building vita3k_web_jit and the fixture):
//   PLAYWRIGHT_MODULE_URL=file://$PWD/build/playwright/node_modules/playwright/index.mjs \
//     node browser/tests/net_offline_chromium.mjs
// NET_OFFLINE_FIXTURE overrides the fixture eboot (default: the web64 build's);
// PLAYWRIGHT_CHROMIUM_EXECUTABLE and GXM_RUNTIME_DIST pass through.
import { spawn } from 'node:child_process';
import { copyFile, mkdir, mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import assert from 'node:assert/strict';

const fixture = resolve(process.env.NET_OFFLINE_FIXTURE
  || 'build/web64/browser/tests/vita_net_offline_fixture/eboot.bin');
const title = 'NETOFF001';
const stage = await mkdtemp(join(tmpdir(), 'net-offline-'));
try {
  await mkdir(join(stage, 'ux0/app', title), { recursive: true });
  await copyFile(fixture, join(stage, 'ux0/app', title, 'eboot.bin'));
  const probe = spawn(process.execPath, ['browser/tests/limbo_app_chromium.mjs'], {
    env: { ...process.env, LIMBO_STAGE: stage, LIMBO_TITLE: title, LIMBO_DEADLINE_MS: '60000',
      LIMBO_MAX_FRAMES: '0', LIMBO_FRAME_OUT: join(stage, 'frame') },
    stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '';
  probe.stdout.on('data', (chunk) => { output += chunk; });
  probe.stderr.on('data', (chunk) => { output += chunk; });
  await new Promise((done) => probe.on('exit', done));
  // The fixture presents no frames, so the probe's own frame assertion fails
  // after the report; the report is what this test checks.
  const lines = output.split('\n');
  const first = lines.indexOf('{');
  const report = JSON.parse(lines.slice(first, lines.indexOf('}', first) + 1).join('\n'));
  assert.deepEqual(report.workerErrors, [], output.slice(-4000));
  assert.deepEqual(report.pageErrors, []);
  assert.deepEqual(report.exit, { exitCode: 100, ok: true },
    `first failed check = exit code\n${report.logTail.join('\n')}`);
  console.log('offline network fixture: every check passed (exit 100)');
} finally {
  await rm(stage, { recursive: true, force: true });
}
