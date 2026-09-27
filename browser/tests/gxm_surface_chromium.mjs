// End-to-end GXM color surfaces in real Chromium: the genuine VitaSDK fixture
// (vita_gxm_surface_fixture, target vita3k_vita_gxm_surface_fixture) runs as
// an app on the retail-app path, once with surface sync (it also checks the
// surfaces' tiled and Morton bytes in guest memory: exit 100) and once
// without (every check reads the GPU through transfers: exit 101). Any other
// exit code names the failed check in its main.c.
//
// Usage (from the repository root, after building vita3k_web_jit and the fixture):
//   PLAYWRIGHT_MODULE_URL=file://$PWD/build/playwright/node_modules/playwright/index.mjs \
//     node browser/tests/gxm_surface_chromium.mjs
// GXM_SURFACE_FIXTURE overrides the fixture eboot (default: the web64 build's);
// PLAYWRIGHT_CHROMIUM_EXECUTABLE and GXM_RUNTIME_DIST pass through.
import { spawn } from 'node:child_process';
import { copyFile, mkdir, mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import assert from 'node:assert/strict';

const fixture = resolve(process.env.GXM_SURFACE_FIXTURE
  || 'build/web64/browser/tests/vita_gxm_surface_fixture/eboot.bin');
const title = 'GXMSURF01';
const stage = await mkdtemp(join(tmpdir(), 'gxm-surface-'));
try {
  await mkdir(join(stage, 'ux0/app', title), { recursive: true });
  await copyFile(fixture, join(stage, 'ux0/app', title, 'eboot.bin'));
  for (const [surfaceSync, exitCode] of [['1', 100], ['0', 101]]) {
    const log = join(stage, `worker-${surfaceSync}.log`);
    const child = spawn(process.execPath, ['browser/tests/limbo_app_chromium.mjs'], {
      env: { ...process.env, LIMBO_STAGE: stage, LIMBO_TITLE: title, LIMBO_SURFACE_SYNC: surfaceSync,
        LIMBO_DEADLINE_MS: '90000', LIMBO_MAX_FRAMES: '0', LIMBO_FRAME_OUT: join(stage, 'frame'), LIMBO_LOG_OUT: log },
      stdio: ['ignore', 'pipe', 'pipe'] });
    let output = '';
    child.stdout.on('data', (chunk) => { output += chunk; });
    child.stderr.on('data', (chunk) => { output += chunk; });
    const code = await new Promise((done) => child.on('exit', done));
    // The report is the harness's first top-level JSON object.
    const start = output.indexOf('{\n'), end = output.indexOf('\n}', start);
    assert.ok(start >= 0 && end > start, output.slice(-3000));
    const report = JSON.parse(output.slice(start, end + 2));
    const logs = await readFile(log, 'utf8');
    assert.deepEqual(report.exit, { exitCode, ok: true },
      `surface sync ${surfaceSync}: fixture exit (see main.c)\n${logs.split('\n').slice(-60).join('\n')}`);
    // The fixture presents no frames; the harness reports that as its own failure.
    assert.ok(code === 0 || /no frames presented/.test(output), output.slice(-3000));
    assert.deepEqual(report.gxmSkips, [], 'no draw may be skipped');
    assert.deepEqual(report.gxmFailures, []);
    assert.doesNotMatch(logs, /\[gxm-note\]/, 'every texture over a surface must sample it');
    assert.doesNotMatch(logs, /sceCommonDialogUpdate.*(unimplemented|not implemented)/i);
    console.log(`surface sync ${surfaceSync}: exit ${report.exit.exitCode}`);
  }
} finally {
  await rm(stage, { recursive: true, force: true });
}
