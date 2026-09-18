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
// OFC/UFC/IXC/IOC bits (mask 0x9f) are returned. Their results come from the
// vendored Dynarmic FP implementation (common/fp), which is linked in, so
// there is no second estimate algorithm to keep in sync.
// Operations 6 and 7 implement the ARM vector float-to-int VCVT for one
// binary32 lane packed in the low 32 bits of `a`: operation 6 = signed
// (vcvt.s32.f32), operation 7 = unsigned (vcvt.u32.f32). The A32 translator
// emits fbits=0, TowardsZero rounding and fpcr_controlled=false for these,
// so both always execute as FPToFixed(ibits=32, fbits=0, TowardsZero) under
// ASIMDStandardValue() (FZ=1, DN=1; explicit rounding overrides RN), regardless
// of `fpscr`, via the vendored implementation. Only newly raised IOC/IXC/IDC
// bits (mask 0x9f) are returned: either sign of subnormal input becomes zero
// with IDC only. The 32-bit integer result rides in the low 32 result bits.
FP64Result fp64_arithmetic(uint32_t operation, uint64_t a, uint64_t b, uint32_t fpscr) noexcept;

} // namespace vita3k::wasmjit
