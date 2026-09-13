# M14 Wasm emitter prototype tests and interface

The emitter implementation is `../src/wasmjit/emit_wasm.{h,cpp}`. It consumes **actual
Dynarmic A32 IR**, not ARM bytes. The native fixture generator additionally uses
Dynarmic's real ARM/Thumb translator; the JavaScript test executes raw emitted
modules in Node's Wasm engine. It does not emulate Wasm or ARM.

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
  vita3k/cpu/src/wasmjit/emit_wasm.cpp \
  vita3k/cpu/tests/wasmjit_emitter_test.cpp \
  build/native/external/dynarmic/src/dynarmic/libdynarmic.a \
  build/native/external/dynarmic/externals/mcl/src/libmcl.a \
  build/native/external/fmt/libfmtd.a \
  -o /tmp/wasmjit-emitter-test
/tmp/wasmjit-emitter-test /tmp/wasmjit-emitter-fixtures
node vita3k/cpu/tests/wasmjit_emitter_test.mjs /tmp/wasmjit-emitter-fixtures
```

Expected: 50 deterministic modules, 20,328 input/expected-state pairs, and
40,656 successful **Wasm `call_indirect`** calls at two nonzero state offsets.
The native generator/emitter was also run with UndefinedBehaviorSanitizer
(`-fsanitize=undefined -fno-sanitize-recover=undefined`) without findings.
Coverage includes:

- Real ARM/Thumb MOV, ADD, SUB, CMP, taken/untaken BNE; ARM MOVS-register and
  predicated MOV; SVC (including post-instruction PC); BX in both directions.
- Add/sub carry-in 0/1, carry/no-borrow and signed overflow, edge values and
  seeded random inputs. Pseudos are read after registers/CPSR have been changed.
- LSL/LSR/ASR/ROR/RRX result/carry and NZ; counts 0..256 (U8 truncation), all
  boundary distinctions at 0, 31, 32, 33 and multiples of 32.
- All 15 supported conditions with all 16 NZCV combinations, separately at
  block entry and terminal; entry conditions are not rechecked after a write.
- Identity, scalar bit operations, packed NZCV, selects, narrowing, and the
  4096-IR-instruction local allocation boundary.
- Whole-state comparisons (including preservation of Q/GE/FPSCR), canaries,
  exact module import/export validation, table insertion, out-of-bounds traps.
- Native fail-closed checks: unsupported/dead IR, memory/exception IR from the
  real translator, invalid/interpret/check-bit terminals, unsupported terminal
  even on an unreachable branch, malformed locations, bad pseudo producer,
  excessive size/depth, missing condition-failure metadata, unsafe SVC shape.

These tests isolate emission. CPUInterface and browser-Worker integration are
covered separately by `browser/tests/wasm_jit_tests.cpp` and `jit_smoke.mjs`.

## ABI / integration

`vita3k::wasmjit::emit_block(const Dynarmic::IR::Block&)` returns raw Wasm bytes,
**empty on unsupported input**. It neither executes guest code nor invokes
helper callbacks. No exception is used for ordinary rejection.

The standard-layout `JitState` is 84 bytes. Its fields in order are `regs[16]`,
`cpsr`, `fpscr`, `svc`, `exit_reason`, `executed`, all `uint32_t`. The emitter uses
`offsetof`, not native Dynarmic state layout. Export `block(i32 stateOffset)`
returns i32; Emscripten signature is `ii`. Its sole import is `env.memory`, an
unshared memory with a minimum of one page and no required maximum. There is
no private memory, data segment, start function, table, or helper import.

Every invocation overwrites `svc`, `exit_reason`, and `executed`. Execution
returns to host after **one** block, even for LinkBlockFast. `regs[15]` is the
next guest PC. Continue is 0; Svc is 1. Fault=2 and Unsupported=3 are available
for the parent; the emitter does not fake execution by emitting those results.
SVC returns after the frontend PC write, records its immediate in `svc`, and
leaves callback handling to the parent. OOB state addresses trap; the parent
must supply a valid state allocation.

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
- `LeastSignificantByte`, `LeastSignificantHalf`, `ZeroExtendByteToWord`,
  `ZeroExtendHalfToWord`, `ConditionalSelect32`, `ConditionalSelectNZCV`.
- `A32UpdateUpperLocationDescriptor`, `A32BXWritePC`, `PushRSB` (prediction-only
  hint; no RSB), `A32CallSupervisor` (must follow PC write and be final IR op).

All other opcodes fail, even if unused. Supported scalar values are U1/U8/U16/
U32/NZCVFlags; the empty NZCV marker is rejected. Arithmetic uses i64 only as
an internal widened intermediate, **not** as general U64 IR support.

Terminals: `LinkBlock`, `LinkBlockFast`, recursive `If`; `ReturnToDispatch`,
`PopRSBHint`, `FastDispatchHint` require an explicit PC write. `CheckHalt` is
accepted **only** when its else-terminal is one of these dispatcher returns:
both outcomes return to host without further guest-state changes, so no halt
field is necessary. No link/If terminal is allowed after SVC. `Interpret`,
`Invalid`, `CheckBit`, other CheckHalt shapes fail.

Conditions EQ..AL are lowered; NV fails. Nonzero IT state, misaligned descriptor
PCs, and descriptor FPSCR-mode changes fail. Guest memory/FP/exceptions are not
supported. Modules are bounded to 4096 IR instructions, 4096 guest ticks,
terminal depth 16 and 256 visited terminal nodes. All values use locals;
addresses and module bytes are independent of native allocation addresses.
