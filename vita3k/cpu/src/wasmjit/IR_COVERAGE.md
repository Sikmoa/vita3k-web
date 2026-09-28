# Portable Dynarmic IR coverage

The shared emitter in `emit_wasm.cpp` dispatches on `IR::Opcode` enum values.
Block JIT, region JIT and AOT use these same lowerings. Every one of the 668
generic/A32 entries in the vendored `opcodes.inc` is accounted for by a lowering
or an explicit native callback exclusion. This count is an opcode inventory,
not a claim that every operand combination has been exhaustively tested.

## Lowering strategy

Integer, packed integer, vector, saturation, permutation, table lookup, CRC,
AES, SHA256 and SM4 operations emit Wasm instructions directly. Scalar shifts
preserve ARM's out-of-range behavior, division handles zero and signed
overflow without Wasm traps, and saturating operations update sticky Q/QC
flags. There is no runtime integer interpreter or opcode-name dispatch.
Translation-time AES/SM4 tables become constants in the generated code.

`FPFixedU32ToDouble` emits unsigned integer-to-f64 conversion followed, when
needed, by multiplication by an exact power of two. Every valid fractional
bit count (0..32) uses this direct path in both JIT and AOT. These conversions
are exact, so their result does not depend on the rounding mode.

Operations that need ARM FP behavior unavailable in ordinary Wasm, including
fused multiply-add, directed rounding, estimates and certain conversions,
call the compiled portable FP implementation through the existing `env.fp64`
import. The selector is an emission-time constant. Operand bits and FPSCR
are explicit; returned exception flags are merged into the emitter's state.
Scalar and vector formats include binary16, binary32 and binary64. Existing
direct FP paths remain where they implement the required behavior.

`prepare_fused.cmake` generates a corrected copy of Dynarmic's portable
`fused.cpp`: cancellation into the low product limb needs a 62-bit exponent
correction. The submodule remains unchanged. Configuration fails if the
expected upstream expression changes. Independent cancellation fixtures and
random `std::fma` comparisons check the correction.

## Explicit unsupported cases

The browser runtime has no callbacks for native `CallHostFunction`,
`A32CoprocInternalOperation`, `A32CoprocSendTwoWords`,
`A32CoprocGetTwoWords`, `A32CoprocLoadWords` or `A32CoprocStoreWords`.
These reject emission with explicit diagnostics. Single-word coprocessor
accesses support the existing configured TLS register; other encodings also
reject. They do not silently become no-ops. `Breakpoint` emits Wasm
`unreachable` and therefore traps when executed.

Existing validity checks still apply: immediate metadata, legal lane/shift
widths, pseudo-operation producers, terminals and frontend locations must
match their IR contracts. Live FP exception trap enables continue to return
`Unsupported`; the browser does not deliver ARM FP traps.

## ABI and AOT

Six private `fp_arguments` words are appended to `JitState` at byte 468 in
wasm32, bringing its size to 492 bytes. Generated FP calls save and restore
all six words. Previous field offsets are unchanged; Memory64 uses
`offsetof` for its wider host-pointer layout. No new Wasm import is required.
AOT metadata version 7 invalidates images built with the previous layout or
semantics. Rebuild existing AOT images to use the new lowerings.

## Validation

The reproducible native-generator, Emscripten FP-helper and Node execution
commands are in [SCRIPTS.md](../../../../SCRIPTS.md). The core suite exercises
block emission and reference/P/K/PK region policies at two state offsets,
compares complete state and memory, validates modules and checks canaries and
out-of-bounds traps. It includes independent integer/crypto references,
saturation and shift boundaries, FP cancellation/NaN cases, and 2,048 random
binary32/binary64 FMA cases against `std::fma`. Vector FP comparisons also
check native versus Wasm execution of the portable arithmetic and its ABI.

The emitter compiles with Emscripten for both wasm32 and Memory64. Generated
core fixtures execute as wasm32 in Node. The external vitaslop corpus was
explicitly omitted with `--core-only`; the full browser/backend integration
suites and retail performance benchmarks were not rerun for this change.
No speedup measurement is claimed.
