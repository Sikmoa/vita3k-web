# M14c region JIT ABI (parent-owned spec, v1.1)

Goal: one WebAssembly module per REGION (many guest basic blocks) with an
in-module dispatch loop. No JS crossing and no C++ cache work per guest block.
Only region-level exits return to the host.

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
  block whose emitted tick cost is T: if executed_this_call + T > budget,
  set next_pc=that block's PC and return Budget.
- `state.executed` accumulates monotonically across calls (host tracks
  deltas). Incremented exactly as today (one tick per guest instruction,
  including condition-failed ticks).
- `state.dispatches` incremented once per dispatch-loop iteration.

## Dispatch loop (inside `run`)

```
loop $dispatch:
  dispatches++
  if (load state.stop_flag) { next_pc=pc; return Stop }
  if (load state.smc_dirty) { next_pc=pc; return Smc }
  pc = load regs[15]
  binary-search pc in the region's sorted entry table
    entry = {pc, psr_mask, psr_value, tick_cost, block_index}
  if not found OR ((load cpsr) & psr_mask) != psr_value:
    next_pc = pc; return Miss
  // v1.1: formation guarantees AT MOST ONE entry per guest PC per region, so
  // the binary search never needs to disambiguate same-PC entries. A PC
  // reached with a different PSR is simply not a member; dispatch Misses and
  // the host forms a separate region keyed at that full location.
  if executed_this_call + tick_cost > budget: next_pc = pc; return Budget
  br block_label[block_index]   // br_table or nested ifs over block_index
```

`psr_mask/psr_value` come from each block's LocationDescriptor PSR: cover at
minimum the T bit and IT bits (and E bit) — caller (backend) computes them and
passes them via RegionMeta.

## Terminals

- `LinkBlock{target}`: if target (pc, psr-match) resolves to a block in this
  region → emit a DIRECT `br` to it (block chaining, no dispatch). Otherwise
  store next_pc=target.PC, `br $dispatch` (dispatch returns Miss to host).
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
};
std::vector<uint8_t> emit_region(
    const std::vector<const Dynarmic::IR::Block *> &blocks,
    const std::vector<RegionBlockMeta> &meta);   // same order
std::vector<uint8_t> emit_block(const Dynarmic::IR::Block &block); // = 1-block region
```

## Implementation design (v1, worked out — follow this)

**Wasm structure** (one function `run(state:i32, budget:i32) -> i32`, type
`(i32,i32)->i32`):
```
locals: 0=state, 1=budget, 2=executed_call, 3=pc, 4=CheckBit,
        5=i64 scratch, 6..=per-block SSA words
body:
  executed_call = 0
  loop $dispatch:
    state.dispatches++
    if (load state.stop_flag) { next_pc=load pc; return Stop }
    if (load state.smc_dirty) { next_pc=load pc; return Smc }
    pc = load state.regs[15]
    ;; STATIC PC SEARCH: nested if/else tree over CONSTANT entry PCs
    ;; (entries are compile-time known — no runtime table/binary search).
    ;; Balanced tree: ≤9 comparisons for 512 entries. Each comparison:
    ;;   if (i32.lt_u $pc, CONST_MID) <left subtree> else <right subtree>
    ;; Leaf: pc == entry_pc → check (cpsr & mask) == value → block idx or Miss
    ;; Miss leaf: next_pc = pc; return Miss
    ;; Budget check per entry (ticks are static): 
    ;;   if (executed_call + CONST_TICKS) > budget → next_pc=pc; return Budget
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

**SSA locals are REUSED per block**: reset `next_local = SSA_BASE (6)` before
each block's body. Blocks chain sequentially, never nest, so a block's SSA
values are dead at its terminal. Max locals = 6 + max per-block words.

**Per-block emission** (reuses existing instruction() machinery unchanged):
```
;; block entry (after br_table lands here):
;; IF conditional block: condition check;
;;   fail path: state.executed += CondFailTicks; location(fail_loc);
;;              br $dispatch   ;; NOT return — fail target may be in-region!
;;   pass path: fall through
;; body instructions (memory IR at ANY CycleCount now)
;; terminal:
;;   LinkBlock{target} in-region (pc matches a member AND that member's
;;     psr_mask/psr_value match the target descriptor):
;;       state.executed += CycleCount; location(target); br $dispatch
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
  32 bits = the instruction PC — VERIFIED); do NOT touch state.executed
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

## Inline memory fast path (task #10) — verified against mem/state.h

`page_table_base` / `page_perms_base` point at `MemState`'s fixed arrays:
- `page_table`: `unique_ptr<PagePtr[]>`, **1,048,576 entries × 4 bytes** (wasm32
  pointers), indexed by guest page. Sparse (browser) entries point at the
  page's own backing start, so the host address of guest byte `addr` is
  `i32.load(page_table_base + (addr>>12)*4) + (addr & 0xFFF)`. A null entry
  means unmapped.
- `page_permissions`: `unique_ptr<MemPerm[]>`, **1,048,576 × 1 byte**;
  `MemPerm : uint8_t` with Read=1, Write=2, Execute=4.

Fast-path shape for an aligned 1/2/4-byte access:
```
page   = addr >>> 12
perm   = i32.load8_u(page_perms_base + page)
if (perm & required) != required -> slow helper
base   = i32.load(page_table_base + page*4)
if base == 0 -> slow helper
if (addr & 0xFFF) + size > 4096 -> slow helper   // cross-page
value  = i32.load(base + (addr & 0xFFF))          // or i32.store
```
Both arrays exist for the MemState's lifetime, but the HOST refreshes the
two JitState base fields before every region run (2 stores) — no generation
tracking needed for the bases. 8-byte (i64) and 16-byte accesses, and any
unaligned access crossing a page boundary, always take the slow helper.
Slow helper keeps today's checked semantics: whole-range preflight,
permissions, fault_address/fault_write, LE lanes in memory_value.

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
