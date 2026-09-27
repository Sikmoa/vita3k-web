// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Reference float-to-integer conversion for the emitter and fp64 helper tests,
// written from the Arm Architecture Reference Manual pseudocode, not from
// Dynarmic. ARMv8-A ARM, shared/functions/float/fptofixed/FPToFixed (the ARMv7
// FPToFixed() in A2.7 is the same with rounding taken from FPSCR.RMode):
//
//   bits(M) FPToFixed(bits(N) op, integer fbits, boolean unsigned,
//                     FPCRType fpcr, FPRounding rounding)
//       (fptype,sign,value) = FPUnpack(op, fpcr);
//       if fptype == FPType_SNaN || fptype == FPType_QNaN then
//           FPProcessException(FPExc_InvalidOp, fpcr);
//       // Scale by fractional bits and produce integer rounded towards minus-infinity
//       value = value * 2.0^fbits;
//       int_result = RoundDown(value);
//       error = value - Real(int_result);
//       // Determine whether supplied rounding mode requires an increment
//       case rounding of
//           when FPRounding_TIEEVEN
//               round_up = (error > 0.5 || (error == 0.5 && int_result<0> == '1'));
//           when FPRounding_POSINF
//               round_up = (error != 0.0);
//           when FPRounding_NEGINF
//               round_up = FALSE;
//           when FPRounding_ZERO
//               round_up = (error != 0.0 && int_result < 0);
//           when FPRounding_TIEAWAY
//               round_up = (error > 0.5 || (error == 0.5 && int_result >= 0));
//       if round_up then int_result = int_result + 1;
//       // Generate saturated result and exceptions
//       (result, overflow) = SatQ(int_result, M, unsigned);
//       if overflow then
//           FPProcessException(FPExc_InvalidOp, fpcr);
//       elsif error != 0 then
//           FPProcessException(FPExc_Inexact, fpcr);
//       return result;
//
// FPUnpack gives NaNs the value 0 and infinities a value beyond every
// integer range; with FZ a denormal input unpacks as zero and raises
// InputDenorm. The result is rounded first and saturated second, so a
// negative value that rounds to 0 is an exact 0 with IXC for an unsigned
// result, and one that rounds into range from below -2^31 is in range.
//
// Where Dynarmic's FPToFixed differs, the tests name the divergence; see
// fp_to_fixed_divergence below.

#include <cstdint>

namespace arm_reference {

struct FixedResult {
    uint32_t bits;
    uint32_t flags; // FPSCR cumulative bits: IOC 0x01, IXC 0x10, IDC 0x80
};

// Dynarmic::FP::RoundingMode numbering: 0 TIEEVEN, 1 POSINF, 2 NEGINF,
// 3 ZERO, 4 TIEAWAY. ibits (M) is 32 or 16 (VCVT.{S,U}16); a 16-bit result
// is returned in the low half, zero-extended.
inline FixedResult fp_to_fixed(uint64_t op, bool is_double, unsigned fbits, bool unsigned_, unsigned rounding,
    bool flush_to_zero, unsigned ibits = 32) {
    const unsigned fraction_bits = is_double ? 52 : 23, exponent_bits = is_double ? 11 : 8;
    const int bias = is_double ? 1023 : 127;
    const bool sign = (op >> (fraction_bits + exponent_bits)) & 1;
    const uint64_t exponent = (op >> fraction_bits) & ((uint64_t{1} << exponent_bits) - 1);
    const uint64_t fraction = op & ((uint64_t{1} << fraction_bits) - 1);
    const uint64_t max_exponent = (uint64_t{1} << exponent_bits) - 1;
    const __int128 low = unsigned_ ? 0 : -(__int128{1} << (ibits - 1));
    const __int128 high = unsigned_ ? (__int128{1} << ibits) - 1 : (__int128{1} << (ibits - 1)) - 1;
    const uint32_t result_mask = ibits == 32 ? 0xffffffffu : (1u << ibits) - 1;
    const auto saturate = [&](bool negative) {
        return FixedResult{static_cast<uint32_t>(negative ? low : high) & result_mask, 0x01};
    };
    // FPUnpack.
    if (exponent == max_exponent)
        return fraction ? FixedResult{0, 0x01} : saturate(sign); // NaN: value 0; infinity: overflow
    uint64_t mantissa;
    int scale; // value = mantissa * 2^scale
    if (exponent == 0) {
        if (fraction == 0)
            return {0, 0};
        if (flush_to_zero)
            return {0, 0x80}; // FPType_Zero with InputDenorm
        mantissa = fraction;
        scale = 1 - bias - static_cast<int>(fraction_bits);
    } else {
        mantissa = fraction | (uint64_t{1} << fraction_bits);
        scale = static_cast<int>(exponent) - bias - static_cast<int>(fraction_bits);
    }
    scale += static_cast<int>(fbits);
    // RoundDown and the error, compared with one half: magnitude m splits
    // into whole units and a remainder rem of 2^shift.
    if (scale > 40)
        return saturate(sign); // |value| >= 2^40: out of every 32-bit range
    enum class Error { Zero, BelowHalf, Half, AboveHalf } error = Error::Zero;
    __int128 whole;
    if (scale >= 0) {
        whole = static_cast<__int128>(mantissa) << scale;
    } else {
        const unsigned shift = static_cast<unsigned>(-scale);
        uint64_t rem_high, half;
        if (shift >= 64) {
            whole = 0;
            rem_high = 1; // nonzero remainder far below one half
            half = 2;
        } else {
            whole = static_cast<__int128>(mantissa >> shift);
            rem_high = mantissa & ((uint64_t{1} << shift) - 1);
            half = uint64_t{1} << (shift - 1);
        }
        if (rem_high != 0)
            error = rem_high < half ? Error::BelowHalf : rem_high == half ? Error::Half : Error::AboveHalf;
    }
    __int128 int_result = whole;
    if (sign) {
        // RoundDown(-m) = -(whole + 1) when m has a fraction; the error
        // measured from there is one minus the magnitude's fraction.
        int_result = -whole;
        if (error != Error::Zero) {
            int_result -= 1;
            error = error == Error::BelowHalf ? Error::AboveHalf : error == Error::AboveHalf ? Error::BelowHalf : Error::Half;
        }
    }
    bool round_up = false;
    switch (rounding) {
    case 0: round_up = error == Error::AboveHalf || (error == Error::Half && (int_result & 1)); break;
    case 1: round_up = error != Error::Zero; break;
    case 2: round_up = false; break;
    case 3: round_up = error != Error::Zero && int_result < 0; break;
    case 4: round_up = error == Error::AboveHalf || (error == Error::Half && int_result >= 0); break;
    }
    if (round_up)
        ++int_result;
    // SatQ.
    if (int_result < low || int_result > high)
        return saturate(int_result < low);
    return {static_cast<uint32_t>(int_result) & result_mask, error != Error::Zero ? 0x10u : 0u};
}

// Dynarmic's FPToFixed (common/fp/op/FPToFixed.cpp) departs from the
// pseudocode in exactly two ways; everywhere else the tests hold the emitter
// to Dynarmic as well:
// - SignedIntoRange: it decides overflow on the magnitude plus one rounding
//   unit whenever it increments the result. A negative signed value in
//   (-2^(M-1), -(2^(M-1) - 1)) scaled units that rounds towards zero into
//   range gets the minimum with IOC instead of -(2^(M-1) - 1) with IXC.
// - UnsignedRoundsToZero: it raises IOC and returns 0 for any nonzero
//   negative input to an unsigned conversion, before rounding. A value that
//   rounds to 0 (-0.5 towards zero, -0.25 to nearest) is 0 with IXC.
// Results are compared in the low ibits.
enum class Divergence { None, SignedIntoRange, UnsignedRoundsToZero, Unexplained };
inline Divergence fp_to_fixed_divergence(FixedResult arm, uint32_t dynarmic_bits, uint32_t dynarmic_flags, bool unsigned_,
    unsigned ibits = 32) {
    const uint32_t mask = ibits == 32 ? 0xffffffffu : (1u << ibits) - 1, minimum = 1u << (ibits - 1);
    dynarmic_bits &= mask;
    if (arm.bits == dynarmic_bits && arm.flags == dynarmic_flags)
        return Divergence::None;
    if (!unsigned_ && arm.bits == minimum + 1 && arm.flags == 0x10 && dynarmic_bits == minimum && dynarmic_flags == 0x01)
        return Divergence::SignedIntoRange;
    if (unsigned_ && arm.bits == 0 && arm.flags == 0x10 && dynarmic_bits == 0 && dynarmic_flags == 0x01)
        return Divergence::UnsignedRoundsToZero;
    return Divergence::Unexplained;
}

} // namespace arm_reference
