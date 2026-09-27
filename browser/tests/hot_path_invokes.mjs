// SPDX-License-Identifier: GPL-2.0-or-later
// The guest hot path must not call through Emscripten's invoke_* wrappers.
// Under JS exception handling, every call from a function with a landing pad
// (a local with a destructor, a try, or a noexcept function calling one that
// may throw) becomes a JS round trip through invoke_*, dynCall_* and
// stackSave; in Limbo that was the largest non-guest cost. This check reads
// the built module's disassembly and fails when one of these functions gets
// such a call again, or when one of them can no longer be found by name.
//
//   node browser/tests/hot_path_invokes.mjs [build/web64/browser/vita3k_web_jit.wasm]
//
// Needs wasm-objdump (wabt): $WASM_OBJDUMP or on PATH. Runs after every
// vita3k_web_jit link (browser/runtime_wasmjit.cmake).
import { spawn } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { createInterface } from 'node:readline';

const wasm = process.argv[2] || 'build/web64/browser/vita3k_web_jit.wasm';
// Function names by index: the module's name section when it has one
// (--profiling-funcs), otherwise the linker's symbol map (--emit-symbol-map).
const symbols = new Map();
const symbolMap = wasm.replace(/\.wasm$/, '.js.symbols');
if (existsSync(symbolMap)) {
  for (const line of readFileSync(symbolMap, 'utf8').split('\n')) {
    const colon = line.indexOf(':');
    if (colon > 0) symbols.set(Number(line.slice(0, colon)),
      line.slice(colon + 1).replace(/\\([0-9a-f]{2})/g, (_, hex) => String.fromCharCode(parseInt(hex, 16))));
  }
}
// Function name (as in the name section) -> calls allowed through invoke_*.
const hot = new Map([
  ['ThreadState::run_host_active_loop()', 0],
  // One: the debugger's single-step path calls the virtual CPUInterface::step.
  ['vita3k::web::GuestThreadRuntime::Impl::run_cpu(ThreadState&, bool)', 1],
]);
// call_import may keep invokes on its cold paths (debug watch, missing NID),
// but the HLE body itself must be a direct call_indirect after resolve_import.
const callImport = 'call_import(EmuEnvState&, CPUState&, unsigned int, int)';

const objdump = spawn(process.env.WASM_OBJDUMP || 'wasm-objdump', ['-d', wasm], { stdio: ['ignore', 'pipe', 'inherit'] });
const found = new Map();
let current = null, callImportLines = null;
for await (const line of createInterface({ input: objdump.stdout })) {
  const header = /^[0-9a-f]+ func\[(\d+)\](?: <(.*)>)?:$/.exec(line);
  if (header) {
    current = header[2] ?? symbols.get(Number(header[1])) ?? null;
    if (hot.has(current)) found.set(current, []);
    if (current === callImport) callImportLines = [];
    continue;
  }
  if (!current) continue;
  let call = /\|\s+(call(?:_indirect)? .*)$/.exec(line)?.[1];
  if (!call) continue;
  const callee = /^call (\d+)$/.exec(call);
  if (callee && symbols.has(Number(callee[1]))) call += ` <${symbols.get(Number(callee[1]))}>`;
  if (hot.has(current) && /<invoke_/.test(call)) found.get(current).push(call);
  if (current === callImport) callImportLines.push(call);
}
const exitCode = await new Promise((done) => objdump.on('close', done));
if (exitCode !== 0) throw new Error(`wasm-objdump failed (${exitCode}) on ${wasm}`);

const failures = [];
for (const [name, allowed] of hot) {
  const calls = found.get(name);
  if (!calls) failures.push(`${name}: not found in ${wasm} (renamed or fully inlined?)`);
  else if (calls.length > allowed) failures.push(`${name}: ${calls.length} invoke_* calls:\n    ${calls.join('\n    ')}`);
}
if (!callImportLines) {
  failures.push(`${callImport}: not found in ${wasm}`);
} else {
  const resolve = callImportLines.findIndex((call) => /<resolve_import\(/.test(call));
  const next = resolve >= 0 ? callImportLines[resolve + 1] : undefined;
  if (!next || !next.startsWith('call_indirect'))
    failures.push(`${callImport}: the HLE body after resolve_import is not a direct call_indirect (next: ${next})`);
}
if (failures.length) {
  console.error(`hot path invoke check failed:\n  ${failures.join('\n  ')}`);
  process.exit(1);
}
console.log(`hot path invoke check passed (${[...hot.keys()].length} functions, call_import HLE call direct)`);
