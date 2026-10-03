# Multithreaded guest execution

Status: phases 0–5 implemented, phase 6 partly. The threaded runtime is
opt-in: the player loads it with `?threads=1` on a cross-origin isolated page.
The single-Worker build stays the default and the fallback. The design
sections below are the original plan; each ends with an "As built" note where
the implementation differs or adds detail.

## Using it

```
# Threaded build, staged into the same dist as the single-Worker build
.limbo_work/tools/nx 'cmake -S . -B build/web64-mt -DCMAKE_C_FLAGS=-pthread \
  -DCMAKE_CXX_FLAGS=-pthread -DVITA3K_WEB_DIST=$PWD/build/web64/dist &&
  cmake --build build/web64-mt --target vita3k_web_dist -j8'

# Shared-memory AOT images: the threaded runtime cannot link the
# single-Worker images (they import unshared memory)
VITA3K_NULL_GPU=1 VITA3K_AOT_BUILD=.limbo_work/aot-mt/<TITLE>.aot.wasm \
  node build/web64-mt/browser/vita3k_web_app_bench.js .limbo_work/stage <TITLE>

# Dev server: LIMBO_AOT_MT_DIR serves them as /aot-mt/<TITLE>.wasm
LIMBO_AOT_DIR=.limbo_work/aot LIMBO_AOT_MT_DIR=.limbo_work/aot-mt \
  node browser/tests/limbo_serve.mjs
```

Open `/?threads=1&title=<TITLE>`. `worker.js` falls back to the single-Worker
runtime when the page is not cross-origin isolated, Memory64 is missing,
`memory=w32` is forced, or `gles=1` is set, and logs why.

## Why

The single-Worker build runs every guest thread on one Worker as an Asyncify fiber
(`GuestThreadRuntime`, `GuestFiberScheduler`). That costs in two ways:

* Only one guest thread runs at a time. Persona 4 Golden's field keeps the
  main thread and two model threads busy at once (in the Velvet Room, about
  50M + 25M guest instructions per 5 s on one host core).
* Every switch is two Asyncify stack unwinds, and Asyncify forces JS-based C++
  exceptions, whose `invoke_*` wrappers allocate BigInts for 64-bit arguments
  (a large part of the garbage collection in profiles).

Desktop Vita3K runs each guest thread on its own host thread. With Emscripten
pthreads the browser can do the same: one Worker per guest thread, all sharing
one `SharedArrayBuffer` memory.

## Target architecture

```
page (main thread)
  ├─ AudioWorklet  ◄── shared PCM rings (one per audio port)
  └─ coordinator Worker  (Emscripten main runtime thread; worker.js)
       • OffscreenCanvas + the WebGPU device (gxm_scene.js)
       • file staging (lazy ranged reads), MEMFS
       • input, dialogs, page messages
       • spawns pool Workers; never runs guest code, never blocks
       └─ pool Workers (pthreads), one per live guest thread
            • ThreadState::run_loop on the desktop kernel paths
            • own JIT region cache; shared AOT image (same compiled Module)
            • block in Atomics.wait (futex) while the guest thread waits
```

### Guest threads: the desktop kernel paths

The kernel already keeps desktop behaviour wherever `KernelState::execution_host`
is null (about 70 guarded sites in `thread.cpp`, `sync_primitives.cpp`,
`SceThreadmgr.cpp`, `kernel.cpp`, `display.cpp`, `SceAudio.cpp`, `SceGxm.cpp`,
`SceProcessmgr.cpp`, `offline_socket.cpp`). Desktop creates a host thread in
`create_thread` (`kernel.cpp`, `SDL_CreateThread`) that runs
`ThreadState::run_loop`, and waits with `std::condition_variable`. Under
`-pthread`, `std::thread` and condition variables are pthreads and futexes, so
the threaded build:

* leaves `execution_host` null (no `GuestThreadRuntime`);
* replaces the one `SDL_CreateThread` with `std::thread` under `__EMSCRIPTEN__`
  (detached, as desktop does);
* keeps the desktop vblank thread and audio pacing paths, which sleep.

As built, `create_thread` keeps `SDL_CreateThread` (SDL is built with
`SDL_PTHREADS` in this variant), and `KernelState::make_cpu` gives each new
thread a `WasmJitCPU` on its allocated core number. Module start entries run
like desktop `start_module`: on their own host thread through
`run_guest_function`, then `exit_delete`; a top-level `run_loop` on the loader
would park it after the guest returns. The application itself runs on a
detached pthread (`vita3k_web_start_app`), so the coordinator stays free.

Waits block the guest thread's own Worker in `Atomics.wait`; an idle Worker
costs no CPU. Priorities become hints, as on desktop: more than three guest
threads may run at once. Desktop Vita3K runs most titles this way; if a title
depends on per-core priority starvation, a later step can cap concurrently
running guest threads at three with a counting semaphore.

### Worker pool

Creating a Worker costs 10–20 ms and must happen on the coordinator, so guest
threads never wait for one in the normal case:

* **Boot**: the coordinator starts `pool_initial` Workers (default 24) while the
  game's files are staged and the AOT image compiles. Each pooled Worker loads
  the runtime and instantiates the AOT image up front (the compiled
  `WebAssembly.Module` is posted, not recompiled), so a guest thread starts on
  a ready Worker in microseconds.
* **Reuse**: a guest thread that exits returns its Worker to the pool
  (Emscripten does this for detached pthreads); the next guest thread reuses it
  with its warmed AOT instance.
* **Background top-up**: when idle pooled Workers drop below `pool_low_water`
  (default 4), the coordinator starts `pool_step` more (default 4) on its event
  loop, up to `pool_max` (default 64). Thread creation never waits for this.
* **Exhausted pool**: `pthread_create` asks the coordinator for a new Worker and
  the new guest thread starts when it is ready (one 10–20 ms delay). This is the
  only slow path and the stats report it (`pool_misses`).

As built (`memory64_post.js`): `-sPTHREAD_POOL_SIZE=24`, and a coordinator
timer checks every 50 ms; with fewer than 4 idle Workers it allocates 4 more
through `PThread.allocateUnusedWorker` / `loadWasmModuleToWorker`, up to 64.
Busy Workers are counted from `PThread.pthreads` (Emscripten 6 has no
`runningWorkers`). `Module.vita3kPoolStats()` reports idle, busy and misses.
Every Worker receives the compiled AOT `WebAssembly.Module` and the `VITA3K_*`
options in a message posted before Emscripten's load message;
`vita3kConfigureWorkers()` updates idle Workers once staging has compiled the
AOT image. A Worker instantiates the AOT image on its first guest entry, not
when it joins the pool.

Memory per pooled Worker (JS heap, instance, region cache) is estimated at
10–20 MB, so 24–32 Workers cost a few hundred MB; the pool reports it.

### Memory

The Memory64 build already uses a fixed 8 GiB memory without growth
(`runtime_memory.cmake`), which is what shared memory needs: no growth, so JS
views never go stale. The threaded build adds `-pthread -sSHARED_MEMORY`.
Requires cross-origin isolation: `limbo_serve.mjs` sends
`Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`; hosts that cannot set headers
need a service-worker shim.

### CPU and JIT

* **Exclusive access** (`LDREX`/`STREX`, `emit_wasm.cpp`): today a reservation
  is (address, width, value) and `STREX` re-reads, compares and writes as three
  steps: correct on one thread, racy on several. The threaded lowering makes the
  write a single `i32/i64.atomic.rmw.cmpxchg` against the reserved value (the
  same value-based semantics as Dynarmic's native monitor). Barriers (`DMB`,
  `DSB`) lower to `atomic.fence`.
* **Inline mutex fast paths** (`INLINE_MUTEX.md`): disabled in the threaded
  build at first; later rewritten on atomics.
* **Lazy JIT**: region modules are instantiated into a per-Worker table, so each
  Worker keeps its own region cache and compiles what it runs. Code-page
  tracking (`g_code_pages`) and write epochs move to shared memory with atomic
  updates; a guest store into code bumps the page epoch, and other Workers
  revalidate on their next entry (the existing `version_syncs` mechanism).
* **AOT**: one compiled image, one instance per Worker. Its lookup table lives
  in shared memory; retirement (`retire_aot`) clears entries with atomic stores,
  so every Worker sees it.
* Process-global JIT state written at run time (`g_aot`, dispatch epochs, region
  slots, statistics) is audited; counters become relaxed atomics or per-Worker.

As built (`wasm_jit_cpu.cpp`, `emit_wasm.cpp`, `mem.cpp`):

* `STREX` calls the checked-memory helper with `kExclusiveAccessFlag`, which
  runs `mem_compare_exchange` (a host `__atomic_compare_exchange_n`) against
  the value `LDREX` saw; `LDREX` loads through `mem_read_exclusive`, an aligned
  atomic load. The swap is a helper call rather than an inline `cmpxchg`.
  `DMB`/`DSB`/`ISB` emit `atomic.fence`.
* Shared memory is imported with the shared flag by JIT region modules and AOT
  images; code-page flags, the AOT lookup table and write epochs use atomic
  loads and stores in emitted code.
* Dispatch epochs and versions are per core: a live guest thread owns its core
  number and only its own dispatch slice. `invalidate_jit_cache` from another
  thread sets a pending flag; the owning thread clears its caches on its next
  entry. `release_code_caches` runs on the exiting thread, because its JS
  table slots belong to its Worker.
* `run()` executes 65,536-instruction slices so stop requests and foreign code
  writes are seen; a slice return reports 0, not a guest return.
* `VITA3K_AOT_DIFF` is disabled: snapshotting all of memory is unsafe while
  other threads run.

### HLE thread safety

Desktop HLE is written for host threads and locks `KernelState::mutex` and
per-object mutexes. The web-specific code is not, and is audited explicitly:

* `gxm_webgpu_bridge.cpp`: texture cache, rendered targets, scene writer;
* `display.cpp` vblank service (threaded build uses the desktop vblank thread);
* `SceAudio.cpp` pacing and `hle_audio_null.cpp`;
* `vita_app.cpp` page messages, input state, dialogs;
* `motion_browser.cpp`, lazy file staging and the HLE profile counters.

Done: a recursive mutex serializes the GXM bridge (scene writer, caches,
submit, present, init and reports) in the threaded build; the display bridge
serializes presentation; null-audio ports keep their own scratch and lock;
input, IME and dialog state have their own short-held mutexes; import reports
take a report lock (compiled out in the single-Worker build). IME and dialog
syncs run after every import, so they check for an idle dialog before
locking: the locks cost the single-Worker build about 20% of Limbo's gameplay
speed until they did.

### Rendering

WebGPU stays on the coordinator. Guest threads build scenes exactly as today
(the bridge's C++ produces a word stream plus data), then hand the finished
scene to the coordinator: the scene buffer lives in shared memory, the producer
posts its descriptor (one message per scene, about 30–60 per second) and the
coordinator submits it. Queue back-pressure (`waitForCapacity`) becomes a futex
wait on a shared in-flight counter. Calls that need a JS answer (GXP program
registration, surface sync readback) are proxied to the coordinator with
`emscripten_proxy_sync`, blocking only the calling guest thread.

As built (`thread_bridge.cpp`, `web/thread_bridge.js`), simpler than the plan:
every browser operation is one proxied call on a dedicated
`em_proxying_queue`. The calling pthread waits in
`emscripten_proxy_sync_with_ctx`; the coordinator starts the JS work, and the
promise finishes the call later through `vita3k_web_proxy_finish`, so the
coordinator's event loop never blocks. Operations: `gxm-init`, `gxm-program`,
`gxm-submit`, `gxm-capacity`, `gxm-read`, `gxm-present`, `frame`, `audio-ring`,
`dialog`, `ime`, `ime-take`, `exit`. Scene data is copied out of shared memory
before `writeBuffer` (WebGPU rejects shared views). Calls pending for over 2 s
are logged (`[thread-bridge] ... pending`), which names the operation a
stalled guest thread waits on.

### Audio

Each audio port gets a shared PCM ring read directly by an AudioWorklet on the
page. `sceAudioOutOutput` blocks on ring space with `Atomics.wait`, which paces
the guest from the audio clock and removes the per-chunk `postMessage` and its
garbage.

As built (`hle_audio_null.cpp`, `web/audio_ring_worklet.js`): a ring is a
32-byte header (write and read frame counts, capacity, channels, rate,
generation, closed) followed by int16 frames; capacity is a power of two of at
least four buffers. The worklet resamples linearly to the context rate,
refills to half the ring after an underrun, and wakes the producer with
`Atomics.notify` on the read count. The producer waits with
`emscripten_futex_wait`, but never past one buffer behind the wall clock:
until audio plays (no user gesture yet) or if it stalls, the wall clock paces
the guest and the chunk is dropped. Rings are recycled, never freed, because a
page reader may still hold one. The worklet logs frames played, underruns and
peak level every 5 s.

Input needs no change: the coordinator's event loop is free, and
`vita3k_web_set_pad` writes the shared pad state the guest thread reads.

### Files

Emscripten proxies file system calls from pthreads to the main runtime thread,
which is the coordinator, so MEMFS and the lazy staging (ranged XHR, chunk
cache) keep working unchanged. If proxying shows in profiles, the chunk cache
moves into shared memory.

### Build variants

| | single Worker (today) | threaded |
|---|---|---|
| flags | `-sASYNCIFY`, `-fexceptions` | `-pthread`, no Asyncify, `-fwasm-exceptions` |
| build directory | `build/web64` | `build/web64-mt` (`-DCMAKE_C_FLAGS=-pthread -DCMAKE_CXX_FLAGS=-pthread`) |
| module | `wasm64/vita3k_web_jit` | `wasm64/vita3k_web_jit_mt` |
| AOT images | `.limbo_work/aot` | `.limbo_work/aot-mt` |
| guest threads | `GuestThreadRuntime` fibers | desktop kernel paths |
| needs | nothing | cross-origin isolation, shared Memory64 support |

`runtime_memory.cmake` turns on `VITA3K_WEB_THREADS` when `CMAKE_CXX_FLAGS`
contains `-pthread`: Wasm objects with and without shared-memory atomics cannot
be linked together, so the variant needs its own build directory. FFmpeg is
built with `-pthread` there. `VITA3K_WEB_DIST` is a cache path, so both
variants stage into one dist. The threaded build has no interpreter module and
no GLES adapter.

`worker.js` loads the threaded build only when asked (`threads=1`, which the
player forwards) and the page is cross-origin isolated with shared Memory64.
It loads only the AOT image made for the runtime it selected (`aotMtUrl` for
the threaded one).

## Phases

Each phase ends with Limbo and Persona 4 Golden still running, compared against
the single-Worker build.

0. **Feasibility probe** (done: Chromium, Firefox, Android Chrome; see Phase 0
   results): a tiny `-pthread -sMEMORY64` program with an 8 GiB shared memory,
   nested Workers from a Worker and `Atomics.wait`; COOP/COEP in
   `limbo_serve.mjs`; Worker start and pool warm-up measured.
1. **Threaded target** (done): `vita3k_web_jit_mt` built beside the current
   target; the coordinator split in `worker.js`; opt-in selection with fallback.
2. **CPU correctness** (done): atomic `STREX` and `LDREX`, fences, inline mutex
   fast paths off, per-Worker region caches, per-core dispatch epochs, shared
   code-page epochs, per-Worker AOT instances. `vita3k_threaded_jit_tests`: four
   host threads run guest `LDREX`/`STREX` loops to exactly 400,000 increments;
   module return, nested callbacks, restart and Worker reuse.
3. **Kernel** (done): desktop thread paths, the pool with background top-up,
   the HLE audit list above. Limbo boots and plays threaded.
4. **Rendering handoff** (done, as proxied calls rather than shared scene
   descriptors): Persona 4 Golden reaches the Velvet Room headless; the
   user reports no problems in Firefox.
5. **Audio and input** (done): shared PCM rings read by an AudioWorklet; input
   through shared state.
6. **Performance** (partly): `-fwasm-exceptions` is done (no `invoke_*`
   wrappers remain in the threaded module). Pool sizing and field profiles
   are not done yet.

## Results

Headless Chromium 153 on the real GPU (Ryzen 5 5500U, Vega 7), Limbo with its
AOT image, frames per second once gameplay starts (`LIMBO_MEASURE=1`):

| | single Worker | threaded |
|---|---|---|
| Limbo gameplay | 16.4 fps | 29.8 fps |
| Limbo loading screen | ~1 fps | ~1 fps |

The same machine in Firefox 156 (user report): Limbo threaded 27–30 fps,
against 22–24 fps single Worker the day before. Limbo's loading phase is bound
by one thread and does not speed up.

Node, null GPU, 45 s with AOT (boot checks, not a speed comparison): Limbo
127 MIPS on 11 threads; Persona 4 Golden 1,875 frames on 17 threads, no missing
imports.

## Known issues

* Headless scripted Persona 4 Golden runs hang threaded when an in-game movie
  (`P4CTOP3.mp4`) plays during the input script: the main thread waits in
  `sceGxmDisplayQueueAddEntry` and the display queue thread never returns from
  `sceDisplayWaitVblankStart`, with no coordinator call pending. Not seen in
  Firefox; not diagnosed.
* Stale staged files in browser storage show up as
  `sceGxmShaderPatcherCreateVertexProgram`/`CreateFragmentProgram` returning
  `INVALID_POINTER` near the end of Limbo's loading screen; the threaded build
  then traps in `gxmSetUniformBuffers`. A private window (or clearing site
  data) fixes it. The manifest's version check should have caught this and
  did not.
* IME and dialog idle checks read their state before locking; the read is a
  hint, rechecked under the lock.
* The threaded build has no GLES adapter; `gles=1` selects the single Worker.
* Some `vita3k_jit_backend_test_node` cases still expect IR that commit
  `8d7cfa0c` implemented (vector FP compare guards) and fail; seen in the threaded
  build, unrelated to threading.

## Phase 0 results

`browser/threads_probe/` (target `vita3k_threads_probe`, served as
`/threads-probe/` by `limbo_serve.mjs`, which now sends COOP/COEP on every
response): the emulator's memory settings plus `-pthread`, a 24-Worker pool,
run from a dedicated Worker like the coordinator.

| | Chromium 153, Linux (Ryzen 5 5500U) | Firefox 156, Linux (same) | Chrome 154, Android 10 (8 cores) |
|---|---|---|---|
| cross-origin isolated, 8 GiB shared Memory64 | yes | yes | yes |
| store at 0x1ffffffff seen by another thread | yes | yes | yes |
| module + 24 Workers ready | 179 ms | 105 ms | 261 ms |
| thread start from the pool (median / max) | 0.06 / 0.36 ms | 0.02 / 0.40 ms | 0.10 / 0.46 ms |
| futex wake/wait round trip | 9 µs | 15 µs | 38 µs |
| CAS, 4 threads x 500k, 32 and 64 bit | exact, 337 ms | exact, 289 ms | exact, 438 ms |
| 40 threads at once, pool of 24 (median / max) | 1.1 / 96 ms | 0.4 / 45 ms | 1.5 / 221 ms |
| background top-up of 8 Workers | 63 ms | 32 ms | 69 ms |

The probe module is small; the emulator's Workers also instantiate the large
runtime and AOT image, which phase 1 measures.

## Risks

* Shared Memory64 support or performance differs between browsers (phase 0).
* Titles that rely on single-core priority starvation (busy-waits that only
  work when a higher priority thread cannot run concurrently).
* Races in web-specific code not covered by the audit; debugging across Workers
  is harder than on one thread.
* Duplicate lazy-JIT compilation per Worker (small while the AOT image covers
  most code).
* Memory per Worker if a title creates many threads.
