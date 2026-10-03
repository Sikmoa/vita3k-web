# Multithreaded guest execution (design)

Status: design, not implemented. The single-worker build stays the default and
the fallback throughout.

## Why

Today every guest thread runs on one Worker as an Asyncify fiber
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

Implementation: Emscripten's `PThread.unusedWorkers` / `allocateUnusedWorker` /
`loadWasmModuleToWorker` (JS library), driven by a coordinator timer and by a
counter in shared memory that pool Workers update on take/return. Boot time is
measured against the 1–10 s target; `pool_initial` shrinks if it hurts.

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

### HLE thread safety

Desktop HLE is written for host threads and locks `KernelState::mutex` and
per-object mutexes. The web-specific code is not, and is audited explicitly:

* `gxm_webgpu_bridge.cpp`: texture cache, rendered targets, scene writer;
* `display.cpp` vblank service (threaded build uses the desktop vblank thread);
* `SceAudio.cpp` pacing and `hle_audio_null.cpp`;
* `vita_app.cpp` page messages, input state, dialogs;
* `motion_browser.cpp`, lazy file staging and the HLE profile counters.

### Rendering

WebGPU stays on the coordinator. Guest threads build scenes exactly as today
(the bridge's C++ produces a word stream plus data), then hand the finished
scene to the coordinator: the scene buffer lives in shared memory, the producer
posts its descriptor (one message per scene, about 30–60 per second) and the
coordinator submits it. Queue back-pressure (`waitForCapacity`) becomes a futex
wait on a shared in-flight counter. Calls that need a JS answer (GXP program
registration, surface sync readback) are proxied to the coordinator with
`emscripten_proxy_sync`, blocking only the calling guest thread.

### Audio

Each audio port gets a shared PCM ring read directly by an AudioWorklet on the
page. `sceAudioOutOutput` blocks on ring space with `Atomics.wait`, which paces
the guest from the audio clock and removes the per-chunk `postMessage` and its
garbage.

### Files

Emscripten proxies file system calls from pthreads to the main runtime thread,
which is the coordinator, so MEMFS and the lazy staging (ranged XHR, chunk
cache) keep working unchanged. If proxying shows in profiles, the chunk cache
moves into shared memory.

### Build variants

| | single Worker (today) | threaded |
|---|---|---|
| flags | `-sASYNCIFY`, `-fexceptions` | `-pthread`, `-sSHARED_MEMORY`, no Asyncify; `-fwasm-exceptions` once Asyncify is gone |
| guest threads | `GuestThreadRuntime` fibers | desktop kernel paths |
| needs | nothing | cross-origin isolation, shared Memory64 support |

`worker.js` picks the threaded build when `crossOriginIsolated` is true and a
probe confirms shared Memory64; otherwise it loads the single-Worker build.

## Phases

Each phase ends with Limbo and Persona 4 Golden still running, compared against
the single-Worker build.

0. **Feasibility probe**: a tiny `-pthread -sMEMORY64` program with an 8 GiB
   shared memory, nested Workers from a Worker and `Atomics.wait`, in Chrome and
   Firefox; COOP/COEP in `limbo_serve.mjs`; measure Worker start and pool warm-up.
1. **Threaded target**: `vita3k_web_jit_mt` built beside the current target; the
   coordinator split in `worker.js`; runtime selection with fallback.
2. **CPU correctness**: atomic `STREX`, fences, inline mutex fast paths off,
   per-Worker region caches, shared code-page epochs, per-Worker AOT instances.
   New test: N threads incrementing one counter with `LDREX`/`STREX` loops.
3. **Kernel**: desktop thread paths with `std::thread`, the pool with background
   top-up, the HLE audit list above. Limbo boots threaded.
4. **Rendering handoff**: shared scene buffers, proxied program registration and
   readback. Persona 4 Golden reaches the field threaded.
5. **Audio and input** through shared memory and an AudioWorklet.
6. **Performance**: `-fwasm-exceptions`, pool sizing, profiles of the field.

## Risks

* Shared Memory64 support or performance differs between browsers (phase 0).
* Titles that rely on single-core priority starvation (busy-waits that only
  work when a higher priority thread cannot run concurrently).
* Races in web-specific code not covered by the audit; debugging across Workers
  is harder than on one thread.
* Duplicate lazy-JIT compilation per Worker (small while the AOT image covers
  most code).
* Memory per Worker if a title creates many threads.
