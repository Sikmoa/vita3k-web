// Independent mathematical lane references, plus real A32/Thumb translations.
namespace vector_compare_tests {
using namespace vector_tests;
enum class Operation { Equal, Greater, Minimum, Maximum, Absolute, Difference };
int64_t number(uint64_t bits_value, unsigned bits, bool is_signed) {
    return is_signed && (bits_value & (uint64_t(1) << (bits - 1)))
        ? int64_t(bits_value) - (int64_t(1) << bits) : int64_t(bits_value);
}
uint64_t expected_lane(Operation operation, unsigned bits, bool is_signed, uint64_t a, uint64_t b) {
    const auto x = number(a, bits, is_signed), y = number(b, bits, is_signed);
    switch (operation) {
    case Operation::Equal: return a == b ? UINT64_MAX : 0;
    case Operation::Greater: return x > y ? UINT64_MAX : 0;
    case Operation::Minimum: return uint64_t(std::min(x, y));
    case Operation::Maximum: return uint64_t(std::max(x, y));
    case Operation::Absolute: return uint64_t(x < 0 ? -x : x);
    case Operation::Difference: return uint64_t(x >= y ? x - y : y - x);
    }
    std::abort();
}

void ir_comparisons() {
    struct Family { Operation operation; bool is_signed; std::array<Op, 3> ops; };
    const std::array<Family, 9> families{{
        {Operation::Equal, false, {Op::VectorEqual8, Op::VectorEqual16, Op::VectorEqual32}},
        {Operation::Greater, true, {Op::VectorGreaterS8, Op::VectorGreaterS16, Op::VectorGreaterS32}},
        {Operation::Minimum, true, {Op::VectorMinS8, Op::VectorMinS16, Op::VectorMinS32}},
        {Operation::Minimum, false, {Op::VectorMinU8, Op::VectorMinU16, Op::VectorMinU32}},
        {Operation::Maximum, true, {Op::VectorMaxS8, Op::VectorMaxS16, Op::VectorMaxS32}},
        {Operation::Maximum, false, {Op::VectorMaxU8, Op::VectorMaxU16, Op::VectorMaxU32}},
        {Operation::Absolute, true, {Op::VectorAbs8, Op::VectorAbs16, Op::VectorAbs32}},
        {Operation::Difference, true, {Op::VectorSignedAbsoluteDifference8, Op::VectorSignedAbsoluteDifference16, Op::VectorSignedAbsoluteDifference32}},
        {Operation::Difference, false, {Op::VectorUnsignedAbsoluteDifference8, Op::VectorUnsignedAbsoluteDifference16, Op::VectorUnsignedAbsoluteDifference32}},
    }};
    Harness h;
    for (const auto &family : families) for (unsigned size = 0; size < 3; ++size) {
        const unsigned bits = 8u << size;
        const uint64_t sign = uint64_t(1) << (bits - 1), mask = (sign << 1) - 1;
        for (unsigned fixed = 0; fixed < 3; ++fixed) {
            auto edges = initial(0);
            // At 32 bits all four pairs are present; narrow vectors repeat
            // them across word boundaries. Use exact mathematical expectations
            // at INT_MIN/INT_MAX, equality and opposite unsigned orderings.
            const std::array<uint64_t, 4> a = fixed == 1
                ? std::array<uint64_t, 4>{sign, sign - 1, sign, 0}
                : std::array<uint64_t, 4>{mask, 1, 0, sign + 1};
            const std::array<uint64_t, 4> b = fixed == 1
                ? std::array<uint64_t, 4>{sign - 1, sign, sign, mask}
                : std::array<uint64_t, 4>{0, mask, 0, sign};
            for (unsigned i = 0; i < 128 / bits; ++i) {
                set_lane(edges, bits, i, a[i % 4]);
                set_lane(edges, bits, i, b[i % 4], 1);
            }
            h.test(family.ops[size], [&](auto &block) {
                const auto input = [&](unsigned q) {
                    return fixed ? append(block, Op::Pack2x64To1x128,
                        {Value{lane(edges, 64, 0, q)}, Value{lane(edges, 64, 1, q)}}) : read_q(block, q);
                };
                const auto lhs = input(0);
                write_q(block, family.operation == Operation::Absolute
                    ? append(block, family.ops[size], {lhs})
                    : append(block, family.ops[size], {lhs, input(1)}));
            }, [&](auto &state) {
                const auto before = fixed ? edges : state;
                for (unsigned i = 0; i < 128 / bits; ++i)
                    set_lane(state, bits, i, expected_lane(family.operation, bits, family.is_signed,
                        lane(before, bits, i), lane(before, bits, i, 1)));
            });
        }
    }
    CHECK(h.missing.empty());
    std::printf("NEON integer comparisons: %u independent IR fixtures passed\n", h.cases);
}

void guest_comparisons(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    for (bool region : {false, true}) {
        jit.set_region_mode(region);
        for (bool thumb : {false, true}) for (bool quad : {false, true}) {
            for (unsigned size = 0; size < 3; ++size) {
                const unsigned bits = 8u << size, d = quad ? 30 : 31;
                // GT, GE, MAX, MIN, ABD, ABA, EQ, TST. First six have S/U forms.
                const std::array<uint32_t, 8> bases{0xf2000300, 0xf2000310, 0xf2000600,
                    0xf2000610, 0xf2000700, 0xf2000710, 0xf3000810, 0xf2000810};
                for (unsigned kind = 0; kind < bases.size(); ++kind) for (bool u : {false, true}) {
                    if (kind >= 6 && u) continue;
                    for (bool alias_m : {false, true}) {
                        const unsigned n = alias_m ? 2 : d, m = alias_m ? d : 2;
                        const uint32_t opcode = bases[kind] | (u ? 1u << 24 : 0) | (size << 20)
                            | ((d >> 4) << 22) | ((d & 15) << 12)
                            | ((n >> 4) << 7) | ((n & 15) << 16)
                            | ((m >> 4) << 5) | (m & 15) | (quad ? 1u << 6 : 0);
                        guest_case(mem, jit, parent, thumb, opcode, [&](auto &s) {
                            const auto before = s;
                            const unsigned di = quad ? 0 : (d & 1) * 64 / bits;
                            const unsigned ni = quad ? 0 : (n & 1) * 64 / bits;
                            const unsigned mi = quad ? 0 : (m & 1) * 64 / bits;
                            for (unsigned i = 0; i < (quad ? 128u : 64u) / bits; ++i) {
                                const auto a = lane(before, bits, ni + i, n / 2), b = lane(before, bits, mi + i, m / 2);
                                uint64_t result;
                                if (kind == 0) result = expected_lane(Operation::Greater, bits, !u, a, b);
                                else if (kind == 1) result = number(a, bits, !u) >= number(b, bits, !u) ? UINT64_MAX : 0;
                                else if (kind == 2) result = expected_lane(Operation::Maximum, bits, !u, a, b);
                                else if (kind == 3) result = expected_lane(Operation::Minimum, bits, !u, a, b);
                                else if (kind == 4 || kind == 5) {
                                    result = expected_lane(Operation::Difference, bits, !u, a, b);
                                    if (kind == 5) result += lane(before, bits, di + i, d / 2);
                                } else if (kind == 6) result = a == b ? UINT64_MAX : 0;
                                else result = (a & b) != 0 ? UINT64_MAX : 0;
                                set_lane(s, bits, di + i, result, d / 2);
                            }
                        });
                    }
                }
                // GT/GE/EQ/LE/LT against zero and non-saturating ABS, with
                // d=m. These exercise translator combinations, not raw ops.
                for (unsigned kind : {0u, 1u, 2u, 3u, 4u, 6u}) {
                    const uint32_t opcode = 0xf3b10000 | (kind << 7) | (size << 18)
                        | ((d >> 4) << 22) | ((d & 15) << 12)
                        | ((d >> 4) << 5) | (d & 15) | (quad ? 1u << 6 : 0);
                    guest_case(mem, jit, parent, thumb, opcode, [&](auto &s) {
                        const auto before = s;
                        for (unsigned i = quad ? 0 : 64 / bits; i < 128 / bits; ++i) {
                            const auto a = lane(before, bits, i, 15);
                            const auto x = number(a, bits, true);
                            const bool predicate = kind == 0 ? x > 0 : kind == 1 ? x >= 0
                                : kind == 2 ? x == 0 : kind == 3 ? x <= 0 : x < 0;
                            const auto result = kind == 6 ? expected_lane(Operation::Absolute, bits, true, a, 0)
                                : predicate ? UINT64_MAX : 0;
                            set_lane(s, bits, i, result, 15);
                        }
                    });
                }
            }
        }
    }
}
} // namespace vector_compare_tests
