// Scalar/vector absolute value (VABS.f16/f32/f64). The emitter clears each
// lane's sign bit; no FPSCR guard or flag accumulation (exact, raises
// nothing, SNaN payload preserved). Retail EventManager died on VABS.f32 d3.
namespace fpvector_abs_tests {
using namespace vector_tests;

struct Case {
    Op opcode;
    unsigned bits;
};

uint32_t abs_word(Case c, unsigned word, uint32_t bits) {
    if (c.bits == 16) return bits & 0x7fff7fffu;
    if (c.bits == 32) return bits & 0x7fffffffu;
    return (word & 1) ? bits & 0x7fffffffu : bits; // f64 sign lives in odd words
}

void ir_abs() {
    Harness h;
    for (const auto c : {Case{Op::FPVectorAbs16, 16}, Case{Op::FPVectorAbs32, 32},
            Case{Op::FPVectorAbs64, 64}}) {
        // Harness register patterns cover zero/all-ones/xorshift fpu words.
        h.test(c.opcode, [&](auto &b) {
            write_q(b, append(b, c.opcode, {read_q(b)}));
        }, [&](auto &s) {
            for (unsigned w = 0; w < 4; ++w) s.fpu[w] = abs_word(c, w, s.fpu[w]);
        });
        // Boundary immediates: +/-0, min-denormal, max, +/-inf, sNaN, qNaN.
        const uint64_t lo = 0x8000000180000000ull, hi = 0x7fa000007fc00001ull;
        const uint64_t den = 0x8000000100000001ull, inf = 0xff8000007f800000ull;
        h.test(c.opcode, [&](auto &b) {
            const auto v = append(b, Op::Pack2x64To1x128, {Value{lo}, Value{hi}});
            const auto d = append(b, Op::Pack2x64To1x128, {Value{den}, Value{inf}});
            write_q(b, append(b, c.opcode, {v}));
            write_q(b, append(b, c.opcode, {d}), 1);
        }, [&](auto &s) {
            uint64_t words[2] = {lo, hi};
            for (unsigned w = 0; w < 4; ++w) {
                uint32_t lane;
                std::memcpy(&lane, reinterpret_cast<const uint8_t *>(words) + w * 4, 4);
                s.fpu[w] = abs_word(c, w, lane);
            }
            uint64_t words1[2] = {den, inf};
            for (unsigned w = 0; w < 4; ++w) {
                uint32_t lane;
                std::memcpy(&lane, reinterpret_cast<const uint8_t *>(words1) + w * 4, 4);
                s.fpu[w + 4] = abs_word(c, w, lane);
            }
        });
    }
    CHECK(h.missing.empty());
    std::printf("FP vector abs: %u independent IR fixtures passed\n", h.cases);
}

void guest_abs(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    // Exact bytes from the retail EventManager crash: Thumb VABS.f32 d3,d3
    // (bytes LE b9 ff 03 37). D3 is fpu words 6-7; the trailing svc ends the run.
    for (bool region : {false, true}) {
        jit.set_region_mode(region);
        for (const auto &[w6, w7] : std::array<std::pair<uint32_t, uint32_t>, 5>{{{0x00000000u, 0x00000000u},
                {0x80000000u, 0x80000000u}, {0xff800000u, 0x7fa00001u},
                {0x80000001u, 0x7fc00000u}, {0xbfc00000u, 0xc79fffffu}}}) {
            put_thumb(mem, jit, {thumb_word(0xffb93703), 0xbf00df00});
            auto s = initial(2);
            auto context = jit.save_context();
            std::copy_n(s.regs, 15, context.cpu_registers.begin());
            std::memcpy(context.fpu_registers.data(), s.fpu, sizeof(s.fpu));
            context.cpsr = s.cpsr | 0x20;
            context.fpscr = s.fpscr;
            std::memcpy(&context.fpu_registers[6], &w6, 4);
            std::memcpy(&context.fpu_registers[7], &w7, 4);
            jit.load_context(context);
            if (jit.run() != 0) {
                std::fprintf(stderr, "fp abs guest failed: %s\n", jit.get_last_error().c_str());
                CHECK(false);
            }
            const auto result = jit.save_context();
            uint32_t r6, r7;
            std::memcpy(&r6, &result.fpu_registers[6], 4);
            std::memcpy(&r7, &result.fpu_registers[7], 4);
            CHECK(r6 == (w6 & 0x7fffffffu) && r7 == (w7 & 0x7fffffffu));
            CHECK(result.cpsr == context.cpsr && result.fpscr == context.fpscr);
            CHECK(jit.get_pc() == code + 6);
        }
    }
    std::printf("FP vector abs: Thumb VABS.f32 guest execution passed (single+region)\n");
}
} // namespace fpvector_abs_tests
