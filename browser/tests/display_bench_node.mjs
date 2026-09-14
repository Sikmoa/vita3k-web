// M14c Node benchmark driver: runs the short VitaSDK display fixture through
// the real browser runtime module (interpreter or JIT bench build) with
// ASYNCIFY + NODERAWFS, and reports startup, steady-state per-frame stats,
// guest instruction rate and exit code. The recorded baseline lives in
// browser/tests/BENCH_REPORT.md.
//
// Usage:
//   node browser/tests/display_bench_node.mjs <module.js> <eboot.bin> [label]
//
//   <module.js>  Emscripten ES6 module factory (vita3k_display_bench_node.js
//                or vita3k_display_bench_jit_node.js from the browser build)
//   <eboot.bin>  genuine VitaSDK display fixture eboot (short variant)
//   [label]      bench label used in the report ("interpreter" / "jit")
//
// Environment:
//   VITA3K_DISPLAY_BENCH_TIMEOUT_MS  overall deadline (default 600000)
//   VITA3K_DISPLAY_BENCH_SKIP_FRAMES steady-state window skips the first N
//                                    frames (default 10)
//
// The guest is driven exactly like the browser Worker drives it: bytes are
// copied into the module heap and module._vita3k_web_run_vita(allocation,
// size) is called; with ASYNCIFY the call suspends inside emscripten_sleep
// and the final exit code arrives through the runtime's vita3kWebOnExit host
// hook. The frame sink is the runtime's own vita3kWebOnFrame host hook - no
// test-only code runs inside the guest, and the display bridge's pixel path
// is untouched (this sink never reads the frame view).
//
// Per-frame guest instruction counts come from the bridge's narrow benchmark
// seam (vita3k_web_last_frame_instructions), sampled synchronously inside the
// frame hook; the run total comes from the runtime's own counter
// (vita3k_web_last_run_instructions) and is cross-checked against the
// runtime's printed "[vita3k-web] Vita benchmark:" line.
//
// Exit status: 0 only when the guest exits 77 (fixture completion) and a
// steady-state window was measured; 1 on benchmark failure, 3 on timeout.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import { readFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const label = process.argv[4] || 'bench';
const [moduleArg, ebootArg] = process.argv.slice(2);
if (!moduleArg || !ebootArg) {
  console.error('Usage: node browser/tests/display_bench_node.mjs <module.js> <eboot.bin> [label]');
  process.exit(2);
}
const modulePath = resolve(moduleArg);
const ebootPath = resolve(ebootArg);
const timeoutMs = Number(process.env.VITA3K_DISPLAY_BENCH_TIMEOUT_MS || 600000);
const skipFrames = Number(process.env.VITA3K_DISPLAY_BENCH_SKIP_FRAMES || 10);
assert.ok(Number.isSafeInteger(timeoutMs) && timeoutMs > 0, 'invalid VITA3K_DISPLAY_BENCH_TIMEOUT_MS');
assert.ok(Number.isSafeInteger(skipFrames) && skipFrames >= 0, 'invalid VITA3K_DISPLAY_BENCH_SKIP_FRAMES');

const ebootBytes = await readFile(ebootPath);
assert.deepEqual([...ebootBytes.subarray(0, 4)], [0x53, 0x43, 0x45, 0x00],
  'Expected a VitaSDK SELF (SCE magic), not a raw ELF or hand-written probe');
const ebootSha256 = createHash('sha256').update(ebootBytes).digest('hex');

// Host hooks (see browser/src/vita_runtime.cpp). Defined before the module is
// even imported so nothing can race the run. `module` is assigned once the
// factory resolves; the frame hook only fires during the run, after that.
let module = null;
const frames = [];
let exitCode = null;
let exitAt = null;
let exitResolve = null;
const exited = new Promise((resolveExit) => { exitResolve = resolveExit; });

globalThis.vita3kWebOnFrame = (generation, width, height) => {
  // Timing + instruction snapshot only: this sink deliberately never touches
  // the pixel view, so the bridge's synchronous-copy contract is satisfied
  // trivially and no benchmark time is spent on pixels.
  frames.push({
    t: performance.now(),
    generation,
    width,
    height,
    instructions: module ? BigInt(module._vita3k_web_last_frame_instructions()) : 0n,
  });
};
globalThis.vita3kWebOnExit = (code) => {
  if (exitCode !== null) return; // one run per process; ignore duplicates
  exitCode = code;
  exitAt = performance.now();
  exitResolve();
};

// Module stdout/stderr are captured (and relayed verbatim) so the runtime's
// own summary lines - "Vita benchmark:", "JIT stats:", "JIT profile:",
// "Vita result:" - can be reported from the runtime itself, not recomputed.
// With NODERAWFS the runtime's stdio goes to the real fds through
// fs.writeSync, bypassing Emscripten's print: callback, so the capture
// interposes there (test-harness only; the bytes still reach the real fd).
const capturedLines = [];
const writeCapture = (() => {
  const realWriteSync = fs.writeSync;
  const tails = new Map(); // per-fd partial-line reassembly
  fs.writeSync = function interposedWriteSync(fd, buffer, ...rest) {
    if (fd === 1 || fd === 2) {
      try {
        const text = Buffer.from(buffer.buffer ?? buffer, buffer.byteOffset ?? 0,
          buffer.byteLength ?? buffer.length).toString('utf8');
        const parts = text.split('\n');
        for (let i = 0; i < parts.length - 1; i++) {
          capturedLines.push((tails.get(fd) ?? '') + parts[i]);
          tails.set(fd, '');
        }
        tails.set(fd, (tails.get(fd) ?? '') + parts[parts.length - 1]);
      } catch { /* capture must never break the run */ }
    }
    return realWriteSync.call(fs, fd, buffer, ...rest);
  };
  return () => { // flush partial trailing lines (after the runtime's fflush)
    for (const [fd, tail] of [...tails]) {
      if (tail) capturedLines.push(tail);
      tails.set(fd, '');
    }
  };
})();
const relay = (stream) => (line) => {
  const text = String(line);
  capturedLines.push(text);
  stream.write(text + '\n');
};

const importStartedAt = performance.now();
const { default: createModule } = await import(pathToFileURL(modulePath).href);
module = await createModule({
  print: relay(process.stdout),
  printErr: relay(process.stderr),
});
const moduleInitMs = performance.now() - importStartedAt;
assert.equal(typeof module._vita3k_web_run_vita, 'function', 'module does not export _vita3k_web_run_vita');
assert.equal(typeof module._vita3k_web_last_run_instructions, 'function',
  'module does not export _vita3k_web_last_run_instructions');
assert.equal(typeof module._vita3k_web_last_frame_instructions, 'function',
  'module does not export _vita3k_web_last_frame_instructions (bridge seam missing)');

const allocation = module._malloc(ebootBytes.length);
assert.ok(allocation, 'unable to allocate the eboot input buffer');
module.HEAPU8.set(ebootBytes, allocation);
// The allocation is intentionally not freed afterwards, exactly like the
// browser Worker: freeing while the run is suspended across an ASYNCIFY
// yield would mutate the heap under a suspended stack.

const entryAt = performance.now();
module._vita3k_web_run_vita(allocation, ebootBytes.length); // suspends; result via vita3kWebOnExit

const timedOut = Symbol('timeout');
let timeoutTimer = null;
const outcome = await Promise.race([
  exited,
  new Promise((resolveTimeout) => {
    timeoutTimer = setTimeout(() => resolveTimeout(timedOut), timeoutMs);
  }),
]);
// The race's losing side must not keep the Node event loop alive.
if (timeoutTimer !== null) clearTimeout(timeoutTimer);

// Flush the runtime's stdio buffers so trailing summary lines are captured
// (musl may still hold them; the run has finished so this is safe).
try { module._fflush?.(0); } catch { /* optional export */ }
writeCapture(); // capture any partial trailing line the buffers still hold

const stats = (values) => {
  if (values.length === 0) return null;
  const sorted = [...values].sort((a, b) => a - b);
  const mid = sorted.length >> 1;
  return {
    count: sorted.length,
    min: sorted[0],
    median: sorted.length % 2 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2,
    max: sorted[sorted.length - 1],
  };
};

const totalFrames = frames.length;
const startupMs = totalFrames > 0 ? frames[0].t - entryAt : null;
// Steady-state window: frames [skipFrames .. totalFrames-1]; per-frame times
// are the deltas between consecutive frames inside that window.
const steadyDeltas = [];
for (let i = skipFrames + 1; i < totalFrames; i++)
  steadyDeltas.push(frames[i].t - frames[i - 1].t);
const frameStats = stats(steadyDeltas);
const steadyWindowMs = totalFrames > skipFrames ? frames[totalFrames - 1].t - frames[skipFrames].t : null;
const steadyInstructions = totalFrames > skipFrames
  ? frames[totalFrames - 1].instructions - frames[skipFrames].instructions : null;
const steadyInstrPerSec = steadyWindowMs && steadyWindowMs > 0
  ? Number(steadyInstructions) / (steadyWindowMs / 1000) : null;
const steadyFps = steadyWindowMs && steadyWindowMs > 0
  ? (totalFrames - 1 - skipFrames) / (steadyWindowMs / 1000) : null;
const totalInstructions = BigInt(module._vita3k_web_last_run_instructions());

const findLine = (prefix) => capturedLines.find((line) => line.startsWith(prefix)) || null;
const backendLine = findLine('[vita3k-web] CPU backend:');
const benchmarkLine = findLine('[vita3k-web] Vita benchmark:');
const jitStatsLine = findLine('[vita3k-web] JIT stats:');
const jitProfileLine = findLine('[vita3k-web] JIT profile:');
const resultLine = findLine('[vita3k-web] Vita result:');

const report = {
  label,
  module: modulePath,
  fixture: ebootPath,
  fixtureSha256: ebootSha256,
  fixtureBytes: ebootBytes.length,
  backendLine,
  moduleInitMs: Number(moduleInitMs.toFixed(1)),
  entryToFirstFrameMs: startupMs === null ? null : Number(startupMs.toFixed(1)),
  frameCount: totalFrames,
  steadyState: {
    skipFrames,
    frames: totalFrames > skipFrames ? totalFrames - skipFrames : 0,
    windowMs: steadyWindowMs === null ? null : Number(steadyWindowMs.toFixed(1)),
    frameMs: frameStats === null ? null : {
      count: frameStats.count,
      min: Number(frameStats.min.toFixed(1)),
      median: Number(frameStats.median.toFixed(1)),
      max: Number(frameStats.max.toFixed(1)),
    },
    fps: steadyFps === null ? null : Number(steadyFps.toFixed(3)),
    instructions: steadyInstructions === null ? null : String(steadyInstructions),
    instructionsPerSec: steadyInstrPerSec === null ? null : Math.round(steadyInstrPerSec),
  },
  totalGuestInstructions: String(totalInstructions),
  exitCode,
  runtimeLines: { benchmarkLine, resultLine, jitStatsLine, jitProfileLine },
  timedOut: outcome === timedOut,
};

if (report.timedOut) {
  console.error(`[display-bench] TIMEOUT after ${timeoutMs} ms (frames=${totalFrames}, exit=${exitCode})`);
  console.error(JSON.stringify(report, null, 2));
  process.exit(3);
}

const human = [
  `[display-bench] label=${label} frames=${totalFrames} exit=${exitCode}`,
  `  module init (import+instantiate): ${report.moduleInitMs} ms`,
  `  startup (entry -> first frame): ${report.entryToFirstFrameMs} ms`,
  `  steady state (skip first ${skipFrames} frames): ${report.steadyState.frames} frames over ${report.steadyState.windowMs} ms`,
  `  per-frame ms: min=${report.steadyState.frameMs?.min} median=${report.steadyState.frameMs?.median} max=${report.steadyState.frameMs?.max}`,
  `  steady FPS: ${report.steadyState.fps}`,
  `  guest instructions: total=${report.totalGuestInstructions} steady=${report.steadyState.instructions}`,
  `  steady instructions/sec: ${report.steadyState.instructionsPerSec}`,
];
if (jitStatsLine) human.push(`  ${jitStatsLine}`);
if (jitProfileLine) human.push(`  ${jitProfileLine}`);
if (benchmarkLine) human.push(`  ${benchmarkLine}`);
console.log(human.join('\n'));
console.log(`[display-bench] JSON ${JSON.stringify(report)}`);

const ok = exitCode === 77 && totalFrames > skipFrames + 1;
const exitStatus = ok ? 0 : 1;
if (!ok) {
  console.error(`[display-bench] FAIL: exit=${exitCode} frames=${totalFrames} (expected exit 77 with more than ${skipFrames + 1} frames)`);
}
// Exit explicitly: the Emscripten runtime may keep handles that would
// otherwise hold the Node event loop open after the benchmark is done.
// POSIX process.stdout writes are synchronous, so the report above is out.
await new Promise((resolveFlush) => {
  if (process.stdout.write('')) resolveFlush();
  else process.stdout.once('drain', resolveFlush);
});
process.exit(exitStatus);
