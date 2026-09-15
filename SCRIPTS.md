# SCRIPTS.md — Wasm JIT build, test & benchmark commands

Working directory: repository root. Emscripten build tree: `build/web`
(configured with the browser/JIT test targets ON; see `browser/runtime_wasmjit.cmake`).

## Build & run the backend test (fast path, regions, SMC, memory matrix)

```sh
cmake --build build/web --target vita3k_jit_backend_test_node -j 8
node build/web/browser/vita3k_jit_backend_test_node.js
```

Compiles `vita3k/cpu/tests/wasmjit_backend_test.cpp` (which includes the backend
`wasm_jit_cpu.cpp` so it can call the checked helpers directly) to a Node
executable and runs it in Node's Wasm engine. Expected last line:
`WasmJit backend: 510 checks passed (real memory, no interpreter)`.

## Emitter fixture suite (real Dynarmic IR → Wasm modules, run in Node)

```sh
c++ -std=c++20 -O1 -Wall -Wextra -Werror \
  -Iexternal/dynarmic/src \
  -Iexternal/dynarmic/externals/mcl/include \
  -Iexternal/fmt/include -Iexternal/boost \
  vita3k/cpu/src/wasmjit/emit_wasm.cpp \
  vita3k/cpu/tests/wasmjit_emitter_test.cpp \
  build/native/external/dynarmic/src/dynarmic/libdynarmic.a \
  build/native/external/dynarmic/externals/mcl/src/libmcl.a \
  build/native/external/fmt/libfmtd.a \
  -o /tmp/wasmjit-emitter-test
/tmp/wasmjit-emitter-test /tmp/wasmjit-emitter-fixtures
node vita3k/cpu/tests/wasmjit_emitter_test.mjs /tmp/wasmjit-emitter-fixtures
```

Step 1 builds the native fixture generator (no CMake target exists for it —
recipe from `vita3k/cpu/tests/wasmjit_emitter_README.md`). Step 2 translates
real ARM/Thumb and emits `.wasm` fixtures + expected-state JSON. Step 3 executes
every module in Node. Expected: ~77 modules, ~21k cases, "Wasm execution passed".

## Exit-42 homebrew fixture (end-to-end JIT, cold start)

```sh
cmake --build build/web --target vita3k_jit_fixture_node -j 8
node build/web/browser/vita3k_jit_fixture_node.js browser/tests/vita_homebrew_fixture/eboot.bin
```

Runs the real VitaSDK eboot through the JIT. Expect `[bench] exit=42`,
`imports=23`, `missing_nids=0`.

## Interpreter instruction tests (oracle stays green)

```sh
cmake --build build/web --target vita3k_web_interpreter_tests -j 8
node build/web/browser/vita3k_web_interpreter_tests.js
```

Expected: `M3 interpreter checks passed`.

## Display-homebrew benchmark (steady-state FPS, JIT)

```sh
cmake --build build/web --target vita3k_display_bench_jit
```

Builds the ASYNCIFY Node bench module AND runs it — the target is both build
and run. The `[display-bench] JSON {...}` line contains `fps`,
`instructionsPerSec` and the JIT profile (`fast_reads`, `fast_writes`,
`slow_*` fallback-reason counters). Interpreter variant:
`--target vita3k_display_bench_interp`.

## Browser (Playwright) smokes

```sh
node browser/tests/jit_smoke.mjs                      # M14 suite in a real Worker (35 checks)
node browser/tests/jit_fixture_smoke.mjs              # exit-42 fixture through the Worker path
PLAYWRIGHT_MODULE_URL=file://$PWD/build/playwright/node_modules/playwright/index.mjs \
  node browser/tests/worker_smoke.mjs build/web/dist  # full display page, visual output
```

All three exit 0 on success. Playwright lives under `build/playwright`.

## Manual browser testing (the animated display fixture)

```sh
cmake --build build/web --target vita3k_web_jit -j 8      # rebuild the JIT module
cmake --build build/web --target vita3k_web_dist_stage    # restage build/web/dist
cd build/web/dist && python3 -m http.server 8080
```

Open <http://localhost:8080/display.html?backend=jit> for the JIT and
`display.html` (no query) for the interpreter. No COOP/COEP headers needed (no
SharedArrayBuffer). The Worker's console prints the JIT profile line ending in
`fast_reads=... fast_writes=...` — its presence proves the fast-path build.

## Debugging generated Wasm

```sh
VITA3K_DUMP_JIT=1 node <any JIT node target>
```

Dumps each generated single-block module to `/tmp/jit-module.wasm` and region
modules to `/tmp/jit-region-*.wasm` for inspection with
`new WebAssembly.Module(fs.readFileSync(...))` in Node.
