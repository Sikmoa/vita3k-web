# Vita3K browser interpreter benchmark report

Measured 2026-09-13 on the port host (1 CPU, 1 GB RAM, shared with the
session daemons; Emscripten 3.1.69, Node 22). All binaries were run via
`nohup`/`timeout` with exact recorded PIDs and polled — never killed by name.

## Binaries

| Build | Wasm | Provenance |
| --- | --- | --- |
| Debug | 33,071,119 B (`vita3k_web_bench.wasm`, 2026-09-13 12:13) | Pre-Release-reconfigure build; contains `.debug_*` and `name` custom sections (unoptimized, `-g`). Snapshotted to `/tmp/vita3k-bench-debug/` before the parent's Release rebuild replaced it. |
| Optimized | 1,506,782 B (`vita3k_web_bench.wasm`, 2026-09-13 13:45) | Parent's Release reconfigure (`-DCMAKE_BUILD_TYPE=Release`), objects `-O3 -DNDEBUG`, link flags `-O3 -DNDEBUG` (see proposal below for the `-O2` nuance). Snapshotted to `/tmp/vita3k-bench-release/`. |

Both are the `vita3k_web_bench` target (Node, `-sNODERAWFS=1`, deliberately
without ASYNCIFY) over `browser/tests/vita_homebrew_fixture/eboot.bin`
(genuine VitaSDK exit-42 fixture) and, for the steady-state probe,
`build/web/browser/tests/vita_display_fixture/eboot.bin`.

## Methodology

- Command: `node <snapshot>/vita3k_web_bench.js <eboot.bin>`; numbers from
  the `[vita3k-web] Vita benchmark: instructions=… elapsed_ms=…` stdout line.
- Clean-machine protocol: runs started only when no `ninja`/`cmake --build`
  processes were active. One debug run was contaminated by a concurrent
  parent build (`-j2` on the same single CPU) and is discarded but recorded
  below for transparency.
- The exit-42 fixture executes only **27,472 guest instructions**, so its
  `instructions/elapsed` is dominated by fixed startup (Wasm instantiation +
  SELF load + HLE init), not interpreter throughput. The display-fixture
  probe below measures true steady-state throughput.

## Results

| Configuration | Fixture | instructions | elapsed_ms | instr/sec |
| --- | --- | --- | --- | --- |
| Debug (clean) | exit-42 | 27,472 | 16,580 | **1,657** |
| Debug (contaminated; discarded) | exit-42 | 27,472 | 48,935 | 561 |
| Optimized (this run) | exit-42 | 27,472 | 290 | **94,731** |
| Optimized (parent's run) | exit-42 | 27,472 | 259 | 106,031 |
| Optimized, steady-state | display fixture (dies in frame 0) | 31,895,864 | 5,060 | **6,304,500** |

- **Debug → Optimized speedup (same host, clean runs): ≈ 57×** (94,731 /
  1,657), consistent with the parent's reported ~55× (106,031 / 1,918).
- Debug steady-state was not directly measurable: the same display-fixture
  probe would need ~5.3 h at debug speed.
- Steady-state ≈ **6.3M instr/s** (Release, Node, no ASYNCIFY). The browser
  target adds ASYNCIFY instrumentation and will be somewhat slower.

### Frame-cost arithmetic (for the display e2e timeout) — SUPERSEDED

> **Update (post-fix):** the issues below were fixed. The interpreter now
> implements the full bitfield/extend/reverse family (GAS-verified decoders,
> 258 focused assertions), and the fixture was redesigned for interpreter
> speed: the gradient background renders **once** at startup and each frame
> redraws only the changed regions (~**1.6M instr/frame** ≈ 0.3 s ≈ 3 fps on
> the optimized interpreter; 600 frames ≈ 3 minutes). The default smoke run
> completed all 600 frames with 600 distinct checksums and exit 77 in ~181 s
> (measured, headless Chromium). The historical numbers are kept for the
> record of why the redesign was needed.

The display fixture's frame 0 (gradient pass, 960×544 CPU-written pixels)
costs ~31.9M guest instructions before it reaches the rectangle pass — call
it **~32M instr/frame**. At 6.3M instr/s that is ~5 s/frame in Node; the
default 600-frame fixture (`VITA_DISPLAY_FRAME_COUNT`) is ~19.2G instructions
≈ **51 minutes** — far beyond the display smoke test's 10-minute default.

Recommendation for the parent (fixture recipe is parent-owned): stage the
browser fixture built with a reduced frame count, e.g.
`-DVITA_DISPLAY_FRAME_COUNT=60` (~5 min at current speed), or plan to run
`browser/tests/display_smoke.mjs` with `DISPLAY_SMOKE_TIMEOUT_MS` raised
(env override is implemented). The smoke test itself only requires ≥5 frames,
≥3 distinct checksums, and an incrementing frame counter.

### Display-fixture probe findings (parent scope, for reference)

- All display imports resolve (`missing_nids=0`) — SceDisplay/SceDisplayUser
  HLE is wired in the current runtime.
- The guest dies inside `render_frame`: the Release build stops at
  **unsupported Thumb32 `ubfx r3, r3, #0, #9`** (opcode `0xf3c30308`, PC
  `0x8100027a` = `render_frame+0x64`, the `& (RECT_X_WRAP-1)` rectangle
  x computation). The stale dist build instead exhausts its instruction
  budget in the same function. The fixture binary also references
  `uxtb`(17), `sxth`(15), `clz`(11), `rev`(6), `ubfx`(5), `sxtb`(1), `bfc`(1)
  — the interpreter needs this bitfield/extend/reverse family before the fixture can
  complete a frame, let alone 600.
  *(Fixed: all of these are implemented and covered by focused tests now; the
  fixture runs to completion.)*

## Proposal: optimization configuration for `browser/CMakeLists.txt`

**Not applied** — `browser/CMakeLists.txt` is parent-owned. Current state:

```cmake
if(NOT CMAKE_BUILD_TYPE AND EMSCRIPTEN)
    set(CMAKE_BUILD_TYPE Release)          # CMAKE_CXX_FLAGS_RELEASE = "-O3 -DNDEBUG"
endif()
add_compile_options($<$<CONFIG:Release>:-O2> $<$<CONFIG:RelWithDebInfo>:-O2> $<$<CONFIG:MinSizeRel>:-Os>)
```

Observed effect: `add_compile_options` only applies to targets defined
**after** it. `vita3k_web_bench` is defined above the line, so bench objects
compile `-O3 -DNDEBUG`, while `vita3k_web` (defined below) compiles
`-O3 -DNDEBUG … -O2` → effective `-O2` (last flag wins). So today's bench
numbers above measure the **-O3** configuration, not the browser target's
actual `-O2`.

1. **Make `-O2` uniform (recommended, 2-line move):** move the
   `add_compile_options(...)` line **above** every Emscripten target
   definition (or instead set
   `set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")` once). Then bench and
   browser target share one optimization level and benchmark numbers are
   representative of what ships. A dedicated measurement of -O2 vs -O3 was
   not taken (would need a second full build on this 1-CPU host); in
   Emscripten the difference is typically small and -O2 keeps the Wasm
   smaller.
2. **Keep `Release` as the default** (`if(NOT CMAKE_BUILD_TYPE …)`); keep a
   second build directory configured `-DCMAKE_BUILD_TYPE=Debug` for
   assertions + DWARF triage, and consider `RelWithDebInfo`
   (`-O2 -g -DNDEBUG`) when optimized crash backtraces are wanted — debug
   info roughly 20×-sizes the Wasm (33 MB vs 1.5 MB).
3. **`-Os` vs `-O2`:** `-Os` shrinks the Wasm further (~5-15% typical
   runtime cost on CPU-bound interpreter loops). The interpreter is the hot
   path — prefer `-O2` for the runtime; `-Os` only if download size ever
   matters more than throughput.
4. **`NDEBUG` (asserts off) is correct for Release:** per-instruction and
   per-import asserts in the interpreter hot path are measurable at 6M
   instr/s. Correctness is covered by the native and Wasm test suites
   (interpreter/loader/memory/guest/generated-ELF tests); use the Debug
   build dir when diagnosing.
5. **ASYNCIFY is enabled on `vita3k_web`** (cooperative vblank/kernel
   waits) and instruments the whole module; expect the browser target to
   run below the bench's 6.3M instr/s. The bench intentionally omits it —
   keep that so it measures pure interpreter throughput. Once yield sites
   are known, `-sASYNCIFY_IGNORE_INDIRECT` and an `ASYNCIFY_ONLY` list can
   shrink the instrumented surface.
6. Optional future experiments (not applied): `-flto` for cross-TU inlining
   into the decoder loop; Binaryen link-level opts are already active via
   the `-O3` link flags (wasm-opt pass).

## Reproduction

```sh
# Debug (12:13 snapshot)
node /tmp/vita3k-bench-debug/vita3k_web_bench.js browser/tests/vita_homebrew_fixture/eboot.bin
# Optimized (13:45 snapshot)
node /tmp/vita3k-bench-release/vita3k_web_bench.js browser/tests/vita_homebrew_fixture/eboot.bin
# Steady-state probe (dies at ubfx in frame 0, after ~31.9M instructions)
timeout 200 node /tmp/vita3k-bench-release/vita3k_web_bench.js \
  build/web/browser/tests/vita_display_fixture/eboot.bin
```
