// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace vita3k::wasmjit {

struct FP64Result {
    uint64_t bits;
    uint32_t flags;
};

// ARM scalar binary64: operation 0 = add, 1 = sub, 2 = mul, 3 = div.
// Uses FPSCR.RMode[23:22] (RN-even, +inf, -inf, zero), FZ[24], DN[25].
// flags contains ONLY newly raised IOC/DZC/OFC/UFC/IXC/IDC in FPSCR bit
// positions (mask 0x9f); the caller must OR it into the cumulative FPSCR.
// Precondition: the emitter has rejected live exception enables [15,12:8].
// Trap delivery is not implemented; other FPSCR bits are ignored.
// Invalid operation selectors return the default NaN with IOC.
// Operations 0..3 are integer-only, allocation-free, independent of host FP
// state and Dynarmic.
// Operations 4 and 5 implement the ARM vector RECPE/VRECPS estimates for one
// binary32 lane packed in the low 32 bits of `a` (and `b`): operation 4 =
// vrecpe.f32(a); operation 5 = vrecps.f32(a, b) = 2.0 + (-a) * b fused. Both
// always execute under ASIMDStandardValue() (RN, FZ=1, DN=1), matching the
// A32 translator's fpcr_controlled=false call sites, regardless of `fpscr`.
// Subnormal inputs flush to signed zero with IDC, NaNs become default NaNs,
// and flushed tiny results raise UFC without IXC. Only newly raised IDC/DZC/
// OFC/UFC/IXC/IOC bits (mask 0x9f) are returned. RECPE memoizes the vendored
// Dynarmic FPRecipEstimate per sign, exponent and top 8 fraction bits (all
// it reads of a normal input); VRECPS is computed exactly in binary64 with
// rounding to odd. The backend tests check both against Dynarmic directly.
// Operations 6 and 7 implement the ARM vector float-to-int VCVT for one
// binary32 lane packed in the low 32 bits of `a`: operation 6 = signed
// (vcvt.s32.f32), operation 7 = unsigned (vcvt.u32.f32), towards zero with no
// fraction bits. The conversion is the ARM ARM FPToFixed(ibits=32) under
// ASIMDStandardValue() (FZ=1; the explicit rounding overrides RN),
// regardless of `fpscr`: it rounds, then saturates, so a negative value that
// rounds to 0 gives an unsigned 0 with IXC (Dynarmic's FPToFixed raises IOC
// there). Only newly raised IOC/IXC/IDC bits (mask 0x9f) are returned: either
// sign of subnormal input becomes zero with IDC only. The 32-bit integer
// result rides in the low 32 result bits.
// Operations 8 and 9 are the reciprocal square root counterparts of 4 and 5,
// under the same contract: operation 8 = vrsqrte.f32(a); operation 9 =
// vrsqrts.f32(a, b) = (3.0 + (-a) * b) / 2 fused.
// Operation 10 is the scalar binary64 square root (vsqrt.f64) of `a` under the
// live FPSCR (all four rounding modes, FZ, DN), integer-only like 0..3.
// Operations 11 (signed) and 12 (unsigned) are the vector float-to-int
// conversions of 6/7 with the rounding mode (Dynarmic::FP::RoundingMode 0..4)
// in bits 0-7 and the fraction bits (0..32) in bits 8-15 of the low 32 bits
// of `b` (VCVT{A,N,P,M}.S32/U32.F32 and fixed-point VCVT.S32/U32.F32 #fbits).
// Other modes or fraction bit counts return the default NaN with IOC.
FP64Result fp64_arithmetic(uint32_t operation, uint64_t a, uint64_t b, uint32_t fpscr) noexcept;

} // namespace vita3k::wasmjit
