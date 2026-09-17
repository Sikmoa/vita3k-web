# M14 Wasm emitter prototype tests and interface

The emitter implementation is `../src/wasmjit/emit_wasm.{h,cpp}`. It consumes **actual
Dynarmic A32 IR**, not ARM bytes. The native fixture generator additionally uses
Dynarmic's real ARM/Thumb translator; the JavaScript test executes raw emitted
modules in Node's Wasm engine. It does not emulate Wasm or ARM.

## Promoted-state candidate: validation pending

The optional implementation and all new tests are **implemented but unvalidated
in this source-only environment**. No build, generated-Wasm execution, tests or
benchmarks were run. See [REGION_ABI.md](../src/wasmjit/REGION_ABI.md#optional-promoted-region-state-candidate-september-2026)
for the state ABI, switches, access audit, limitations and required validation.

The existing generator/harness now emits an explicit reference region and
P/K/PK variants for every input corpus. Single-block architectural expectations
remain checked through `block`; their additional region cases compare against
reference `run`, since its cumulative accounting and exit ABI differ. Existing
explicit region expectations are also checked. Each candidate receives the same
fresh input and guest memory at both nonzero state offsets; return reason and
the complete test memory image (including all JitState bytes) are compared.
Module validation, imports/exports and OOB-state traps are checked per variant.
The reference region is always emitted with `{false, false}`, independent of
environment switches. These are source tests, not reported test results.

New/extended unexecuted coverage:

- The existing arithmetic random/edge corpus, carry/overflow SSA lifetime,
  LSL/LSR/ASR/ROR/RRX counts 0..256, all EQ..AL conditions over all NZCV
  combinations, selects, full CPSR reads, ARM/Thumb branches and IT faults,
  memory probes, supported VFP/NEON data paths, now as A/P/K/PK regions.
- Translated ADD/ADDS, ADC/ADCS, SUB/SUBS, SBC/SBCS, RSB/RSBS, CMP, CMN,
  AND/ANDS, EOR/EORS, ORR/ORRS, MOV/MOVS and MVN/MVNS immediately followed
  by SVC, over zero/negative/overflow/carry inputs and all incoming flags.
- NZ-only writes preserving C/V, later carry consumption and full CPSR read;
  ADDS immediately before read/write fault with a nonzero next_pc sentinel.
- Existing rejection corpus for all four region policies, preserving the
  unsupported opcode/terminal boundary. This adds no multiply, saturation,
  Q/GE-write or FPSCR-write instruction support.
- Backend region regressions, store continuations and M16 pump cases under
  all four explicit policies: budgets 0/1/below/equal/above block cost,
  non-divisible/exact exhaustion, zero progress, SVC/fault, stop, SMC, miss,
  stale epoch, wraparound counters and continuation prefixes.
- Cross-member flag production/conditional consumption/NZ-only update/full
  read/SVC; flags before a continuation's Budget/Stop/Smc exit; mixed-policy
  M16 carry production/consumption, true Miss and resumed invocation.

In the real environment, use the commands below to build/generate/run the
emitter suite. Then run the existing backend and InterpreterCPU integration
test entry points in four fresh processes, setting both independent switches:

| Mode | `VITA3K_WASMJIT_PROMOTE_FLAGS` | `VITA3K_WASMJIT_PROMOTE_ACCOUNTING` |
| --- | --- | --- |
| A | 0 | 0 |
| P | 1 | 0 |
| K | 0 | 1 |
| PK | 1 | 1 |

Unset `VITA3K_ABLATE`/`VITA3K_ABLATE_PC`. CPU integration tests use the process
selection, while direct emitter/backend cases explicitly exercise the matrix.
Run genuine exit-42 and display fixtures in each mode before benchmarking;
repeat the hot-region WAT census on exactly that build. Record profile option
fields so results cannot be mistaken for another policy. The public external
cache-invalidation epoch discrepancy noted in REGION_ABI.md has a source fix
in the Memory64 patch; that fix remains unbuilt and unvalidated. A stale-epoch
unit case is not a proof that every host eviction path bumps the epoch.

## Run from repository root

Requires the existing native Dynarmic/mcl/fmt archives (paths may differ with
build configuration), a C++20 compiler, and Node. Dynarmic/mcl headers require
exceptions enabled even though ordinary emitter rejection uses an empty vector
(the frontend CMake target already propagates `-fexceptions` for Emscripten).
No new dependencies or CMake changes are needed. Compile serially on small hosts:

```sh
c++ -std=c++20 -O1 -Wall -Wextra -Werror \
  -Iexternal/dynarmic/src \
  -Iexternal/dynarmic/externals/mcl/include \
  -Iexternal/fmt/include -Iexternal/boost \
  -Ivita3k/mem/include \
  vita3k/cpu/src/wasmjit/emit_wasm.cpp \
  vita3k/cpu/tests/wasmjit_emitter_test.cpp \
  build/native/external/dynarmic/src/dynarmic/libdynarmic.a \
  build/native/external/dynarmic/externals/mcl/src/libmcl.a \
  build/native/external/fmt/libfmtd.a \
  -o /tmp/wasmjit-emitter-test
/tmp/wasmjit-emitter-test /tmp/wasmjit-emitter-fixtures
node vita3k/cpu/tests/wasmjit_emitter_test.mjs /tmp/wasmjit-emitter-fixtures
```

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

## ABI / integration

`vita3k::wasmjit::emit_block(const Dynarmic::IR::Block&)` returns raw Wasm bytes,
**empty on unsupported input**. It neither executes guest code nor invokes
helper callbacks. No exception is used for ordinary rejection.

The standard-layout `JitState` is 420 bytes. Its original fields in order are
`regs[16]`, `cpsr`, `fpscr`, `svc`, `exit_reason`, `executed`,
`memory_cookie`, `fault_address`, `fault_write`, `memory_value[4]`,
`fpu[64]` and `tpidruro`, followed by `next_pc`, `fault_pc`, `page_table_base`,
`page_perms_base`, `smc_dirty`, `stop_flag`, `dispatches`, `code_pages_base`,
`mem_fast_reads`, `mem_fast_writes`, `smc_page` and `tx_wasm`, all `uint32_t`; static asserts pin the layout.
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
function or table, and no helper other than the two memory functions.

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

## Exact supported IR whitelist

- `Void` (Dynarmic invalidated-instruction marker), scalar `Identity`.
- `A32GetRegister`, `A32SetRegister` (R0..R15); `A32GetCpsr`, `A32GetCFlag`.
- `A32SetCpsrNZ`, `A32SetCpsrNZC`, `A32SetCpsrNZCV`, `A32SetCpsrNZCVRaw`.
- `Add32`, `Sub32` (including carry-in), `LogicalShiftLeft32`,
  `LogicalShiftRight32`, `ArithmeticShiftRight32`, `RotateRight32`,
  `RotateRightExtended`.
- `GetCarryFromOp` on those arithmetic/shifts; `GetOverflowFromOp` and
  `GetNZCVFromOp` **only on Add32/Sub32**; `GetNZFromOp` on U32 values (including
  immediates). Each producer's carry/overflow is explicitly lowered and saved.
- `NZCVFromPackedFlags`, `GetCFlagFromNZCV` (internal NZCV uses CPSR bit positions).
- `And32`, `Or32`, `Eor32`, `Not32`, `AndNot32`, `IsZero32`, `MostSignificantBit`.
- `LeastSignificantByte`, `LeastSignificantHalf`, `LeastSignificantWord`,
  `ZeroExtendByteToWord`, `ZeroExtendHalfToWord`, `SignExtendByteToWord`,
  `SignExtendHalfToWord`, `ZeroExtendWordToLong` (U32 in, low word out),
  `ConditionalSelect32`, `ConditionalSelectNZCV`.
- `MostSignificantWord` (word 1 of a U64; the carry pseudo is bit 0 of word 1,
  like the x64 backend's `shr r64,32`+`setc`), `LogicalShiftRight64` and
  `Pack2x32To1x64` (U64 producers whose **both** words are published via the
  i64 scratch local), `VectorBroadcast32` (all four lanes).
- `FPSingleToFixedS32`, `FPSingleToFixedU32`, `FPDoubleToFixedS32`,
  `FPDoubleToFixedU32` (VCVT.S32/U32.F32/F64 with fbits == 0; rounding
  immediates 0 = nearest-even and 3 = towards-zero only, matching plain
  VCVT and VCVTR under a default FPSCR; other explicit modes and scaled
  fixed-point forms fail closed). Bit-pattern integer lowering with no
  host FP and no trapping Wasm conversion: NaN/infinity/overflow and
  negative-to-unsigned inputs saturate with IOC, dropped fractions raise
  IXC, FZ-flushed denormals raise IDC, DN is ignored, and live trap
  enables bail to Unsupported like every other FP lowering.
- `A32GetVector`/`A32SetVector` with explicit D or Q registers (Dn words
  2n/2n+1, Qn words 4n..4n+3; a D access never touches its neighbour's
  words; S registers are rejected), `A32GetExtendedRegister64`/
  `A32SetExtendedRegister64` with explicit D registers only (both words;
  S/Q rejected).
- `A32ReadMemory8/16/32/64` and `A32WriteMemory8/16/32/64`. 1/2/4-byte accesses
  lower INLINE when the M15 fast path is armed (see REGION_ABI.md); every
  fallback and the 64-bit width call the checked helpers (arg0 is the location
  immediate, arg1 the guest address, writes publish their value first). The
  helper's `bytes` argument carries a fallback reason in its HIGH byte (1..5,
  see emit_wasm.h) which the host must mask off before use. `A32UpdateUpperLocationDescriptor`,
  `A32BXWritePC`, `A32SetCheckBit`, `PushRSB` (prediction-only hint; no RSB),
  `A32CallSupervisor` (must follow PC write and be final IR op).

All other opcodes fail, even if unused. Supported scalar values are U1/U8/U16/
U32/U64/NZCVFlags; the empty NZCV marker is rejected. Arithmetic uses i64 only as
an internal widened intermediate (plus the explicit U64 producers above).
Supported memory accesses go through the checked helpers; there is no
unchecked load/store lowering, and guest memory is not the Wasm linear memory.

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
