// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "fp64.h"

#include "dynarmic/common/fp/op.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/fp/fpsr.h"
#include "dynarmic/common/fp/op/FPToFixed.h"
#include "dynarmic/common/fp/rounding_mode.h"

namespace vita3k::wasmjit {
namespace {

constexpr uint64_t sign_bit = UINT64_C(0x8000000000000000);
constexpr uint64_t fraction_mask = UINT64_C(0x000fffffffffffff);
constexpr uint64_t hidden_bit = UINT64_C(0x0010000000000000);
constexpr uint64_t infinity = UINT64_C(0x7ff0000000000000);
constexpr uint64_t quiet_bit = UINT64_C(0x0008000000000000);
constexpr uint64_t default_nan = infinity | quiet_bit;
constexpr uint32_t ioc = 1u << 0;
constexpr uint32_t dzc = 1u << 1;
constexpr uint32_t ofc = 1u << 2;
constexpr uint32_t ufc = 1u << 3;
constexpr uint32_t ixc = 1u << 4;
constexpr uint32_t idc = 1u << 7;
constexpr uint32_t fz = 1u << 24;
constexpr uint32_t dn = 1u << 25;

enum class Kind { Zero, Finite, Infinity, QuietNaN, SignalingNaN };

struct Operand {
    Kind kind;
    bool sign;
    // For finite nonzero: value = significand * 2^(exponent - 52),
    // with bit 52 set, including normalized subnormal inputs.
    int exponent;
    uint64_t significand;
};

Operand unpack(uint64_t bits, uint32_t fpscr, uint32_t &flags) noexcept {
    const bool sign = (bits & sign_bit) != 0;
    const unsigned exp = unsigned((bits >> 52) & 0x7ff);
    uint64_t sig = bits & fraction_mask;
    if (exp == 0x7ff) {
        return {sig == 0 ? Kind::Infinity : (sig & quiet_bit) ? Kind::QuietNaN : Kind::SignalingNaN, sign, 0, 0};
    }
    if (exp != 0)
        return {Kind::Finite, sign, int(exp) - 1023, sig | hidden_bit};
    if (sig == 0)
        return {Kind::Zero, sign, 0, 0};
    if (fpscr & fz) {
        flags |= idc;
        return {Kind::Zero, sign, 0, 0};
    }
    int exponent = -1022;
    while ((sig & hidden_bit) == 0) {
        sig <<= 1;
        --exponent;
    }
    return {Kind::Finite, sign, exponent, sig};
}

bool is_nan(Kind kind) noexcept {
    return kind == Kind::QuietNaN || kind == Kind::SignalingNaN;
}

// Preserve all information relevant to subsequent rounding: bit zero is
// sticky. In particular, never shift by the integer type's width.
uint64_t shift_right_jam(uint64_t value, unsigned shift) noexcept {
    if (shift == 0)
        return value;
    if (shift >= 64)
        return uint64_t(value != 0);
    return (value >> shift) | uint64_t((value << (64 - shift)) != 0);
}

// Round a nonzero magnitude * 2^scale. Arithmetic below retains at least
// nine low guard/sticky bits for normal results. No intermediate binary64
// rounding occurs. Tininess is tested BEFORE rounding, matching Dynarmic's
// common/fp/unpacked.cpp FPRoundBase, not host after-rounding underflow.
FP64Result round_pack(bool sign, uint64_t magnitude, int scale, uint32_t fpscr, uint32_t flags) noexcept {
    const uint64_t sign_bits = sign ? sign_bit : 0;
    int top = 0;
    for (uint64_t scan = magnitude; scan >>= 1;)
        ++top;
    const int exponent = scale + top;
    const bool tiny = exponent < -1022;
    if (tiny && (fpscr & fz))
        return {sign_bits, flags | ufc}; // ARM FZ: no IXC, even if rounding would make it normal.

    // Normal spacing is 2^(exponent-52); subnormal spacing is 2^-1074.
    const int unit = tiny ? -1074 : exponent - 52;
    const int shift = unit - scale;
    uint64_t retained;
    bool inexact = false;
    bool above_half = false;
    bool exactly_half = false;
    if (shift <= 0) {
        // top - shift <= 52 here, so this cannot overflow or shift by 64.
        retained = magnitude << unsigned(-shift);
    } else if (shift < 64) {
        retained = magnitude >> unsigned(shift);
        const uint64_t remainder = magnitude & ((UINT64_C(1) << unsigned(shift)) - 1);
        const uint64_t half = UINT64_C(1) << unsigned(shift - 1);
        inexact = remainder != 0;
        above_half = remainder > half;
        exactly_half = remainder == half;
    } else {
        retained = 0;
        inexact = true;
        // With shift > 64 the nonzero magnitude is strictly below half.
        above_half = shift == 64 && magnitude > sign_bit;
        exactly_half = shift == 64 && magnitude == sign_bit;
    }

    const unsigned mode = (fpscr >> 22) & 3;
    const bool increment = mode == 0 ? (above_half || (exactly_half && (retained & 1)))
        : mode == 1                 ? (inexact && !sign)
        : mode == 2                 ? (inexact && sign)
                                    : false;
    if (tiny && inexact)
        flags |= ufc;
    if (inexact)
        flags |= ixc;
    if (increment)
        ++retained;

    // Rounding a subnormal up to min-normal naturally sets exponent bit 0.
    if (tiny)
        return {sign_bits | retained, flags};

    int rounded_exponent = exponent;
    if (retained >= (hidden_bit << 1)) {
        retained >>= 1;
        ++rounded_exponent;
    }
    if (rounded_exponent > 1023) {
        const bool to_infinity = mode == 0 || (mode == 1 && !sign) || (mode == 2 && sign);
        return {sign_bits | (to_infinity ? infinity : infinity - 1), flags | ofc | ixc};
    }
    return {sign_bits | (uint64_t(rounded_exponent + 1023) << 52) | (retained & fraction_mask), flags};
}

// Exact 53 x 53 product, reduced to 62/63 bits with a sticky tail. Using
// 32-bit limbs avoids both a dependency on Dynarmic's u128 and a compiler
// __int128 requirement on non-WASM native test hosts. All products fit u64.
uint64_t product_jam(uint64_t a, uint64_t b) noexcept {
    const uint64_t a_lo = uint32_t(a), a_hi = a >> 32;
    const uint64_t b_lo = uint32_t(b), b_hi = b >> 32;
    const uint64_t low_product = a_lo * b_lo;
    const uint64_t cross = a_hi * b_lo + a_lo * b_hi + (low_product >> 32);
    const uint64_t low = (cross << 32) | uint32_t(low_product);
    const uint64_t high = a_hi * b_hi + (cross >> 32);
    // Full product >> 43, jamming the discarded low 43 bits.
    return (high << 21) | (low >> 43) | uint64_t((low & ((UINT64_C(1) << 43) - 1)) != 0);
}

// floor((a / b) * 2^62), with nonzero remainder jammed into bit zero.
// Both operands have 53 bits, so remainder*2 always fits uint64_t.
// Fixed 62-step restoring division avoids a wasm __udivti3 dependency.
uint64_t quotient_jam(uint64_t a, uint64_t b) noexcept {
    uint64_t quotient = 0;
    uint64_t remainder = a;
    if (remainder >= b) {
        remainder -= b;
        quotient = 1;
    }
    for (unsigned i = 0; i < 62; ++i) {
        remainder <<= 1;
        quotient <<= 1;
        if (remainder >= b) {
            remainder -= b;
            quotient |= 1;
        }
    }
    return quotient | uint64_t(remainder != 0);
}

// Binary32 lane helpers backed by the vendored Dynarmic implementation
// (common/fp/op/FPRecipEstimate.cpp, FPRecipStepFused.cpp). The A32 decoder
// emits fpcr_controlled=false for the vector RECPE/VRECPS instructions, which
// means the STANDARD FPSCR value (FPCR: RN, FZ=0, DN=0; AHP irrelevant at
// esize 32) and the FPSR cumulative flags in bits [7,4:0]. Exception enables
// are rejected by the emitter before the helper can run, so FPProcessException
// never hits its ASSERT_FALSE trap path. The u32 result and the newly raised
// flag bits map one-to-one onto the FP64Result contract.
FP64Result fp32_lane_estimate(uint32_t operation, uint32_t lane_a, uint32_t lane_b) noexcept {
    const Dynarmic::FP::FPCR fpcr{0}; // standard FPSCR: RN, FZ=0, DN=0
    Dynarmic::FP::FPSR fpsr{0};       // cumulative flags, freshly cleared
    uint32_t result;
    if (operation == 4) {
        result = Dynarmic::FP::FPRecipEstimate<uint32_t>(lane_a, fpcr, fpsr);
    } else {
        result = Dynarmic::FP::FPRecipStepFused<uint32_t>(lane_a, lane_b, fpcr, fpsr);
    }
    return {result, fpsr.Value() & 0x9f};
}

// Binary32 lane float-to-int VCVT backed by the vendored Dynarmic
// implementation (common/fp/op/FPToFixed.cpp). The A32 translator emits the
// standard VCVT shape (fbits=0, TowardsZero, fpcr_controlled=false), so the
// conversion always runs as FPToFixed(ibits=32, fbits=0, TowardsZero) under
// the standard FPSCR value. The emitter rejects any other immediate shape
// before the helper can run. The u32 result and the newly raised flag bits
// map one-to-one onto the FP64Result contract.
FP64Result fp32_lane_to_fixed(uint32_t operation, uint32_t lane) noexcept {
    const Dynarmic::FP::FPCR fpcr{0}; // standard FPSCR: RN, FZ=0, DN=0
    Dynarmic::FP::FPSR fpsr{0};       // cumulative flags, freshly cleared
    const bool unsigned_ = operation == 7;
    const uint64_t result = Dynarmic::FP::FPToFixed<uint32_t>(
        32, lane, 0, unsigned_, fpcr, Dynarmic::FP::RoundingMode::TowardsZero, fpsr);
    return {result & 0xffffffffu, fpsr.Value() & 0x9f};
}

} // namespace

} // namespace vita3k::wasmjit

// Dispatch boundary: keep the fp64_arithmetic entry point after the anonymous
// namespace so the helpers above stay file-local.
namespace vita3k::wasmjit {

FP64Result fp64_arithmetic(uint32_t operation, uint64_t a, uint64_t b, uint32_t fpscr) noexcept {
    // Vector RECPE/VRECPS binary32 lane estimates (see fp64.h). The emitter
    // packs the a lane into memory_value[0] and, for VRECPS, the b lane into
    // memory_value[2]; the i64 arguments arrive as those packed words.
    if (operation == 4)
        return fp32_lane_estimate(4, uint32_t(a), uint32_t(b));
    if (operation == 5)
        return fp32_lane_estimate(5, uint32_t(a), uint32_t(b));
    if (operation == 6)
        return fp32_lane_to_fixed(6, uint32_t(a));
    if (operation == 7)
        return fp32_lane_to_fixed(7, uint32_t(a));
    if (operation > 7)
        return {default_nan, ioc};

    uint32_t flags = 0;
    Operand lhs = unpack(a, fpscr, flags);
    Operand rhs = unpack(b, fpscr, flags);
    // Unpack BOTH operands before NaN selection: even a NaN operation may
    // raise IDC from the other input. Select first signaling, then first quiet.
    // In particular subtraction must not negate rhs before processing NaNs.
    if (is_nan(lhs.kind) || is_nan(rhs.kind)) {
        uint64_t selected;
        if (lhs.kind == Kind::SignalingNaN) {
            selected = a;
            flags |= ioc;
        } else if (rhs.kind == Kind::SignalingNaN) {
            selected = b;
            flags |= ioc;
        } else {
            selected = is_nan(lhs.kind) ? a : b;
        }
        return {(fpscr & dn) ? default_nan : (selected | quiet_bit), flags};
    }

    if (operation <= 1) {
        rhs.sign = rhs.sign != (operation == 1);
        if (lhs.kind == Kind::Infinity || rhs.kind == Kind::Infinity) {
            if (lhs.kind == Kind::Infinity && rhs.kind == Kind::Infinity && lhs.sign != rhs.sign)
                return {default_nan, flags | ioc};
            const bool sign = lhs.kind == Kind::Infinity ? lhs.sign : rhs.sign;
            return {infinity | (sign ? sign_bit : 0), flags};
        }
        const bool negative_zero = ((fpscr >> 22) & 3) == 2;
        if (lhs.kind == Kind::Zero && rhs.kind == Kind::Zero) {
            const bool sign = lhs.sign == rhs.sign ? lhs.sign : negative_zero;
            return {sign ? sign_bit : 0, flags};
        }
        if (lhs.kind == Kind::Zero)
            return round_pack(rhs.sign, rhs.significand, rhs.exponent - 52, fpscr, flags);
        if (rhs.kind == Kind::Zero)
            return round_pack(lhs.sign, lhs.significand, lhs.exponent - 52, fpscr, flags);

        // Put the larger magnitude first, leaving cancellation nonnegative.
        if (lhs.exponent < rhs.exponent || (lhs.exponent == rhs.exponent && lhs.significand < rhs.significand)) {
            const Operand temporary = lhs;
            lhs = rhs;
            rhs = temporary;
        }
        const uint64_t large = lhs.significand << 10;
        const uint64_t small = shift_right_jam(rhs.significand << 10, unsigned(lhs.exponent - rhs.exponent));
        const uint64_t magnitude = lhs.sign == rhs.sign ? large + small : large - small;
        if (magnitude == 0)
            return {negative_zero ? sign_bit : 0, flags};
        // If alignment discarded bits, exponent difference >= 11: subtraction
        // can then cancel at most one leading bit, leaving ample guard bits.
        // If cancellation is deeper, alignment and subtraction were exact.
        return round_pack(lhs.sign, magnitude, lhs.exponent - 62, fpscr, flags);
    }

    const bool sign = lhs.sign != rhs.sign;
    const uint64_t sign_bits = sign ? sign_bit : 0;
    const bool lhs_zero = lhs.kind == Kind::Zero, rhs_zero = rhs.kind == Kind::Zero;
    const bool lhs_inf = lhs.kind == Kind::Infinity, rhs_inf = rhs.kind == Kind::Infinity;
    if (operation == 2) {
        if ((lhs_inf && rhs_zero) || (rhs_inf && lhs_zero))
            return {default_nan, flags | ioc};
        if (lhs_inf || rhs_inf)
            return {sign_bits | infinity, flags};
        if (lhs_zero || rhs_zero)
            return {sign_bits, flags};
        return round_pack(sign, product_jam(lhs.significand, rhs.significand), lhs.exponent + rhs.exponent - 61, fpscr, flags);
    }

    if ((lhs_zero && rhs_zero) || (lhs_inf && rhs_inf))
        return {default_nan, flags | ioc};
    // Infinity / zero is infinity WITHOUT DZC: only finite nonzero / zero
    // raises divide-by-zero. Handle infinity before the zero denominator.
    if (lhs_inf)
        return {sign_bits | infinity, flags};
    if (rhs_inf)
        return {sign_bits, flags};
    if (rhs_zero)
        return {sign_bits | infinity, flags | dzc};
    if (lhs_zero)
        return {sign_bits, flags};
    return round_pack(sign, quotient_jam(lhs.significand, rhs.significand), lhs.exponent - rhs.exponent - 62, fpscr, flags);
}

} // namespace vita3k::wasmjit
