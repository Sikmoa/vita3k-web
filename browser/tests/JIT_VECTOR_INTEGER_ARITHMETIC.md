# NEON modular integer arithmetic (source-only batch B1)

Implemented, **not compiled or run**: VectorAdd8/16/32/64,
VectorSub8/16/32/64, VectorMultiply8/16/32. This adds eleven emitter routes;
it does not establish complete NEON support or a new retail checkpoint.

## Guest-to-IR evidence

Paths in this table are relative to
`external/dynarmic/src/dynarmic/frontend/A32/translate/impl/`.
GetVector/SetVector emit A32GetVector/A32SetVector (D or Q architectural width).

| Guest instructions | Translator and exact arithmetic sequence |
| --- | --- |
| VADD.I8/I16/I32/I64 | `asimd_three_regs.cpp:422`: GetVector(m), GetVector(n), VectorAdd(esize,n,m), SetVector(d). |
| VSUB.I8/I16/I32/I64 | `asimd_three_regs.cpp:440`: GetVector(m), GetVector(n), VectorSub(esize,n,m), SetVector(d). |
| VMUL.I8/I16/I32 | `asimd_three_regs.cpp:597`: GetVector(n), GetVector(m), VectorMultiply(esize,n,m), SetVector(d), with P=false. Size=3 is undefined; polynomial multiply is a separate, still unsupported IR op. |
| VMLA/VMLS.I8/I16/I32 | `asimd_three_regs.cpp:572`: GetVector(n,m,d), VectorMultiply(n,m), VectorAdd(d,product) or VectorSub(d,product), SetVector(d). |
| VMUL/VMLA/VMLS.I16/I32 by scalar lane | `asimd_two_regs_scalar.cpp:33,138,150`: GetVector(n), GetVector(m), VectorBroadcastElement(esize,m,index), VectorMultiply; optionally GetVector(d) and VectorAdd/Sub(d,product); SetVector(d). F=false only. |
| VNEG.S8/S16/S32 | `asimd_two_regs_misc.cpp:433`: GetVector(m), ZeroVector, VectorSub(esize,zero,m), SetVector(d), with F=false. |

`ir/ir_emitter.cpp:977,1391,1927` selects the width-specific opcodes;
`ir/opcodes.inc:294,399,522` declares U128 inputs/results. Native semantic
reference: `backend/x64/emit_x64_vector.cpp:462-476,2322-2368,5019-5033` uses
packed wrapping add/subtract and low-half products. No flag or FPSCR update
occurs for these operations. Signed and unsigned forms have the same low bits.
Widening instructions also use some of these operations, but their extend/
widen prerequisites remain unsupported, so they are **not** claimed here.

## Construction invariants

- Reuses `vector_element_word` and the existing vector SSA representation.
  For 8/16-bit lanes, mask the result before shifting/OR packing. Each result
  word is independently initialized; no carry, borrow or product crosses lanes.
- For 32-bit lanes Wasm integer arithmetic wraps naturally. For 64-bit add/sub,
  compose each pair of source words into i64, then use the `Mul64` producer's
  existing i64 scratch / i32 low+high publication convention.
- Every U128 producer defines four words before any architectural write. A
  source register overlapping the destination is safe because reads are SSA
  values. A D write still preserves its architectural neighbor.
- No new memory access, host import or state offset. Memory64 uses the same
  register values; host addresses and JitState layout are untouched.
- CPSR and FPSCR are unchanged, including NZCV, Q, GE, QC, sticky exception
  bits and live trap enables. Integer NEON arithmetic does not require FP
  rounding/trap guards. Saturating, polynomial and FP arithmetic still reject.
- VectorMultiply64 is deliberately not added: the generic IR helper exposes
  it to the conservative inventory, but the A32 multiply translators forbid it.
  First-rejection reporting and instruction/byte diagnostics remain intact.

## Fixtures and pre-fix failures

`wasmjit_vector_integer_tests.inc` uses the existing Harness (real Wasm
installation, single blocks and regions, three initial patterns, full register/
flag comparisons). There are **25 independent IR fixtures**: eleven dynamic
binary cases, eleven asymmetric constant boundary cases and three zero-minus
VNEG sequences. Before this batch each arithmetic route rejects emission.

Boundary constants exercise bit-31 carry within a 64-bit lane, bit-63 overflow,
signed minima, all-one products, and adjacent byte/halfword isolation. Dynamic
cases use zero, all-one and unrelated asymmetric source registers. ARM and
Thumb guest cases cover every legal width in D/Q forms, d=n and d=m aliasing,
Q15/D31 destinations, integer VNEG, and I16/I32 scalar multiplication/accumulation.
They execute to SVC and compare all untouched registers and both status words.
A separate fixture asserts that VectorMultiply64 still rejects.

`browser/tests/jit_vector_integer_encodings.S` is an assembly oracle for the
new guest cases. Encodings were derived from `A32/decoder/asimd.inc:24-29,74,77,118`
and have **not** been assembled in this session. Run the exact commands in
VERIFICATION_QUEUE.md before trusting the encoding-derived cases. The runner
must verify both Wasm validation and semantic assertions on wasm32/Memory64.
