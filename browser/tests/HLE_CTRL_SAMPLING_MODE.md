# SceCtrl sampling-mode frontier (Limbo imports=3995)

Status: implemented, **not built or executed**. Parent verifies serially.
This note is the reconciliation record for Task #19; it establishes no new
Limbo checkpoint on its own.

## Frontier evidence

- `module-coop-limbo.log` tail: HLE imports #3993 (`sceKernelLockLwMutex`)
  and #3994 (`sceKernelUnlockLwMutex2`) return, then import #3995
  `NID=a497b150 PC=8126af40 name=sceCtrlSetSamplingMode` hits
  `module_parent.cpp:182` (`Import function for NID 0xA497B150 not found`).
- Result line: `Vita result: process_exit=0 code=0 imports=3995
  missing_nids=1 PC=8126af40` plus `missing NID=a497b150
  (sceCtrlSetSamplingMode)`.
- Static-import anchor (`static-imports.log`): Limbo `SceCtrl`
  (`library_nid=D197E3C7`) declares exactly two function imports,
  `sceCtrlReadBufferPositive` (`nid=67E7AB83`, `entry=8126AF2C`) and
  `sceCtrlSetSamplingMode` (`nid=A497B150`, `entry=8126AF3C`). The faulting
  PC (`8126af40` = entry + 4) is the `SetSamplingMode` stub, so it is the
  single observed blocker. `ReadBufferPositive` has not been called yet in
  any trace; it is the predicted next frontier, not part of this step.

## Change (only `browser/runtime_hle.cmake`)

1. `_hle_exports` gains exactly one name, `sceCtrlSetSamplingMode`, with a
   comment recording the frontier (`imports=3995`, `PC=8126af40`) and the
   production-body rationale. Generated `startup_nids.inc` therefore grows
   from 126 to 127 NID lines; the value comes from the authoritative
   database, `vita3k/nids/include/nids/nids.inc:1022`
   (`NID(sceCtrlSetSamplingMode, 0xA497B150)`). No NID was invented.
2. `_hle_module_sources` gains
   `vita3k/modules/SceCtrl/SceCtrl.cpp` (verified: no `LIBRARY_INIT`, no
   `CALL_EXPORT` in that file), so `startup_libraries.inc` correctly stays
   `LIBRARY(SceSysmem)` and the existing `FATAL_ERROR` guards (absent from
   `nids.inc`, no source implementation) remain armed for this name.

## Upstream body audit (production, no stub parity needed)

`vita3k/modules/SceCtrl/SceCtrl.cpp:239-248`:

- No `UNIMPLEMENTED()`/`STUBBED()`; returns `RET_ERROR(...)` only for an
  out-of-range mode and otherwise stores `emuenv.ctrl.input_mode` and
  returns the previous mode. `CtrlState::input_mode` defaults to
  `SCE_CTRL_MODE_DIGITAL` (`vita3k/ctrl/include/ctrl/state.h:70,92`).
- Link safety: the selected body touches only that field plus existing
  `TRACY_FUNC`/`RET_ERROR` machinery. All other `SceCtrl.cpp` bodies
  (including the `ctrl_get`/SDL callers) compile but register no bridge
  (`VITA3K_HLE_SKIP_BRIDGE`) and are dropped by `-ffunction-sections`;
  only the referenced `export_sceCtrlSetSamplingMode` section is kept.
  SDL3-static is already linked for motion/camera/display-manager objects.

## Static-import reconciliation

- `unselected-imports.tsv` rows `A497B150`/`67E7AB83` (`sceCtrlSetSamplingMode`
  / `sceCtrlReadBufferPositive`, importing module `Limbo`): the former is
  now selected; the latter stays `NOT_SELECTED` by design until an observed
  PC demands it. Its upstream body (`SceCtrl.cpp:160-163`, via `ctrl_get`)
  is production, so no stub-parity concern is pre-judged either way.
- All other Limbo static imports (SceTouch, SceIme, the rest of SceGxm,
  SceDisplay wait/swap, sysmodule/RTC/NP/audio/Fios2/libc, etc.) remain
  unselected and still reject through the unchanged
  `module_parent.cpp:182` / `missing_nids` path. No rejection path was
  touched: no edits to `module_parent.cpp`, CPU/JIT, renderer, upstream
  implementations, or any other workstream file.
- `all-module-imports.log` and `static-imports.log` both enumerate 773
  static imports; the selected set intentionally remains a small subset,
  exactly as documented in `runtime_hle.cmake`'s header comment.

## Verification for parent (serial, do not run in parallel with other builds)

1. `cmake -S . -B build/web -DVITA3K_WEB=ON` then
   `grep -c '^NID(' build/web/browser/runtime-hle-generated/startup_nids.inc`
   Expected: `127` (was 126), including the line
   `NID(sceCtrlSetSamplingMode, 0xA497B150)` verbatim from `nids.inc`.
2. `grep -c sceCtrl build/web/browser/runtime-hle-generated/startup_bridge_selection.inc`
   Expected: `1` (`VITA3K_HLE_BRIDGE_sceCtrlSetSamplingMode` emitted;
   every other `SceCtrl` bridge skipped).
3. Re-run the Limbo HLE trace used for `module-coop-limbo.log`.
   Expected: `imports>3995`, `missing NID=a497b150` gone, fault (if any)
   moves to a new single `missing NID` with a new PC — predicted
   `67E7AB83 (sceCtrlReadBufferPositive)` once the game polls input, but
   the actual next NID/PC governs the following step, not this note.

## Assumptions and next frontier

- Assumes `CtrlState` construction in `EmuEnvState` already initializes
  `input_mode` (DIGITAL default) on the browser path, as on desktop; the
  first `SetSamplingMode` call then just records the game's requested mode
  and returns the old one.
- Assumes no guest thread contends on input state at this point; the body
  performs no wait and needs no scheduler support.
- Next frontier: `sceCtrlReadBufferPositive` (`67E7AB83`, static entry
  `8126AF2C`) — select only after a trace shows it as the missing NID.
  SceTouch (`10A2CA25`, `1B9C5D14`, `FF082DF0`) and SceIme (`0E050613`,
  `71D6898A`, `889A8421`) static imports follow behind it and each needs
  its own upstream-body audit before selection.
