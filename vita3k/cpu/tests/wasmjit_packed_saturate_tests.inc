// Scalar packed saturating arithmetic (ARM QADD8/QSUB8/QADD16/QSUB16 and
// unsigned UQADD/UQSUB family). Dynarmic lowers each to one U32->U32 IR op
// with no Q-flag update; the emitter lowers it lane by lane in plain i32.
// Also the Q-setting saturations (SSAT/USAT[16], QADD/QSUB/QDADD/QDSUB:
// SignedSaturation, UnsignedSaturation, SignedSaturated{Add,Sub}WithFlag32
// with GetOverflowFromOp into A32OrQFlag) and SEL (A32GetGEFlags +
// PackedSelect), against models of Dynarmic's IR definitions
// (backend/x64/emit_x64_saturation.cpp, emit_x64_packed.cpp).
namespace packed_saturate_tests {
using namespace vector_tests;

struct Case {
    Op opcode;
    unsigned bits;
    bool is_signed;
    bool is_add;
};

uint32_t reference(Case c, uint32_t x, uint32_t y) {
    const uint32_t mask = c.bits == 32 ? 0xffffffffu : ((1u << c.bits) - 1);
    uint32_t result = 0;
    for (unsigned i = 0; i < 32 / c.bits; ++i) {
        const unsigned shift = i * c.bits;
        uint32_t lane;
        if (!c.is_signed) {
            const uint32_t a = (x >> shift) & mask, b = (y >> shift) & mask;
            lane = c.is_add ? (a + b > mask ? mask : a + b) : (a < b ? 0 : a - b);
        } else {
            int32_t a = int32_t((x >> shift) & mask);
            int32_t b = int32_t((y >> shift) & mask);
            if (a & (1 << (c.bits - 1))) a -= (1 << c.bits);
            if (b & (1 << (c.bits - 1))) b -= (1 << c.bits);
            const int32_t s = c.is_add ? a + b : a - b;
            const int32_t max = (1 << (c.bits - 1)) - 1;
            const int32_t min = -(1 << (c.bits - 1));
            lane = uint32_t(s > max ? max : s < min ? min : s) & mask;
        }
        result |= lane << shift;
    }
    return result;
}

constexpr Case cases[] = {
    {Op::PackedSaturatedAddU8, 8, false, true},
    {Op::PackedSaturatedSubU8, 8, false, false},
    {Op::PackedSaturatedAddS8, 8, true, true},
    {Op::PackedSaturatedSubS8, 8, true, false},
    {Op::PackedSaturatedAddU16, 16, false, true},
    {Op::PackedSaturatedSubU16, 16, false, false},
    {Op::PackedSaturatedAddS16, 16, true, true},
    {Op::PackedSaturatedSubS16, 16, true, false},
};

void ir_saturation() {
    Harness h;
    for (const auto c : cases) {
        // Register patterns from the harness cover zero/all-ones/xorshift.
        h.test(c.opcode, [&](auto &b) {
            const auto a = append(b, Op::A32GetRegister, {Value{A32::Reg::R1}});
            const auto d = append(b, Op::A32GetRegister, {Value{A32::Reg::R2}});
            append(b, Op::A32SetRegister, {Value{A32::Reg::R0}, append(b, c.opcode, {a, d})});
        }, [&](auto &s) { s.regs[0] = reference(c, s.regs[1], s.regs[2]); });
        // Boundary immediates: every lane sees 0/min/max/adjacent values.
        const uint32_t a = c.bits == 8 ? 0xff80017fu : 0x8000ffffu;
        const uint32_t b = c.bits == 8 ? 0x017f8000u : 0x7fff0001u;
        h.test(c.opcode, [&](auto &b_) {
            append(b_, Op::A32SetRegister, {Value{A32::Reg::R0},
                append(b_, c.opcode, {Value{a}, Value{b}})});
        }, [&](auto &s) { s.regs[0] = reference(c, a, b); });
    }
    CHECK(h.missing.empty());
    std::printf("Packed saturation: %u independent IR fixtures passed\n", h.cases);
}

// UADD8: bytewise sum modulo 256; CPSR.GE[i] = carry out of byte i.
uint32_t uadd8(uint32_t x, uint32_t y, uint32_t &ge) {
    uint32_t result = 0;
    ge = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t sum = ((x >> (8 * i)) & 0xff) + ((y >> (8 * i)) & 0xff);
        result |= (sum & 0xff) << (8 * i);
        ge |= (sum >> 8) << i;
    }
    return result;
}

void ir_unsigned_add8() {
    Harness h;
    for (bool with_ge : {false, true}) {
        const auto build = [&](auto &b, Value a, Value d) {
            const auto sum = append(b, Op::PackedAddU8, {a, d});
            append(b, Op::A32SetRegister, {Value{A32::Reg::R0}, sum});
            if (with_ge) append(b, Op::A32SetGEFlags, {append(b, Op::GetGEFromOp, {sum})});
        };
        const auto expect = [&](JitState &s, uint32_t x, uint32_t y) {
            uint32_t ge;
            s.regs[0] = uadd8(x, y, ge);
            if (with_ge) s.cpsr = (s.cpsr & ~0x000f0000u) | (ge << 16);
        };
        h.test(Op::PackedAddU8, [&](auto &b) {
            build(b, append(b, Op::A32GetRegister, {Value{A32::Reg::R1}}),
                append(b, Op::A32GetRegister, {Value{A32::Reg::R2}}));
        }, [&](auto &s) { expect(s, s.regs[1], s.regs[2]); });
        // Byte sums of 0x100 (carry, zero byte), 0xff (no carry), 0x1fe and 0.
        for (const auto &[x, y] : std::array<std::pair<uint32_t, uint32_t>, 3>{{
                 {0x80ff7f00u, 0x80018000u}, {0xff01fe00u, 0xffff0100u}, {0x00ff00ffu, 0xff00ff01u}}}) {
            h.test(Op::PackedAddU8, [&](auto &b) { build(b, Value{x}, Value{y}); },
                [&](auto &s) { expect(s, x, y); });
        }
    }
    CHECK(h.missing.empty());
    std::printf("UADD8/GE: %u independent IR fixtures passed\n", h.cases);
}

void guest_unsigned_add8(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    for (bool region : {false, true}) {
        jit.set_region_mode(region);
        for (bool thumb : {false, true}) {
            for (const auto &[x, y] : std::array<std::pair<uint32_t, uint32_t>, 4>{{{0u, 0u},
                     {0xffffffffu, 0xffffffffu}, {0x80ff7f01u, 0x8001807fu}, {0xa58173dfu, 0x13579bdfu}}}) {
                // UADD8 r0, r1, r2
                if (thumb) put_thumb(mem, jit, {thumb_word(0xfa81f042), 0xbf00df00});
                else put(mem, jit, {0xe6510f92, 0xef000000});
                auto context = jit.save_context();
                context.cpu_registers[1] = x;
                context.cpu_registers[2] = y;
                context.cpsr = 0x000a0010u | (thumb ? 0x20 : 0); // stale GE bits must be replaced
                jit.load_context(context);
                if (jit.run() != 0) {
                    std::fprintf(stderr, "UADD8 guest failed: %s\n", jit.get_last_error().c_str());
                    CHECK(false);
                }
                uint32_t ge;
                const uint32_t sum = uadd8(x, y, ge);
                const auto result = jit.save_context();
                CHECK(result.cpu_registers[0] == sum);
                CHECK(result.cpsr == ((context.cpsr & ~0x000f0000u) | (ge << 16)));
                CHECK(jit.get_pc() == code + (thumb ? 6 : 8));
            }
        }
    }
    std::printf("UADD8/GE: ARM and Thumb guest execution passed (single+region)\n");
}

void guest_saturation(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    // Exact bytes from the retail EventManager crash (Thumb UQADD8 R4,R12,R4).
    // thumb_word swaps halfwords to LE storage; the trailing svc ends the run.
    for (bool region : {false, true}) {
        jit.set_region_mode(region);
        for (const auto &[r12, r4] :
            std::array<std::pair<uint32_t, uint32_t>, 4>{{{0x00000000u, 0x00000000u},
                {0xffffffffu, 0xffffffffu}, {0xff007f80u, 0x0180807fu}, {0xa58173dfu, 0x13579bdfu}}}) {
            put_thumb(mem, jit, {thumb_word(0xfa8cf454), 0xbf00df00});
            jit.set_reg(12, r12);
            jit.set_reg(4, r4);
            if (jit.run() != 0) {
                std::fprintf(stderr, "packed guest failed: %s\n", jit.get_last_error().c_str());
                CHECK(false);
            }
            CHECK(jit.get_reg(4) == reference(cases[0], r12, r4));
            CHECK(jit.get_reg(12) == r12); // sources other than Rd preserved
            CHECK(jit.get_pc() == code + 6);
        }
    }
    std::printf("Packed saturation: Thumb UQADD8 guest execution passed (single+region)\n");
}

// Saturating clamp; q records whether it clamped.
int64_t clamp_q(int64_t x, int64_t lo, int64_t hi, bool &q) {
    if (x < lo) { q = true; return lo; }
    if (x > hi) { q = true; return hi; }
    return x;
}
uint32_t ssat(uint32_t x, unsigned n, bool &q) {
    return uint32_t(clamp_q(int32_t(x), -(int64_t(1) << (n - 1)), (int64_t(1) << (n - 1)) - 1, q));
}
uint32_t usat(uint32_t x, unsigned n, bool &q) {
    return uint32_t(clamp_q(int32_t(x), 0, (int64_t(1) << n) - 1, q));
}
uint32_t qadd(uint32_t a, uint32_t b, bool subtract, bool &q) {
    return uint32_t(clamp_q(subtract ? int64_t(int32_t(a)) - int32_t(b) : int64_t(int32_t(a)) + int32_t(b), INT32_MIN, INT32_MAX, q));
}
uint32_t expand_ge(uint32_t cpsr) {
    uint32_t mask = 0;
    for (unsigned i = 0; i < 4; ++i)
        if ((cpsr >> (16 + i)) & 1) mask |= 0xffu << (8 * i);
    return mask;
}

void ir_q_saturation() {
    Harness h;
    constexpr uint32_t q_flag = 1u << 27;
    const std::array<uint32_t, 6> edges{0x00007fffu, 0x00008000u, 0xffff8000u, 0xffff7fffu, 0x7fffffffu, 0x80000000u};
    for (bool is_signed : {true, false}) {
        const Op op = is_signed ? Op::SignedSaturation : Op::UnsignedSaturation;
        for (uint8_t n : is_signed ? std::array<uint8_t, 5>{1, 8, 16, 31, 32} : std::array<uint8_t, 5>{0, 1, 8, 16, 31}) {
            const auto reference = [&](JitState &s, uint32_t x) {
                bool q = false;
                s.regs[0] = is_signed ? (n == 32 ? x : ssat(x, n, q)) : usat(x, n, q);
                if (q) s.cpsr |= q_flag;
            };
            const auto build = [&](auto &b, Value x) {
                const auto result = append(b, op, {x, Value{n}});
                append(b, Op::A32SetRegister, {Value{A32::Reg::R0}, result});
                append(b, Op::A32OrQFlag, {append(b, Op::GetOverflowFromOp, {result})});
            };
            h.test(op, [&](auto &b) { build(b, append(b, Op::A32GetRegister, {Value{A32::Reg::R1}})); },
                [&](auto &s) { reference(s, s.regs[1]); });
            // Constant edges from a clear Q (harness pattern 0) prove the set.
            for (const uint32_t x : edges)
                h.test(op, [&](auto &b) { build(b, Value{x}); }, [&](auto &s) { reference(s, x); });
        }
    }
    for (bool subtract : {false, true}) {
        const Op op = subtract ? Op::SignedSaturatedSubWithFlag32 : Op::SignedSaturatedAddWithFlag32;
        const auto build = [&](auto &b, Value x, Value y) {
            const auto result = append(b, op, {x, y});
            append(b, Op::A32SetRegister, {Value{A32::Reg::R0}, result});
            append(b, Op::A32OrQFlag, {append(b, Op::GetOverflowFromOp, {result})});
        };
        const auto reference = [&](JitState &s, uint32_t x, uint32_t y) {
            bool q = false;
            s.regs[0] = qadd(x, y, subtract, q);
            if (q) s.cpsr |= q_flag;
        };
        h.test(op, [&](auto &b) {
            build(b, append(b, Op::A32GetRegister, {Value{A32::Reg::R1}}), append(b, Op::A32GetRegister, {Value{A32::Reg::R2}}));
        }, [&](auto &s) { reference(s, s.regs[1], s.regs[2]); });
        for (const auto &[x, y] : std::array<std::pair<uint32_t, uint32_t>, 4>{{{0x7fffffffu, 1u}, {0x80000000u, 1u},
                 {0x80000000u, 0x80000000u}, {5u, 0xfffffffdu}}})
            h.test(op, [&](auto &b) { build(b, Value{x}, Value{y}); }, [&](auto &s) { reference(s, x, y); });
    }
    // SEL: GE byte masks from CPSR.GE (0 or 0xf in the harness patterns,
    // then each single GE value set in-block).
    h.test(Op::PackedSelect, [&](auto &b) {
        const auto ge = append(b, Op::A32GetGEFlags);
        append(b, Op::A32SetRegister, {Value{A32::Reg::R3}, ge});
        append(b, Op::A32SetRegister, {Value{A32::Reg::R0}, append(b, Op::PackedSelect,
            {ge, append(b, Op::A32GetRegister, {Value{A32::Reg::R1}}), append(b, Op::A32GetRegister, {Value{A32::Reg::R2}})})});
    }, [&](auto &s) {
        const uint32_t ge = expand_ge(s.cpsr);
        s.regs[3] = ge;
        s.regs[0] = (s.regs[2] & ge) | (s.regs[1] & ~ge);
    });
    for (uint32_t bits = 0; bits < 16; ++bits) {
        h.test(Op::A32GetGEFlags, [&](auto &b) {
            uint32_t mask = 0;
            for (unsigned i = 0; i < 4; ++i) if ((bits >> i) & 1) mask |= 0xffu << (8 * i);
            append(b, Op::A32SetGEFlags, {Value{mask}});
            append(b, Op::A32SetRegister, {Value{A32::Reg::R0}, append(b, Op::A32GetGEFlags)});
        }, [&](auto &s) {
            s.cpsr = (s.cpsr & ~0x000f0000u) | (bits << 16);
            s.regs[0] = expand_ge(s.cpsr);
        });
    }
    CHECK(h.missing.empty());
    std::printf("Q saturation/SEL: %u independent IR fixtures passed\n", h.cases);
}

// SSAT/USAT[16], QADD/QSUB/QDADD/QDSUB and SEL through the ARM/Thumb
// translator, from a clear Q (set by any saturation) and a set one.
void guest_q_saturation(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    struct Case { uint32_t arm, thumb; uint32_t (*reference)(uint32_t r0, uint32_t r1, uint32_t r3, uint32_t cpsr, bool &q); unsigned rd; };
    const Case cases[] = {
        {0xe6af4010u, 0xf300040fu, [](uint32_t r0, uint32_t, uint32_t, uint32_t, bool &q) { return ssat(r0, 16, q); }, 4},
        {0xe6af9210u, 0xf300190fu, [](uint32_t r0, uint32_t, uint32_t, uint32_t, bool &q) { return ssat(r0 << 4, 16, q); }, 9},
        {0xe6e87013u, 0xf3830708u, [](uint32_t, uint32_t, uint32_t r3, uint32_t, bool &q) { return usat(r3, 8, q); }, 7},
        {0xe6a74f30u, 0xf3200407u, [](uint32_t r0, uint32_t, uint32_t, uint32_t, bool &q) {
            return (ssat(uint32_t(int32_t(int16_t(r0))), 8, q) & 0xffffu) | ssat(uint32_t(int32_t(r0) >> 16), 8, q) << 16; }, 4},
        {0xe6e84f30u, 0xf3a00408u, [](uint32_t r0, uint32_t, uint32_t, uint32_t, bool &q) {
            return usat(uint32_t(int32_t(int16_t(r0))), 8, q) | usat(uint32_t(int32_t(r0) >> 16), 8, q) << 16; }, 4},
        {0xe1012050u, 0xfa81f280u, [](uint32_t r0, uint32_t r1, uint32_t, uint32_t, bool &q) { return qadd(r0, r1, false, q); }, 2},
        {0xe1212050u, 0xfa81f2a0u, [](uint32_t r0, uint32_t r1, uint32_t, uint32_t, bool &q) { return qadd(r0, r1, true, q); }, 2},
        {0xe1412050u, 0xfa81f290u, [](uint32_t r0, uint32_t r1, uint32_t, uint32_t, bool &q) { return qadd(r0, qadd(r1, r1, false, q), false, q); }, 2},
        {0xe1612050u, 0xfa81f2b0u, [](uint32_t r0, uint32_t r1, uint32_t, uint32_t, bool &q) { return qadd(r0, qadd(r1, r1, false, q), true, q); }, 2},
        {0xe6802fb1u, 0xfaa0f281u, [](uint32_t r0, uint32_t r1, uint32_t, uint32_t cpsr, bool &) {
            const uint32_t ge = expand_ge(cpsr);
            return (r0 & ge) | (r1 & ~ge); }, 2},
    };
    const std::array<std::array<uint32_t, 3>, 6> inputs{{{0x00001234u, 0x00000001u, 1000u}, {0x00020000u, 0x7fffffffu, 0xfffe0000u},
        {0xfffe0000u, 0x80000000u, 0x7fu}, {0x7fff8000u, 0x40000000u, 0x100u}, {0x80000000u, 0xc0000000u, 0xffffffffu},
        {0x0081ff7fu, 0x00000000u, 0xffu}}};
    unsigned count = 0;
    for (bool region : {false, true}) for (bool thumb : {false, true}) for (const auto &c : cases)
        for (const auto &in : inputs) for (uint32_t entry : {0x00050010u, 0x080a0010u}) {
            jit.set_region_mode(region);
            // Halfword swap only: vector_tests::thumb_word also rewrites the
            // 0xf2/0xf3 NEON prefix, which SSAT/USAT share.
            if (thumb) put_thumb(mem, jit, {(c.thumb << 16) | (c.thumb >> 16), 0xbf00df00});
            else put(mem, jit, {c.arm, 0xef000000});
            auto context = jit.save_context();
            for (unsigned r = 0; r < 13; ++r) context.cpu_registers[r] = 0x5a5a0000u + r;
            context.cpu_registers[0] = in[0];
            context.cpu_registers[1] = in[1];
            context.cpu_registers[3] = in[2];
            context.cpsr = entry | (thumb ? 0x20 : 0); // GE 0101 / 1010, Q clear / set
            jit.load_context(context);
            if (jit.run() != 0) {
                std::fprintf(stderr, "Q saturation guest failed: %08x: %s\n", c.arm, jit.get_last_error().c_str());
                CHECK(false);
            }
            bool q = false;
            auto expected = context.cpu_registers;
            expected[c.rd] = c.reference(in[0], in[1], in[2], context.cpsr, q);
            const auto result = jit.save_context();
            for (unsigned r = 0; r < 13; ++r) {
                if (result.cpu_registers[r] != expected[r])
                    std::fprintf(stderr, "Q saturation %08x (%s) r%u=%08x want %08x (in %08x %08x %08x)\n", c.arm,
                        thumb ? "Thumb" : "ARM", r, result.cpu_registers[r], expected[r], in[0], in[1], in[2]);
                CHECK(result.cpu_registers[r] == expected[r]);
            }
            CHECK(result.cpsr == (context.cpsr | (q ? 1u << 27 : 0)));
            CHECK(jit.get_pc() == code + (thumb ? 6 : 8));
            ++count;
        }
    std::printf("Q saturation/SEL: %u ARM/Thumb guest cases passed (single+region)\n", count);
}
} // namespace packed_saturate_tests
