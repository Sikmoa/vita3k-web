# Wasm emitter tests and interface

The emitter implementation is `../src/wasmjit/emit_wasm.{h,cpp}`. It consumes **actual
Dynarmic A32 IR**, not ARM bytes. The native fixture generator additionally uses
Dynarmic's real ARM/Thumb translator; the JavaScript test executes raw emitted
modules in Node's Wasm engine. It does not emulate Wasm or ARM.

## Current coverage and validation

The shared JIT/AOT emitter now accounts for all 668 generic/A32 opcode entries,
with explicit unsupported native callback operations. See
[IR_COVERAGE.md](../src/wasmjit/IR_COVERAGE.md) for the scope, FP helper strategy,
ABI changes and remaining runtime restrictions.

The core emitter suite has been built and executed in Node, including the
reference/P/K/PK region variants at two state offsets. Extended FP operations
execute the real portable helper compiled to Wasm. The suite compares complete
state/memory, verifies module structure, and checks canaries and OOB traps.
The external vitaslop corpus was explicitly omitted with `--core-only`.
Full backend/browser integration and retail benchmarks were not rerun for
this change; their separate recipes remain in SCRIPTS.md and REGION_ABI.md.

## Run from repository root

The build and run commands are in [SCRIPTS.md](../../../SCRIPTS.md) ("Emitter
fixture suite"): a standalone native Dynarmic (x64 backend included, for the
vitaslop oracle), the generator linked against it, then Node on the fixtures.
Dynarmic/mcl headers require exceptions enabled even though ordinary emitter
rejection uses an empty vector.

Historical pre-candidate counts: 78 deterministic modules, 21,432 input/expected-state pairs,
42,856 successful **Wasm `call_indirect`** calls and 8 region calls at two
nonzero state offsets. The fixture generator serializes the complete JitState;
the harness derives the state and canary sizes from those arrays.
These are prior results, not results for the current patch; module/case counts
now increase with the policy matrix. The previous generator/emitter was also run with UndefinedBehaviorSanitizer
(`-fsanitize=undefined -fno-sanitize-recover=undefined`) without findings.
Coverage includes:

- Real ARM/Thumb MOV, ADD, SUB, CMP, taken/untaken BNE; ARM MOVS-register and
  predicated MOV; SVC (including post-instruction PC); BX in both directions.
- The M14b NEON memset loop, from the VitaSDK fixture: Thumb `vdup.32 q8,lr`
  broadcast into all four Q lanes; `vdup.32 d16,lr` neighbour preservation;
  `vst1.32 {d16-d17},[ip]!` element stores at the guest buffer (never the
  block's own code) with ip writeback +16; `cmp`/`bne` loop terminator with
  NZCV and taken/not-taken targets. Stores are verified in guest memory.
- The fixture's VFPv3/64-bit memory sites, with the fixture's own encodings:
  `vpush {d8}` and `vstr d8,[r4,#176]` (GetExtendedRegister64 words through
  two checked stores), `vldr d8,[pc,#140]` and `vpop {d8}`/`vpop {d8-d11}`
  (checked loads seeded in guest memory, packed and committed to the right
  D-register words, sp writeback), and `strd r5,r9,[r4,#20]` (a single
  8-byte WriteMemory64 whose two `memory_value` words the helper must
  reassemble). Pre-seeded loads and post-store guest words are both checked.
- Add/sub carry-in 0/1, carry/no-borrow and signed overflow, edge values and
  seeded random inputs. Pseudos are read after registers/CPSR have been changed.
- LSL/LSR/ASR/ROR/RRX result/carry and NZ; counts 0..256 (U8 truncation), all
  boundary distinctions at 0, 31, 32, 33 and multiples of 32.
- LSR64 with register counts 0..256 and immediate boundary counts: shifts of
  64 or more produce zero, including both result words.
- Inline 8/16/32-bit reads and writes with every table base populated or one
  missing; distinct guest/backing values and counters verify the chosen path.
- Upper-half guest addresses and accesses crossing the end of test memory
  fault through the JS helpers, which normalize signed Wasm i32 arguments.
- Region read/write faults inside ITT EQ preserve arithmetic flags and prior
  instructions while recovering the faulting slot's IT state and PC metadata;
  condition-failed paths skip both slots and advance IT normally.
- All 15 supported conditions with all 16 NZCV combinations, separately at
  block entry and terminal; entry conditions are not rechecked after a write.
- Identity, scalar bit operations, packed NZCV, selects, narrowing, and the
  4096-IR-instruction local allocation boundary.
- Whole-state comparisons (including preservation of Q/GE/FPSCR), canaries,
  exact module import/export validation, table insertion, out-of-bounds traps.
- Native fail-closed checks: unsupported/dead IR, memory/exception IR from the
  real translator, invalid/interpret/check-bit terminals, unsupported terminal
  even on an unreachable branch, malformed locations, bad pseudo producer,
  excessive size/depth, missing condition-failure metadata, unsafe SVC shape;
  vector/64-bit register selection with S or Q registers (RegNumber alone
  cannot distinguish them, so anything but an explicit D/Q choice is
  rejected rather than mis-lowered).

These tests isolate emission. CPUInterface and browser-Worker integration are
covered separately by `browser/tests/wasm_jit_tests.cpp` and `jit_smoke.mjs`.

Register-cache regression coverage includes single-block architectural/SSA
local isolation. `wasmjit_backend_test.cpp` additionally covers linked and
condition-failed region edges, entry at an interior member, early exits,
later-block faults with and without memory probes, and code validation for
overlapping blocks across a page boundary. These added cases have not been
run in the environment used for the register-cache fix.

Store-continuation cases in `wasmjit_backend_test.cpp` cover two ordinary
stores in one dispatcher visit, checked and inline memory paths, budgets
0..4, post-index writeback, SMC at an exhausted budget, all elements of STM,
faults after a completed store, Thumb continuation PCs, and preservation of
predicated/single-instruction boundaries. They are added for execution on a
machine with the toolchain; tests and builds were not run for this change.

Scalar float-to-integer conversion (`wasmjit_f64_tests.inc`: `float_to_int32`,
`float_to_uint32`, extended `mode_guards`, `guest_float_to_int`) covers the
observed Limbo frontier past VMUL.F64: independent IR fixtures over zeros,
halves, nearest-even ties, exact 2^31/2^32 boundaries, infinities,
quiet/signaling NaNs, denormals with and without FZ, DN indifference, sticky
FPSCR preservation, trap-enable bails with zero drift, fail-closed scaled
forms and explicit VCVTA/P/M rounding modes, plus real ARM/Thumb VCVT[S][R]
encodings (S32/U32, F32/F64 sources) through the translator and dispatcher in
both block and region modes. Added for execution on a machine with the
toolchain; tests and builds were not run for this change.

## vitaslop conformance import

The generator (`wasmjit_emitter_test.cpp`, `vitaslop::` section) imports the
vitaslop ARM/NEON conformance corpus (`.limbo_work/vitaslop/projects/
vitaslop-conformance-suite-arm/cases/`, overridable via the generator's
optional second argv) into this fixture suite, so every case runs through OUR
Dynarmic-IR -> Wasm emitter and executes in Node. Each case file is split by
`# --- generated by regen ---` into a human top (description/`asm`/seed) and
a machine bottom (assembled bytes + qemu golden). The importer reads `mode`
and `[in].regs` from the top but NEVER the `asm` text; `[bin].base64`,
`[out.regs]` and `[out.flags]` come strictly from below the marker (a case
missing its generated half aborts the generator: an error, not a skip). The
hand-rolled TOML-subset reader adds no dependencies.

Each case loads at the same 0x1000 base as the existing corpus, translates
through the real Dynarmic A32 translator, and becomes a `vitaslop_<stem>`
reference + P/K/PK fixture exactly like existing entries: seeded input regs
over zeroed state with cleared flags and a clear FPSCR (sp gets scratch
0x5000, which no golden captures), expected integer regs from the golden
(unlisted = 0, following the existing sp/pc convention) and expected NZCV from
`[out.flags]`. The goldens do not record CPSR.Q, the GE bits or FPSCR (QC and
the cumulative exception flags); those come from running the same case on
Dynarmic's own x64 backend (`DynarmicOracle`), whose registers and NZCV must
first equal the golden. NEON cases round-trip vectors through integer regs
via vmov; a small IR dataflow tracker fills the fpu scratch words the Wasm is
known to write (vmov seeds evaluated from input regs and in-block register
writes, ALU results constrained by their golden readback, cross-checked where
both apply). Any written word left uncovered, or any unexpected shape, aborts
loudly instead of guessing zero.

Skip rule: only `capture = "output"` programs (svc/host-output, e.g.
`hello`) are skipped; everything else must fixture or gap-report.

Coverage gaps are never silently skipped. Whole-emitter gaps (block and every
region policy reject) are pinned fail-closed with `reject_block` and
reported as `VITASLOP_GAP ir-coverage` with guest bytes and IR ops. Two
further shapes are reported, not fixtured: `needs-runtime-split` (the block
path rejects by design, e.g. multi-tick memory blocks the runtime splits,
while the region path lowers them through store continuations) and
`multi-block-program` (loops/IT splits whose whole-program golden cannot
live in one single-block fixture). The generator's `vitaslop import:` line
reports the current fixture, skip and gap counts.

Pinning future retail crashes as cases: mirror the corpus split discipline.
Add one file per crash with a human top only — description of the crash,
minimal `asm`, `mode`, `[in].regs` seed — then run the corpus `regen` (as +
qemu oracle) to write the machine bottom. Never hand-author a golden and
never let the importer depend on the `asm` text; rebuild the generator,
regenerate fixtures, and run the Node suite to green.

## ABI / integration

`vita3k::wasmjit::emit_block(const Dynarmic::IR::Block&)` returns raw Wasm bytes,
**empty on unsupported input**. It neither executes guest code nor invokes
helper callbacks. No exception is used for ordinary rejection.

The standard-layout wasm32 `JitState` is 492 bytes; Memory64 widens host-address
fields. `emit_wasm.h` defines the current layout. Its original fields in order are
`regs[16]`, `cpsr`, `fpscr`, `svc`, `exit_reason`, `executed`,
`memory_cookie`, `fault_address`, `fault_write`, `memory_value[4]`,
`fpu[64]` and `tpidruro`, followed by `next_pc`, `fault_pc`, `page_table_base`,
`page_perms_base`, `smc_dirty`, `stop_flag`, `dispatches`, `code_pages_base`,
`mem_fast_reads`, `mem_fast_writes`, `smc_page` and `tx_wasm`. Later fields include dispatch/accounting metadata and the six
private `fp_arguments` words. Static asserts pin the wasm32 layout.
Extended registers overlay `fpu` exactly as the x64 backend's MJitStateExtReg:
Sn is word n, Dn words 2n/2n+1, Qn words 4n..4n+3. The emitter uses
`offsetof`, not native Dynarmic state layout. Export `block(i32 stateOffset)`
returns i32. Imports are `env.memory` (unshared, minimum one page, no required
maximum) plus the checked helpers `env.mem_read(state, address, bytes)` and
`env.mem_write(state, address, bytes)`, both `(i32,i32,i32)->i32` returning
0 on success and 2 on fault (the host then sets `fault_address`/`fault_write`).
Reads zero all four `memory_value` words then fill the addressed guest bytes
little-endian; writes consume bytes across all four words, so 8-byte
transfers use two words. Stores publish the value to `memory_value` **before**
calling `mem_write` (which reads it back); the value words remain in the
state after a successful block. On a faulting helper the module stores
`executed=0`, `exit_reason=2` and returns 2; the parent restores its
pre-block state snapshot (which contains the fault fields) and re-executes
the block interpretively. There is no private memory, data segment, start
function or table. The `env.fp64(state, operation, fpscr)` import implements
exact FP operations and returns exception flags; see IR_COVERAGE.md.

Every invocation overwrites `svc`, `exit_reason`, and `executed`. Execution
returns to host after **one** block, even for LinkBlockFast. `regs[15]` is the
next guest PC. Continue is 0; Svc is 1. Fault=2 and Unsupported=3 are available
for the parent; the emitter does not fake execution by emitting those results.
SVC returns after the frontend PC write, records its immediate in `svc`, and
leaves callback handling to the parent. OOB state addresses trap; the parent
must supply a valid state allocation. Blocks containing memory ops must have
`CycleCount()==1` (the runtime splits memory instructions into single-
instruction blocks, so the whole-block tick budget stays exact).

**Budget contract:** translation must charge exactly one tick per instruction.
`CycleCount()`/`ConditionFailedCycleCount()` are written to `executed` (not
accumulated). Parent must bound translation by the remaining instruction budget
and check it between block calls; cached blocks exceeding that budget cannot
be invoked as-is. There is no mid-block interrupt/budget check. Cache lookup
must match the full frontend location/mode; entry state must match that mode.

## IR support

The authoritative implementation is the enum switch in `emit_wasm.cpp`.
[IR_COVERAGE.md](../src/wasmjit/IR_COVERAGE.md) describes the supported portable
families and native callback exclusions. Invalid operand shapes still reject
emission, even when the opcode itself has a lowering. Guest memory accesses
retain their checked helper or guarded inline paths.

The following terminal/location notes describe the original block interface;
region and AOT contracts are documented in REGION_ABI.md and AOT.md.

Terminals: `LinkBlock`, `LinkBlockFast`, recursive `If`; `ReturnToDispatch`,
`PopRSBHint`, `FastDispatchHint` require an explicit PC write. `CheckHalt` is
accepted **only** when its else-terminal is one of these dispatcher returns:
both outcomes return to host without further guest-state changes, so no halt
field is necessary. No link/If terminal is allowed after SVC. `Interpret`,
`Invalid`, `CheckBit`, other CheckHalt shapes fail.

Conditions EQ..AL are lowered; NV fails. Nonzero IT state at block entry,
misaligned descriptor PCs, and descriptor FPSCR-mode changes fail (Thumb
locations reached through a terminal may carry IT state, which the frontend
advances). Modules are bounded to 4096 IR instructions, 4096 guest ticks,
terminal depth 16 and 256 visited terminal nodes. All values use locals;
addresses and module bytes are independent of native allocation addresses.
