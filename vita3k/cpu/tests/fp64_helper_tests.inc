// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
// Unwired, independent integer-bit fixtures. Include at global scope after
// defining CHECK(expr), link ../src/wasmjit/fp64.cpp, then call
// fp64_helper_tests::run(). No interpreter, host FP, or Dynarmic oracle.
#include "../src/wasmjit/fp64.h"

namespace fp64_helper_tests {

inline void run() {
    using vita3k::wasmjit::fp64_arithmetic;
    constexpr uint32_t rn = 0x00000000, rp = 0x00400000, rm = 0x00800000, rz = 0x00c00000;
    constexpr uint32_t fz = 0x01000000, dn = 0x02000000;
    constexpr uint32_t ioc = 0x01, dzc = 0x02, ofc = 0x04, ufc = 0x08, ixc = 0x10, idc = 0x80;
    struct Case {
        uint32_t op;
        uint64_t a, b;
        uint32_t fpscr;
        uint64_t bits;
        uint32_t flags;
    };
    constexpr Case cases[] = {
        // Exact ordinary arithmetic and cancellation, including distant exponents.
        {0, 0x3ff0000000000000, 0x4000000000000000, rn, 0x4008000000000000, 0},
        {1, 0x4008000000000000, 0x3ff0000000000000, rn, 0x4000000000000000, 0},
        {2, 0x3ff8000000000000, 0x4000000000000000, rn, 0x4008000000000000, 0},
        {3, 0x4008000000000000, 0x4000000000000000, rn, 0x3ff8000000000000, 0},
        {1, 0x3ff0000000000001, 0x3ff0000000000000, rn, 0x3cb0000000000000, 0},
        {1, 0x3ff0000000000000, 0x3fefffffffffffff, rn, 0x3ca0000000000000, 0},
        {1, 0x3fefffffffffffff, 0x3ff0000000000000, rn, 0xbca0000000000000, 0},
        {1, 0x3ff0000000000000, 0x0000000000000001, rn, 0x3ff0000000000000, ixc},
        {1, 0x3ff0000000000000, 0x0000000000000001, rm, 0x3fefffffffffffff, ixc},
        {1, 0x3ff0000000000000, 0x0000000000000001, rz, 0x3fefffffffffffff, ixc},
        {0, 0x3ff0000000000000, 0x0000000000000001, rp, 0x3ff0000000000001, ixc},
        {0, 0x0000000000000001, 0x3ff0000000000000, rn, 0x3ff0000000000000, ixc},
        // Nearest-even midpoint, odd/even retained LSB, and either side of midpoint.
        {0, 0x3ff0000000000000, 0x3ca0000000000000, rn, 0x3ff0000000000000, ixc},
        {0, 0x3ff0000000000001, 0x3ca0000000000000, rn, 0x3ff0000000000002, ixc},
        {0, 0x3ff0000000000000, 0x3c9fffffffffffff, rn, 0x3ff0000000000000, ixc},
        {0, 0x3ff0000000000000, 0x3ca0000000000001, rn, 0x3ff0000000000001, ixc},
        // Exact products must retain low limbs; division must retain the remainder.
        {2, 0x3ff0000000000001, 0x3ff0000000000001, rn, 0x3ff0000000000002, ixc},
        {2, 0x3ff0000000000001, 0x3ff0000000000001, rp, 0x3ff0000000000003, ixc},
        {2, 0x3fffffffffffffff, 0x3fffffffffffffff, rn, 0x400ffffffffffffe, ixc},
        {3, 0x3ff0000000000000, 0x3ff0000000000001, rn, 0x3feffffffffffffe, ixc},
        {3, 0x3ff0000000000000, 0x3ff0000000000001, rp, 0x3fefffffffffffff, ixc},
        {3, 0x3ff0000000000001, 0x3ff0000000000003, rn, 0x3feffffffffffffc, ixc},
        // Gradual underflow: exact subnormals do NOT raise UFC/IDC.
        {0, 0x0000000000000001, 0x0000000000000001, rn, 0x0000000000000002, 0},
        {1, 0x0010000000000000, 0x0000000000000001, rn, 0x000fffffffffffff, 0},
        {3, 0x0010000000000000, 0x4000000000000000, rn, 0x0008000000000000, 0},
        {2, 0x0000000000000001, 0x3ff0000000000000, rn, 0x0000000000000001, 0},
        {3, 0x0000000000000001, 0x0000000000000001, rn, 0x3ff0000000000000, 0},
        {3, 0x0000000000000001, 0x0000000000000002, rn, 0x3fe0000000000000, 0},
        {3, 0x0010000000000000, 0x0000000000000001, rn, 0x4330000000000000, 0},
        {3, 0x0000000000000001, 0x0010000000000000, rn, 0x3cb0000000000000, 0},
        {3, 0x0000000000000001, 0x4000000000000000, rn, 0x0000000000000000, ufc | ixc},
        {3, 0x0000000000000003, 0x4000000000000000, rn, 0x0000000000000002, ufc | ixc},
        {3, 0x8000000000000001, 0x4000000000000000, rn, 0x8000000000000000, ufc | ixc},
        {2, 0x0000000000000001, 0x0000000000000001, rn, 0x0000000000000000, ufc | ixc},
        {2, 0x0000000000000001, 0x0000000000000001, rp, 0x0000000000000001, ufc | ixc},
        {3, 0x0000000000000001, 0x7fefffffffffffff, rn, 0x0000000000000000, ufc | ixc},
        // Tiny BEFORE rounding: midpoint rounds to normal but still raises UFC.
        {2, 0x0010000000000000, 0x3fefffffffffffff, rn, 0x0010000000000000, ufc | ixc},
        {2, 0x0010000000000000, 0x3fefffffffffffff, rz, 0x000fffffffffffff, ufc | ixc},
        // ARM FZ flushes before rounding and raises UFC WITHOUT IXC.
        {2, 0x0010000000000000, 0x3fefffffffffffff, fz, 0x0000000000000000, ufc},
        {2, 0x8010000000000000, 0x3fefffffffffffff, fz | rm, 0x8000000000000000, ufc},
        {3, 0x0010000000000000, 0x4000000000000000, fz, 0x0000000000000000, ufc},
        {1, 0x0010000000000001, 0x0010000000000000, fz | rp, 0x0000000000000000, ufc},
        {1, 0x0010000000000000, 0x0000000000000001, fz, 0x0010000000000000, idc},
        {2, 0x8000000000000001, 0x3ff0000000000000, fz, 0x8000000000000000, idc},
        {3, 0x3ff0000000000000, 0x8000000000000001, fz, 0xfff0000000000000, idc | dzc},
        {3, 0x0000000000000001, 0x0000000000000002, fz, 0x7ff8000000000000, idc | ioc},
        {2, 0x7ff0000000000000, 0x0000000000000001, fz, 0x7ff8000000000000, idc | ioc},
        // Overflow vs a finite result at the top of the representable range.
        {0, 0x7fefffffffffffff, 0x0000000000000001, rn, 0x7fefffffffffffff, ixc},
        {0, 0x7fefffffffffffff, 0x0000000000000001, rp, 0x7ff0000000000000, ofc | ixc},
        {0, 0x7fefffffffffffff, 0x7c90000000000000, rn, 0x7ff0000000000000, ofc | ixc},
        {0, 0x7fefffffffffffff, 0x7c90000000000000, rz, 0x7fefffffffffffff, ixc},
        {3, 0x7fefffffffffffff, 0x0000000000000001, rn, 0x7ff0000000000000, ofc | ixc},
        // Invalid and exceptional finite/infinite cases.
        {0, 0x7ff0000000000000, 0xfff0000000000000, rn, 0x7ff8000000000000, ioc},
        {1, 0xfff0000000000000, 0xfff0000000000000, rn, 0x7ff8000000000000, ioc},
        {1, 0x3ff0000000000000, 0xfff0000000000000, rn, 0x7ff0000000000000, 0},
        {2, 0xfff0000000000000, 0x0000000000000000, rn, 0x7ff8000000000000, ioc},
        {2, 0x8000000000000000, 0x7ff0000000000000, rn, 0x7ff8000000000000, ioc},
        {2, 0xfff0000000000000, 0xc000000000000000, rn, 0x7ff0000000000000, 0},
        {3, 0x0000000000000000, 0x8000000000000000, rn, 0x7ff8000000000000, ioc},
        {3, 0xfff0000000000000, 0x7ff0000000000000, rn, 0x7ff8000000000000, ioc},
        {3, 0xfff0000000000000, 0x8000000000000000, rn, 0x7ff0000000000000, 0},
        {3, 0x3ff0000000000000, 0x8000000000000000, rn, 0xfff0000000000000, dzc},
        {3, 0xbff0000000000000, 0x7ff0000000000000, rn, 0x8000000000000000, 0},
        {3, 0x8000000000000000, 0xbff0000000000000, rn, 0x0000000000000000, 0},
        // flags are newly raised only: do not echo NZCV/QC or cumulative bits.
        {0, 0x3ff0000000000000, 0x3ff0000000000000, 0xf800009f, 0x4000000000000000, 0},
        // Out-of-range operation selectors (vector RECPE/VRECPS occupy 4/5,
        // vector float-to-int VCVT 6/7, vector VRSQRTE/VRSQRTS 8/9).
        {10, 0x3ff0000000000000, 0x3ff0000000000000, rn, 0x7ff8000000000000, ioc},
    };
    for (const auto &c : cases) {
        const auto actual = fp64_arithmetic(c.op, c.a, c.b, c.fpscr);
        CHECK(actual.bits == c.bits);
        CHECK(actual.flags == c.flags);
    }

    constexpr uint32_t modes[] = {rn, rp, rm, rz};
    // Each column is an independently specified rounding-mode expectation.
    constexpr uint64_t thirds[] = {0x3fd5555555555555, 0x3fd5555555555556, 0x3fd5555555555555, 0x3fd5555555555555};
    constexpr uint64_t negative_thirds[] = {0xbfd5555555555555, 0xbfd5555555555555, 0xbfd5555555555556, 0xbfd5555555555555};
    constexpr uint64_t overflow[] = {0x7ff0000000000000, 0x7ff0000000000000, 0x7fefffffffffffff, 0x7fefffffffffffff};
    constexpr uint64_t negative_overflow[] = {0xfff0000000000000, 0xffefffffffffffff, 0xfff0000000000000, 0xffefffffffffffff};
    constexpr uint64_t tiny[] = {0, 1, 0, 0};
    constexpr uint64_t negative_tiny[] = {0x8000000000000000, 0x8000000000000000, 0x8000000000000001, 0x8000000000000000};
    for (unsigned m = 0; m < 4; ++m) {
        const Case rounded[] = {
            {3, 0x3ff0000000000000, 0x4008000000000000, modes[m], thirds[m], ixc},
            {3, 0xbff0000000000000, 0x4008000000000000, modes[m], negative_thirds[m], ixc},
            {2, 0x7fefffffffffffff, 0x4000000000000000, modes[m], overflow[m], ofc | ixc},
            {2, 0xffefffffffffffff, 0x4000000000000000, modes[m], negative_overflow[m], ofc | ixc},
            {3, 0x0000000000000001, 0x4000000000000000, modes[m], tiny[m], ufc | ixc},
            {3, 0x8000000000000001, 0x4000000000000000, modes[m], negative_tiny[m], ufc | ixc},
        };
        for (const auto &c : rounded) {
            const auto actual = fp64_arithmetic(c.op, c.a, c.b, c.fpscr);
            CHECK(actual.bits == c.bits);
            CHECK(actual.flags == c.flags);
        }
        // All signed-zero combinations, all operations, every rounding mode.
        for (unsigned sa = 0; sa < 2; ++sa) {
            for (unsigned sb = 0; sb < 2; ++sb) {
                const uint64_t a = uint64_t(sa) << 63, b = uint64_t(sb) << 63;
                for (unsigned op = 0; op < 4; ++op) {
                    const bool effective_b = bool(sb) != (op == 1);
                    const bool zero_sign = op == 2 ? sa != sb : (bool(sa) == effective_b ? bool(sa) : m == 2);
                    const auto actual = fp64_arithmetic(op, a, b, modes[m]);
                    CHECK(actual.bits == (op == 3 ? 0x7ff8000000000000 : uint64_t(zero_sign) << 63));
                    CHECK(actual.flags == (op == 3 ? ioc : 0));
                }
            }
        }
        const auto cancellation = fp64_arithmetic(1, 0x3ff0000000000000, 0x3ff0000000000000, modes[m]);
        CHECK(cancellation.bits == (m == 2 ? 0x8000000000000000 : 0));
        CHECK(cancellation.flags == 0);
    }

    // NaN priority and sign/payload preservation for EVERY operation, including
    // subtraction's second operand. DN changes the result, never exception flags.
    constexpr uint32_t nan_controls[] = {0, dn};
    for (unsigned op = 0; op < 4; ++op) {
        for (uint32_t control : nan_controls) {
            const Case nans[] = {
                {op, 0xfff8000000000011, 0x7ff8000000000022, control, 0xfff8000000000011, 0},
                {op, 0x7ff8000000000011, 0xfff0000000000022, control, 0xfff8000000000022, ioc},
                {op, 0xfff0000000000011, 0x7ff0000000000022, control, 0xfff8000000000011, ioc},
                {op, 0xfff0000000000011, 0x7ff8000000000022, control, 0xfff8000000000011, ioc},
                {op, 0x3ff0000000000000, 0xfff8000000000022, control, 0xfff8000000000022, 0},
                {op, 0x3ff0000000000000, 0xfff0000000000022, control, 0xfff8000000000022, ioc},
                {op, 0xfff8000000000011, 0x0000000000000001, control | fz, 0xfff8000000000011, idc},
                {op, 0x8000000000000001, 0xfff0000000000022, control | fz, 0xfff8000000000022, idc | ioc},
            };
            for (const auto &c : nans) {
                const auto actual = fp64_arithmetic(c.op, c.a, c.b, c.fpscr);
                CHECK(actual.bits == (control ? 0x7ff8000000000000 : c.bits));
                CHECK(actual.flags == c.flags);
            }
        }
    }
}

} // namespace fp64_helper_tests
