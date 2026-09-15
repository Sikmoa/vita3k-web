# M14c region JIT ABI (parent-owned spec, v1.2)

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
uint32_t code_pages_base;// +400 host offset of code-page refcounts, 0=off
uint32_t mem_fast_reads; // +404 fast-path reads this call (host accumulates)
uint32_t mem_fast_writes;// +408 fast-path writes this call (host accumulates)
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

**SSA locals are REUSED per block**: reset `next_local = SSA_BASE (6)` before
each block's body. Blocks chain sequentially, never nest, so a block's SSA
values are dead at its terminal. Max locals = 6 + max per-block words.

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
page   = addr >>> 12
if page == 0 -> fall back (checked path rejects addr < host_page_size)
if any of page_table_base, page_perms_base, code_pages_base is zero -> fall back (5)
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
