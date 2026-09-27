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
// std::dynamic_extent is size_t(-1), which differs between wasm32 and wasm64.
const normalize = (name) => name?.replace(/\b(?:18446744073709551615|4294967295)ul\b/g, 'dynamic_extent') ?? null;
// Function name (as in the name section, dynamic_extent normalized) -> calls
// allowed through invoke_*.
const hot = new Map([
  ['ThreadState::run_host_active_loop()', 0],
  // One: the debugger's single-step path calls the virtual CPUInterface::step.
  ['vita3k::web::GuestThreadRuntime::Impl::run_cpu(ThreadState&, bool)', 1],
  // Every guest thread switch: the dispatch loop and the fiber swaps
  // (guest_thread_runtime.cpp, guest_fiber_scheduler.cpp). Two: suspend's
  // logic_error constructions on its misuse paths.
  ['vita3k::web::GuestThreadRuntime::Impl::dispatch(unsigned long)', 0],
  ['vita3k::web::GuestThreadRuntime::Impl::suspend(bool, bool)', 2],
  // GXM command recording, several calls per guest draw (renderer.cpp,
  // SceGxm.cpp): commands are allocated through the context's std::function.
  ['renderer::set_program(renderer::State&, renderer::Context*, Ptr<void const>, bool)', 0],
  ['renderer::set_texture(renderer::State&, renderer::Context*, unsigned int, SceGxmTexture)', 0],
  ['renderer::set_vertex_stream(renderer::State&, renderer::Context*, unsigned long, unsigned long, Ptr<void const>)', 0],
  ['gxmSetUniformBuffers(renderer::State&, GxmState&, SceGxmContext*, SceGxmProgram const&, std::__2::span<Ptr<void const>, dynamic_extent>, std::__2::array<unsigned int, 15ul> const&, MemState const&)', 0],
]);
// Functions whose invoke_* calls may only reach container growth inside
// inlined standard-library code (an unordered_map rehash when a key is first
// inserted); every call a scene makes per command must be direct. The browser
// GXM consumer's command loop (gxm_webgpu_bridge.cpp) runs once per command.
const growthOnly = new Map([
  ['renderer::consume_commands(renderer::State&, renderer::Context*, renderer::Command*&, MemState&, renderer::scene::Writer&, renderer::Submission&)',
    /^(?:std::__2::__next_prime\(|void std::__2::__hash_table<.*>::__do_rehash<|void std::__2::__hash_table<.*>::__rehash<)/],
]);
// call_import may keep invokes on its cold paths (debug watch, missing NID),
// but the HLE body itself must be a direct call_indirect after resolve_import.
const callImport = 'call_import(EmuEnvState&, CPUState&, unsigned int, int)';

// Table index -> function name, to name the callee of an invoke_* call.
const tableNames = new Map();
{
  const elements = spawn(process.env.WASM_OBJDUMP || 'wasm-objdump', ['-x', '-j', 'Elem', wasm], { stdio: ['ignore', 'pipe', 'inherit'] });
  for await (const line of createInterface({ input: elements.stdout })) {
    const element = /^\s*- elem\[(\d+)\] = (?:ref\.func:|func\[)(\d+)/.exec(line);
    if (element) tableNames.set(Number(element[1]), symbols.get(Number(element[2])) ?? `func ${element[2]}`);
  }
  if (await new Promise((done) => elements.on('close', done)) !== 0) throw new Error(`wasm-objdump -x failed on ${wasm}`);
}
const objdump = spawn(process.env.WASM_OBJDUMP || 'wasm-objdump', ['-d', wasm], { stdio: ['ignore', 'pipe', 'inherit'] });
const found = new Map();
let current = null, callImportLines = null, recent = [];
for await (const line of createInterface({ input: objdump.stdout })) {
  const header = /^[0-9a-f]+ func\[(\d+)\](?: <(.*)>)?:$/.exec(line);
  if (header) {
    current = normalize(header[2] ?? symbols.get(Number(header[1])) ?? null);
    if (hot.has(current) || growthOnly.has(current)) found.set(current, []);
    if (current === callImport) callImportLines = [];
    recent = [];
    continue;
  }
  if (!current) continue;
  const instruction = line.split('|').pop().trim();
  if (growthOnly.has(current)) { recent.push(instruction); if (recent.length > 32) recent.shift(); }
  let call = /\|\s+(call(?:_indirect)? .*)$/.exec(line)?.[1];
  if (!call) continue;
  const callee = /^call (\d+)$/.exec(call);
  if (callee && symbols.has(Number(callee[1]))) call += ` <${symbols.get(Number(callee[1]))}>`;
  // wasm32 disassembly labels imports with their module (<env.invoke_vii>).
  if (hot.has(current) && /<(?:env\.)?invoke_/.test(call)) found.get(current).push(call);
  if (growthOnly.has(current) && /<(?:env\.)?invoke_/.test(call)) {
    // Asyncify guards each call site with `if`; the first constant after it
    // is the invoke's table index (a callee not known statically fails).
    const guard = recent.lastIndexOf('if', recent.length - 2);
    const index = /^i(?:32|64)\.const (\d+)$/.exec(recent[guard + 1] ?? '')?.[1];
    const callee = index === undefined ? undefined : tableNames.get(Number(index));
    if (!callee || !growthOnly.get(current).test(callee)) found.get(current).push(`${call} -> ${callee ?? 'unknown callee'}`);
  }
  if (current === callImport) callImportLines.push(call);
}
const exitCode = await new Promise((done) => objdump.on('close', done));
if (exitCode !== 0) throw new Error(`wasm-objdump failed (${exitCode}) on ${wasm}`);

const failures = [];
for (const name of growthOnly.keys()) {
  const calls = found.get(name);
  if (!calls) failures.push(`${name}: not found in ${wasm} (renamed or fully inlined?)`);
  else if (calls.length) failures.push(`${name}: ${calls.length} invoke_* calls outside container growth:\n    ${calls.join('\n    ')}`);
}
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
console.log(`hot path invoke check passed (${hot.size + growthOnly.size} functions, call_import HLE call direct)`);
