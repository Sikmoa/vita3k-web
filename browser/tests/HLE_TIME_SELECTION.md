# Nonblocking time and kernel imports (source-only batch A1)

Status: implemented, **not built or executed**. This does not establish a new
Limbo startup checkpoint or gameplay. The previous checkpoint remains the
reported missing `sceKernelGetProcessTimeWide` import.

## Selected implementations

All NIDs are copied by CMake from `vita3k/nids/include/nids/nids.inc`.
No NIDs, bridges or function bodies are duplicated by this batch.

| Newly selected exports | Implementation and invariant |
| --- | --- |
| `sceKernelGetProcessTime`, `sceKernelGetProcessTimeWide` | `vita3k/modules/SceLibKernel/SceLibKernel.cpp:1382,1395`: the same RTC ticks minus `kernel.start_tick` as the already selected Low variant. The pointer form writes a U64; the Wide form uses the original U64 AAPCS return bridge. |
| `sceKernelGetSystemTimeWide` | `vita3k/modules/SceKernelThreadMgr/SceThreadmgr.cpp:1210`: `get_current_time()`, with no wait. |
| `sceKernelLibcClock`, `sceKernelLibcTime` | `vita3k/modules/SceProcessmgr/SceProcessmgr.cpp:183,259`: process ticks and seconds since RTC_OFFSET respectively. |
| `sceKernelGetThreadCurrentPriority`, `sceKernelGetThreadExitStatus` | `SceLibKernel.cpp:1444,1454`: query the real thread object; the latter reports NOT_DORMANT rather than waiting for termination. |
| `sceKernelGetThreadCpuAffinityMask`, `sceKernelGetThreadCpuAffinityMask2` | `SceThreadmgr.cpp:1215`, `SceLibKernel.cpp:1430`, common implementation `SceThreadmgr.cpp:471`: thread lookup with current-thread alias handling. |
| `sceKernelGetSemaInfo` | `SceLibKernel.cpp:1405`, `SceThreadmgr.cpp:398`: validates pointer/size and reads the production semaphore object. |
| `sceKernelTryLockMutex` | `SceThreadmgr.cpp:1427`, `vita3k/kernel/src/sync_primitives.cpp:708`: `only_try=true` returns the production contention error before enqueuing any wait. |

The handoff named SceProcessmgr as the location of GetProcessTimeWide; in this
checkout it is SceLibKernel. Both files were already in `_hle_module_sources`.

## Library resolution audit

`vita3k/module/src/load_module.cpp` maps LLE sysmodule dependencies, not HLE
registrations. `vita3k/kernel/src/load_self.cpp:128` uses the import's library
NID to choose LLE bindings; otherwise it installs a supervisor-call trampoline
carrying the function NID. `vita3k/modules/module_parent.cpp:72` resolves that
NID using the generated NID switch, independently of library initialization.
`init_libraries` at line 476 simply calls `import_library_init_<name>`.
`browser/src/vita_app.cpp:206` invokes it before loading modules.

Only SceSysmem in the selected source set defines LIBRARY_INIT. There are no
SceLibKernel/SceProcessmgr initialization symbols to emit. Consequently the
generated list remains `LIBRARY(SceSysmem)`; CMake now explains why. Adding a
library name for each selected export would create unresolved symbols, not
make more imports resolvable.

## Deliberately unselected / scheduling boundary

- GetSystemTime and GetThreadRunStatus are upstream UNIMPLEMENTED functions.
  There is no user `sceKernelGetSystemTimeLow` entry in the authoritative NID
  database (only the driver `ksce...` name).
- WaitSema/CB, DelayThread/CB/200, WaitThreadEnd/CB, WaitEventFlag/CB and
  WaitLwCond/CB need a resumable wait record and scheduler. Production waits
  use host condition variables or sleep; selecting them is not an implementation.
- Existing LockMutex/LockLwMutex bridges can still block on contention; current
  startup success only establishes their uncontended path. Existing vblank
  handling does not establish scheduling between multiple guest threads.
- Other pre-existing selected exports may be upstream stubs. This batch adds
  no stubs and does not certify the whole historical selection.

Assumptions requiring execution: RTC initialization in both launch paths,
64-bit bridge marshalling on both ABIs, and link reachability of the newly
selected implementations. Missing imports remain reported and stop the browser
launch path; no rejection has been weakened. See VERIFICATION_QUEUE.md.
