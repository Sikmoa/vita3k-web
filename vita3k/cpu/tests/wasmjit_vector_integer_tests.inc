// Source-only fixtures; see browser/tests/JIT_VECTOR_INTEGER_ARITHMETIC.md.
// Reuse the existing independent IR and ARM/Thumb production-JIT harness.
namespace vector_integer_tests {
using namespace vector_tests;

enum class Arithmetic { Add, Subtract, Multiply, Accumulate, SubtractProduct };
uint64_t reference(Arithmetic operation, uint64_t a, uint64_t b, uint64_t d = 0) {
    // Unsigned 64-bit modular arithmetic is defined even on overflow. The
    // byte-based set_lane reference truncates to the architectural lane width.
    switch (operation) {
    case Arithmetic::Add: return a + b;
    case Arithmetic::Subtract: return a - b;
    case Arithmetic::Multiply: return a * b;
    case Arithmetic::Accumulate: return d + a * b;
    case Arithmetic::SubtractProduct: return d - a * b;
    }
    std::abort();
}

void ir_arithmetic() {
    Harness h;
    const std::array<Op, 4> adds{Op::VectorAdd8, Op::VectorAdd16, Op::VectorAdd32, Op::VectorAdd64};
    const std::array<Op, 4> subs{Op::VectorSub8, Op::VectorSub16, Op::VectorSub32, Op::VectorSub64};
    const std::array<Op, 3> muls{Op::VectorMultiply8, Op::VectorMultiply16, Op::VectorMultiply32};
    for (unsigned size = 0; size < 4; ++size) {
        const unsigned bits = 8u << size;
        for (const auto operation : {Arithmetic::Add, Arithmetic::Subtract, Arithmetic::Multiply}) {
            if (bits == 64 && operation == Arithmetic::Multiply) continue; // not an A32 VMUL width
            const auto opcode = operation == Arithmetic::Add ? adds[size]
                : operation == Arithmetic::Subtract ? subs[size] : muls[size];
            h.test(opcode, [&](auto &b) {
                write_q(b, append(b, opcode, {read_q(b), read_q(b, 1)}));
            }, [&](auto &s) {
                const auto before = s;
                for (unsigned i = 0; i < 128 / bits; ++i)
                    set_lane(s, bits, i, reference(operation, lane(before, bits, i), lane(before, bits, i, 1)));
            });
            // Asymmetric constants put carry/borrow across bit 31 inside a
            // 64-bit lane, overflow at bit 63, and signed minima / adjacent
            // byte and halfword boundaries into the narrow lanes.
            const std::array<uint64_t, 2> a{0x7fff8000ffffffffull, 0x800000008001ff00ull};
            const std::array<uint64_t, 2> c{0x8000800100000001ull, 0x80000000ff017fffull};
            h.test(opcode, [&](auto &b) {
                const auto lhs = append(b, Op::Pack2x64To1x128, {Value{a[0]}, Value{a[1]}});
                const auto rhs = append(b, Op::Pack2x64To1x128, {Value{c[0]}, Value{c[1]}});
                write_q(b, append(b, opcode, {lhs, rhs}));
            }, [&](auto &s) {
                auto inputs = s;
                for (unsigned i = 0; i < 2; ++i) {
                    set_lane(inputs, 64, i, a[i]);
                    set_lane(inputs, 64, i, c[i], 1);
                }
                for (unsigned i = 0; i < 128 / bits; ++i)
                    set_lane(s, bits, i, reference(operation, lane(inputs, bits, i), lane(inputs, bits, i, 1)));
            });
        }
        if (bits < 64) {
            h.test(subs[size], [&](auto &b) {
                write_q(b, append(b, subs[size], {append(b, Op::ZeroVector), read_q(b)}));
            }, [&](auto &s) {
                const auto before = s;
                for (unsigned i = 0; i < 128 / bits; ++i)
                    set_lane(s, bits, i, uint64_t(0) - lane(before, bits, i));
            });
        }
    }
    CHECK(h.missing.empty());
    std::printf("NEON integer arithmetic: %u independent IR fixtures passed\n", h.cases);
}

void guest_arithmetic(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    // Opcode fields come from A32/decoder/asimd.inc:24-29,74,77,118.
    // Companion assembly source lets the verifier check these with VitaSDK.
    for (bool region : {false, true}) {
        jit.set_region_mode(region);
        for (bool thumb : {false, true}) for (bool quad : {false, true}) {
            for (unsigned size = 0; size < 4; ++size) {
                const unsigned bits = 8u << size;
                for (unsigned operation = 0; operation < 5; ++operation) {
                    if (size == 3 && operation >= 2) continue;
                    const std::array<uint32_t, 5> bases{0xf2000800, 0xf3000800,
                        0xf2000910, 0xf2000900, 0xf3000900};
                    // D-register numbers even for Q forms: d=n and d=m cases
                    // cover source/destination aliasing, high Q15 and odd D31.
                    for (bool alias_m : {false, true}) {
                        const unsigned d = quad ? 30 : 31;
                        const unsigned n = alias_m ? 2 : d, m = alias_m ? d : 2;
                        const uint32_t opcode = bases[operation] | (size << 20)
                            | ((d >> 4) << 22) | ((d & 15) << 12)
                            | ((n >> 4) << 7) | ((n & 15) << 16)
                            | ((m >> 4) << 5) | (m & 15) | (quad ? 1u << 6 : 0);
                        guest_case(mem, jit, parent, thumb, opcode, [&](auto &s) {
                            const auto before = s;
                            const unsigned lanes = (quad ? 128 : 64) / bits;
                            const unsigned di = quad ? 0 : (d & 1) * 64 / bits;
                            const unsigned ni = quad ? 0 : (n & 1) * 64 / bits;
                            const unsigned mi = quad ? 0 : (m & 1) * 64 / bits;
                            for (unsigned i = 0; i < lanes; ++i)
                                set_lane(s, bits, di + i, reference(static_cast<Arithmetic>(operation),
                                    lane(before, bits, ni + i, n / 2), lane(before, bits, mi + i, m / 2),
                                    lane(before, bits, di + i, d / 2)), d / 2);
                        });
                    }
                }
                if (size == 3) continue;
                // Integer VNEG q15,q15 / d31,d31; upper neighbor untouched.
                const unsigned d = quad ? 30 : 31;
                const uint32_t neg = 0xf3b10380 | (size << 18) | ((d >> 4) << 22)
                    | ((d & 15) << 12) | ((d >> 4) << 5) | (d & 15) | (quad ? 1u << 6 : 0);
                guest_case(mem, jit, parent, thumb, neg, [&](auto &s) {
                    const auto before = s;
                    const unsigned start = quad ? 0 : 64 / bits;
                    for (unsigned i = start; i < 128 / bits; ++i)
                        set_lane(s, bits, i, uint64_t(0) - lane(before, bits, i, 15), 15);
                });
            }
            for (unsigned size : {1u, 2u}) for (unsigned operation = 2; operation < 5; ++operation) {
                // q15/d31 times d3[3] (i16) or d3[1] (i32).
                const unsigned d = quad ? 30 : 31, bits = 8u << size;
                const uint32_t base = operation == 2 ? 0xf2800840
                    : operation == 3 ? 0xf2800040 : 0xf2800440;
                const uint32_t scalar = base | (quad ? 1u << 24 : 0) | (size << 20)
                    | ((d >> 4) << 22) | ((d & 15) << 12)
                    | ((d >> 4) << 7) | ((d & 15) << 16) | (1u << 5)
                    | (size == 1 ? 11u : 3u);
                guest_case(mem, jit, parent, thumb, scalar, [&](auto &s) {
                    const auto before = s;
                    const auto multiplier = lane(before, bits, 128 / bits - 1, 1);
                    const unsigned start = quad ? 0 : 64 / bits;
                    for (unsigned i = start; i < 128 / bits; ++i) {
                        const auto value = lane(before, bits, i, 15);
                        set_lane(s, bits, i, reference(static_cast<Arithmetic>(operation), value, multiplier, value), 15);
                    }
                });
            }
        }
    }
}
} // namespace vector_integer_tests
