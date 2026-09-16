# M14c region JIT ABI (parent-owned spec, v1.2)

Goal: one WebAssembly module per REGION (many guest basic blocks) with an
in-module dispatch loop. No JS crossing and no C++ cache work per guest block.
Only region-level exits return to the host.

## Optional promoted region state (candidate, September 2026)

**Implemented but unvalidated in this source-only environment.** No compilation,
Wasm validation, tests, fixture execution, browser execution or performance
measurement was possible for this patch. The default is the existing reference
emission. This section describes the candidate and takes precedence over older
design sketches below; it does not assert runtime correctness.

### Selection and lifetime

`RegionStateOptions` is an emission-time policy shared by `Emitter` and the
region prologue, search tree and exit epilogue. No guest instruction checks a
runtime option, and no second complete emitter exists. `emit_region(blocks,
meta, {false, false})` explicitly selects reference emission; `{true, false}`,
`{false, true}`, `{true, true}` select P, K and PK for differential tests using
the same IR and metadata. `validate_region_block` uses the reference policy;
formation, supported opcodes, memory probes and terminal routing are unchanged.
`emit_block` / CPU `step()` always retain reference emission.

The default argument and production CPU use `region_state_options()`, which
reads configuration once per process/Emscripten module. Each CPU holds that
immutable selection for its cache lifetime. Start a fresh process/Worker to
change it; changing environment variables after first use has no effect.

| Run | `VITA3K_WASMJIT_PROMOTE_FLAGS` | `VITA3K_WASMJIT_PROMOTE_ACCOUNTING` |
| --- | --- | --- |
| A, reference | `0` | `0` |
| P, flags (production default) | unset | `0` |
| K, accounting | `0` | `1` |
| PK, both | `1` | `1` |

Since R2, `PROMOTE_FLAGS` defaults to true (production); set it to `0` for
the reference representation. `PROMOTE_ACCOUNTING` defaults to false;
`VITA3K_WASMJIT_PROMOTED_STATE=1` supplies a true default for accounting
(flags are already on); an explicitly present individual option overrides the
umbrella. Only the exact string `1` enables an option; any other present
value (including `0`) disables it. Native emission uses `getenv`.
Emscripten reads identically named `Module` properties first, then Node's
`process.env`; browser operators set the properties on the module configuration
before creating the CPU. Browser frontend changes are not required by this ABI.
CPU profile output includes `promote_flags=0/1 promote_accounting=0/1`.
Keep the older `VITA3K_ABLATE` and `VITA3K_ABLATE_PC` unset for validation and
measurement; the existing experimental guard/dispatch ablations are unrelated.

Promoted locals live for **one region call only**. Before any return, including
a cached region transfer in the M16 pump, the common epilogue materializes them.
The next region reloads memory. `JitState` layout remains 420 bytes, including
`tx_wasm` at +416. Function signatures, imports, table slots, map keys and
dispatcher byte emission are unchanged.

### Representations and dirty policy

The reference stores architectural CPSR in `JitState.cpsr` (+64), with NZCV in
bits 31..28. IR `NZCVFlags` values also use these packed CPSR positions (not the
native backend's opaque flag representation). Carry/overflow pseudos are U1
SSA values in producer slots +4/+5. Arithmetic/shift computation is unchanged.
ITSTATE uses bits 26:25 and 15:10; T is bit 5, E bit 9. Descriptor updates use
the existing `Location::CPSR_MODE_MASK`, which is statically required to exclude
NZCV. It describes T/E/IT, not the processor mode bits in CPSR[4:0].

P loads CPSR once at entry into four normalized i32 N/Z/C/V locals, plus a
separate `other_psr` containing `cpsr & 0x0fffffff`. N/Z/C/V are never updated by
reconstructing full CPSR. `other_psr` receives the same masked descriptor and
BX.T updates as the reference, preserving Q (bit 27), GE (19:16), processor
mode, interrupt bits and all other incoming bits. IT has no independent runtime
state in this emitter; descriptors supply its advances/recovery. FPSCR/FPU
remain memory-backed and their supported operations/validation are unchanged.

Reference accounting already keeps `executed_call` in local 2, dispatch count
in local 7, and budget in parameter 1. There is no additional memory-backed
tick accumulator in region bodies in this revision. K retains `regs[15]` and
`next_pc` in two more locals. Both load their own incoming memory value; they
must NOT be aliased because faults can leave `next_pc` unchanged. PC is the
pending/resume value used by the reference, not a synthesized per-instruction
architectural PC+4/+8. The generic search's local 3 remains search scratch.

Reference local numbering is unchanged (SSA begins at 26). Any promoted policy
reserves 26=N, 27=Z, 28=C, 29=V, 30=other_psr, 31=PC, 32=next_pc, with SSA at
33. An unused half of that small reservation is left unused in P/K runs.
R0..R14 remain in the existing locals 11..25; this is not new GPR promotion.

Dirty tracking is conservative: enabled locals become authoritative on entry
and are always published at an observation boundary, even after zero work.
There is no compile-time dirty bit that could incorrectly summarize a runtime
branch or a later loop iteration. `materialize_flags` merges all four flag
locals with `other_psr`; a full CPSR reader uses it before its memory read.
That read does not invalidate locals. `reload_flags` is used at invocation
entry; no supported in-body operation modifies CPSR outside this abstraction.
`materialize_accounting` commits ticks/dispatch totals **only once at exit**;
it is not an idempotent helper-call spill and must never be used as one.

### CPSR access audit

The six original logical CPSR access sites in region emission are accounted
for below. The four extra direct CPSR loads in `emit_dispatch` remain unchanged
because M16 reads the fully materialized T/E/IT key between region calls.

| Existing operation/site | P behavior |
| --- | --- |
| `flag`, used by `A32GetCFlag`, entry/terminal conditions and conditional selects | `read_flag`: local.get N/Z/C/V; EQ/NE, CS/CC, MI/PL, VS/VC, HI/LS, GE/LT, GT/LE, AL expressions unchanged; NV still rejected |
| `A32SetCpsrNZ`, `NZC`, `NZCV`, `NZCVRaw` common setter | `write_nzcv`: set only the selected locals; NZ preserves C/V, NZC preserves V |
| `A32GetCpsr` | `read_full_cpsr`: materialize complete CPSR, then read memory; preserves full-state observation semantics |
| `upper_location` from descriptor updates, terminals, condition failure, continuation exit, fault recovery | `write_psr_field`: update T/E/IT in `other_psr`, preserving all other bits and the four flags |
| `A32BXWritePC` | Descriptor update, then masked T-bit update in `other_psr`; the aligned target goes through `write_pc`; late upper-location updates remain suppressed |
| Generic entry search PSR check | Read `other_psr`, then the original descriptor mask/comparison; FPSCR load/comparison unchanged |

ADD/ADC, SUB/SBC/RSB, CMP/CMN use the existing Add32/Sub32 result and carry/
overflow pseudos, then the common flag setters. Logical/MOV/MVN and
LSL/LSR/ASR/ROR/RRX use existing result/shift-carry SSA and the same setters.
Packed flags, `GetNZFromOp`, `GetNZCVFromOp`, and `GetCFlagFromNZCV` keep their
existing packed SSA representation; only architectural consumers change.
No supported `Mul32`, saturation/Q writer, GE writer, full CPSR writer, or
FPSCR writer case exists in this checkout's emitter. Such IR continues to
fail closed; the presence of `GetGEFromOp`'s PackedAddU8 check does not make
PackedAddU8 supported (its producer is rejected). This patch does not add
instruction coverage or silently accept those operations. VMRS/VMSR/FP state
interactions that require unsupported IR likewise remain rejected; supported
VFP/NEON data moves and memory accesses do not change CPSR/FPSCR.

### Bookkeeping and observation boundaries

| Field or boundary | Candidate behavior |
| --- | --- |
| `executed_call`, budget checks | Original local arithmetic through `read_executed`/`add_ticks`; no changed formula or check placement |
| `state.executed` (+80) | Original wrapping `state.executed + executed_call`, exactly once in shared epilogue |
| `regs[15]` (+60) | K `read_pc`/`write_pc` for IR access, BX, terminals and generic search; initialized before any early exit; spilled on every return |
| `next_pc` (+372) | K `set_next_pc` at the original publication sites; spilled on every return; incoming value retained when reference leaves it untouched |
| `dispatches` (+396) | Existing local 7 tally, committed once at exit; profiling, not architectural instruction count |
| `svc`, `exit_reason` | Existing memory stores; SVC is recorded IR, not an in-body HLE callback; exit reason set after state publication |
| `fault_pc`, `fault_address`, `fault_write` | Existing memory fields; fault location comes from memory-op arg0, never from the promoted pending PC |
| `stop_flag`, `smc_dirty`, code-page probes | Remain live memory reads at all existing poll points; never promoted or hoisted by this option |
| `mem_fast_reads/writes`, `tx_wasm` | Profiling counters remain memory-backed; their traffic is not claimed as a semantic-state reduction |
| SVC / HLE / host inspection / context save / process exit | Region returns through the common epilogue before the host or callback observes state |
| Fault | Recover faulting T/E/IT, preserve current flags/IR effects, charge only earlier completed segments, then epilogue; host still sets regs[15]=fault_pc; next_pc keeps reference semantics |
| Budget / Stop / Smc / true Miss / cached transfer | Original pending PC and reason, all enabled locals published by the common epilogue |
| Unsupported IR / invalid terminal | Reject the entire emission; no partially generated guest execution |

Checked `mem_read`/`mem_write` imports are explicitly **non-observers**:
`memory_slow_call` goes through `before_checked_memory_helper`. The actual
helpers use memory_cookie, memory_value, fault and SMC fields (plus process
profiling counters), and cannot inspect/mutate cached CPU state or invoke
CPU/HLE/debug/context callbacks. Thus they need no flag/PC flush or reload,
including on fast-path fallbacks. A failing helper branches to the shared
fault epilogue after mode recovery. Guest mappings must remain disjoint from
JitState storage, as already required for the existing GPR cache.

Any future helper that observes or modifies CPU state must terminate the
region and run after the epilogue (as SVC does), with subsequent execution
reentering through `load_region_state`. Do not reuse the non-observer import
path for it. A future in-region full-CPSR writer also requires an explicit
split/update or materialize/write/reload operation before adding its opcode
to the whitelist; it is currently rejected. Asynchronous context observation
inside a call is not supported by the existing single-worker ABI.

### Tick, continuation and invalidation invariants

Normal terminals add the entire block's CycleCount once. Condition-failed
paths add ConditionFailedCycleCount once. Continued-store fallthrough adds
nothing: side exits add their completed prefix once, while later faults add
only `completed_store_ticks` (the earlier completed store-delimited prefix).
No prefix is subtracted/re-added at materialization. Faulting segments and
partial multi-access instructions retain the reference counting convention.

Entry and linked-edge admission still compare executed_call plus successor
cost with budget; continuation checks use the cumulative next-segment end.
M16 still clamps slices to 131072, checks returned deltas and handles exact
exhaustion, non-divisible slices and zero progress. Arbitrarily large direct
`run` budgets should use the same slice bound: this patch does not change the
reference's unsigned addition guards. The host's 64-bit total and wrapping
32-bit delta logic are unchanged.

No SMC mechanism, region formation, guest memory mapping or dispatcher was
redesigned. Successful code-page stores still mark smc_dirty; existing
continuation/loop/edge exits reach the epilogue before returning. M16 reads
smc_dirty before chaining a Miss/Budget and the host normalizes pending SMC on
other reasons, including Budget-before-loop-poll paths. Epoch lookup occurs
between fully materialized calls. Local promotion cannot hide stop/SMC flags.

Static review also noted a **pre-existing** discrepancy: the public
`WasmJitCPU::invalidate_jit_cache` region-erasure loop does not bump
`dispatch_epoch`, unlike `clear_regions`, `clear_regions_for_page`, LRU and
byte-validation eviction. This candidate leaves that reference behavior
unchanged; explicitly validate external invalidation and shared-table slot
reuse before drawing correctness conclusions. The new matrix tests include
stale-epoch lookup and mixed-policy transfers, but do not establish this
pre-existing external invalidation path correct.

### Expected structural effect (not a WAT measurement)

Source-level replacements for P:

```text
flag read: OLD load cpsr; shift; mask       NEW local.get N/Z/C/V
NZ write:  OLD load cpsr; merge; store      NEW extract SSA N/Z; local.set N/Z
NZC write: OLD load cpsr; merge; store      NEW local.set N/Z/C (V untouched)
mode/IT:   OLD load cpsr; mask/OR; store    NEW local.get/set other_psr
```

All four architectural NZCV setter opcodes share one changed lowering site;
the two non-NZCV update sites and common condition/carry reader also route
through RegionState. For P, those repeated CPSR memory operations disappear
by source construction, replaced by one entry load, one exit store, and a
store+load at each full CPSR read. The candidate still packs/unpacks IR flag
SSA values and adds local operations; fewer memory accesses do not prove a
speedup or lower native register pressure.

For K, location/IR/BX PC stores and generic/exit PC loads become local.set/get;
next_pc publication becomes local.set. Two entry loads and two exit stores
replace those sites' memory accesses. Tick/dispatch totals were already
local, so no new reduction in their hot-body traffic is claimed. Profiling
memory counters remain untouched. No generated Wasm/WAT or dynamic counts
were produced here; the earlier hot-region census must be repeated on this
exact revision to reconcile traffic with the current source.

### Required validation in the real environment

1. Build this source with reference defaults, then enable P/K/PK using the
   same binary (or equivalent separately configured builds). Disable ablations.
2. Run backend tests in fresh A/P/K/PK processes; direct region tests also
   exercise an explicit four-policy matrix in each process. Check counters,
   continuation prefixes, 0/1/small/non-divisible budgets, SVC/fault/stop/SMC,
   true misses, epoch invalidation and mixed-policy M16 transfers.
3. Run the native emitter fixture generator and `wasmjit_emitter_test.mjs`.
   It validates candidate modules and compares complete state and memory
   images with reference regions for the existing input corpus and added
   flag/SVC/fault cases. These test changes are **unexecuted** here.
4. Run the existing InterpreterCPU differential integration suite in all four
   modes, then genuine exit-42 and display fixtures including HLE context
   visibility, ARM/Thumb/IT, Q/GE preservation, SMC and repeated invocation.
5. Only after correctness checks, run repeated interleaved uncapped browser
   benchmarks and the hot-region WAT/native census. Compare CPSR, PC, tick and
   profiling traffic separately; record browser/version/options and raw data.

Compilation, generated-Wasm validity, flag differential correctness, fixture
correctness, browser execution, performance, FPS and MIPS remain unestablished.

### Source-only review record

Starting revision: `ba153401e5848b5fe5c2774a4db3c62f801568c1`, initially clean.
The existing Task #10/#11 ablation controls were retained, default-off; no
experimental patch was reverted and no fixture-specific selection was added.
The external Dynarmic source/dependency directory is absent in this checkout;
the review traced the checked-in frontend wrapper, complete Wasm lowering,
backend helpers (including MemState copy paths) and test sources. Native
Dynarmic internals and actual emitted modules require the real environment.

After editing, a source search of `Emitter` through `emit_region` found zero
direct `offsetof(JitState, cpsr)`, zero `offsetof(JitState, next_pc)`, and zero
fixed R15-offset accesses outside RegionState. Variable-register access is
explicitly routed through the abstraction when index is 15. The M16
`emit_dispatch` function text matches the starting revision (ignoring line
endings). The four single-block `executed` initialization/completion/fault
store sites remain reference-only. Both region tick/dispatch commits reside
in the single exit materializer. These are structural source observations,
not generated-code or runtime results.

## JitState (emit_wasm.h) — append ONLY, existing offsets fixed

Existing layout (regs[16], cpsr, fpscr, svc, exit_reason, executed,
memory_cookie, fault_address, fault_write, memory_value[4], fpu[64],
tpidruro; sizeof 372) is unchanged. Append:

```cpp
uint32_t next_pc;        // +372 resume PC for Miss/Budget/Smc/Stop
uint32_t fault_pc;       // +376 guest PC of the faulting instruction (0 unknown)
uint32_t page_table_base;// +380 host offset of MemState page_table entries, 0=off
uint32_t page_perms_base;// +384 host offset of page permission bytes, 0=off
uint32_t smc_dirty;      // +388 host sets 1 after a store hits a code page
uint32_t stop_flag;      // +392 host sets 1 to request a return
uint32_t dispatches;     // +396 count of dispatch iterations (profiling)
uint32_t code_pages_base;// +400 host offset of code-page refcounts, 0=off
uint32_t mem_fast_reads; // +404 fast-path reads this call (host accumulates)
uint32_t mem_fast_writes;// +408 fast-path writes this call (host accumulates)
uint32_t smc_page;       // +412 code page that set smc_dirty
```

ExitReason extended: Continue=0, Svc=1, Fault=2, Unsupported=3, Miss=4,
Budget=5, Smc=6, Stop=7.

## Module export

`run(state: i32, budget: i32) -> i32` (single export; still signature "ii"
table-compatible is NOT required — the backend calls it via its own slot with
`vita3k_jit_run(slot, stateOffset, budget)`).

Contract:
- Begins at `regs[15]`; loops internally over blocks; NEVER falls through.
- `budget` = max ADDITIONAL executed ticks this call. Before executing a
  block or store-delimited segment whose emitted tick cost is T: if its
  completion would exceed budget, set next_pc to its PC and return Budget.
- `state.executed` accumulates monotonically across calls (host tracks
  deltas). The generated loop keeps the count in a Wasm local and commits it
  on exit; the value is still one tick per guest instruction, including
  condition-failed ticks.
- `state.dispatches` incremented once per dispatch-loop iteration.

## Dispatch loop (inside `run`)

```
loop $dispatch:
  dispatches++
  if (load state.stop_flag) { next_pc=pc; return Stop }
  if (load state.smc_dirty) { next_pc=pc; return Smc }
  ;; v1.2 light dispatch path: a statically-chained edge preloaded local 6
  ;; with the successor's CONSTANT block index (full LocationDescriptor match
  ;; at emission: PC + CPSR mode/IT + FPSCR mode bits). Only fresh entries
  ;; and non-member edges carry kLightDispatchSentinel and run the search.
  if (dispatch_index == kLightDispatchSentinel):
    pc = load regs[15]
    binary-search pc in the region's sorted entry table
      entry = {pc, psr_mask, psr_value, tick_cost, block_index}
    if not found OR ((load cpsr) & psr_mask) != psr_value:
      next_pc = pc; return Miss
    // v1.1: formation guarantees AT MOST ONE entry per guest PC per region, so
    // the binary search never needs to disambiguate same-PC entries. A PC
    // reached with a different PSR is simply not a member; dispatch Misses and
    // the host forms a separate region keyed at that full location.
  ;; v1.2: the budget check moved ONTO the chained edge (executed_call +
  ;; target.ticks > budget -> next_pc = pending target PC; return Budget), so
  ;; a budget-failing chained iteration is NOT counted as a dispatch. The
  ;; generic path still checks at its search leaf. br_table remains the only
  ;; block transfer; stop/smc polling stays per-iteration (2 loads).
  br block_label[block_index]   // br_table or nested ifs over block_index
```

`psr_mask/psr_value` come from each block's LocationDescriptor PSR: cover at
minimum the T bit and IT bits (and E bit) — caller (backend) computes them and
passes them via RegionMeta.

## Terminals

- `LinkBlock{target}`: if target (pc, psr-match) resolves to a block in this
  region → LIGHT PATH (v1.2): store the successor's constant block index and
  `br $dispatch` (PC reload + search skipped; PSR/FPSCR checks redundant by
  the emission-time full-Location match). Otherwise store next_pc=target.PC,
  `br $dispatch` (dispatch returns Miss to host).
- `If{then_, else_}` / `CheckBit{then_, else_}`: evaluate as today, then each
  arm follows the LinkBlock rule above.
- `ReturnToDispatch` → `br $dispatch`.
- `CheckHalt{then}`: preserve current emitter semantics for the halt bit; if
  halting, set next_pc and return Stop; else follow the inner terminal.

## Memory IR (restriction REMOVED)

Memory IR is accepted at ANY CycleCount. Fault handling inside a region:
- Helper returns nonzero → store fault_address/fault_write (helper does),
  store `fault_pc` = **arg0 of the faulting memory op** — VERIFIED by
  dumping real multi-instruction translations (/tmp/ir_dump): every A32
  memory IR op's first argument is the U64 location descriptor of ITS OWN
  guest instruction (str@0x1000 → WriteMemory32 #0x1000; ldr@0x1004 →
  ReadMemory32 #0x1004; both loads of one LDM share that instruction's PC).
  For fault accounting `executed` = ticks of instructions completed BEFORE
  the faulting one, return Fault.
- NO host-side register rollback: the emitter commits SetRegister in IR
  order, so JitState regs are as-of the faulting instruction. Stores earlier
  in the same multi-access instruction may have committed (unchanged
  semantics).
- Single-instruction `emit_block` path keeps today's observable behavior
  where tests depend on it.

## emit API

```cpp
struct RegionBlockMeta {
    uint32_t entry_pc, psr_mask, psr_value, ticks;
    std::vector<StoreContinuation> store_continuations;
};
std::vector<uint8_t> emit_region(
    const std::vector<const Dynarmic::IR::Block *> &blocks,
    const std::vector<RegionBlockMeta> &meta);   // same order
std::vector<uint8_t> emit_block(const Dynarmic::IR::Block &block); // = 1-block region
```

## Register cache and exit publication (current implementation)

R0..R14 have fixed Wasm locals shared by all region members. `run` loads
the union of registers read or written by any member once, before dispatch.
Write-only registers are initialized too: a conditional skip or an early
Stop/Smc/Budget/Miss exit must preserve their incoming values. Linked and
condition-failed edges neither flush nor reload these registers.

Every exit branches to one epilogue outside the dispatch loop. It stores
the union of registers any member can write, publishes executed ticks and
dispatch counts, and returns the exit reason. Faults use this same epilogue:
writes from earlier blocks and earlier IR in the faulting block survive;
unexecuted writes retain their incoming values. Checked memory helpers use
the memory, fault and SMC fields, and must not read or modify cached GPRs.
In reference mode PC, CPSR and extended registers still use their architectural
state fields. The optional candidate described above changes only PC/CPSR.

Reference local layout: 0=state, 1=budget, 2=executed_call, 3=dispatch PC,
4=CheckBit, 5=i64 scratch, 6=dispatch index (exit reason after leaving the
loop), 7=dispatch count, 8..10=memory bases, 11..25=R0..R14, 26..=SSA.
Single-block emission reserves locals 3..17 for R0..R14 and starts SSA at
18, preventing architectural registers from aliasing instruction results.

The exit label surrounds the dispatch loop, so existing `br_table` and
loop-back depths are unchanged. A body exit uses depth
`body_index + open_ifs + 1`; `open_ifs` includes memory-probe and helper-status
ifs as well as terminal and entry-condition ifs. Dispatcher exits also skip
the default label, all member labels and their search-tree ifs.

Region validation groups code ranges by page once during formation, with a
comparison span for every member's original bytes, including overlapping
blocks. Each subsequent entry fetches each page range into a reusable 4 KiB
scratch buffer and compares those spans without allocations or searches.
Permission and mapping checks still run on every entry, including after HLE
writes. Memory fast paths retain the probed page pointer for the actual
access rather than loading the same page-table entry twice.

## Store continuations (current implementation)

Region formation requests `StoreContinuation` metadata from `translate_block`, up to an explicit per-call cap (production default: 2, i.e. three store-delimited segments per block). Measured 2026-09-15: uncapped merging cuts dispatcher visits ~60% but its inline side exits raise per-call Wasm entry cost past the break-even point on the Node bench; the cap-2 default keeps roughly half the dispatch reduction near pre-continuation entry cost, and local browser testing showed +2 FPS over both the uncapped and the disabled variants (see `kDefaultMaxStoreContinuations` in `frontend.h`).
For unconditional blocks, an ordinary store no longer terminates translation.
At the next `PreCodeReadHook`, the frontend records the IR offset, cumulative
completed tick count and full continuation location. This hook runs after
the entire previous guest instruction, including all STM/VST1 elements,
writeback and Thumb IT advance. The metadata must remain paired with the
unmodified IR; it uses indices rather than pointers so block moves are safe.

The emitter inserts a side exit at each recorded boundary. Ordinary stores
fall through without PC/CPSR writes, tick/dispatch updates or `br_table`.
Stop and SMC flags are still polled after every completed store instruction.
On an exit, the boundary's PC/mode and completed ticks are published through
the shared region epilogue. SMC is handled before a subsequent instruction,
including when that instruction's bytes were overwritten in this same body.
The existing host invalidation path then recompiles the continuation.

Budget checks preserve the former store boundaries: entry checks only the
first segment's ticks; each continuation checks the cumulative cost through
the end of the next segment against the per-call budget. No ticks are added
on the fallthrough path. A normal terminal adds the full block count once;
a side exit adds its completed prefix once. On a memory fault, completed
segments before the faulting segment are counted, matching the former
separate-block accounting. Partial effects of a faulting multi-access
instruction retain the existing no-rollback semantics.

`meta.ticks` remains the conservative full-block cost for region-size limits.
Predicated blocks retain the original store-ending behavior and condition-fail
budget rules. Single-block translation and stepping do not request metadata
and retain their original boundaries. Final stores use the ordinary terminal;
metadata at a boundary where translation subsequently stops is discarded.
`dispatches` counts actual dispatcher visits, so continued stores reduce it.

## Original implementation design (v1; superseded above where noted)

**Wasm structure** (one function `run(state:i32, budget:i32) -> i32`, type
`(i32,i32)->i32`):
```
locals: 0=state, 1=budget, 2=executed_call, 3=pc, 4=CheckBit,
        5=i64 scratch, 6=dispatch index, 7..=per-block SSA words
body:
  executed_call = 0
  dispatch_index = kLightDispatchSentinel   ;; locals zero-init per call
  loop $dispatch:
    state.dispatches++
    if (load state.stop_flag) { next_pc=load pc; return Stop }
    if (load state.smc_dirty) { next_pc=pc; return Smc }
    ;; v1.2 light path: chained edges prewrite dispatch_index = CONST successor
    ;; block index; only the sentinel still runs the generic path below.
    if (dispatch_index == kLightDispatchSentinel):
      pc = load state.regs[15]
      ;; STATIC PC SEARCH: nested if/else tree over CONSTANT entry PCs
      ;; (entries are compile-time known — no runtime table/binary search).
      ;; Balanced tree: ≤9 comparisons for 512 entries. Each comparison:
      ;;   if (i32.lt_u $pc, CONST_MID) <left subtree> else <right subtree>
      ;; Leaf: pc == entry_pc → check (cpsr & mask) == value → block idx or Miss
      ;; Miss leaf: next_pc = pc; return Miss
      ;; Budget check per entry (ticks are static): 
      ;;   if (executed_call + CONST_TICKS) > budget → next_pc=pc; return Budget
      ;; v1.2: light iterations skip ALL of this — the edge already paid the
      ;; successor's budget check and wrote the constant index.
    br_table → $b0..$bN, default Miss-return
  ;; Block bodies: each AFTER its label's `end` in the nested-block chain:
  block $default  ;; default → return Miss
  block $bN ... block $b0
    br_table $b0 $b1 ... $bN $default (idx)
  end($b0) → BLOCK 0 BODY
  end($b1) → BLOCK 1 BODY  ;; bodies are nested in outer scopes — chaining
  ...                        ;; via br $dispatch is always in scope
  end($bN) → BLOCK N BODY
  end($default) → return Miss (br_table default target)
```

**SSA locals are REUSED per block**: reset `next_local = SSA_BASE (26)` before
each block's body. Blocks chain sequentially, never nest, so a block's SSA
values are dead at its terminal. First SSA index = 26; the declared local
count uses the maximum per-block SSA requirement.

**Per-block emission** (reuses existing instruction() machinery unchanged):
```
;; block entry (after br_table lands here):
;; IF conditional block: condition check;
;;   fail path: state.executed += CondFailTicks; location(fail_loc);
;;              v1.2: member fail target → LIGHT PATH (index const; budget
;;              check target.ticks); else br $dispatch   ;; NOT return —
;;              fail target may be in-region!
;;   pass path: fall through
;; body instructions (memory IR at ANY CycleCount now)
;; terminal:
;;   LinkBlock{target} in-region (pc matches a member AND that member's
;;     psr_mask/psr_value match the target descriptor):
;;       state.executed += CycleCount; location(target); v1.2 LIGHT PATH:
;;       budget-check target.ticks (Budget exit on fail); block_index = CONST;
;;       br $dispatch (PC reload + PC search skipped)
;;   LinkBlock out-of-region: state.executed += CycleCount;
;;       next_pc = target.PC; return Miss
;;   SVC path (CallSupervisor seen): state.executed += CycleCount;
;;       return Svc (pc already written by BranchWritePC)
;;   ReturnToDispatch/PopRSBHint/FastDispatchHint: state.executed +=
;;       CycleCount; return Continue
;;   CheckHalt{else_}: descend else_ (same rules as today)
;;   If/CheckBit arms: each arm follows the LinkBlock rule above
```

**Fault path change** (memory_call/checked_status in region mode):
- Helper nonzero → set fault_pc = arg0 of the faulting memory op (its low
  32 bits = the instruction PC — VERIFIED). Restore CPSR mode bits, including
  IT, from that full location descriptor while preserving arithmetic flags;
  earlier instructions in the block may have advanced IT. Do NOT touch state.executed
  (it holds ticks of all PREVIOUS blocks; the faulting block's earlier
  instructions are not counted — undercount ≤ block ticks, fault_pc exact);
  return Fault.
- NO rollback of any kind (region ABI).

**executed accounting summary**: adds happen ONLY at terminals/cond-fail
(state.executed += static ticks for the completed block; also
executed_call local += same for budget). Budget check at dispatch compares
executed_call + entry_ticks > budget. Fault/Svc/Continue/Miss/Stop/Smc all
leave state.executed exactly as accumulated by completed blocks.

**Module assembly**: same shape as emit_block but type section adds
`(i32,i32)->i32` for run; export name `run`. Single-block modules keep
export `block` and the old `(i32)->i32` shape (60-module suite green).

`emit_block` MUST keep passing the existing 60-module emitter suite
unchanged (same module shape: export `block(state:i32)->i32` — keep that
export name for single-block modules; regions export `run`). Region modules
MAY additionally export `block` aliasing the first block for table
compatibility, but the backend uses `run`.

Limits (scale existing): 4096 IR instructions PER BLOCK unchanged; region
cap 512 blocks, 32768 total ticks, 4 MiB module bytes. Empty vector =
unsupported, no partial module.

## Inline memory fast path (task #10, implemented) — verified against mem/state.h

`page_table_base` / `page_perms_base` / `code_pages_base` point at MemState's
fixed arrays (backend refreshes all three before EVERY run call; a zero base
disables the fast path and every access uses the checked helper):
- `page_table`: `unique_ptr<PagePtr[]>`, **1,048,576 entries × 4 bytes** (wasm32
  pointers), indexed by guest page. Sparse (browser) entries point at the
  page's own backing start, so the host address of guest byte `addr` is
  `i32.load(page_table_base + (addr>>12)*4) + (addr & 0xFFF)`. Under
  Emscripten `MemState::memory.get()` is null, the table is initialized to
  null, alloc fills live entries and free/trim nulls them — so a NULL entry
  is exactly an unallocated page (the allocator bitmap adds nothing).
- `page_permissions`: `unique_ptr<MemPerm[]>`, **1,048,576 × 1 byte**;
  `MemPerm : uint8_t` with Read=1, Write=2, Execute=4. mem_read requires Read
  on every touched page, mem_write requires Write.
- `code_pages`: backend refcount array (`g_code_pages`, 1,048,576 × 4 bytes);
  nonzero = page holds cached JIT code. Writes to such pages MUST use the
  checked helper so smc_dirty/invalidation stays exact.

Fast-path shape for a 1/2/4-byte access (probe ORDER matters: permission
first — it is the only check whose failure the checked path detects BEFORE
any mapping question, and it costs one byte load):
```
Branchless single gate (R3f): every probe load is provably in-bounds (the
page index is 20 bits into fixed 1M-entry/4M-byte arrays with proven or
guarded-nonzero bases), so all predicates evaluate speculatively with no
observable effect and exactly one hot branch remains:
```
page_ok = page != 0   // checked path rejects addr < host_page_size even
                      // when sparse backing was force-allocated at page 0
perm_ok = (i32.load8_u(page_perms_base + page) & required) == required
base    = i32.load(page_table_base + page*4)   // 0 = unmapped
in_page = (addr & 0xFFF) <= 4096 - size        // only cross-page path
code_ok = *not a store* or i32.load(code_pages_base + page*4) == 0
if !(page_ok & perm_ok & (base != 0) & in_page & code_ok) -> cold arm below
value   = i32.load8_u/16_u/load(base + (addr & 0xFFF))   // or matching store
++state.mem_fast_reads (or mem_fast_writes)
```
The enabled guard (any base zero -> fall back (5)) stays OUTSIDE the gate
when kept: region emission drops it under RegionStateOptions::assume_fast_bases
(set by the host only when all three bases are provably nonzero for the
module's whole lifetime: the arrays allocate once in MemState init and free
only at deinit, while regions compile and run strictly inside; single-block
emission and the default policy always keep it).

Cold arm (never taken in production: slow_* = 0 over tens of millions of
accesses): ONE slow-helper call with reason 5 (Other). The reason only
feeds process profiling counters (helpers mask it off before use), and a
nested exact re-derive costs ~5-6x cold code per access, which keeps hot
modules out of the optimizing tier and slows baseline execution (measured
2x on run_js_ms; --liftoff-only confirms bloated modules never tier up).
Exact reasons live behind VITA3K_WASMJIT_SLOW_REASONS=1 (Module prop first,
then process.env; native getenv), a strictly diagnostic shape covered by
running the backend/emitter/exit-42 suites with the flag set.
perm   = i32.load8_u(page_perms_base + page)
if (perm & required) != required -> fall back (reason 2)
base   = i32.load(page_table_base + page*4)
if base == 0 -> fall back (reason 1, unmapped)
if (addr & 0xFFF) > 4096 - size -> fall back (reason 3, cross-page)
// stores only: if i32.load(code_pages_base + page*4) != 0 -> fall back (4)
value  = i32.load8_u/16_u/load(base + (addr & 0xFFF))   // or matching store
++state.mem_fast_reads (or mem_fast_writes)
```
Alignment is NOT checked: Wasm unaligned access is a little-endian byte-wise
access, identical to mem_read/mem_write's per-page memcpy, and every A32/Thumb
load width lowers to a raw zero-extending ReadMemoryN (sign extension is a
separate IR op), so load8_u/load16_u preserve exact semantics. An unaligned
16-bit access at offset 0xFFF crosses a page and takes the checked fallback,
as do 32-bit accesses at offsets 0xFFD through 0xFFF.

Fallbacks call the imported helper with `bytes = size | reason<<8`
(1=unmapped, 2=perms, 3=cross-page, 4=code page, 5=other/disabled); the
helpers mask the reason off (`bytes & 0xff`) and account it in
process-lifetime counters (g_mem_slow_*). Fault semantics are unchanged:
helpers still validate the whole range, set fault_address/fault_write and
return 2. Fast successes increment state.mem_fast_reads/mem_fast_writes
(JitState), which the host accumulates and zeroes per call — they are
per-call scratch, NOT saved across fault rollbacks.

## Backend (implemented by parent)

- Region formation on miss: DFS from the missed descriptor following
  terminal LinkBlock targets, translating each with its own PSR, capped.
- One install per region; slot holds `run`.
- Cache: pc → Region* index (unordered_map), PSR validated by dispatch.
- Region-entry validation: byte-compare all member block code bytes
  (replaces per-block `unchanged`), plus smc_dirty early-exit, plus existing
  invalidate_jit_cache flush.
- Single-instruction retry path DELETED (memory IR unrestricted now).

## Instrumentation (already added by parent)

Per-phase ms (emit/install/run), js_calls (region entries), misses, svc,
fault, smc, stop, budget exits, dispatches, mem helper call counts.

## M16 Wasm-side multi-region dispatch pump

Steady-state region->region transfers stay inside Wasm. The host compiles
regions exactly as before; a small dispatcher module (one function
`dispatch(state, remaining, map_base, epoch_addr) -> ExitReason`, built once
by `emit_dispatch`) chains them through a host-shared funcref table:

```text
host -> dispatch -> region A -Miss-> lookup(B) -> region B -Miss-> ...
       -> host only on Svc/Fault/Stop/Smc/Budget-boundary/true-miss/overrun
```

Transfer lookup is an open-addressed guest-location-hash -> table-slot map
in linear memory (2048 x 16B entries `{key_lo, key_hi, slot, epoch}`,
empty = epoch 0). The key is the region-cache `UniqueHash(pc, cpsr, fpscr)`
recomputed in Wasm; the hash/index function is shared verbatim with the
host (`dispatch_map_index`). Entries carry the map epoch at insert; the
dispatcher only matches entries whose epoch equals `*epoch_addr`. The host
bumps the epoch on EVERY region eviction, so stale slots can never match;
released table slots are additionally nulled (a missed guard would trap
loudly instead of executing the wrong region).

Per pump iteration the dispatcher: checks `stop_flag`, clamps a
`REGION_CALL_TICKS` slice out of the remaining budget, `call_indirect`s the
slot, verifies the region reported no more ticks than its slice (else
`DispatchReason::RegionOverrun`, which the host fails like the equivalent
single-region overrun), and routes the reason. `Miss` with a mapped target
chains in-Wasm (`tx_wasm++`); unmapped targets return `Miss` with `next_pc`
published, and the host resolves-or-compiles through the unchanged
loop-top path. `Svc`/`Fault`/`Stop` pass through untouched so the host's
pre-switch SMC normalization and all exit handling behave exactly as in
host-driven execution. `Smc` with pending dirt routes to the host
normalizer rather than chaining into a stale region. `Budget` returns to
the host at exhaustion/no-progress boundaries only, so the existing
budget-exhaustion-fail and clean-slice semantics (and their tests) are
unchanged; internal slice continues stay in the pump.

`Miss` intentionally keeps its single meaning ("next_pc published, resolve
or compile"); the compile-vs-cached distinction lives on the host, which
holds the authoritative `region_cache` (the Wasm map is a hint cache: on a
dispatcher Miss the host looks up the full key, refreshes the map entry on
a hit, or forms/compiles/installs/inserts on a true miss).

Termination mirrors the host loop: `left == 0` returns `Budget` (the host
fails on full consumption, as before), and a `Budget` return with no
forward progress since pump entry returns `Budget` (the host reports the
clean slice boundary). No exact-zero reliance: all comparisons are
`>=`/`==`-on-empty, and slices are clamped, never assumed to divide the
total. `JitState.tx_wasm` counts chained transfers per call (host
accumulates like `dispatches`); `host_miss` counts dispatcher Miss returns
(~all resolve to compiled regions at the loop top; true compiles remain
`region_misses`).
