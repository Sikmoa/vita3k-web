// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
// Included by fp64.cpp so every runtime/test target uses the same helpers.
#include "dynarmic/common/fp/info.h"
#include "dynarmic/common/fp/process_nan.h"
#include "dynarmic/common/fp/unpacked.h"

namespace vita3k::wasmjit {
namespace {
namespace FP = Dynarmic::FP;

template <typename T>
uint64_t to_fixed(T input, unsigned bits, unsigned fraction, bool unsigned_,
    unsigned rounding, FP::FPCR control, FP::FPSR &status) {
    const auto [type, negative, value] = FP::FPUnpack(input, control, status);
    if (type == FP::FPType::QNaN || type == FP::FPType::SNaN) { status.IOC(true); return 0; }
    if (type == FP::FPType::Zero) return 0;
    const uint64_t max = bits == 64 ? UINT64_MAX : (uint64_t(1) << bits) - 1;
    const uint64_t bound = unsigned_ ? max : negative ? uint64_t(1) << (bits - 1) : (uint64_t(1) << (bits - 1)) - 1;
    const int shift = value.exponent + int(fraction) - 62;
    uint64_t magnitude = 0;
    bool overflow = type == FP::FPType::Infinity;
    bool inexact = false, above = false, tie = false;
    if (!overflow && shift >= 0) {
        overflow = shift >= 64 || value.mantissa > (UINT64_MAX >> shift);
        if (!overflow) magnitude = value.mantissa << shift;
    } else if (!overflow) {
        const unsigned right = unsigned(-shift);
        if (right < 64) {
            magnitude = value.mantissa >> right;
            const uint64_t remainder = value.mantissa & ((uint64_t(1) << right) - 1);
            const uint64_t half = uint64_t(1) << (right - 1);
            inexact = remainder != 0; above = remainder > half; tie = remainder == half;
        } else {
            inexact = value.mantissa != 0;
        }
        const bool increment = rounding == 0 ? above || (tie && (magnitude & 1))
            : rounding == 1 ? inexact && !negative : rounding == 2 ? inexact && negative
            : rounding == 4 ? above || tie : false;
        if (increment) { ++magnitude; overflow = magnitude == 0; }
    }
    if (overflow || magnitude > bound || (unsigned_ && negative && magnitude != 0)) {
        status.IOC(true);
        if (unsigned_) return negative ? 0 : max;
        return negative ? uint64_t(1) << (bits - 1) : bound;
    }
    if (inexact) status.IXC(true);
    return (negative ? uint64_t(0) - magnitude : magnitude) & max;
}

template <typename T>
uint64_t extended_fp(FPOperation operation, T a, uint64_t b, uint64_t c,
    unsigned result_bits, bool unsigned_, FP::FPCR control, FP::FPSR &status) {
    using Info = FP::FPInfo<T>;
    const auto rounding = FP::RoundingMode(c);
    if (operation >= FPOperation::Add && operation <= FPOperation::Sqrt) {
        const uint32_t arithmetic = operation == FPOperation::Sqrt ? 10 : uint32_t(operation) - uint32_t(FPOperation::Add);
        if constexpr (sizeof(T) == 8) {
            const auto result = fp64_arithmetic(arithmetic, a, b, control.Value());
            status = FP::FPSR{status.Value() | result.flags};
            return result.bits;
        } else {
            // Binary32 division/sqrt need at most 48 bits to distinguish a
            // binary32 rounding boundary. Binary64 has 53; convert once more
            // using the requested direction and accumulate both sets of flags.
            const uint64_t left = FP::FPConvert<uint64_t>(a, control, control.RMode(), status);
            const uint64_t right = operation == FPOperation::Sqrt ? 0 : FP::FPConvert<uint64_t>(T(b), control, control.RMode(), status);
            const auto result = fp64_arithmetic(arithmetic, left, right, control.Value());
            status = FP::FPSR{status.Value() | result.flags};
            const T converted = FP::FPConvert<T>(result.bits, control, control.RMode(), status);
            if (control.FZ() && (converted & ~Info::sign_mask) == 0 && status.UFC()) status.IXC(false);
            return converted;
        }
    }
    switch (operation) {
    case FPOperation::MulAdd: return FP::FPMulAdd(a, T(b), T(c), control, status);
    case FPOperation::MulSub: return FP::FPMulSub(a, T(b), T(c), control, status);
    case FPOperation::RecipEstimate: return FP::FPRecipEstimate(a, control, status);
    case FPOperation::RecipExponent: {
        const auto [type, sign, value] = FP::FPUnpack(a, control, status);
        if (type == FP::FPType::QNaN || type == FP::FPType::SNaN)
            return FP::FPProcessNaN(type, a, control, status);
        const T exponent = (a & Info::exponent_mask) >> Info::explicit_mantissa_width;
        const T inverse = exponent ? T(~exponent) : T((1u << Info::exponent_width) - 2);
        return Info::Zero(sign) | ((uint64_t(inverse) << Info::explicit_mantissa_width) & Info::exponent_mask);
    }
    case FPOperation::RecipStep: return FP::FPRecipStepFused(a, T(b), control, status);
    case FPOperation::RSqrtEstimate: return FP::FPRSqrtEstimate(a, control, status);
    case FPOperation::RSqrtStep: return FP::FPRSqrtStepFused(a, T(b), control, status);
    case FPOperation::RoundInt:
        return FP::FPRoundInt(a, control, FP::RoundingMode(b), c != 0, status);
    case FPOperation::Convert:
        switch (result_bits) {
        case 16: if constexpr (sizeof(T) == 2) return a; else return FP::FPConvert<uint16_t>(a, control, FP::RoundingMode(b), status);
        case 32: if constexpr (sizeof(T) == 4) return a; else return FP::FPConvert<uint32_t>(a, control, FP::RoundingMode(b), status);
        default: if constexpr (sizeof(T) == 8) return a; else return FP::FPConvert<uint64_t>(a, control, FP::RoundingMode(b), status);
        }
    case FPOperation::ToFixed:
        return to_fixed(a, result_bits, unsigned(b), unsigned_, unsigned(c), control, status);
    case FPOperation::FromFixed: {
        constexpr unsigned width = sizeof(T) * 8;
        const bool negative = !unsigned_ && (a & (T(1) << (width - 1)));
        const uint64_t magnitude = negative ? uint64_t(T(0 - a)) : uint64_t(a);
        if (!magnitude) return 0;
        // FPUnpacked has 63 significand bits. Jam the one discarded bit for
        // unsigned inputs >= 2^63; both output formats need fewer than 63.
        const auto value = magnitude >> 63
            ? FP::FPUnpacked{negative, 63 - int(b), (magnitude >> 1) | (magnitude & 1)}
            : FP::ToNormalized(negative, -int(b), magnitude);
        return result_bits == 32 ? FP::FPRound<uint32_t>(value, control, rounding, status)
                                 : FP::FPRound<uint64_t>(value, control, rounding, status);
    }
    default: break;
    }
    const auto [ta, sa, va] = FP::FPUnpack(a, control, status);
    const auto [tb, sb, vb] = FP::FPUnpack(T(b), control, status);
    T right = T(b);
    if (ta == FP::FPType::Zero) a = Info::Zero(sa);
    if (tb == FP::FPType::Zero) right = Info::Zero(sb);
    if (operation == FPOperation::Equal || operation == FPOperation::Greater || operation == FPOperation::GreaterEqual) {
        if (ta == FP::FPType::QNaN || ta == FP::FPType::SNaN || tb == FP::FPType::QNaN || tb == FP::FPType::SNaN) {
            if (operation != FPOperation::Equal || ta == FP::FPType::SNaN || tb == FP::FPType::SNaN) status.IOC(true);
            return 0;
        }
        const bool equal = a == right || (ta == FP::FPType::Zero && tb == FP::FPType::Zero);
        const bool greater = !equal && (sa != sb ? !sa : sa ? a < right : a > right);
        const bool result = operation == FPOperation::Equal ? equal : operation == FPOperation::Greater ? greater : greater || equal;
        return result ? uint64_t(T(~T(0))) : 0;
    }
    if (operation == FPOperation::MulX) {
        if (const auto nan = FP::FPProcessNaNs(ta, tb, a, right, control, status)) return *nan;
        if ((ta == FP::FPType::Zero && tb == FP::FPType::Infinity)
            || (tb == FP::FPType::Zero && ta == FP::FPType::Infinity)) {
            const T two = T(Info::exponent_bias + 1) << Info::explicit_mantissa_width;
            return two | Info::Zero(sa != sb);
        }
        return FP::FPMulAdd(Info::Zero(sa != sb), a, right, control, status);
    }
    const bool numeric = operation == FPOperation::MinNumeric || operation == FPOperation::MaxNumeric;
    const bool maximum = operation == FPOperation::Max || operation == FPOperation::MaxNumeric;
    if (numeric && ta == FP::FPType::QNaN && tb != FP::FPType::QNaN && tb != FP::FPType::SNaN) return right;
    if (numeric && tb == FP::FPType::QNaN && ta != FP::FPType::QNaN && ta != FP::FPType::SNaN) return a;
    if (const auto nan = FP::FPProcessNaNs(ta, tb, a, right, control, status)) return *nan;
    if (ta == FP::FPType::Zero && tb == FP::FPType::Zero)
        return maximum ? (a & right) : (a | right);
    const bool less = sa != sb ? sa : sa ? a > right : a < right;
    return less == maximum ? right : a;
}
} // namespace

FP64Result fp_extended_arithmetic(uint32_t selector, uint64_t a, uint64_t b, uint64_t c, uint32_t fpscr) noexcept {
    const auto operation = FPOperation((selector >> 8) & 0xff);
    const unsigned result_bits = 16u << (selector & 3);
    const bool unsigned_ = (selector & 0x40) != 0;
    FP::FPCR control{fpscr};
    FP::FPSR status{0};
    uint64_t result;
    switch ((selector >> 4) & 3) {
    case 0: result = extended_fp(operation, uint16_t(a), b, c, result_bits, unsigned_, control, status); break;
    case 1: result = extended_fp(operation, uint32_t(a), b, c, result_bits, unsigned_, control, status); break;
    default: result = extended_fp(operation, a, b, c, result_bits, unsigned_, control, status); break;
    }
    return {result, status.Value() & 0x9f};
}
} // namespace vita3k::wasmjit
