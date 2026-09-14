// Real VitaSDK SELF -> production Worker -> vita3k_web_run_vita, on both CPUs.
// Usage: node browser/tests/jit_fixture_smoke.mjs [build/web/dist] [build/vita-homebrew-fixture/eboot.bin]
// Defaults are repository-relative. Never builds/stages assets or uses port 5173.
// Parent must stage worker.js (selecting ?backend=jit), storage.js, and both
// vita3k_web{,_jit}.{js,wasm} outputs before running this test.
// Environment: PLAYWRIGHT_MODULE_URL (default build/playwright installation),
// PLAYWRIGHT_CHROMIUM_EXECUTABLE (otherwise Playwright's installed Chromium),
// JIT_FIXTURE_TIMEOUT_MS (per backend, including compilation; default 120000),
// JIT_FIXTURE_REQUIRE_CPU_STATE=1 (optional stricter trace contract once settled).
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { access, readFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { dirname, extname, resolve, sep } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const repository = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const root = resolve(process.argv[2] || resolve(repository, 'build/web/dist'));
const fixture = resolve(process.argv[3] || resolve(repository, 'build/vita-homebrew-fixture/eboot.bin'));
const timeoutMs = Number(process.env.JIT_FIXTURE_TIMEOUT_MS || 120000);
assert.ok(Number.isSafeInteger(timeoutMs) && timeoutMs > 0 && timeoutMs <= 600000,
  'JIT_FIXTURE_TIMEOUT_MS must be an integer between 1 and 600000');
const requireCpuState = process.env.JIT_FIXTURE_REQUIRE_CPU_STATE === '1';
const required = ['worker.js', 'storage.js', 'vita3k_web.js', 'vita3k_web.wasm',
  'vita3k_web_jit.js', 'vita3k_web_jit.wasm'];
const missing = [];
for (const file of [...required.map(file => resolve(root, file)), fixture]) {
  try { await access(file); } catch { missing.push(file); }
}
assert.deepEqual(missing, [], `Parent targets/fixture not ready; no build attempted. Missing: ${missing.join(', ')}`);
const fixtureBytes = await readFile(fixture);
assert.deepEqual([...fixtureBytes.subarray(0, 4)], [0x53, 0x43, 0x45, 0x00],
  'Expected a VitaSDK SELF (SCE magic), not a raw ELF or a hand-written instruction probe');
const fixtureSha256 = createHash('sha256').update(fixtureBytes).digest('hex');
const playwright = process.env.PLAYWRIGHT_MODULE_URL
  || pathToFileURL(resolve(repository, 'build/playwright/node_modules/playwright/index.mjs')).href;
const { chromium } = await import(playwright);

const requests = [];
const serverErrors = [];
const server = createServer(async (req, res) => {
  try {
    const url = new URL(req.url, 'http://127.0.0.1');
    requests.push(url.pathname + url.search);
    const pathname = decodeURIComponent(url.pathname);
    let type, content;
    if (pathname === '/__jit_fixture_smoke.html') {
      // An inert host page avoids index.html launching an extra default worker.
      // The worker and all runtime dependencies are served verbatim from dist.
      type = 'text/html';
      content = '<!doctype html><meta charset="utf-8"><link rel="icon" href="data:,"><title>Vita JIT fixture smoke</title>';
    } else if (pathname === '/__jit_fixture.bin') {
      type = 'application/octet-stream';
      content = fixtureBytes;
    } else {
      const file = resolve(root, `.${pathname}`);
      if (!file.startsWith(root + sep)) throw new Error('Path outside dist');
      type = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm',
        '.html': 'text/html', '.json': 'application/json' }[extname(file)] || 'application/octet-stream';
      content = await readFile(file);
    }
    res.writeHead(200, {
      'Content-Type': type,
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
      'Cache-Control': 'no-store',
    });
    res.end(content);
  } catch (error) {
    serverErrors.push(`${req.url}: ${String(error)}`);
    res.writeHead(404);
    res.end();
  }
});
await new Promise((resolveListen, reject) => {
  server.once('error', reject);
  server.listen(0, '127.0.0.1', resolveListen);
});
const origin = `http://127.0.0.1:${server.address().port}`;

async function runBackend(browser, backend) {
  // Separate contexts prevent cached modules/storage from crossing the oracle
  // and JIT runs. Sequential execution avoids competing compiler/memory loads.
  const context = await browser.newContext();
  const errors = [];
  const startRequest = requests.length;
  let result;
  try {
    context.on('requestfailed', request => errors.push(`request failed: ${request.url()}: ${request.failure()?.errorText}`));
    context.on('response', response => {
      if (response.status() >= 400) errors.push(`HTTP ${response.status()}: ${response.url()}`);
    });
    const page = await context.newPage();
    page.on('pageerror', error => errors.push(`pageerror: ${String(error)}`));
    page.on('crash', () => errors.push('browser page crashed'));
    page.on('console', message => {
      if (message.type() === 'error') errors.push(`console.error: ${message.text()}`);
    });
    await page.goto(`${origin}/__jit_fixture_smoke.html`, { timeout: 15000 });
    result = await page.evaluate(async ({ backend, timeoutMs }) => {
      const started = performance.now();
      const messages = [];
      const workerErrors = [];
      const controller = new AbortController();
      // No query at all for the oracle: exercise the unchanged default path.
      const worker = new Worker(backend === 'jit' ? './worker.js?backend=jit' : './worker.js', { type: 'module' });
      let timer, statusTimer;
      let ready = null, outcome = null, status = null, failure = null;
      try {
        await new Promise((resolveRun, reject) => {
          const fail = error => reject(new Error(String(error)));
          timer = setTimeout(() => fail(`Worker deadline exceeded (${timeoutMs} ms)`), timeoutMs);
          worker.onerror = event => {
            const detail = `${event.message} at ${event.filename}:${event.lineno}`;
            workerErrors.push(detail);
            fail(detail);
          };
          worker.onmessageerror = () => {
            workerErrors.push('Worker message deserialization failed');
            fail(workerErrors.at(-1));
          };
          const sendFixture = async () => {
            const response = await fetch('./__jit_fixture.bin', { signal: controller.signal });
            if (!response.ok) throw new Error(`Fixture HTTP ${response.status}`);
            const bytes = await response.arrayBuffer();
            // Structured-cloned File, not an injected export call or fake worker.
            worker.postMessage({ type: 'run-vita', file: new File([bytes], 'eboot.bin', { type: 'application/octet-stream' }) });
          };
          worker.onmessage = ({ data }) => {
            messages.push(data);
            if (messages.length > 10000) { fail('Excessive Worker messages'); return; }
            if (data.type === 'error') {
              workerErrors.push(String(data.message));
              fail(data.message);
            } else if (data.type === 'ready') {
              if (ready) { fail('Duplicate Worker ready'); return; }
              ready = data;
              void sendFixture().catch(fail);
            } else if (data.type === 'vita-exit') {
              if (!ready || outcome) { fail('Unexpected/duplicate vita-exit'); return; }
              outcome = data;
              // Receiving an exit alone is not proof that the worker survived.
              // This is the only status request, sent strictly after exit.
              worker.postMessage({ type: 'status' });
              statusTimer = setTimeout(() => fail('No post-exit Worker status reply (5000 ms)'), 5000);
            } else if (data.type === 'status') {
              if (!outcome) { fail('Unsolicited status before exit'); return; }
              status = data;
              resolveRun();
            }
          };
        });
      } catch (error) {
        failure = String(error);
      } finally {
        clearTimeout(timer);
        clearTimeout(statusTimer);
        controller.abort();
        worker.terminate(); // Only this test's worker; never touches external processes.
      }
      return { backend, ready, outcome, status, failure, workerErrors, messages,
        crossOriginIsolated: self.crossOriginIsolated, elapsedMs: performance.now() - started };
    }, { backend, timeoutMs });
  } finally {
    await context.close();
  }
  result.browserErrors = errors;
  result.requests = requests.slice(startRequest);
  return result;
}

function inspectRun(run) {
  const label = run.backend;
  assert.equal(run.failure, null, `${label}: ${run.failure}`);
  assert.deepEqual(run.workerErrors, [], `${label}: Worker errors`);
  assert.deepEqual(run.browserErrors, [], `${label}: browser errors`);
  assert.equal(run.crossOriginIsolated, true, `${label}: COOP/COEP isolation missing`);
  assert.equal(run.ready?.diagnostics?.worker, true, `${label}: not a real runtime Worker`);
  assert.equal(run.ready?.diagnostics?.wasm, true, `${label}: not a Wasm runtime`);
  assert.equal(run.outcome?.ok, true, `${label}: Vita run failed`);
  assert.equal(run.outcome?.exitCode, 42, `${label}: genuine fixture must exit 42`);
  assert.equal(run.status?.state, 'ready', `${label}: worker not alive/ready after exit`);
  const module = label === 'jit' ? 'vita3k_web_jit' : 'vita3k_web';
  const other = label === 'jit' ? 'vita3k_web' : 'vita3k_web_jit';
  const paths = run.requests.map(path => new URL(path, origin).pathname);
  assert.ok(run.requests.includes(label === 'jit' ? '/worker.js?backend=jit' : '/worker.js'),
    `${label}: real dist worker transport not fetched`);
  for (const extension of ['js', 'wasm']) {
    assert.ok(paths.includes(`/${module}.${extension}`), `${label}: ${module}.${extension} not fetched`);
    assert.ok(!paths.includes(`/${other}.${extension}`), `${label}: wrong backend/fallback module fetched`);
  }
  const lines = run.messages.filter(message => message.type === 'log')
    .flatMap(message => String(message.message).split(/\r?\n/));
  const faults = lines.filter(line => /\bCPU\s+(?:error|fault)|Vita runtime error|\bRuntimeError\b|\bCompileError\b|\bAborted\b|unreachable|memory access out of bounds|\[(?:error|critical)\]|\b(?:InterpreterCPU|WasmJitCPU)\b.*\b(?:failed|unsupported|error|fault)\b/i.test(line));
  assert.deepEqual(faults, [], `${label}: CPU/runtime errors`);
  const fallbacks = lines.filter(line => /fallback|falling back/i.test(line.replace('(no fallback)', '')));
  assert.deepEqual(fallbacks, [], `${label}: fallback is forbidden`);
  const backendLines = lines.filter(line => line.startsWith('[vita3k-web] CPU backend:'));
  if (label === 'jit') {
    assert.deepEqual(backendLines, ['[vita3k-web] CPU backend: WasmJitCPU (no fallback)'],
      'JIT backend must identify itself explicitly (not a reference-only probe)');
  } else {
    assert.ok(backendLines.every(line => /CPU backend: InterpreterCPU\b/.test(line)),
      'Default backend must remain InterpreterCPU');
  }
  const imports = lines.flatMap(line => {
    const match = /^\[vita3k-web\] Vita import: (\S+) NID=([\da-f]{8}) PC=([\da-f]{8})$/i.exec(line);
    return match ? [{ name: match[1], nid: match[2].toLowerCase(), pc: match[3].toLowerCase() }] : [];
  });
  assert.equal(imports.length, 23, `${label}: expected 23 startup/exit HLE imports`);
  assert.equal(imports.at(-1).name, 'sceKernelExitProcess', `${label}: missing real process-exit import`);
  assert.equal(imports.at(-1).nid, '7595d9aa', `${label}: wrong process-exit NID`);
  const summaries = lines.filter(line => line.startsWith('[vita3k-web] Vita result:'));
  assert.equal(summaries.length, 1, `${label}: missing/duplicate Vita result`);
  const summary = /^\[vita3k-web\] Vita result: process_exit=1 code=42 imports=23 missing_nids=0 PC=([\da-f]{8})$/i.exec(summaries[0]);
  assert.ok(summary, `${label}: process exit/import/missing-NID contract failed: ${summaries[0]}`);
  let stats = null;
  if (label === 'jit') {
    const statsLines = lines.filter(line => line.startsWith('[vita3k-web] JIT stats:'));
    assert.equal(statsLines.length, 1, 'Missing/duplicate final JIT stats');
    const match = /^\[vita3k-web\] JIT stats: instructions=(\d+) compiled=(\d+) hits=(\d+) invalidated=(\d+)(?: regions=(\d+))?$/.exec(statsLines[0]);
    assert.ok(match, `Malformed JIT stats: ${statsLines[0]}`);
    stats = Object.fromEntries(['instructions', 'compiled', 'hits', 'invalidated', 'regions'].map((key, index) => [key, match[index + 1] ?? '0']));
    assert.ok(BigInt(stats.instructions) > 0n, 'JIT executed no guest instructions');
    assert.ok(BigInt(stats.compiled) > 0n || BigInt(stats.regions) > 0n, 'JIT compiled no blocks or regions');
    // Zero hits/invalidations are valid for this small fixture; never invent work.
  }
  // Keep every CPU field verbatim. No PC/register masking or dropping fields to
  // make the oracle agree. Absence/incompleteness is explicit while optional.
  const cpuStates = [];
  const traceIssues = [];
  let pendingState = null;
  for (const line of lines) {
    if (line.startsWith('[vita3k-web] CPU state:')) {
      if (pendingState !== null) traceIssues.push('Multiple CPU states before an import');
      try {
        pendingState = JSON.parse(line.slice('[vita3k-web] CPU state:'.length));
        if (!pendingState || typeof pendingState !== 'object' || Array.isArray(pendingState)
          || Object.keys(pendingState).length === 0) throw new Error('Expected nonempty CPU-state object');
      } catch (error) {
        traceIssues.push(String(error));
        pendingState = null;
      }
    } else if (line.startsWith('[vita3k-web] Vita import:')) {
      cpuStates.push(pendingState);
      pendingState = null;
    }
  }
  if (pendingState !== null) traceIssues.push('CPU state without a following import');
  const completeCpuStates = traceIssues.length === 0 && cpuStates.length === 23 && cpuStates.every(state => state !== null);
  return { imports, finalPc: summary[1].toLowerCase(), stats, cpuStates, completeCpuStates, traceIssues };
}

let browser;
const runs = [];
try {
  browser = await chromium.launch({ headless: true, timeout: 30000,
    ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE
      ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}),
  });
  for (const backend of ['interpreter', 'jit']) runs.push(await runBackend(browser, backend));
  assert.deepEqual(serverErrors, [], 'HTTP asset-serving errors');
  const [interpreter, jit] = runs.map(inspectRun);
  assert.deepEqual(jit.imports.map(entry => entry.nid), interpreter.imports.map(entry => entry.nid),
    'JIT/interpreter import NID sequences differ');
  assert.deepEqual(jit.imports, interpreter.imports, 'JIT/interpreter import names or PCs differ');
  assert.equal(jit.finalPc, interpreter.finalPc, 'JIT/interpreter final PCs differ');
  const compareCpuState = interpreter.completeCpuStates && jit.completeCpuStates;
  if (requireCpuState) assert.ok(compareCpuState, 'Full CPU state required before all 23 imports on both backends');
  if (compareCpuState) {
    assert.deepEqual(jit.cpuStates, interpreter.cpuStates, 'JIT/interpreter full CPU-state traces differ');
  }
  console.log(JSON.stringify({ result: 'PASS', fixture, fixtureSha256, dist: root,
    exitCode: 42, imports: jit.imports, missingNids: 0, jitStats: jit.stats,
    cpuStateComparison: compareCpuState ? 'compared all 23 JSON objects without normalization'
      : 'NOT COMPARED: optional CPU-state traces absent or incomplete; no full-state equivalence claimed',
    cpuTraceIssues: { interpreter: interpreter.traceIssues, jit: jit.traceIssues },
    runs: runs.map(run => ({ backend: run.backend, elapsedMs: run.elapsedMs, status: run.status, requests: run.requests })),
  }, null, 2));
} catch (error) {
  console.error(JSON.stringify({ result: 'FAIL', fixture, fixtureSha256, dist: root,
    error: String(error.stack || error), serverErrors, runs }, null, 2));
  process.exitCode = 1;
} finally {
  try { await browser?.close(); }
  finally { await new Promise(resolveClose => server.close(resolveClose)); }
}
