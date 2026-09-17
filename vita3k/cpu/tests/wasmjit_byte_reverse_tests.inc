// Source-only fixtures; see browser/tests/JIT_BYTE_REVERSE.md.
namespace byte_reverse_tests {
using namespace vector_tests;

uint32_t reverse_word(uint32_t value) {
    return (value >> 24) | ((value >> 8) & 0x0000ff00u)
        | ((value << 8) & 0x00ff0000u) | (value << 24);
}
uint16_t reverse_half(uint16_t value) {
    return uint16_t((value >> 8) | (value << 8));
}
uint64_t reverse_dual(uint64_t value) {
    return (uint64_t(reverse_word(uint32_t(value))) << 32)
        | reverse_word(uint32_t(value >> 32));
}

void ir_reversal() {
    Harness h;
    h.test(Op::ByteReverseWord, [&](auto &b) {
        const auto input = append(b, Op::A32GetRegister, {Value{A32::Reg::R1}});
        append(b, Op::A32SetRegister, {Value{A32::Reg::R0}, append(b, Op::ByteReverseWord, {input})});
    }, [&](auto &s) { s.regs[0] = reverse_word(s.regs[1]); });
    h.test(Op::ByteReverseHalf, [&](auto &b) {
        const auto input = append(b, Op::LeastSignificantHalf,
            {append(b, Op::A32GetRegister, {Value{A32::Reg::R1}})});
        append(b, Op::A32SetRegister, {Value{A32::Reg::R0}, append(b, Op::ByteReverseHalf, {input})});
    }, [&](auto &s) { s.regs[0] = reverse_half(uint16_t(s.regs[1])); });
    h.test(Op::ByteReverseDual, [&](auto &b) {
        const auto input = append(b, Op::A32GetExtendedRegister64, {Value{A32::ExtReg::D2}});
        append(b, Op::A32SetExtendedRegister64,
            {Value{A32::ExtReg::D0}, append(b, Op::ByteReverseDual, {input})});
    }, [&](auto &s) { set_lane(s, 64, 0, reverse_dual(lane(s, 64, 0, 1))); });

    // Immediate paths cover the byte and word boundaries independently of
    // the harness's register patterns and verify U16/U64 result publication.
    h.test(Op::ByteReverseWord, [&](auto &b) {
        append(b, Op::A32SetRegister, {Value{A32::Reg::R0},
            append(b, Op::ByteReverseWord, {Value{uint32_t(0x01234567)}})});
    }, [&](auto &s) { s.regs[0] = 0x67452301; });
    h.test(Op::ByteReverseHalf, [&](auto &b) {
        append(b, Op::A32SetRegister, {Value{A32::Reg::R0},
            append(b, Op::ByteReverseHalf, {Value{uint16_t(0xabcd)}})});
    }, [&](auto &s) { s.regs[0] = 0xcdab; });
    h.test(Op::ByteReverseDual, [&](auto &b) {
        append(b, Op::A32SetExtendedRegister64, {Value{A32::ExtReg::D0},
            append(b, Op::ByteReverseDual, {Value{uint64_t(0x0123456789abcdefull)}})});
    }, [&](auto &s) { set_lane(s, 64, 0, 0xefcdab8967452301ull); });
    CHECK(h.missing.empty());
    std::printf("Byte reversal: %u independent IR fixtures passed\n", h.cases);
}

void guest_reversal(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    for (bool region : {false, true}) {
        jit.set_region_mode(region);
        for (unsigned d : {0u, 12u}) for (unsigned m : {1u, 14u}) {
            const uint32_t rev = 0xe6bf0f30 | (d << 12) | m;
            const uint32_t rev16 = 0xe6bf0fb0 | (d << 12) | m;
            const uint32_t revsh = 0xe6ff0fb0 | (d << 12) | m;
            for (const auto &[opcode, kind] : std::array<std::pair<uint32_t, unsigned>, 3>{
                     {{rev, 0}, {rev16, 1}, {revsh, 2}}}) {
                guest_case(mem, jit, parent, false, opcode, [&](auto &s) {
                    const uint32_t source = s.regs[m];
                    s.regs[d] = kind == 0 ? reverse_word(source)
                        : kind == 1 ? ((source >> 8) & 0x00ff00ffu) | ((source << 8) & 0xff00ff00u)
                        : uint32_t(int32_t(int16_t(reverse_half(uint16_t(source)))));
                });
            }
        }
    }
}
} // namespace byte_reverse_tests
