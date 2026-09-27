# Scalar packed saturation (QADD/QSUB family, batch B4)

Implemented in `vita3k/cpu/src/wasmjit/emit_wasm.cpp`
(`packed_saturating` helper plus eight instruction-switch cases) and covered by
`vita3k/cpu/tests/wasmjit_packed_saturate_tests.inc`, included by the
production backend test. Verified on wasm32 Node and Memory64 Chromium
(backend suite green on both); retail Limbo crossed this frontier past
119k imports / 37 frames.

## Guest-to-IR evidence

| Guest instruction | Exact IR route | Source |
| --- | --- | --- |
| ARM/Thumb `UQADD8` | `PackedSaturatedAddU8(GetRegister(n), GetRegister(m)) -> SetRegister(d)` | `parallel.cpp:295-308`, `thumb32_parallel.cpp:275-288` |
| ARM/Thumb `UQADD16` | `PackedSaturatedAddU16(...)` | `parallel.cpp:310-323` and Thumb32 counterpart |
| ARM/Thumb `UQSUB8`/`UQSUB16` | `PackedSaturatedSubU8` / `PackedSaturatedSubU16(...)` | `parallel.cpp:325-355` and counterparts |
| ARM/Thumb `QADD8`/`QADD16`/`QSUB8`/`QSUB16` | `PackedSaturatedAddS8/S16`, `PackedSaturatedSubS8/S16(...)` | `parallel.cpp:235-293`, `thumb32_parallel.cpp:185-269` |

The IR declarations are `U32 <- U32, U32` at
`external/dynarmic/src/dynarmic/ir/opcodes.inc:246-253`. Dynarmic's x64
reference uses host `paddusb`/`paddsb`/`psubusb`/`psubsb`
(`backend/x64/emit_x64_packed.cpp:606-618`); arm64 uses `UQADD`/`SQADD`/
`UQSUB`-class NEON lanes (`backend/arm64/emit_arm64_packed.cpp:346-358`).
Neither the frontend nor any upstream backend updates CPSR/Q/GE for these
ops, so the Wasm lowering produces the result word only.

## Construction invariants

- One shared helper for all eight ops (lane width 8/16, signed/unsigned,
  add/sub). Every partial sum/difference fits in i32, so the clamp is exact;
  lanes reassemble only after their final shift. No Wasm SIMD, no i64
  scratch, no new imports.
- Unsigned add clamps up (`sum > max ? max : sum`); unsigned sub saturates
  borrows to zero (`a < b ? 0 : diff`, recomputed from source lanes, not from
  the wrapped difference); signed add/sub clamp both sides with the newly
  added `LtS` opcode (`0x48`; the enum previously had `LtU`/`GtS`/`GtU` only).
- Uses reserved per-instruction SSA words +4..+8, the same pattern as
  `inline_mutex`; the result is published to the instruction's word 0 and no
  carry/overflow pseudo-words are involved.
- Retail trigger: Thumb `UQADD8 R4,R12,R4` (bytes LE `8c fa 54 f4`,
  `thumb_word` form `0xfa8cf454`) at guest PC `0x811f8414` in Limbo's
  `AK::EventManager` thread, reached after ~262 s / ~119k imports with inline
  mutexes enabled. Before this batch the block rejected emission and the
  thread died with a CPU error (`exitCode -8`).

## Fixtures and verification

- `ir_saturation`: 16 independent IR fixtures (8 ops x register-patterns and
  boundary immediates), run in single-block and region modes with full
  architectural state comparison (CPSR/FPSCR preservation included).
- `guest_saturation`: the exact crashing Thumb bytes executed in single and
  region modes across zero/all-ones/boundary/xorshift operand pairs, checking
  Rd, untouched sources and resume PC.
- Full backend suite green: wasm32 Node
  (`WasmJit backend: 13048797 checks passed`) and Memory64 Chromium
  (`13048068 checks`). The inventory report (`jit_coverage_inventory.py`)
  then listed dispatch routes for all eight ops (Packed integer/DSP: 8 of 34).
- Remaining packed family (`PackedAddU8` with GE results, halving variants,
  `PackedSelect`, ...) still rejects emission; the next retail crash names
  the next opcode to lower.
