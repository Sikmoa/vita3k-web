# FP vector absolute value (VABS.f16/f32/f64, batch B5)

Implemented in `vita3k/cpu/src/wasmjit/emit_wasm.cpp` (three
instruction-switch cases sharing one per-lane sign-clear loop) and covered by
`vita3k/cpu/tests/wasmjit_fpvector_abs_tests.inc`, included by the production
backend test. Verified on wasm32 Node and Memory64 Chromium.

## Guest-to-IR evidence

| Guest instruction | Exact IR route | Source |
| --- | --- | --- |
| ARM/Thumb `VABS.f32` | `FPVectorAbs32(GetVector) -> SetVector` | Dynarmic A32 translators; retail crash block below |
| ARM/Thumb `VABS.f16` | `FPVectorAbs16(...)` | same family, `opcodes.inc:651` |
| ARM/Thumb `VABS.f64` | `FPVectorAbs64(...)` | same family, `opcodes.inc:653` |

The IR declarations are `U128 <- U128` at
`external/dynarmic/src/dynarmic/ir/opcodes.inc:651-653`. Neither the frontend
nor any upstream backend updates CPSR/FPSCR for these ops, so the Wasm
lowering produces result words only.

## Construction invariants

- Per-lane sign clear: f16 lanes mask `0x7fff7fff` per word, f32 lanes mask
  `0x7fffffff`, f64 lanes clear the odd words' top bit and pass even words
  through. No Wasm SIMD, no i64 scratch, no new imports.
- Exact in every FPSCR mode and raising nothing: an SNaN keeps its payload
  with the sign cleared, denormals pass through untouched (no flush — abs
  cannot create or destroy tininess), so no FPSCR guard or flag accumulation.
- The D-form upper U128 lanes pass through to `SetVector`, which stores only
  its architectural footprint.
- Retail trigger: Thumb `VABS.f32 d3,d3` (bytes LE `b9 ff 03 37`,
  `thumb_word` form `0xffb93703`) at guest PC `0x811e509a` in Limbo's
  `AK::EventManager` thread, reached after ~244 s / ~119k imports with inline
  mutexes enabled. Before this batch the block rejected emission and the
  thread died with a CPU error (`exitCode -8`).

## Fixtures and verification

- `ir_abs`: 6 independent IR fixtures (3 ops x register-patterns and
  +/-0/denormal/max/inf/sNaN/qNaN immediates across Q0/Q1), run in
  single-block and region modes with full architectural state comparison
  (CPSR/FPSCR preservation included).
- `guest_abs`: the exact crashing Thumb bytes executed in single and region
  modes across zero/negative-zero/inf+sNaN/denormal+qNaN/negative-operand
  pairs, checking D3, untouched sources, CPSR/FPSCR and resume PC.
- Full backend suite green: wasm32 Node
  (`WasmJit backend: 13051802 checks passed`) and Memory64 Chromium
  (`13051073 checks`).
- Sibling vector-unary gaps (`FPVectorNeg`, `FPVectorSqrt`, reciprocal
  estimates beyond the existing routes) still reject emission; the next
  retail crash names the next opcode to lower.
