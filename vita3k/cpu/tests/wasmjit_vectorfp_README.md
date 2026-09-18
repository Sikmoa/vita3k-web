# A32 vector floating-point comparisons on the Wasm JIT

## Supported contract

`FPVectorEqual32`, `FPVectorGreater32` and `FPVectorGreaterEqual32` accept only
an **immediate `fpcr_controlled=false`**. The A32 frontend lowers zero-comparison
LT/LE forms through GT/GE with swapped operands; there are no separate
`FPVectorLess*` IR opcodes in this checkout. Vector 16/64-bit comparison forms
and controlled/dynamic-control forms remain rejected, with no module emitted.

A32 **StandardFPSCRValue is RN, FZ=1, DN=1**, not `FPCR{0}`. See:

- `external/dynarmic/src/dynarmic/common/fp/fpcr.h`, `ASIMDStandardValue()`;
- `external/dynarmic/src/dynarmic/backend/x64/a32_emit_x64.cpp`, `FPCR(bool)`;
- `external/dynarmic/src/dynarmic/backend/x64/emit_x64_vector_floating_point.cpp`,
  `EmitFPVectorEqual32`, `EmitFPVectorGreater32`, `EmitFPVectorGreaterEqual32`;
- `external/dynarmic/src/dynarmic/common/fp/unpacked.cpp`, `FPUnpackBase`.

| Input condition | Result / newly raised cumulative flags |
| --- | --- |
| Ordered, finite or infinite | Lane-wide `0xffffffff` or zero |
| Either signed zero | Both zero signs compare equal |
| Subnormal input | Flush to signed zero; raise IDC |
| Any NaN | False, regardless of payload/sign or identical operands |
| EQ with quiet NaN only | No IOC |
| EQ with signaling NaN | IOC |
| GT/GE with **any** NaN | IOC |
| NaN and subnormal in the same lane | Include IDC from unpacking the other operand |

Emission uses integer bit-pattern ordering, not host floating-point conversion,
so signaling-NaN classification remains intact. Four lanes execute; D-register
sources have zeroed upper lanes and D-register destinations preserve their
neighbours. Results and cumulative flags do not depend on live RMode/FZ/DN.
FPSCR NZCV/QC and existing cumulative flags are preserved.

As with the other supported FP paths, live exception enables (`0x9f00`) return
`ExitReason::Unsupported` before the FP instruction's writes. Trap delivery is
not implemented. Region dispatch still requires a matching FPSCR location key;
a mismatched key is a clean Miss, not proof that the operation executed.

The comparison regression also caught a shared opcode-table defect: `GeU` must
encode **`0x4f` (`i32.ge_u`)**, not `0x4e` (`i32.ge_s`). Negative operands and
zero lie on opposite sides of the integer sort key's high bit.

## Related standard-FPSCR corrections

The same audit corrected existing vector RECPE/VRECPS, float-to-fixed32, and
add/sub/mul paths that previously treated the standard FPSCR as FZ=DN=0:

- Helper operations 4–7 now use Dynarmic's `ASIMDStandardValue()`.
- RECPE/VRECPS reject controlled and non-immediate-control shapes at emission.
- Vector add/sub/mul flush subnormal inputs with IDC and return default NaNs.
  Tiny nonzero results flush to signed zero with UFC **without IXC**, including
  exact subnormal results and results tiny before rounding to min-normal.
- Scalar FP remains governed by its own location FPSCR. Vector-versus-scalar
  differential tests explicitly use `0x03000000` for the scalar oracle.

No helper selector or JS interpreter fallback was added for comparisons.

## Regression coverage

`wasmjit_vectorfp_compare_tests.inc` is included by `wasmjit_backend_test.cpp`:

- Independent float-ordering oracle versus the emitter's integer sort keys:
  **6,532 four-lane cases × three operations × block/A/P/K/PK**. The corpus
  includes edge Cartesian products, all exponents with boundary mantissas,
  and deterministic random values.
- Hand-pinned NaN/denormal/infinity/signed-zero contracts, with each exceptional
  lane isolated so a stale NaN mask cannot contaminate later lanes.
- All 16 RMode/FZ/DN combinations with matching region keys, plus mismatch,
  live-trap, immediate-shape and unsupported-width rejection tests.
- SSA destination aliases, packed constant operands, cross-member local reuse,
  whole-register preservation and untouched helper scratch words.
- Real ARM and Thumb D/Q/high-register encodings, including Limbo's public
  `vcgt.f32 d0,d5,d1` instruction shape (`25 ff 01 0e`) and zero LT/LE forms.

The reciprocal, to-fixed and vector arithmetic suites additionally pin the
corrected standard-FPSCR edges independently of their differential oracles.

Assembler cross-check (source-owned instructions, no game assets):

```sh
"$VITASDK/bin/arm-vita-eabi-as" browser/tests/jit_vector_fp_compare_encodings.S -o /tmp/neon-fp-compare.o
"$VITASDK/bin/arm-vita-eabi-objdump" -d /tmp/neon-fp-compare.o
```

## Verification

Observed 2026-09-18:

- Full backend suite: **13,013,182 checks passed per run**, on both wasm32 and
  Memory64, for each A/P/K/PK process policy (eight matrix runs).
- Native emitter fixture generator: 378 reference + 1,146 candidate modules,
  58,798 input cases. Node execution: 58,792 block calls and 235,252 region
  calls, including table and out-of-bounds checks.
- Guest-thread lifecycle tests, including standard/non-standard HLE exception
  failure accounting and teardown, passed on both ABIs.
- GXM3 packet/viewport/sampler/rejection tests passed. This is not GPU execution
  or evidence that retail rendering works.

Commands from the repository root:

```sh
EMCC_CORES=1 cmake --build build/web --parallel 1 --target vita3k_jit_backend_test_node
node build/web/browser/vita3k_jit_backend_test_node.js

# Memory64 requires a compatible Node and a complete writable Emscripten cache.
EM_FROZEN_CACHE=0 EMCC_CORES=1 cmake --build build/web64 --parallel 1 --target vita3k_jit_backend_test_node
"$NODE64" build/web64/browser/vita3k_jit_backend_test_node.js

for flags in 0 1; do
  for accounting in 0 1; do
    VITA3K_WASMJIT_PROMOTE_FLAGS=$flags VITA3K_WASMJIT_PROMOTE_ACCOUNTING=$accounting \
      node build/web/browser/vita3k_jit_backend_test_node.js
    VITA3K_WASMJIT_PROMOTE_FLAGS=$flags VITA3K_WASMJIT_PROMOTE_ACCOUNTING=$accounting \
      "$NODE64" build/web64/browser/vita3k_jit_backend_test_node.js
  done
done
```

Set `EM_CACHE` to that writable cache before the Memory64 build; do not modify a
frozen system cache. Unset `VITA3K_ABLATE` and `VITA3K_ABLATE_PC` for verification.
For the native fixture generator command, see
[wasmjit_emitter_README.md](wasmjit_emitter_README.md#run-from-repository-root).
The native fixture counts above are separate from the new backend comparison
corpus; neither is an end-to-end gameplay test.
