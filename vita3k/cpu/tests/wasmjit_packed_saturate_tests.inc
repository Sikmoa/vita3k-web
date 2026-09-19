// Scalar packed saturating arithmetic (ARM QADD8/QSUB8/QADD16/QSUB16 and
// unsigned UQADD/UQSUB family). Dynarmic lowers each to one U32->U32 IR op
// with no Q-flag update; the emitter lowers it lane by lane in plain i32.
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
} // namespace packed_saturate_tests
