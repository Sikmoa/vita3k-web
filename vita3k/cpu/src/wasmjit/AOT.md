# Ahead-of-time (AOT) Wasm module

The lazy region JIT (REGION_ABI.md) forms, emits and installs regions while the
guest runs, keeps them in an LRU cache, revalidates them after HLE and chains
them through a hashed dispatch map. On a retail title that streams through tens
of thousands of regions, that machinery costs far more than the generated code.
AOT removes it: every function of the loaded modules is translated once, before
the guest starts, into ONE Wasm module. The lazy JIT stays as the fallback for
anything the module does not cover.

## Build and use

The builder runs inside the normal app launch path, after the preload chain and
`app0:eboot.bin` are loaded, so module addresses are exactly the ones the
runtime will see:

```sh
VITA3K_NULL_GPU=1 VITA3K_AOT_BUILD=limbo.aot.wasm [VITA3K_AOT_SEEDS=seeds.txt] \
  node build/web64/browser/vita3k_web_app_bench.js .limbo_work/stage PCSE00268
```

* Code ranges: text segment (segment 0) of each loaded module with an unwind
  table, an entry point or a seed, up to the `.ARM.exidx` end-of-code sentinel
  (a final `EXIDX_CANTUNWIND` entry) or the module info, whichever comes first;
  the rest of the segment is read-only data (`WasmJitCPU::aot_code_size`).
* Roots: module start/stop, `.ARM.exidx` function starts
  (`WasmJitCPU::aot_exidx_functions`, which restores the Thumb bit SCE tables
  omit for some Thumb routines), function exports (variable exports are data),
  Thumb values of the module's absolute relocations (function pointers; a
  fixed-address executable such as Limbo has none), and optional seeds. Seeds
  are full location keys of blocks the lazy JIT executed
  (`VITA3K_AOT_SEEDS_OUT=<file>` records them in any JIT run).
* The build also reports untranslatable blocks by first rejected IR op, and
  imported NIDs this build has no HLE body for.

At run time the module is supplied as a compiled `WebAssembly.Module`
(`Module.vita3kAotModule`; the Worker compiles `run-app`'s `aotUrl`, the Node
bench reads `VITA3K_AOT`). `WasmJitCPU::load_aot` verifies each range's bytes
against the build-time hash and refuses the module on any mismatch.

## Module ABI

Metadata version 7 includes the extended portable FP operand area and the
shared generic/A32 lowerings described in [IR_COVERAGE.md](IR_COVERAGE.md).
Rebuild older AOT images; the runtime rejects their metadata version.

Imports: `env.memory` (the building runtime's linear memory: Memory64 from the
web64 build, 32-bit from web32; the metadata records the width and
`load_aot` refuses an image for the other one), `env.mem_read`,
`env.mem_write`, `env.fp64` (as for regions) and the immutable global `env.aot_lut` (host pointer to the lookup
table). Functions: 3 `entry`, 4 region fault helper, 5 `lookup`, 6 `transfer`,
then one region-shaped `run(state, budget)` per AOT function, in table order.

* Lookup table: one u32 per halfword of each code range;
  `aot_lut_entry(slot, block index, thumb)` or 0. Only member blocks in the
  default execution state (IT=0, E=0, FPSCR mode 0) are addressable.
* `transfer(state, remaining, pc)`: all state is in memory. Returns `Budget`
  when `remaining <= 0` (signed), `Miss` when no function owns `pc`,
  `EntryMiss` when the live CPSR/FPSCR mode does not match the entry; otherwise
  stores the block index in `JitState.aot_entry` and tail-calls the owner.
* A function's `Miss` exit tail-calls `transfer` with its unspent budget, so
  guest-to-guest control flow never returns to the host. SVC, faults, budget
  exhaustion and uncovered targets return to the host exactly like regions.

## Emission policy (deliberate differences from lazy regions)

* Entry by block index; no PC search tree, no stop/SMC polls, no dispatch
  counter. Budget is checked at transfers and backward member edges only, so a
  call may exceed its budget by one function's forward path (bounded by the
  region limit); the host accepts that overrun for AOT calls.
* Backward member edges branch directly to the target body.
* `unchecked_memory` (Memory64 only): guest accesses are plain window loads and
  stores. Guest faults are not reported and stores do not raise `smc_dirty`.
  AOT text is read-execute; a store into a covered page that does reach the
  checked path disables the whole module. `VITA3K_AOT_CHECKED_MEMORY=1` keeps
  the probes for diagnosis.
* `fast_fp`: FP add/sub/mul/div/sqrt lower to native Wasm arithmetic without
  cumulative FPSCR flags; NaN payloads and NEON denormal flushing may differ.
  `VITA3K_AOT_EXACT_FP=1` restores the exact lowering.

## Tests

`vita3k_jit_aot_module_test_node` runs one Thumb program on the interpreter,
on lazy regions (whole and in slices) and on a built AOT module (whole and in
slices down to one instruction) and requires identical state, memory and
instruction counts.
