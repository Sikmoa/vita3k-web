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
cmake -S external/dynarmic -B build/native-dynarmic -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDYNARMIC_FRONTENDS=A32 -DDYNARMIC_TESTS=OFF -DDYNARMIC_USE_BUNDLED_EXTERNALS=ON
cmake --build build/native-dynarmic
c++ -std=c++20 -O1 -Wall -Wextra -Werror \
  -Ivita3k/cpu/include -Ivita3k/mem/include \
  -Iexternal/dynarmic/src \
  -Iexternal/dynarmic/externals/mcl/include \
  -Iexternal/dynarmic/externals/fmt/include -Iexternal/boost \
  vita3k/cpu/src/wasmjit/emit_wasm.cpp \
  vita3k/cpu/tests/wasmjit_emitter_test.cpp \
  build/native-dynarmic/src/dynarmic/libdynarmic.a \
  build/native-dynarmic/externals/mcl/src/libmcl.a \
  build/native-dynarmic/externals/fmt/libfmt.a \
  build/native-dynarmic/externals/zydis/libZydis.a \
  build/native-dynarmic/externals/zydis/zycore/libZycore.a \
  -o /tmp/wasmjit-emitter-test
/tmp/wasmjit-emitter-test /tmp/wasmjit-emitter-fixtures
node vita3k/cpu/tests/wasmjit_emitter_test.mjs /tmp/wasmjit-emitter-fixtures
```

The first two commands build a standalone native Dynarmic (with its x64
backend, which runs the vitaslop cases as the oracle for the flags their
goldens do not record) and its bundled fmt; the headers must be that fmt
(`external/fmt` is a different version and does not link). Then the native fixture generator is built (no CMake target
exists for it; see `vita3k/cpu/tests/wasmjit_emitter_README.md`), translates
real ARM/Thumb into `.wasm` fixtures with expected-state JSON, and Node executes
every module. Expected last line: `Wasm execution passed: ...`.

## Audio audibility probe (square-wave homebrew through the Worker path)

```sh
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
PLAYWRIGHT_MODULE_URL=file://$PWD/build/playwright/node_modules/playwright/index.mjs \
  node browser/tests/dist_chromium.mjs build/web64/dist  # dist served as static files
```

All exit 0 on success. Playwright lives under `build/playwright`.
`dist_chromium.mjs` checks a deployment: the Worker, asked for the dist's memory
model (`DIST_MEMORY=w32` for a wasm32 build's dist), must load its module, and
GXM must initialise from `dist/shaders/`.

## Manual browser testing (the animated display fixture)

```sh
cmake --build build/web --target vita3k_web_dist    # modules + web files -> build/web/dist
cd build/web/dist && python3 -m http.server 8080
```

`vita3k_web_dist` (also part of the default build) produces the complete
deployable directory; serve it as-is.

Open <http://localhost:8080/display.html?backend=jit> for the JIT and
`display.html` (no query) for the interpreter. No COOP/COEP headers needed (no
SharedArrayBuffer). The Worker's console prints the JIT profile line ending in
`fast_reads=... fast_writes=...` — its presence proves the fast-path build.

## Retail app (Limbo) in a browser

```sh
cmake --build build/web64 --target vita3k_web_dist
HOST=0.0.0.0 PORT=5173 node browser/tests/limbo_serve.mjs
```

Open the printed URL (add `?auto=1` to start on load). The page stages
`.limbo_work/stage` (`LIMBO_STAGE`, `LIMBO_TITLE`, `LIMBO_APP`) into the Worker
and boots the app with the same Worker messages the headless probe uses. The
runtime files come from `browser/tests/runtime_routes.mjs`, shared with the
probes: `browser/web` from source, everything else from the built dist
`GXM_RUNTIME_DIST` (default `build/web64/dist`), including the shader
toolchain in `dist/shaders/` (GXP compiler, Naga, WASI shim; configuring the
build runs `npm ci` on `browser/shaders/package-lock.json`, so it needs npm and
the registry or an npm cache).
`LIMBO_AOT=<file>` supplies an AOT module (below). The option list is the
header of `limbo_serve.mjs`; the ones that change what is measured:

- vblank runs at real 60 Hz by default; `?fastvblank=1` free-runs it (headroom,
  not real-time speed), `?fpsHack=1` is Vita3K's fps-hack (display waits use
  one vblank).
- `?scale=N` sets the internal resolution (1-4, default 2).
- `?surfaceSync=1` reads rendered surfaces back into guest memory (see
  `browser/tests/GXM_WEBGPU.md`); off by default.
- `?memory=w64|w32|auto`, `?backend=jit|interp`, `?inlineMutex=0`.
- `?present=readback` reads every frame back to a page canvas (for
  `limbo_watch.mjs`); by default the Worker presents to a transferred
  OffscreenCanvas.

A guest message dialog (`sceMsgDialog`) is drawn over the screen and answered
with the keys; a guest on-screen keyboard (`SceIme`) is a text field over the
screen (Enter presses its enter key, Escape closes it). Restart the Node server after editing its inline page or the staged content;
runtime files are read per request.

Rendering: the runtime encodes each GXM command list as a GXS1 scene stream
(`browser/src/gxm_webgpu_bridge.cpp`) that `browser/web/gxm_scene.js` executes
in the Worker. Render targets stay on the GPU and are presented from there; see
`browser/tests/GXM_WEBGPU.md`.

**WebGPU needs a secure origin.** `navigator.gpu` exists only in secure
contexts: serve through a TLS reverse proxy or open `http://localhost:<PORT>/`
over a forwarded port. The page names this reason when it applies. A Chromium
without a usable GPU needs `--enable-unsafe-webgpu --enable-unsafe-swiftshader`.

`browser/tests/limbo_watch.mjs` runs the same page headless and mirrors frames
to `.limbo_work/live/` (`latest.png`, `frame_NNNNN.png`, `status.json`).

### Headless probe

```sh
PLAYWRIGHT_MODULE_URL=file://$PWD/build/playwright/node_modules/playwright/index.mjs \
  LIMBO_DEADLINE_MS=90000 node browser/tests/limbo_app_chromium.mjs
```

Exit 0 requires at least one presented frame; `LIMBO_DEADLINE_MS` bounds the
observation window. The JSON reports frames, exit and errors separately, plus
the renderer's `gxmSceneStats`, `gxmFailures` and `gxmSkips`. The option list
is the file header; commonly used:

- `LIMBO_GPU=1 LIMBO_HEADED=1` (with `PLAYWRIGHT_CHROMIUM_EXECUTABLE`) uses the
  hardware adapter; headless Chrome only has SwiftShader.
- `LIMBO_FAST_VBLANK=0` paces vblank at 60 Hz (the probe free-runs by default).
- `LIMBO_FPS_HACK=1`, `LIMBO_SCALE=N`, `LIMBO_SURFACE_SYNC=1`: as `?fpsHack=1`,
  `?scale=N`, `?surfaceSync=1`.
- `LIMBO_DIALOG=cross|circle|none` answers every guest message dialog
  (default `cross`); `LIMBO_IME=<text>` types the text into every guest
  on-screen keyboard and presses Enter (unset: left open).
- `LIMBO_TEXTURE_VERIFY=1` checks cached textures and vertex streams against
  guest memory and logs writes the tracking missed (`[gxm-verify]`).
- `LIMBO_INPUT="<ms>:<input>[+<input>]:<hold ms>,..."` scripts pad input.
- `LIMBO_MEASURE=1` runs the fixed loading/gameplay measurement scenario with a
  Worker CPU profile per window; `LIMBO_PROFILE=0` keeps the windows without the
  profiler, `LIMBO_PROFILE=alloc` samples allocations instead;
  `LIMBO_PROFILE_OUT=<prefix>` writes the raw profiles.
- `LIMBO_LOG_OUT=<file>` keeps the worker log tail; `LIMBO_INLINE_MUTEX=0` is
  the inline-mutex ablation (see
  [`INLINE_MUTEX.md`](vita3k/cpu/src/wasmjit/INLINE_MUTEX.md)).

Raw assets, frames and logs stay in the ignored `.limbo_work/`.

### Page-bridge and offline-network fixtures

```sh
cmake --build build/web64 --target vita3k_web_dist vita3k_vita_msg_dialog_fixture \
  vita3k_vita_ime_fixture vita3k_vita_net_offline_fixture
node browser/tests/msg_dialog_chromium.mjs   # sceMsgDialog: probe answers and page keys
node browser/tests/ime_chromium.mjs          # SceIme: LIMBO_IME and the page text field
node browser/tests/net_offline_chromium.mjs  # SceNet/SceNetCtl on the offline stack
```

Each stages a genuine VitaSDK fixture as an app (with `PLAYWRIGHT_MODULE_URL`
as above) and checks its exit code. The browser has no host sockets: SceNet
runs on `vita3k/net/include/net/offline_socket.h`, a Vita without a
connection (loopback only, no route, no DNS). SceRegMgr reads come from the
staged firmware's `os0/kd/registry.db0`; without it they return 0, as desktop
Vita3K does without firmware.

## Ahead-of-time module (AOT) for a retail app

The Node app bench (`vita3k_web_app_bench`, no GPU: `VITA3K_NULL_GPU=1`) loads
the app like the browser does and builds, records seeds for, or runs an AOT
module. Format and policy: [`AOT.md`](vita3k/cpu/src/wasmjit/AOT.md).

```sh
cmake --build build/web64 --target vita3k_web_app_bench
mkdir -p .limbo_work/aot
# 1. Record the blocks the lazy JIT executes, driving the title into gameplay.
VITA3K_NULL_GPU=1 VITA3K_BENCH_SECONDS=120 VITA3K_AOT_SEEDS_OUT=.limbo_work/aot/seeds.txt \
  VITA3K_BENCH_INPUT="30000:cross:200,40000:lstick-right:5000" \
  node build/web64/browser/vita3k_web_app_bench.js .limbo_work/stage PCSE00268
# 2. Build the module from the loaded modules plus those seeds.
VITA3K_NULL_GPU=1 VITA3K_AOT_BUILD=.limbo_work/aot/limbo.aot.wasm VITA3K_AOT_SEEDS=.limbo_work/aot/seeds.txt \
  node build/web64/browser/vita3k_web_app_bench.js .limbo_work/stage PCSE00268
# 3. Use it in Node (VITA3K_AOT), or in the probe and the dev server (LIMBO_AOT).
VITA3K_NULL_GPU=1 VITA3K_AOT=.limbo_work/aot/limbo.aot.wasm \
  node build/web64/browser/vita3k_web_app_bench.js .limbo_work/stage PCSE00268
LIMBO_AOT=.limbo_work/aot/limbo.aot.wasm node browser/tests/limbo_serve.mjs
```

`VITA3K_BENCH_INPUT` uses the probe's `LIMBO_INPUT` syntax. Every run logs
`AOT on`, `off` or `REJECTED` with the reason: a module built from other game
code or an older AOT format version is refused and has to be rebuilt.

## Debugging generated Wasm

```sh
VITA3K_DUMP_JIT=1 node <any JIT node target>
```

Dumps each generated single-block module to `/tmp/jit-module.wasm` and region
modules to `/tmp/jit-region-*.wasm` for inspection with
`new WebAssembly.Module(fs.readFileSync(...))` in Node.
