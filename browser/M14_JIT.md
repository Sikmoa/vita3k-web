# M14a: first Dynarmic-IR → WebAssembly CPU proof

Status: **working, opt-in prototype**, not a JIT-enabled homebrew release.
The existing display application still uses the unchanged `InterpreterCPU`.
Neither the native/default Dynarmic backend nor the interpreter was edited.

## Path actually executed

```text
ARM / Thumb instruction bytes in real MemState
  → Dynarmic::A32::Translate (existing decoder and translator)
  → Dynarmic::IR::Block
  → emit_block (new, deliberately limited Wasm backend)
  → WebAssembly.Module + WebAssembly.Instance in the Worker
  → exported block installed in Emscripten's function table
  → WasmJitCPU / CPUInterface → Wasm call_indirect
  → shared JitState registers / CPSR / PC
  → architectural SVC → normal ThreadState pre-import boundary
```

No ARM decoder was added. No synthetic import/HLE system was added. The
controlled thread test stops at the existing `kernel.call_import` boundary;
it is not a claim that a real VitaSDK executable runs under JIT yet.

The source for the mechanism was [WATaBoy](https://humphri.es/blog/WATaBoy/):
runtime Wasm generation, host compilation and installation in a growable
function table. Its Game Boy measurements are not Vita performance predictions.
`__ABOUT_M14.md` remains the broader design discussion.

## Implementation boundaries

- `browser/runtime_dynarmic_frontend.cmake` builds the existing A32 frontend
  and IR without native executable-memory code or native CPU backends.
  Four unused `CallHostFunction` overloads assume 64-bit function pointers;
  a checked build-local source adaptation makes them explicitly throw instead.
  The vendored Dynarmic tree is unchanged. No host function address is faked.
- `vita3k/cpu/src/wasmjit/frontend.*` provides checked `mem_fetch` callbacks,
  one tick per guest instruction, bounded translation, and full Dynarmic
  PC/T/E/IT/FPSCR location descriptors. Thumb fetches currently require the
  full aligned four-byte instruction word to be executable.
- `emit_wasm.*` emits MVP Wasm with only `env.memory` imported. The 84-byte
  `JitState` is a backend ABI, not a second kernel/memory/process model.
  Integer values use Wasm locals; register state lives in the same Wasm heap.
  Unsupported IR or terminals reject the **whole block before execution**.
- `WasmJitCPU` implements the existing `CPUInterface`. It is instantiated
  explicitly in the tests; there is no automatic interpreter fallback.
  `step()` translates at most one instruction; `run()` dispatches bounded
  blocks and stops at SVC, failure, a stop request or its instruction budget.
- Compilation crosses JavaScript. Generated execution uses a **noexcept**
  function-pointer type so Emscripten emits `call_indirect`, not its JS
  `invoke_ii` exception trampoline. Guest faults in future generated memory
  operations must be explicit return reasons, not C++ exceptions.
- Table entries are installed/cleared through `setWasmTableEntry`, keeping
  Emscripten's table mirror coherent. Slots are recycled on invalidation or
  backend destruction. The prototype cache is capped at 1024 entries.

### Cache and invalidation

Keys include the full Dynarmic location descriptor plus the translation
instruction limit. The latter keeps single-step and last-budget-slice blocks
separate. Every cache hit rechecks the source bytes with **mem_fetch** before
execution. This catches checked writes, trusted `Ptr`/HLE writes, unmapping,
remapping and execute-permission removal without modifying the oracle or
missing native write paths. Explicit `invalidate_jit_cache` invalidates all
blocks touching the specified guest pages.

This conservative entry-time validation is intentionally not the final fast
page-generation scheme. There are **no generated guest stores yet**; when
those are added, executable-page stores must also end/guard the current block
so self-modifying code cannot continue executing stale translated instructions.

### Supported versus missing

The emitter covers scalar register operations, integer add/sub and NZCV,
barrel shifts/carry, bitwise operations, condition evaluation, direct branches,
BX/interworking, and SVC. ARM conditional blocks and Thumb conditional terminals
are different Dynarmic representations, and both are tested. Full whitelist
and ABI restrictions are in
[`wasmjit_emitter_README.md`](../vita3k/cpu/tests/wasmjit_emitter_README.md).

**Not implemented:** guest load/store IR, IT blocks, full integer coverage,
VFP/NEON, exclusives, generated exceptions, tiering, async compilation,
region/module batching or block-to-block chaining. One module per block is
acceptable for this proof only. Native function-pointer escape hatches fail.

Therefore **the VitaSDK exit-42 and 600-frame fixtures do NOT run under JIT**.
They continue to pass on the original interpreter path.

## Reproduce

Use the restored repository dependencies and Emscripten toolchain:

```sh
emcmake cmake -S . -B build/web -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DVITASDK=/opt/vitasdk/vitasdk \
  -DVITA3K_WEB_JIT_TESTS=ON -DVITA3K_WEB_JIT_IR_PROBE=ON
cmake --build build/web --target vita3k_web_jit_ir_probe \
  vita3k_jit_tests_node vita3k_jit_tests vita3k_jit_bench -j1
node build/web/browser/vita3k_web_jit_ir_probe.js
node build/web/browser/vita3k_jit_tests_node.js
node build/web/browser/vita3k_jit_bench.js
```

For Chromium Worker execution:

```sh
npm install --prefix build/playwright playwright
# If a browser is not installed: use Playwright's install command first.
# Alternatively set PLAYWRIGHT_CHROMIUM_EXECUTABLE to an installed Chromium.
node browser/tests/jit_smoke.mjs build/web/browser
```

The smoke script starts and stops its own ephemeral loopback server. It does
not restart or alter the display demo's server on port 5173. A restrictive
production CSP will need to allow Wasm compilation (`wasm-unsafe-eval`).
This prototype uses unshared memory and does not require pthreads; the test
server supplies COOP/COEP headers for consistency with the other smoke tests.

## Verified results

- **54 frontend checks**, executed as Wasm under Node, including permission/
  fetch failures, limits, mode preservation and rejected native host calls.
- **50 emitted modules, 20,328 input/expected-state pairs, 40,656 actual Wasm
  call_indirect calls** at two state offsets. Strict native generator build
  and UBSan passed. Covers flag boundaries, randomized arithmetic, shift
  counts 0–256, all supported conditions, SVC/BX, determinism, canaries and
  OOB traps. Reproduction is in the emitter test README.
- Integrated Node and Chromium Worker runs:

  ```text
  M14 SUMMARY mode=emscripten-jit passed=35 failed=0 oracle_gaps=1
  M14 Worker JIT smoke passed (oracle gaps, if any, remain explicit above)
  ```

  Comparisons cover r0–r15, CPSR, FPSCR, TPIDRURO, preserved FP registers,
  SVC state and unchanged guest memory. Includes hot cache, explicit page
  invalidation, checked and trusted code writes, NX/unmap rejection, shared
  Wasm-memory growth, bounded execution, no-fallback rejection, and real
  `KernelState + ThreadState::run_loop(true)` preimport integration.
- **Oracle gap is real:** the current interpreter's ARM decoder lacks the
  SUB/CMP coverage needed by the full ARM loop. That loop is checked against
  independent architectural expectations, NOT claimed as a full differential
  pass. The corresponding Thumb loop and supported ARM cases do compare
  against the interpreter. The reference implementation was left untouched.
- Native/default Dynarmic build remains up to date; **2/2 CTests pass**.
  Native interpreter build: **6/6 CTests pass**. Existing Node M2/M3/guest/
  generated-ELF/runtime-convergence suites pass. Chromium generic ELF and
  genuine VitaSDK interpreter Worker regressions both still exit **42**.
- Active LSP probes could not confirm diagnostics for the new files;
  actual compiler/linker/executable checks above are the validation evidence.

## First measurement — not a display performance claim

`jit_bench.S` defines a tiny Thumb register-only ADD/SUB/BNE loop ending at
SVC. `jit_bench.cpp` executes the same **600,002 guest instructions** on each
backend, checks the final architectural result and compares CPU states.
Both backends link into the same optimized Node/Wasm executable, without
Asyncify. Five alternating-order samples on the current VM:

| Sample | Interpreter ms | JIT ms |
|---|---:|---:|
| First (includes cold code/cache effects) | 43.291 | 23.187 |
| Warm 1 | 36.752 | 14.051 |
| Warm 2 | 34.008 | 12.513 |
| Warm 3 | 36.275 | 12.328 |
| Warm 4 | 37.532 | 15.741 |

Warm median: **36.514 ms interpreter / 13.282 ms JIT ≈ 2.75×**
(~16.4 vs ~45.2 million guest instructions/s on this specific loop).
13 compiled entries and 999,992 cache hits across five runs, no fallback.
The module-per-block cache still checks source bytes on every entry.

These are short Node microbenchmark samples, not browser FPS predictions,
not the framebuffer workload, and not a promise of 130 MIPS. Guest memory,
HLE, compilation latency, Asyncify integration and browser engine differences
remain to be measured as coverage expands.

## Next increment

Add checked guest memory helper calls with precise failure exits, then extend
IR coverage only as genuine fixtures require it. Keep conservative invalidation
until write tracking is complete. The next whole-program target is the
VitaSDK exit-42 executable on JIT with no fallback; framebuffer checksum parity
comes after that. Keep batching/tiering design in view, but do not enable this
backend by default before those correctness milestones pass.
