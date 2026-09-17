# WASM JIT vector construction and data movement

This batch is implemented in `vita3k/cpu/src/wasmjit/emit_wasm.cpp` and tested in
`vita3k/cpu/tests/wasmjit_vector_tests.inc`, included by the production-backend
integration test. It does **not** imply complete NEON or CPU support.

## Implemented family

| IR operations | Semantics / restrictions |
| --- | --- |
| `VectorBroadcast8/16/32/64` | Repeat the scalar across all 128 bits. Narrow inputs are masked; 64-bit inputs retain both words. |
| `VectorBroadcastElement8/16/32/64` | Repeat the selected lane. Index must be an immediate U8 within the vector. |
| `VectorGetElement8/16/32/64` | Extract an unsigned lane. Signed VMOV forms use separate sign extension. |
| `VectorSetElement8/16/32/64` | Replace only the indexed lane; preserve every other bit. Immediate, in-range index required. |
| `VectorAnd/AndNot/Or/Eor/Not` | Full 128-bit Boolean operations; AndNot is `a & ~b`. |
| `And64/AndNot64/Or64/Eor64/Not64` | Companion two-word scalar operations, including D-form modified immediates. |
| `ZeroVector`, `VectorZeroUpper`, `ZeroExtendLongToQuad`, `Pack2x64To1x128` | Initialize all four SSA words, including explicit zero upper halves. |
| `Identity` | Preserve all words of U64/U128 values as well as existing scalar forms. |
| `VectorExtract`, `VectorExtractLower` | Extract from the concatenation `b:a`; position is a byte-aligned **bit** offset. Lower uses only the two low 64-bit operands and zero-extends the result. Endpoints 128/64 are supported; malformed or dynamic positions reject emission. |

Vectors remain four i32 SSA locals in generated Wasm; no interpreter, guest
instruction skipping, JS arithmetic, or new host callbacks are involved. SIMD
performance tuning is separate from semantic coverage.

D-vector reads zero-extend the temporary to U128, matching Dynarmic's native
backends and optimizer forwarding. D-vector writes affect only the selected
64-bit architectural register, never its paired D register. Full-width vector
operations can legitimately set high temporary bits before a D write discards
them. Region members reuse SSA locals, so upper-word initialization is essential.

The source audit used Dynarmic's `ir/ir_emitter.cpp`, `ir/opcodes.inc`, A32
`vfp.cpp`, `asimd_misc.cpp`, `asimd_one_reg_modified_immediate.cpp`,
`asimd_load_store_structures.cpp`, native vector emitters, and
`ir/opt/a32_get_set_elimination_pass.cpp`. Ordinary A32 lane VMOV does not
exercise Pack2x64 or SetElement64; these have explicit IR fixtures. Some width
variants are conservative inventory candidates, not demonstrated A32 emissions.

## Verification

- The initial independent matrix reported **27 missing/incomplete routes** in
  one run; the companion U64 Boolean matrix subsequently reported five more.
  No rejected instruction was executed or bypassed to obtain these results.
- **142 independent IR fixtures** now pass in both single-block and region
  emission, each with zero, all-one, and asymmetric inputs.
- Every legal lane position at 8/16/32/64 bits; constant and dynamic values;
  extract boundaries; invalid and dynamic lane/position rejection.
- ARM and Thumb guest encodings: VDUP from core/lane, signed/unsigned lane
  VMOV, modified immediates, VAND/VBIC/VORR/VORN/VEOR/VMVN, VBSL/VBIT/VBIF,
  VSWP and VEXT. Includes high registers, odd D registers, overlapping
  destinations, and a failed ARM condition. Representative encodings were
  assembled with VitaSDK (`.arch armv7-a`, `.fpu neon`).
- VLD1 single-lane/replicate loads and VST1 lane stores, including unaligned
  page-crossing input and output sentinels. All untouched registers and flags
  are compared, including FPSCR QC/cumulative/trap-enable bits.
- Five-member region regression poisons reused vector SSA slots, then verifies
  zero-upper/zero-vector/narrow construction in every flag/accounting mode.
- Full backend suite: **99,716 checks**, exit 0, on wasm32 and Memory64. All four
  process flag/accounting combinations pass on each ABI. Memory64 execution
  used Node 24; wasm32 used Node 22. Inventory Python suite: **9 tests**, exit 0.

Build the `vita3k_jit_backend_test_node` target in the appropriate build directory,
then run `browser/vita3k_jit_backend_test_node.js` with a compatible Node version.
Use `VITA3K_WASMJIT_PROMOTE_FLAGS=0/1` and
`VITA3K_WASMJIT_PROMOTE_ACCOUNTING=0/1` to reproduce the process-mode matrix.
Memory64 needs the configured wasm64 Emscripten sysroot cache; the system's
frozen wasm32-only cache is insufficient. Builds here were serial and bounded;
tests had a 40-second hard deadline.

Limbo integration passed the former `VectorBroadcast8` rejection and advanced
from 2,547 to **3,599 imports**, stopping at missing HLE NID `B110C123`
(`sceKernelGetProcessTimeWide`). This is a startup checkpoint, **not gameplay**.
The retail package, firmware, decrypted assets and runtime logs are not fixtures
and are not committed.
