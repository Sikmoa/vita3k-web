# NEON integer comparisons and selection (source-only batch B2)

Status: implemented with pending fixtures, **not built or executed**.
Adds 27 dispatch routes: each of VectorEqual, VectorGreaterS, VectorMinS/U,
VectorMaxS/U, VectorAbs, VectorSignedAbsoluteDifference and
VectorUnsignedAbsoluteDifference at 8/16/32 bits. No 64-bit variants or
saturating/FP comparisons are claimed.

## Guest routes and exact IR composition

References below use `external/dynarmic/src/dynarmic/` as their root. All
routes start with A32GetVector and finish with A32SetVector, whose D/Q footprint
is unchanged. `n`, `m`, `d` denote the corresponding SSA register reads.

| Guest instruction | Arithmetic IR sequence and source |
| --- | --- |
| VCGT.S | VectorGreaterS(n,m); `frontend/A32/translate/impl/asimd_three_regs.cpp:78,406`. |
| VCGT.U | VectorMinU(n,m), VectorEqual(min,n), VectorNot(equal); same translator, expansion `ir/ir_emitter.cpp:1197`. |
| VCGE.S | VectorGreaterS(n,m), VectorEqual(n,m), VectorOr(gt,eq); translator line 410, expansion `ir/ir_emitter.cpp:1189`. |
| VCGE.U | VectorMaxU(n,m), VectorEqual(max,n); same translator, expansion `ir/ir_emitter.cpp:1193`. |
| VCEQ.I | VectorEqual(n,m); translator `asimd_three_regs.cpp:568`. |
| VTST | VectorAnd(n,m), ZeroVector, VectorEqual(and,zero), VectorNot(equal); `asimd_three_regs.cpp:545`. |
| VMIN/VMAX.S/U | VectorMinS/U or VectorMaxS/U(n,m); `asimd_three_regs.cpp:515`. |
| VABD.S/U | VectorSigned/UnsignedAbsoluteDifference(n,m); `asimd_three_regs.cpp:149,414`. |
| VABA.S/U | Same difference, A32GetVector(d), VectorAdd(d,difference); `asimd_three_regs.cpp:149,418`; requires B1. |
| VABS.S | VectorAbs(m); `frontend/A32/translate/impl/asimd_two_regs_misc.cpp:407`, F=false. |
| VCGT/VCGE/VCEQ/VCLE/VCLT against zero | ZeroVector, then signed comparison helpers with (m,zero); `asimd_two_regs_misc.cpp:23,387-403`. GT/GE/EQ expand as above; LE = VectorNot(GreaterS); LT = VectorNot(VectorOr(GreaterS,Equal)); `ir/ir_emitter.cpp:1277,1285`. |

All these translators reject size=3 for this family. Wider generic IREmitter
methods explain conservative inventory candidates, not A32 instruction support.
Pairwise min/max still need interleave prerequisites and remain unsupported.

Native semantic references in `backend/x64/emit_x64_vector.cpp`:
VectorAbs at 377-459; Equal at 1367-1377; GreaterS at 1475-1485; Min/Max at
2082-2319; signed absolute difference at 4005-4067; unsigned at 5614-5678.
`ir/opcodes.inc:290,340,347,383,485,533` declares the operand/result types.
In particular, native signed absolute difference uses signed min/max followed
by a wrapping subtraction, not the sign of a possibly overflowing difference.

## Construction invariants

`vector_integer_select` reuses `vector_element_word`, the existing scalar
shift-based sign extension and Wasm Select patterns. It uses two of each
instruction's ten reserved SSA locals for the lane operands and initializes
every result word. Signed narrow operands are extended before ordering; lane
results are masked before packing. True predicates become zero minus one,
then truncate to the full architectural lane mask (not scalar value one).

Absolute value and absolute difference are non-saturating. abs(INT_MIN)
retains its bits, and max-signed minus min-signed yields the unsigned lane
maximum. CPSR and FPSCR, including QC and exception flags, remain untouched.
No rounding or live-FP-trap guard belongs on these integer operations. The
existing memory addressing, Memory64 ABI, SSA read-before-write and first
rejection diagnostic paths are unchanged.

## Verification and failure interpretation

`wasmjit_vector_compare_tests.inc` adds **81 independent IR fixtures**: 27
dynamic cases plus two boundary vectors per operation/width. The oracle uses
mathematical signed 64-bit values for comparison, min/max and distance; the
emitter uses 32-bit bit operations. Zero, ones, asymmetric, INT_MIN/INT_MAX,
equal-minimum, zero-versus-all-ones and reversed-sign-order lanes are covered.
The common harness now exercises both clear and set status bits, so an
accidental QC/sticky write cannot be hidden by an initially set flag.

Each fixture runs single-block and region emission. ARM/Thumb guest cases cover
all listed families at every legal width, high Q15/odd D31, both source aliases,
scalar-zero comparisons and VABA accumulation. All registers and status words
are compared. A companion `browser/tests/jit_vector_compare_encodings.S`
provides VitaSDK assembly for checking the unassembled decoder-derived words.

Before B2 the raw routes reject emission; composed unsigned comparisons also
fail on their min/max prerequisite. A predicate assertion after B2 implicates
signedness or mask construction; extremes-only failure implicates overflow;
status failure implicates accidental flags; D/Q neighbor failure implicates
write footprint. Exact bounded commands are in VERIFICATION_QUEUE.md.

Assumptions awaiting execution: Wasm validation of the composed stack, compiler
acceptance of fixtures, decoder-derived guest encodings, and both ABI/mode
matrices. No new retail result or renderer completeness is implied.
