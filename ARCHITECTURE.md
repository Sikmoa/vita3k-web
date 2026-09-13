# Vita3K WebAssembly Port — M0 Architecture Map

This document records the first repository survey for the browser port. It is deliberately a map and decision record, not an implementation plan disguised as a rewrite.

## Source baseline

The upstream source was inspected from Vita3K commit `046543d738b7e1dfbb3d12d5a6cce2b25e5f21b4` (the current upstream `master` at the time of survey). The upstream tree is kept outside this repository while this port is being designed.

The native build is a monolithic CMake graph rooted at `CMakeLists.txt` and `vita3k/CMakeLists.txt`. Nearly all emulator subsystems are static libraries linked into the `vita3k` executable. The native application target additionally links Qt, SDL, and platform libraries.

## Execution and ownership flow

```text
native main.cpp
  -> Qt QApplication / SDL setup
  -> app::init_paths + config::init_config
  -> EmuEnvState
       -> MemState
       -> KernelState / threads / CPUState
       -> HLE module graph
       -> IO/VFS, GXM, display, audio, input
  -> AppSessionController
       -> renderer::FrameHost
       -> renderer::State
       -> renderer command queue + native render thread
  -> ELF/SELF loader and Vita process threads
       -> CPUInterface (currently Dynarmic only)
       -> HLE imports and kernel scheduling
```

The core is not currently host-independent. Initialization reaches platform/frontend code before the emulator state is fully useful, and the renderer, input, audio, filesystem paths, and CPU all have native assumptions.

## Subsystem classification

### PORTABLE — preserve with minimal changes

| Area | Evidence and notes |
|---|---|
| `vita3k/modules` | Vita HLE modules are organized as regular C++ libraries and contain the project’s Vita API knowledge. Keep this code independent of browser APIs. |
| `vita3k/kernel` | Process/module loading, relocations, object store, and much of Vita synchronization model are emulator logic. It currently uses host `std::mutex`, condition variables, and threads and therefore needs a scheduling adaptation later, but the Vita semantics should remain. |
| `vita3k/gxm` | GXM command/state interpretation, stream handling, attributes, and texture metadata are valuable portable logic. The final renderer boundary is where host-specific work begins. |
| `vita3k/shader` | GXP parsing, USSE decoding, analysis, and translator logic should be retained. `spirv_recompiler` is an existing output path, not yet a browser output path. |
| `vita3k/module`, `vita3k/packages`, `vita3k/codec`, `vita3k/nids`, `vita3k/regmgr`, `vita3k/rtc` | Primarily emulator formats, ABI, metadata, and HLE behavior. Dependencies still need compile audits, but there is no reason to duplicate their Vita behavior. |
| `vita3k/mem` allocator tables and semantic validation | Allocation bookkeeping, Vita address rules, page names, and protection metadata are reusable. The native backing and fault mechanism are not. |
| `vita3k/emuenv` state model | Useful aggregation of emulator state, although construction currently assumes the native subsystem types. |

### ADAPTABLE — retain interfaces, remove host assumptions

| Area | Current dependency | Browser direction |
|---|---|---|
| CPU | `vita3k/cpu` only constructs `DynarmicCPU`; `CPUInterface` already supplies a useful backend seam. | Add a browser interpreter implementation selected in `cpu/src/cpu.cpp`; keep Dynarmic native. Do not begin with a JIT. |
| Memory | `mem/src/mem.cpp` reserves 4 GiB using `mmap`/`VirtualAlloc`, changes permissions with `mprotect`/`VirtualProtect`, and installs SIGSEGV/SIGBUS handlers. | Use Wasm linear memory plus allocator/page permission metadata. Fault callbacks become explicit checked access paths; do not emulate OS faults literally. Make the total capacity a configurable browser build property rather than hard-coding native reservation. |
| VFS/storage | `util/fs.h` aliases Boost.Filesystem; install and VFS code calls filesystem APIs directly. | Keep VFS path and package semantics. Introduce a storage boundary beneath filesystem operations, initially backed by Emscripten’s virtual filesystem as a bring-up aid and later OPFS-backed streaming. |
| Renderer lifecycle | `FrameHost` is already an explicit host boundary; `renderer::State` owns a native render thread and backend-specific state. | Preserve GXM and command processing. Add a browser frame host/presentation boundary and a WebGPU backend. Initially run render processing synchronously or on the emulator Worker. |
| Audio | Vita mixer feeds SDL or cubeb adapters (`audio/src/impl`). | Preserve mixer/port semantics. Add block-based browser output; JS should not be called per sample. |
| Input | `ctrl` and app code directly poll SDL gamepads; overlay input is also SDL-based. | Keep Vita controller state and binding logic. Replace polling source with a browser input queue populated by the frontend/Worker bridge. |
| Paths/config | `app::init_paths` calls SDL path APIs and uses platform-specific home/config conventions. | Browser paths must be logical roots (`/vita`, config, cache) and must not expose host path assumptions to HLE. |
| Time/sleep | `std::chrono`, SDL timers, and native waits are used in app, camera, controller, renderer, and kernel code. | Centralize browser clock/yield behavior where it affects emulated timing. Avoid scattered Emscripten conditionals. |
| Threading | `std::thread`, condition variables, and render worker are used throughout. | First browser target is single-threaded in an emulator Worker. Add a host scheduler/worker policy only after correctness. Threaded builds will require SharedArrayBuffer and cross-origin isolation. |

### BACKEND — existing native implementations can remain

| Backend | Location |
|---|---|
| CPU | `DynarmicCPU` in `vita3k/cpu/src/dynarmic_cpu.cpp`; it implements `CPUInterface`. |
| Graphics | OpenGL and Vulkan implementations under `vita3k/renderer/src/gl` and `src/vulkan`; `renderer::Backend` currently contains only `OpenGL` and `Vulkan`. |
| Audio | SDL and cubeb adapters under `vita3k/audio/src/impl`. |
| Presentation | Native `FrameHost` implementations are supplied by app/Android/frontend code. |
| Input/camera | SDL-backed controller and camera implementations. |

### NATIVE — exclude from the first browser target

* `vita3k/main.cpp`, Qt application and all `vita3k/gui-qt` code.
* Native window/context creation, SDL event loop, SDL camera, SDL gamepad polling, and native presentation.
* Vulkan/OpenGL renderer sources and Vulkan memory/loader integration.
* Discord RPC, updater, GDB integration, native dialogs, Android JNI, and platform deployment code.
* POSIX/Windows path discovery, signals, virtual memory protection, and desktop dynamic-library assumptions.

### BLOCKER — browser/runtime restrictions that require a design change

1. **Native JIT executable memory:** Dynarmic emits host machine code and cannot be treated as a normal browser backend. A correct interpreter is required first; ARM-to-Wasm compilation is a later project.
2. **Native virtual-memory semantics:** the current 4 GiB reserved address space and signal-based protection callbacks cannot be reproduced as an ordinary portable Wasm allocation. Semantic page tracking must replace host faults.
3. **Graphics API:** Web browsers do not expose Vita3K’s native Vulkan device. WebGPU needs a new renderer backend and WGSL shader output path.
4. **Synchronous filesystem assumptions:** OPFS APIs are asynchronous/browser-owned. Existing Boost filesystem and archive installation code needs a storage boundary or an Emscripten filesystem staging layer.
5. **Host threads and waits:** browser workers and Wasm threads have different deployment requirements. Blocking waits and native render-thread ownership must be phased carefully.
6. **Frontend lifecycle:** Qt’s application/event-loop contract is not suitable for the web UI. The emulator must have an explicit initialize/boot/pause/resume/stop/shutdown protocol.
7. **Native audio/input:** SDL/cubeb are host adapters, not browser APIs. Browser audio must be block/ring-buffer based and browser controls must not leak into HLE.

## Important existing seams

* `CPUInterface` (`vita3k/cpu/include/cpu/impl/interface.h`) already exposes run/step, context, memory invalidation, breakpoints, and register access. This is the preferred CPU backend seam.
* `renderer::FrameHost` (`vita3k/renderer/include/renderer/frame_host.h`) separates drawable/presentation concerns from renderer state. It should be extended conservatively for browser presentation rather than replaced.
* `renderer::State` has virtual lifecycle and frame methods, but backend selection and state dispatch are currently hard-coded for OpenGL/Vulkan. WebGPU will require an explicit third backend and corresponding dispatch audit.
* `MemState` already has allocator, page table, protection tree, and external mapping metadata. Those structures can support semantic Wasm memory permissions without relying on native page faults.
* Audio has adapter-level boundaries (`AudioAdapter` and SDL/cubeb implementations), making browser audio a backend addition rather than a mixer rewrite.
* VFS-facing functions such as `vfs::read_file` are narrower than the installer’s direct `fs::` usage; this is a likely first filesystem extraction point.

## Build graph findings

`vita3k/CMakeLists.txt` unconditionally adds most emulator libraries, then conditionally adds Qt only when not Android. The top-level build also unconditionally adds external Dynarmic, SDL, Vulkan-related dependencies, FFmpeg, glslang, SPIRV-Cross, and other desktop-oriented targets. The executable links `app`, which in turn links SDL and many platform-facing subsystems.

A browser build must therefore be an explicit target graph, not merely `-DEMSCRIPTEN=ON` on the current executable. The first target should compile a small browser entrypoint and the portable state libraries, while excluding Qt, native renderer sources, Dynarmic, desktop integration, and native-only utilities. This should be implemented in a dedicated browser CMake path; native targets must remain unchanged.

## Proposed incremental milestones from this survey

1. **M0 (this document):** repository map and blockers recorded.
2. **M1:** add a dedicated `VITA3K_WEB` CMake option and a tiny browser entrypoint that only initializes logging/config-independent state. No emulator functionality is stubbed silently.
3. **M2:** add browser memory backing and tests for allocation, free, validity, semantic permissions, and address translation.
4. **M3:** add `InterpreterCPU` behind `CPUInterface`, beginning with a small ARM/Thumb test harness; native Dynarmic remains the reference.
5. **M4:** make a minimal worker lifecycle and expose diagnostics/capability checks.
6. **M5+:** storage, WebGPU bootstrap, GXM/shader backend, then homebrew integration.

Each step must retain a functioning native build. Commercial software and Persona 4 Golden are integration milestones, not bring-up tests.

## M1 browser bootstrap validation

The M1 target is a deliberately separate Emscripten lifecycle probe; it does not
compile the emulator core or native frontend dependencies. Validate it with:

```sh
emcmake cmake -S . -B build/web -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/web --verbose
find build/web -maxdepth 3 -type f | sort
cd build/web/dist
python3 -m http.server 8080
```

Open `http://127.0.0.1:8080/` from a browser with WebAssembly and Worker
support. The page and Worker must be served over HTTP (not `file://`), and the
Worker must be able to fetch `vita3k_web.js` and the adjacent
`vita3k_web.wasm`. A successful page reports `Vita3K WebAssembly bootstrap
ready.` and logs the M1 initialization message. The current target produces
`build/web/dist/index.html`, `worker.js`, `capabilities.js`, `vita3k_web.js`,
and `vita3k_web.wasm`. Browser automation is optional; static artifact inspection
and an HTTP smoke check are useful when no headless browser is installed.

## M2 browser memory progress

M2 now includes a standalone `browser::web::Memory` model in
`browser/src/memory.{h,cpp}`. It uses a configurable byte vector with 4 KiB
allocation metadata instead of native `mmap`, `mprotect`, or fault handlers.
Allocations reserve guest pages, keep page zero unavailable, zero newly
allocated storage, support fixed-address allocation and release, and expose
explicit validity, permission, checked translation, read, and write APIs.

The focused `vita3k_web_memory_tests` executable is built as part of the
browser graph and runs without Qt, SDL, Vulkan, Dynarmic, or Emscripten APIs in
the memory implementation. The current smoke tests cover allocation/release,
zero-page reservation, fixed-address conflicts, names, read/write translation,
read-only permissions, and invalid access. This is an incremental M2 seam; the
native `MemState` and `Ptr<T>` paths remain unchanged, and protection callbacks,
aligned allocation, broader parity tests, and interpreter integration remain
later work.

## M3 interpreter bring-up slice

The browser target now includes a standalone `vita3k::web::Interpreter` over
`browser::Memory`. This is an execution probe, not yet the production
`CPUInterface` backend. It deliberately supports only a small deterministic
subset: ARM immediate MOV/ADD/SUB/CMP and B/BL, positive-immediate word
LDR/STR, plus Thumb-1 immediate MOVS/ADDS/SUBS, unconditional B, BX, and
word LDR/STR. It also supports Thumb low-register ADD/SUB and the basic
register ALU forms (AND/EOR/TST/CMP/ORR/BIC/MOV). It tracks the PC, general
registers, Thumb state, and the CPSR N/Z flags. Unsupported instructions and checked
memory faults halt the probe instead of being silently treated as successful.

Validate the slice with:

```sh
clang++ -std=c++23 -Wall -Wextra -Werror \
  browser/src/memory.cpp browser/src/interpreter.cpp \
  browser/tests/interpreter_tests.cpp -o /tmp/vita3k_web_interpreter_tests
/tmp/vita3k_web_interpreter_tests
cmake --build build/web --verbose
node build/web/browser/vita3k_web_interpreter_tests.js
```

The native Dynarmic path remains unchanged. The next interpreter increment
should expand instruction coverage and add differential tests before adapting
`CPUInterface`; it should not yet attempt full Vita process/thread integration.
The memory operations use `Memory::read`/`write`, so permission and bounds
faults are explicit and testable rather than host signal handlers. The
interpreter exposes `StepResult` (`Executed`, `MemoryFault`, `Unsupported`, and
`Halted`) so callers can distinguish a failed guest access from an unsupported
opcode or a previously halted core. ARM condition
codes (EQ/NE and the remaining standard conditions) are evaluated, and
arithmetic instructions update N/Z/C/V for the supported immediate forms.
Thumb immediate shifts (LSL/LSR/ASR) and conditional branches are also
covered, including carry updates from shifts. Thumb PUSH/POP is now covered
for low registers plus LR/PC, with checked stack memory and Thumb-state
updates on PC restores. Thumb byte and halfword transfers are also covered:
register-offset STRH/LDRH/STRB/LDRB/LDSB/LDSH plus immediate STRB/LDRB and
STRH/LDRH forms, all routed through checked browser memory.

## M4-M7 browser host seams

M4 adds an explicit Worker lifecycle (`loading`, `ready`, `paused`, `stopped`,
and `error`) with timestamped events, status/pause/resume/shutdown commands,
and module diagnostics. M5 adds `browser/web/storage.js`, a logical-path Fetch
read bridge; writes intentionally require an application upload endpoint rather
than pretending that HTTP is writable storage. M6 adds `browser/web/webgpu.js`,
which reports WebGPU support and can request a device without coupling the
native renderer to browser APIs. M7 adds `browser/web/audio_input.js`, with a
queued input API and block-oriented Float32 audio validation; actual
AudioWorklet output and SDL/HLE integration remain future work.

All three host bridges are staged by the browser CMake target and are exposed
through `globalThis.vita3kWeb`. Validate the assembled browser target with:

```sh
cmake --build build/web --verbose
(cd build/web/dist && python3 -m http.server 8080 --bind 127.0.0.1)
```

Playwright validation must observe the lifecycle loading/ready messages, the
existing `Vita3K WebAssembly bootstrap ready.` status, and no page errors. These
milestones establish host-facing seams only; they do not claim OPFS persistence,
full WebGPU rendering, AudioWorklet playback, or emulator-core integration.

## Guest execution vertical slice

The browser target now has a small end-to-end execution probe in
`browser/src/guest.*`. A versioned-independent synthetic guest image is loaded
into `browser::Memory`, executed by the standalone ARM/Thumb interpreter, and
can terminate through the reserved ARM `0xef000001` or Thumb `0xdf01` probe
instruction. The Worker invokes this path through the exported
`vita3k_web_run_guest_probe` function and reports the guest exit code (42 in
the deterministic probe) back to the page. The exit value is read from guest
`r0` and reported as a `GuestResult`, making the execution observable without
pulling the native loader, `KernelState`, or HLE graph into the minimal Wasm
target.

This is deliberately not a Vita ELF/SELF loader and the probe instruction is
not wired to the native `sceKernelExitProcess` implementation. It proves the
browser memory -> guest instruction -> host syscall boundary and provides a
place to add a real image format and HLE dispatch incrementally.

Validate it with:

```sh
clang++ -std=c++23 -Wall -Wextra -Werror \
  browser/src/memory.cpp browser/src/interpreter.cpp browser/src/guest.cpp \
  browser/tests/guest_tests.cpp -o /tmp/vita3k_web_guest_tests
/tmp/vita3k_web_guest_tests
cmake --build build/web --verbose
node build/web/browser/vita3k_web_guest_tests.js
```

The browser-level check should also log `guest exit: 42` after the Worker
reaches `ready`. This catches runtime-lifetime errors that standalone Node
execution cannot catch.

## Next inspection targets before code changes

* Enumerate all `cpu::init_cpu` call paths and thread run-loop/SVC handling to define interpreter ownership.
* Audit all `mem::protect_*`, `Ptr<T>::get`, and external mapping users before changing memory.
* Trace `renderer::Backend` dispatch in creation, scene, state, batch, and shader code.
* Identify direct `fs::` writes in package installation and config initialization, separating logical VFS operations from host storage.
* Check available Emscripten toolchain/dependency support locally before choosing the first CMake target.

## Real Vita3K runtime convergence and genuine VitaSDK launch (complete)

The browser target no longer uses a parallel emulator model. The standalone
`browser::web::Memory`/`Interpreter`/guest-image probes remain only as early
milestone regressions; production browser execution now runs the real Vita3K
architecture:

```text
browser File / fixture bytes
  -> Worker (browser/web/worker.js, run-vita)
  -> Wasm module export vita3k_web_run_vita (browser/src/vita_runtime.cpp)
  -> load_self_sized() -> existing kernel/src/load_self.cpp
  -> real MemState (sparse Wasm backing) + real KernelState/EmuEnvState
  -> real main ThreadState (cooperative run_loop(true))
  -> InterpreterCPU behind the production CPUInterface contract
  -> genuine ARM/Thumb guest execution of crt0/newlib/main
  -> real import stubs -> SVC -> NID -> modules::call_import
  -> real selected HLE exports (no substitute resolver)
  -> sceKernelExitProcess -> observable exit code
```

Key seams, all preserving native behavior:

* `browser/runtime_core.cmake` compiles the real `mem`, `cpu`, `kernel`,
  `rtc`, `nids`, miniz and loader sources under Emscripten. Nothing in that
  graph is a browser reimplementation.
* `browser/runtime_hle.cmake` selects 27 existing HLE exports (SceSysmem,
  SceLibKernel, SceThreadmgr, SceProcessmgr, SceIofilemgr) via
  registration-only adapters over the unchanged `module_parent.cpp`
  `::call_import`/`make_bridge` machinery and the authoritative `nids.inc`.
  Full `EmuEnvState` is constructed unchanged; SDL is built static with all
  device backends off so real subsystem destructors still link.
* `MemState` gained a sparse Wasm host backend (one contiguous buffer per
allocation, flat 1M-entry page table, shared guest allocator metadata) plus
checked `mem_read`/`mem_write`/`mem_fetch`/`mem_set_permissions` and safe
`mem_host_to_guest`. Native direct mapping is untouched.
* `ThreadState::run_loop(true)` is an opt-in cooperative mode for hosts that
  drive an already-started thread synchronously (browser/Worker); the native
  SDL host-thread path is unchanged.
* `InterpreterCPU` implements the startup Thumb16/Thumb32/ARM families with
  correct NZCV/IT/interworking and the post-SVC PC import contract
  (NID at post-SVC PC + 4 after `mov pc, lr`), using checked memory access.

Genuine fixture: `browser/tests/vita_homebrew_fixture` builds a real VitaSDK
executable from `main.c` (`return 42`) with `arm-vita-eabi-gcc`,
`vita-elf-create`, and `vita-make-fself` entirely in the build tree
(`VITA3K_VITASDK_LAUNCH_TEST=ON`, `VITASDK=/opt/vitasdk/vitasdk`). The
committed `eboot.bin`/`fixture.velf`/`fixture.elf` are regenerable SDK outputs
kept so tests run without VitaSDK installed.

### Validation record

* Native interpreter build, genuine fixture launch through production
  `EmuEnvState`/`create_thread`/`run_loop`/`::call_import`:
  `process_exit_requested=true status=42 import_calls=23 missing_nids=0`
  (`ctest -R vita3k_vitasdk_launch`, ~0.3 s).
* Chromium/Playwright Worker end-to-end on the same `eboot.bin`:
  `vita-exit exitCode=42 ok=true`, 23 real imports (LwMutex ops, memblock
  alloc/base/free, TLS, `sceIoOpen` tty0: x3, `sceIoClose` x3, mutex ops,
  `sceKernelExitProcess`), zero page errors
  (`browser/tests/worker_smoke.mjs`).
* Loader hardening: 2,973 checks per suite (normal and ASan/UBSan/NDEBUG)
  covering genuine VELF, SELF and compressed SELF through both APIs.
* MemState: 517 native checks (Clang+GCC, UBSan, direct and page-table) and
  291 executed wasm32 checks under Node (also UBSan); allocator suite 13/13.
* Interpreter: 233 focused assertions (also ASan/UBSan), 348
  assembler-verified encodings, Unicorn differential 31,650 Thumb + 600 ARM
  cases; native convergence and instruction tests registered as
  `vita3k_interpreter_runtime` / `vita3k_interpreter_instruction`.
* Browser regressions all pass under Node: M2 memory, M3 interpreter, guest
  probe, generated ARM ELF exit-42, real-loader Wasm probe, and the generic
  Worker ELF smoke in Chromium.

### Current limitations

* Interpreter performance is bring-up quality: the debug Wasm build needs
  tens of seconds for the fixture; optimized builds and later an ARM-to-Wasm
  JIT are future work.
* Only the startup HLE subset is selected for the browser target; other NIDs
  remain unsupported rather than faked. Graphics, audio, input, and further
  syscalls are not wired.
* `load_self_sized` is validated on genuine fixtures, not hostile input;
  relocation hardening is documented in `vita3k/kernel/tests/README.md`.
* Browser execution is a single cooperative main thread; SDL host threads
  and multi-threaded Wasm remain future work.
