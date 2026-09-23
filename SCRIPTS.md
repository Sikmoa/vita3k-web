# SCRIPTS.md — Wasm JIT build, test & benchmark commands

Working directory: repository root. Configured Emscripten trees are `build/web`
(wasm32) and `build/web64` (Memory64); see `browser/runtime_wasmjit.cmake`.
The `EM_CACHE` paths below are specific to this development host: its system
cache supplies wasm32 libraries, while `.vscratch/emcache` supplies wasm64.

## Build & run the backend test (fast path, regions, SMC, memory matrix)

```sh
env EM_CACHE=/usr/share/emscripten/cache cmake --build build/web --target vita3k_jit_backend_test_node -j2
node build/web/browser/vita3k_jit_backend_test_node.js
```

Compiles `vita3k/cpu/tests/wasmjit_backend_test.cpp` (which includes the backend
`wasm_jit_cpu.cpp` so it can call the checked helpers directly) to a Node
executable and runs it in Node's Wasm engine. Expected last line:
`WasmJit backend: <N> checks passed (real memory, no interpreter)` (currently
over 13 million checks; the count grows with coverage). Includes the direct-
emitter P/K/PK x fast-bases matrix and inline lock/unlock tests;
also run with `VITA3K_WASMJIT_PROMOTE_FLAGS=0` to cover the reference
process default, and with `VITA3K_WASMJIT_SLOW_REASONS=1` to cover the
diagnostic slow-reason shape.

### Memory64 backend and cooperative-runtime suites (Chromium)

The local Node 22 cannot instantiate these Memory64 executables. Use the
classic-executable Chromium runner instead of treating that as a test failure:

```sh
env EM_CACHE=/home/user/.vscratch/emcache cmake --build build/web64 \
  --target vita3k_jit_backend_test_node vita3k_guest_thread_tests -j2
export PLAYWRIGHT_MODULE_URL=file://$PWD/build/playwright/node_modules/playwright/index.mjs
node browser/tests/wasm_executable_chromium.mjs vita3k_jit_backend_test_node 'WasmJit backend:'
node browser/tests/wasm_executable_chromium.mjs vita3k_guest_thread_tests \
  'Guest thread exceptions: diagnostics, failure accounting and clean teardown passed'
```

The runner checks aborts, page errors, exit status and the supplied success
marker. `WASM_TEST_DIST` overrides `build/web64/browser`. For wasm32, build the
same two targets in `build/web` with the system cache and run their `.js` files
in Node. The runtime suite includes real mutex park/wake, cancellation,
timeouts, deletion/reuse, multiple dirty-owner commits and exception teardown.
The inline-mutex regression suites intentionally require acceleration enabled;
use the retail ablation below for a disabled-path performance comparison.

## Emitter fixture suite (real Dynarmic IR → Wasm modules, run in Node)

```sh
c++ -std=c++20 -O1 -Wall -Wextra -Werror \
  -Ivita3k/cpu/include -Ivita3k/mem/include \
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

## Audio audibility probe (square-wave homebrew through the Worker path)

```sh
cmake -S browser/tests/vita_audio_fixture -B build/vita-audio-fixture \
  -DVITASDK=/opt/vitasdk/vitasdk
cmake --build build/vita-audio-fixture --target vita3k_vita_audio_fixture
PLAYWRIGHT_MODULE_URL=file://$PWD/build/playwright/node_modules/playwright/index.mjs \
  node browser/tests/audio_fixture_chromium.mjs
```

Builds a genuine VitaSDK homebrew that outputs 40 full-scale 440 Hz stereo
buffers through production `sceAudioOut` HLE, runs it through the wasm64
Worker (`run-vita`), and asserts exit code 7 with 40 chunks at peak 16000.
Expected last line: `AUDIO PATH AUDIBLE: 40 full-scale chunks through
HLE->worker->page`. A passing probe with silent retail PCM means the game
emits silence (e.g. still on a loading screen with no input), not that the
page drops sound. The headless Limbo harness (`limbo_app_chromium.mjs`)
reports aggregate PCM `audio` (chunks/bytes/peak/nonzero/freqs) for the same
separation, and the dev page shows a rolling `peak=` in its stats line.

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
env EM_CACHE=/home/user/.vscratch/emcache cmake --build build/web64 --target vita3k_web_jit -j2
HOST=0.0.0.0 PORT=5173 node browser/tests/limbo_serve.mjs
```

Open the printed URL (`HOST=0.0.0.0` lists the machine's addresses; add
`?auto=1` to start on load). The page stages `.limbo_work/stage`
(`LIMBO_STAGE`, `LIMBO_TITLE`, `LIMBO_APP`), boots the retail app through the
same Worker messages the headless probe uses, and draws every presented frame
to a canvas next to a live guest log. Query parameters: `?memory=w64|w32|auto`
(auto probes Memory64 and falls back), `?backend=jit|interp`, `?inlineMutex=0`
(disables the default-on inline lock/unlock paths for comparison). Start a new
Worker/reload to change the selection. After changing the page's inline HTML,
restart the **Node dev server**: it constructs that HTML once at startup.
Static worker/JS/Wasm assets are read from disk per request.

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
at least one presented frame; `LIMBO_DEADLINE_MS` bounds the observation window,
not the emulated game). Inspect `exit`, `timedOut` and error arrays separately;
one presented frame is not proof of a clean guest exit.

For a sequential inline-mutex comparison using the same binary:

```sh
for enabled in 1 0; do
  env PLAYWRIGHT_MODULE_URL=file://$PWD/build/playwright/node_modules/playwright/index.mjs \
    LIMBO_INLINE_MUTEX=$enabled LIMBO_DEADLINE_MS=180000 LIMBO_FRAME_EVERY=10 \
    LIMBO_FRAME_OUT=.limbo_work/inline-$enabled \
    node browser/tests/limbo_app_chromium.mjs > .limbo_work/inline-$enabled.log 2>&1
done
```

The JSON retains full-run draw counts (not just a truncated log tail), the
latest live progress/per-thread profiles and first-frame `sinceRunMs`, excluding
staging time. Raw assets/images/logs stay in ignored `.limbo_work/`. See
[`INLINE_MUTEX.md`](vita3k/cpu/src/wasmjit/INLINE_MUTEX.md) for the coherence
contract, regression coverage and the initial measured comparison. Do not infer
per-import nanoseconds or steady FPS from cumulative MIPS/frame counts.

## Debugging generated Wasm

```sh
VITA3K_DUMP_JIT=1 node <any JIT node target>
```

Dumps each generated single-block module to `/tmp/jit-module.wasm` and region
modules to `/tmp/jit-region-*.wasm` for inspection with
`new WebAssembly.Module(fs.readFileSync(...))` in Node.
