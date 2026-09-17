# A32 byte reversal (source-only batch B3)

Implemented in `vita3k/cpu/src/wasmjit/emit_wasm.cpp` and covered by
`vita3k/cpu/tests/wasmjit_byte_reverse_tests.inc`, included by the production
backend test. This batch is source-reviewed but **not compiled or run**.

## Guest-to-IR evidence

| Guest instruction or helper | Exact IR route | Source |
| --- | --- | --- |
| ARM `REV` | `A32GetRegister(m) -> ByteReverseWord -> A32SetRegister(d)` | `external/dynarmic/src/dynarmic/frontend/A32/translate/impl/reversal.cpp:41-53` |
| ARM/Thumb `REVSH` | `A32GetRegister(m) -> LeastSignificantHalf -> ByteReverseHalf -> SignExtendHalfToWord -> A32SetRegister(d)` | `reversal.cpp:75-87`, `thumb16.cpp:838-841`, `thumb32_misc.cpp:132-142` |
| ARM/Thumb `REV16` | two logical shifts and masks, or two `ByteReverseHalf` values combined into one word | `reversal.cpp:57-70`, `thumb16.cpp:821-832`, `thumb32_misc.cpp:117-128` |
| A32 EFlag-aware register helpers | `ByteReverseHalf`, `ByteReverseWord` or `ByteReverseDual` around scalar register/memory values | `external/dynarmic/src/dynarmic/frontend/A32/a32_ir_emitter.cpp:249-359` |

The IR declarations are `U32 <- U32`, `U16 <- U16` and `U64 <- U64` at
`external/dynarmic/src/dynarmic/ir/opcodes.inc:161-163`. Dynarmic's x64
reference uses host `bswap` for word and dual and an eight-bit rotate for the
halfword (`external/dynarmic/src/dynarmic/backend/x64/emit_x64_data_processing.cpp:1536-1550`).

## Construction invariants

- `ByteReverseWord` expands to four masked byte contributions in an i32 local;
  `ByteReverseHalf` masks its U16 input and output; neither route depends on
  host endianness or Wasm SIMD.
- `ByteReverseDual` reads the two little-endian i32 SSA words, reverses each,
  swaps their positions, and defines both U64 result words. This matches the
  existing `value_word`/`A32SetExtendedRegister64` convention.
- The shared per-instruction scratch local is i64. Narrow packed arithmetic
  therefore uses the spare i32 SSA slot instead; this is part of the same
  source correction and prevents Wasm local type mismatch.
- Reversal changes no CPSR, FPSCR, memory, or flags. Invalid PC operands remain
  Dynarmic decoder rejections; no rejection path is weakened.

## Fixtures and pending verification

The independent fixtures cover dynamic register values and constants for all
three opcodes, including unequal 64-bit words and U16 publication. ARM guest
fixtures cover low and high general registers with `REV`, `REV16` and `REVSH`;
`browser/tests/jit_byte_reverse_encodings.S` is the VitaSDK assembler oracle
for ARM and Thumb spellings. The source contains no proprietary game data.

Before this batch, all three raw IR routes rejected emission. A Wasm validation
failure points to an i32/i64 local or stack mismatch; a 64-bit mismatch points
to word order; a REVSH mismatch points to U16 masking or sign extension. Run
the B3 section of `VERIFICATION_QUEUE.md` on wasm32 and Memory64 before
regenerating the inventory snapshot.
