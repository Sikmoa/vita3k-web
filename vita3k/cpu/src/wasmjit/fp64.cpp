// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "fp64.h"

#include <bit>
#include <cmath>

#include "dynarmic/common/fp/op.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/fp/fpsr.h"

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

// ARM FPSqrt on binary64. The vendored Dynarmic has no portable square root
// (its x64 backend uses the host instruction), so this follows the ARM
// pseudocode directly and rounds through round_pack like the other operations.
FP64Result sqrt64(uint64_t a, uint32_t fpscr) noexcept {
    uint32_t flags = 0;
    const Operand x = unpack(a, fpscr, flags);
    if (is_nan(x.kind)) {
        if (x.kind == Kind::SignalingNaN)
            flags |= ioc;
        return {(fpscr & dn) ? default_nan : (a | quiet_bit), flags};
    }
    if (x.kind == Kind::Zero) // includes an FZ-flushed subnormal (IDC already set)
        return {x.sign ? sign_bit : 0, flags};
    if (x.sign)
        return {default_nan, flags | ioc};
    if (x.kind == Kind::Infinity)
        return {infinity, flags};
    // value = m * 2^scale with an even scale; m < 2^54 is 27 bit pairs.
    uint64_t m = x.significand;
    int scale = x.exponent - 52;
    if (scale & 1) {
        m <<= 1;
        --scale;
    }
    // Digit-by-digit floor(sqrt(m * 4^34)): a 61-bit root (8 bits below the
    // 53-bit result) with the remainder jammed into bit zero. The remainder
    // stays below 2^62, so the shifted remainder never overflows.
    constexpr int extra_pairs = 34;
    uint64_t root = 0, remainder = 0;
    for (int pair = 27 + extra_pairs - 1; pair >= 0; --pair) {
        const uint64_t bits = pair >= extra_pairs ? (m >> (2 * (pair - extra_pairs))) & 3 : 0;
        remainder = (remainder << 2) | bits;
        const uint64_t trial = (root << 2) | 1;
        root <<= 1;
        if (remainder >= trial) {
            remainder -= trial;
            root |= 1;
        }
    }
    return round_pack(false, root | uint64_t(remainder != 0), scale / 2 - extra_pairs, fpscr, flags);
}

// Binary32 lane helpers backed by the vendored Dynarmic implementation
// (common/fp/op/FPRecipEstimate.cpp, FPRecipStepFused.cpp, FPRSqrtEstimate.cpp,
// FPRSqrtStepFused.cpp). The A32 decoder emits fpcr_controlled=false for the
// vector RECPE/VRECPS/VRSQRTE/VRSQRTS instructions, which
// means the STANDARD FPSCR value (FPCR: RN, FZ=1, DN=1; AHP/FZ16 irrelevant
// at esize 32) and the FPSR cumulative flags in bits [7,4:0]. Exception enables
// are rejected by the emitter before the helper can run, so FPProcessException
// never hits its ASSERT_FALSE trap path. The u32 result and the newly raised
// flag bits map one-to-one onto the FP64Result contract.
FP64Result fp32_lane_estimate(uint32_t operation, uint32_t lane_a, uint32_t lane_b) noexcept {
    const auto fpcr = Dynarmic::FP::FPCR{0}.ASIMDStandardValue(); // RN, FZ=1, DN=1
    Dynarmic::FP::FPSR fpsr{0}; // cumulative flags, freshly cleared
    uint32_t result;
    switch (operation) {
    case 4: result = Dynarmic::FP::FPRecipEstimate<uint32_t>(lane_a, fpcr, fpsr); break;
    case 5: result = Dynarmic::FP::FPRecipStepFused<uint32_t>(lane_a, lane_b, fpcr, fpsr); break;
    case 8: result = Dynarmic::FP::FPRSqrtEstimate<uint32_t>(lane_a, fpcr, fpsr); break;
    default: result = Dynarmic::FP::FPRSqrtStepFused<uint32_t>(lane_a, lane_b, fpcr, fpsr); break;
    }
    return {result, fpsr.Value() & 0x9f};
}

// Lane fast paths for the hot vector estimates and conversions (Limbo runs
// ~1M of them a second). Each returns exactly what the vendored Dynarmic
// functions above return, flags included; wasmjit_recip_tests.inc and
// wasmjit_tofixed_tests.inc check them against Dynarmic called directly.

// VRECPE: for a normal input, FPRecipEstimate depends only on the sign, the
// exponent and the top 8 fraction bits (it scales the mantissa to 9 bits), so
// results are memoized per bits >> 15 from the vendored implementation.
// Zero, subnormal (flushed, IDC), infinity and NaN inputs call it directly.
FP64Result recip_estimate_lane(uint32_t lane) noexcept {
    const uint32_t exponent = (lane >> 23) & 0xff;
    if (exponent == 0 || exponent == 0xff)
        return fp32_lane_estimate(4, lane, 0);
    static uint64_t memo[1u << 17]; // bits | flags << 32 | filled << 40
    uint64_t &entry = memo[lane >> 15];
    if (!(entry >> 40)) {
        const FP64Result result = fp32_lane_estimate(4, lane, 0);
        entry = (result.bits & 0xffffffffu) | uint64_t(result.flags) << 32 | uint64_t(1) << 40;
    }
    return {entry & 0xffffffffu, uint32_t(entry >> 32) & 0xff};
}

// VRECPS: 2.0 + (-a) * b fused, standard FPSCR (RN, FZ, DN). The product of
// two binary32 values is exact in binary64; the sum is rounded to odd (TwoSum
// error term) so that one binary32 rounding of it is correct, and the FZ
// underflow test sees the exact magnitude (rounding to odd never lands on the
// binary64 value 2^-126 from below).
FP64Result recip_step_lane(uint32_t a, uint32_t b) noexcept {
    uint32_t flags = 0;
    const auto flush = [&flags](uint32_t lane) {
        if ((lane & 0x7f800000u) == 0 && (lane & 0x007fffffu) != 0) {
            flags |= idc;
            return lane & 0x80000000u;
        }
        return lane;
    };
    const uint32_t x = flush(a ^ 0x80000000u), y = flush(b);
    const auto nan = [](uint32_t v) { return (v & 0x7fffffffu) > 0x7f800000u; };
    if (nan(x) || nan(y)) {
        const auto signaling = [&](uint32_t v) { return nan(v) && !(v & 0x00400000u); };
        if (signaling(x) || signaling(y))
            flags |= ioc;
        return {0x7fc00000u, flags};
    }
    const bool inf_x = (x & 0x7fffffffu) == 0x7f800000u, inf_y = (y & 0x7fffffffu) == 0x7f800000u;
    const bool zero_x = (x & 0x7fffffffu) == 0, zero_y = (y & 0x7fffffffu) == 0;
    if ((inf_x && zero_y) || (zero_x && inf_y))
        return {0x40000000u, flags}; // +2.0
    if (inf_x || inf_y)
        return {((x ^ y) & 0x80000000u) | 0x7f800000u, flags};
    const double product = double(std::bit_cast<float>(x)) * double(std::bit_cast<float>(y));
    double sum = 2.0 + product;
    const double back = sum - 2.0;
    const double error = (2.0 - (sum - back)) + (product - back);
    if (sum == 0.0)
        return {0, flags}; // exact zero: +0 under RN
    if (error != 0.0) {
        uint64_t bits = std::bit_cast<uint64_t>(sum);
        if (!(bits & 1))
            bits += ((error > 0.0) == (sum > 0.0)) ? 1 : uint64_t(-1); // toward the exact value
        sum = std::bit_cast<double>(bits);
    }
    if (std::fabs(sum) < 0x1p-126)
        return {sum < 0.0 ? 0x80000000u : 0u, flags | ufc};
    const float rounded = float(sum);
    if (std::isinf(rounded))
        return {std::bit_cast<uint32_t>(rounded), flags | ofc | ixc};
    if (error != 0.0 || double(rounded) != sum)
        flags |= ixc;
    return {std::bit_cast<uint32_t>(rounded), flags};
}

// Vector VCVT.S32/U32.F32 (FZ): ARM ARM FPToFixed(32, fbits) for one binary32
// lane in a Dynarmic::FP::RoundingMode. It rounds first and saturates second,
// unlike Dynarmic's FPToFixed: a negative value that rounds to 0 converts to
// an unsigned 0 with IXC, not IOC (wasmjit_tofixed_tests.inc). The scaled
// value, its floor and the error below 1 are exact in binary64.
FP64Result to_fixed_lane(uint32_t lane, bool unsigned_, uint32_t rounding, uint32_t fbits) noexcept {
    const uint32_t exponent = (lane >> 23) & 0xff, fraction = lane & 0x007fffffu;
    const auto saturate = [unsigned_](bool below) {
        return FP64Result{below ? (unsigned_ ? 0u : 0x80000000u) : (unsigned_ ? 0xffffffffu : 0x7fffffffu), ioc};
    };
    if (exponent == 0)
        return {0, fraction ? idc : 0u}; // zero, or a subnormal flushed to zero
    if (exponent == 0xff)
        return fraction ? FP64Result{0, ioc} : saturate(lane >> 31); // NaN, infinity
    double value = double(std::bit_cast<float>(lane));
    if (fbits)
        value = std::ldexp(value, int(fbits));
    const double down = std::floor(value);
    const double error = value - down;
    bool round_up = false;
    switch (rounding) {
    case 0: round_up = error > 0.5 || (error == 0.5 && std::floor(down * 0.5) != down * 0.5); break;
    case 1: round_up = error != 0.0; break;
    case 3: round_up = error != 0.0 && down < 0.0; break;
    case 4: round_up = error > 0.5 || (error == 0.5 && down >= 0.0); break;
    default: break;
    }
    const double result = down + (round_up ? 1.0 : 0.0);
    if (result < (unsigned_ ? 0.0 : -0x1p31) || result > (unsigned_ ? 0x1p32 - 1 : 0x1p31 - 1))
        return saturate(result < 0.0);
    return {unsigned_ ? uint32_t(result) : uint32_t(int32_t(result)), error != 0.0 ? ixc : 0u};
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
        return recip_estimate_lane(uint32_t(a));
    if (operation == 5)
        return recip_step_lane(uint32_t(a), uint32_t(b));
    if (operation == 6 || operation == 7)
        return to_fixed_lane(uint32_t(a), operation == 7, 3, 0);
    if (operation == 8 || operation == 9)
        return fp32_lane_estimate(operation, uint32_t(a), uint32_t(b));
    if (operation == 10)
        return sqrt64(a, fpscr);
    if ((operation == 11 || operation == 12) && (uint32_t(b) & 0xff) <= 4 && uint32_t(b) >> 8 <= 32)
        return to_fixed_lane(uint32_t(a), operation == 12, uint32_t(b) & 0xff, uint32_t(b) >> 8);
    if (operation > 10)
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
