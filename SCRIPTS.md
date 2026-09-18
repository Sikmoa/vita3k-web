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
`WasmJit backend: 9157 checks passed (real memory, no interpreter)`
(8-mode direct-emitter matrix: P/K/PK flags x fast-bases guard shape;
also run with `VITA3K_WASMJIT_PROMOTE_FLAGS=0` to cover the reference
process default, and with `VITA3K_WASMJIT_SLOW_REASONS=1` to cover the
diagnostic slow-reason shape).

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
every module in Node. Expected: 378 reference + 1146 candidate modules,
~59k cases, "Wasm execution passed" (P/K/PK x fast-bases variants plus
the shifts_imm immediate-count suite; counts grow with coverage).

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

For CPU-throughput measurement (not vblank-capped FPS), drive the module
directly with the fast-vblank headroom mode:
```sh
VITA3K_FAST_VBLANK=1 node browser/tests/display_bench_node.mjs \
  build/web/browser/vita3k_display_bench_jit_node.js \
  build/web/browser/tests/vita_display_fixture/eboot-short.bin jit
```
Guest work is identical (60 frames, 156399069 instructions, exit 77);
steady `instructionsPerSec` (~330 MIPS Node) and the `run_js_ms` profile
field (pure Wasm dispatch time) are the CPU signals. The box is noisy
(±15% run-to-run): stage each variant under its own directory (the
Emscripten glue hardcodes the `.wasm` filename), run A/B interleaved
with alternating order and `nice -n -15`, compare medians over ≥8
samples each, and require consistent pair agreement for large claims.

## CPU profiling the JIT bench (V8 sampling profiler, no code changes)

```sh
node --cpu-prof --cpu-prof-dir=/tmp/prof browser/tests/display_bench_node.mjs \
  build/web/browser/vita3k_display_bench_jit_node.js \
  build/web/browser/tests/vita_display_fixture/eboot-short.bin jit
```

Writes `/tmp/prof/CPU.*.cpuprofile` — opens in Chrome DevTools (Performance →
Load profile) or VS Code. Useful because each generated region module is a
dynamically compiled script with its own `wasm://wasm/<hash>` URL, so guest
code time is separable from host C++ time. The host module is a minified
Emscripten build without a Wasm name section, so frames appear as anonymous
`wasm-function[N]`; attribute them with:

```sh
wasm-objdump -d build/web/browser/vita3k_display_bench_jit_node.wasm > /tmp/host.dis
```

then map each hot function index (`func[N]`) to its code range and fingerprint
it by its `i32.const`/load/store mix (JitState field offsets, page masks).
Baseline shape (2026-09-15, light-dispatch build): host module 44% self time
(run loop 17.7%, state movers ~11%, region lookup 3.8%), generated region
modules 24%, Wasm↔JS trampolines 7%, timers 6%, idle (ASYNCIFY vblank) 4.5%.

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

## Retail app (Limbo) in a browser

```sh
env EM_CACHE=/home/user/.vscratch/emcache cmake --build build/web64 --target vita3k_web_jit -j$(nproc)
HOST=0.0.0.0 PORT=8099 node browser/tests/limbo_serve.mjs
```

Open the printed URL (`HOST=0.0.0.0` lists the machine's addresses; add
`?auto=1` to start on load). The page stages `.limbo_work/stage`
(`LIMBO_STAGE`, `LIMBO_TITLE`, `LIMBO_APP`), boots the retail app through the
same Worker messages the headless probe uses, and draws every presented frame
to a canvas next to a live guest log. Query parameters: `?memory=w64|w32|auto`
(auto probes Memory64 and falls back), `?backend=jit|interp`.

**WebGPU needs a secure origin.** `navigator.gpu` is exposed only to secure
contexts, so a page served over plain HTTP from a non-loopback address reaches
`Vita import #NNNN` lines and then fails at the first draw with "WebGPU
unavailable". Either put a TLS reverse proxy (e.g. Caddy) in front and open
`https://<name>/`, or forward the port and open `http://localhost:<PORT>/`
(loopback is a secure context). The page and the bridge both name this reason
when it applies. A Chromium without a usable GPU additionally needs
`--enable-unsafe-webgpu --enable-unsafe-swiftshader`. The wasm32 fallback is a
separate target (`vita3k_web`) and only presents frames when it was built from
the same tree as the wasm64 module.

Headless equivalent that mirrors frames into the workspace as they arrive (use
it when the page's port is not reachable from this machine):

```sh
PLAYWRIGHT_MODULE_URL=file://$PWD/build/playwright/node_modules/playwright/index.mjs \
  LIMBO_DEADLINE_MS=300000 node browser/tests/limbo_watch.mjs      # -> .limbo_work/live/
```

`latest.png` is rewritten on every poll and each new generation is also kept as
`frame_NNNNN.png`; `status.json` carries the current status, frame count and the
guest log. The headless browser uses SwiftShader (`--use-angle=swiftshader`,
`--enable-unsafe-webgpu`); set `LIMBO_GPU=1` on a machine with a real GPU.

`browser/tests/limbo_app_chromium.mjs` is the assertion probe (exit 0 requires
at least one presented frame; `LIMBO_DEADLINE_MS` bounds the run).

## Debugging generated Wasm

```sh
VITA3K_DUMP_JIT=1 node <any JIT node target>
```

Dumps each generated single-block module to `/tmp/jit-module.wasm` and region
modules to `/tmp/jit-region-*.wasm` for inspection with
`new WebAssembly.Module(fs.readFileSync(...))` in Node.
