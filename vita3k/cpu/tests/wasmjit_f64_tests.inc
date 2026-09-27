// Included by wasmjit_backend_test.cpp after its shared CHECK/put helpers.
// Independent scalar-binary64 IR fixtures. The block's LocationDescriptor
// carries the FPSCR fixed at emission time (FZ/DN are part of the location
// key), so each FZ/DN variant builds its own block and the runtime FPSCR must
// match it. No guest code and no assembler are needed: fixtures build IR and
// check the exact architectural state afterwards.
namespace f64_tests {
namespace A32 = Dynarmic::A32;
namespace IR = Dynarmic::IR;
using Op = IR::Opcode;
using Value = IR::Value;
using Reason = vita3k::wasmjit::ExitReason;

constexpr uint32_t fpscr_fz = 0x01000000;
constexpr uint32_t fpscr_dn = 0x02000000;

IR::Block blank(uint32_t fpscr) {
    const A32::LocationDescriptor here{code, A32::PSR{0x10}, A32::FPSCR{fpscr}};
    const A32::LocationDescriptor next{code + 4, A32::PSR{0x10}, A32::FPSCR{fpscr}};
    IR::Block block{here};
    block.SetEndLocation(next);
    block.SetTerminal(IR::Term::LinkBlock{next});
    block.CycleCount() = 1;
    return block;
}
Value append(IR::Block &block, Op op, std::initializer_list<Value> args = {}) {
    block.AppendNewInst(op, args);
    return Value{&block.back()};
}
Value read_d(IR::Block &block, unsigned d = 0) {
    return append(block, Op::A32GetExtendedRegister64, {Value{A32::ExtReg::D0 + d}});
}
void write_d(IR::Block &block, Value value, unsigned d = 0) {
    append(block, Op::A32SetExtendedRegister64, {Value{A32::ExtReg::D0 + d}, value});
}
Value read_s(IR::Block &block, unsigned s = 0) {
    return append(block, Op::A32GetExtendedRegister32, {Value{A32::ExtReg::S0 + s}});
}
void write_s(IR::Block &block, Value value, unsigned s = 0) {
    append(block, Op::A32SetExtendedRegister32, {Value{A32::ExtReg::S0 + s}, value});
}

uint64_t double_bits(double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
float float_from_bits(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
uint64_t d_bits(const JitState &state, unsigned d = 0) {
    return uint64_t(state.fpu[d * 2]) | (uint64_t(state.fpu[d * 2 + 1]) << 32);
}
void set_d_bits(JitState &state, uint64_t bits, unsigned d = 0) {
    state.fpu[d * 2] = uint32_t(bits);
    state.fpu[d * 2 + 1] = uint32_t(bits >> 32);
}
uint32_t s_bits(const JitState &state, unsigned s = 0) { return state.fpu[s]; }
void set_s_bits(JitState &state, uint32_t bits, unsigned s = 0) { state.fpu[s] = bits; }

JitState base_state(uint32_t fpscr) {
    JitState state{};
    for (auto &word : state.fpu) word = 0x5a5a5a5au;
    for (unsigned r = 0; r < 16; ++r) state.regs[r] = 0xabc00000 + r;
    state.regs[15] = code;
    state.fpscr = fpscr;
    state.cpsr = 0x10;
    return state;
}
// Run one fixture; when expect_state is given, every architectural word is
// compared, so a stray write into a neighbouring register fails the test.
void run(const IR::Block &block, const JitState &initial, const JitState *expected) {
    const A32::LocationDescriptor at{block.Location()};
    for (bool region : {false, true}) {
        const auto bytes = region
            ? vita3k::wasmjit::emit_region({&block}, {{at.PC(), PSR_DISPATCH_MASK,
                at.CPSR().Value() & PSR_DISPATCH_MASK, 1}})
            : vita3k::wasmjit::emit_block(block);
        CHECK(!bytes.empty());
        const int slot = region
            ? vita3k_jit_install_region(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write)
            : vita3k_jit_install(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write);
        CHECK(slot >= 0);
        auto state = initial;
        const auto reason = static_cast<Reason>(region
            ? vita3k_jit_run(slot, reinterpret_cast<uintptr_t>(&state), 1)
            : vita3k_jit_call(slot, reinterpret_cast<uintptr_t>(&state)));
        CHECK(reason == (region ? Reason::Miss : Reason::Continue));
        if (expected) {
            for (unsigned i = 0; i < 64; ++i) {
                if (state.fpu[i] != expected->fpu[i])
                    std::fprintf(stderr, "f64 region=%d word=%u got=%08x expected=%08x input=%08x:%08x fpscr=%08x\n",
                        region, i, state.fpu[i], expected->fpu[i], initial.fpu[1], initial.fpu[0], initial.fpscr);
                CHECK(state.fpu[i] == expected->fpu[i]);
            }
            for (unsigned i = 0; i < 15; ++i) CHECK(state.regs[i] == expected->regs[i]);
            if (state.fpscr != expected->fpscr)
                std::fprintf(stderr, "f64 region=%d fpscr=%08x expected=%08x input=%08x:%08x\n", region, state.fpscr,
                    expected->fpscr, initial.fpu[1], initial.fpu[0]);
            CHECK(state.fpscr == expected->fpscr);
        }
        if (region) vita3k_jit_release_region(slot);
        else vita3k_jit_release(slot);
    }
}

// Integer U64 helpers used by NEON structure loads. Use asymmetric words so
// an incorrect carry direction cannot pass as it would with a zero high word.
void integer_widen_shift() {
    for (uint64_t input : {0ull, 1ull, 0x89abcdef01234567ull, 0xffffffffffffffffull}) {
        for (uint8_t shift : {0, 1, 8, 16, 24, 31, 32, 33, 40, 56, 63, 64, 65, 127, 255}) {
            for (bool literal : {false, true}) for (bool dynamic : {false, true}) {
                auto block = blank(0);
                const auto operand = literal ? Value{input} : read_d(block);
                const auto amount = dynamic
                    ? append(block, Op::LeastSignificantByte, {read_s(block, 4)}) : Value{shift};
                write_d(block, append(block, Op::LogicalShiftLeft64, {operand, amount}));
                auto state = base_state(0x90000090);
                set_d_bits(state, input);
                set_s_bits(state, 0xa5a50000u | shift, 4);
                auto expected = state;
                set_d_bits(expected, shift < 64 ? input << shift : 0);
                run(block, state, &expected);
            }
        }
    }
    for (uint8_t byte : {0, 1, 127, 128, 255}) {
        auto block = blank(0);
        auto narrow = append(block, Op::LeastSignificantByte, {read_s(block)});
        write_d(block, append(block, Op::ZeroExtendByteToLong, {narrow}));
        auto state = base_state(0x90000090);
        set_s_bits(state, 0xdead0000u | byte);
        auto expected = state;
        set_d_bits(expected, byte);
        run(block, state, &expected);
    }
}

void binary_arithmetic() {
    struct Case { Op op; uint64_t a, b, result; uint32_t flags; };
    const Case cases[] = {
        {Op::FPMul64, 0x3ff0000000000001ull, 0x3ff0000000000001ull, 0x3ff0000000000002ull, 0x10},
        {Op::FPMul64, 0x7fefffffffffffffull, 0x4000000000000000ull, 0x7ff0000000000000ull, 0x14},
        {Op::FPMul64, 0x0010000000000001ull, 0x3fe0000000000000ull, 0x0008000000000000ull, 0x18},
        {Op::FPMul64, 0x0010000000000000ull, 0x3fe0000000000000ull, 0x0008000000000000ull, 0},
        {Op::FPMul64, 0x0010000000000000ull, 0x3fefffffffffffffull, 0x0010000000000000ull, 0x18},
        {Op::FPMul64, 0x7ff0000000000000ull, 0, 0x7ff8000000000000ull, 1},
        {Op::FPMul64, 0x8000000000000000ull, 0x4000000000000000ull, 0x8000000000000000ull, 0},
        {Op::FPAdd64, 0x3ff0000000000000ull, 0x3ca0000000000000ull, 0x3ff0000000000000ull, 0x10},
        {Op::FPAdd64, 0x3ff0000000000000ull, 1, 0x3ff0000000000000ull, 0x10},
        {Op::FPAdd64, 0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull, 1},
        {Op::FPAdd64, 0x8000000000000000ull, 0x8000000000000000ull, 0x8000000000000000ull, 0},
        {Op::FPSub64, 0x3ff0000000000000ull, 0x3ff0000000000000ull, 0, 0},
        {Op::FPSub64, 0x0010000000000001ull, 0x0010000000000000ull, 1, 0},
        {Op::FPSub64, 0x3ff0000000000000ull, 0xfff8123456789abcull, 0xfff8123456789abcull, 0},
        {Op::FPSub64, 0x7ff8123456789abcull, 0xfff0123456789abcull, 0xfff8123456789abcull, 1},
        {Op::FPDiv64, 0x3ff0000000000000ull, 0x4008000000000000ull, 0x3fd5555555555555ull, 0x10},
        {Op::FPDiv64, 0x3ff0000000000000ull, 0, 0x7ff0000000000000ull, 2},
        {Op::FPDiv64, 0, 0, 0x7ff8000000000000ull, 1},
        {Op::FPDiv64, 0x7ff0000000000000ull, 0x7ff0000000000000ull, 0x7ff8000000000000ull, 1},
        {Op::FPDiv64, 1, 0x4000000000000000ull, 0, 0x18},
    };
    for (const auto &c : cases) {
        auto block = blank(0);
        write_d(block, append(block, c.op, {read_d(block), read_d(block, 1)}));
        auto state = base_state(0x80000080u); // preserve NZCV and sticky IDC
        set_d_bits(state, c.a); set_d_bits(state, c.b, 1);
        auto expected = state;
        set_d_bits(expected, c.result); expected.fpscr |= c.flags;
        run(block, state, &expected);
    }
    for (Op op : {Op::FPAdd64, Op::FPSub64, Op::FPMul64, Op::FPDiv64}) {
        auto block = blank(0);
        write_d(block, append(block, op, {read_d(block), read_d(block, 1)}));
        auto state = base_state(0);
        set_d_bits(state, 0x4018000000000000ull); // 6
        set_d_bits(state, 0x4000000000000000ull, 1); // 2
        auto expected = state;
        set_d_bits(expected, op == Op::FPAdd64 ? 0x4020000000000000ull :
            op == Op::FPSub64 ? 0x4010000000000000ull :
            op == Op::FPMul64 ? 0x4028000000000000ull : 0x4008000000000000ull);
        run(block, state, &expected);
    }
}

// Exact opcode at Limbo's current frontier: VMUL.F64 d1,d1,d2.
void guest_multiply(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    for (bool regions : {false, true}) for (bool thumb : {false, true}) {
        jit.set_region_mode(regions);
        if (thumb) put_thumb(mem, jit, {0x1b02ee21u, 0xbf00df00u});
        else put(mem, jit, {0xee211b02u, 0xef000000u});
        auto before = jit.save_context();
        const uint64_t a = 0x3ff0000000000001ull, b = a;
        std::memcpy(&before.fpu_registers[2], &a, 8);
        std::memcpy(&before.fpu_registers[4], &b, 8);
        before.fpscr = 0x90000080u;
        jit.load_context(before);
        CHECK(jit.run() == 0);
        const auto after = jit.save_context();
        uint64_t result;
        std::memcpy(&result, &after.fpu_registers[2], 8);
        CHECK(result == 0x3ff0000000000002ull);
        CHECK(after.fpscr == (before.fpscr | 0x10));
        CHECK(after.cpsr == before.cpsr);
        CHECK(parent.svc_called);
    }
}

// MCR p15,0,Rt,c13,c0,3 stores the guest TLS base; the matching MRC read is
// already supported and must observe the written value in later blocks.
void guest_tls_write(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    for (bool regions : {false, true}) {
        jit.set_region_mode(regions);
        // MCR p15,0,r2,c13,c0,3 (Thumb 0x0d 0xee 0x70 0x2f) then read it back
        // through the MRC form so the stored value is architecturally visible.
        put_thumb(mem, jit, {0x2f70ee0du, 0x3f70ee1du, 0xbf00df00u});
        auto before = jit.save_context();
        before.cpu_registers[2] = 0x12345678;
        before.cpu_registers[3] = 0x0f0f0f0f;
        jit.load_context(before);
        CHECK(jit.run() == 0);
        const auto after = jit.save_context();
        CHECK(after.cpu_registers[3] == 0x12345678);
        CHECK(after.cpu_registers[2] == 0x12345678);
        CHECK(after.cpsr == before.cpsr);
        CHECK(parent.svc_called);
    }
}

// Integer -> binary64 conversions are exact for fbits == 0, so they must not
// disturb FPSCR and must not read the source after writing the destination.
void integer_to_double() {
    // Fixed-bit sanity check, independent of host numeric conversions.
    {
        auto block = blank(0);
        write_d(block, append(block, Op::FPFixedU32ToDouble,
            {append(block, Op::A32GetRegister, {Value{A32::Reg::R1}}), Value{uint8_t(0)}, Value{uint8_t(0)}}));
        auto state = base_state(0);
        state.regs[1] = 1;
        auto expected = state;
        expected.fpu[0] = 0x00000000;
        expected.fpu[1] = 0x3ff00000;
        run(block, state, &expected);
        std::fprintf(stderr, "u32(1) -> f64: low=00000000 high=3ff00000 passed (block and region)\n");
    }
    for (uint32_t input : {0u, 1u, 2u, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0x12345678u}) {
        auto block = blank(0);
        const auto value = append(block, Op::FPFixedU32ToDouble,
            {append(block, Op::A32GetRegister, {Value{A32::Reg::R1}}), Value{uint8_t(0)}, Value{uint8_t(0)}});
        write_d(block, value);
        auto state = base_state(0), expected = state;
        state.regs[1] = input;
        expected.regs[1] = input;
        set_d_bits(expected, double_bits(double(input)));
        run(block, state, &expected);
    }
    for (int32_t input : {0, 1, -1, INT32_MAX, INT32_MIN, -12345, 0x40000000}) {
        auto block = blank(0);
        const auto value = append(block, Op::FPFixedS32ToDouble,
            {append(block, Op::A32GetRegister, {Value{A32::Reg::R1}}), Value{uint8_t(0)}, Value{uint8_t(0)}});
        write_d(block, value);
        auto state = base_state(0), expected = state;
        state.regs[1] = uint32_t(input);
        expected.regs[1] = uint32_t(input);
        set_d_bits(expected, double_bits(double(input)));
        run(block, state, &expected);
    }
    // Scaled fixed-point forms are deliberately unimplemented; the route must
    // be rejected instead of silently converting without the scale.
    auto scaled = blank(0);
    write_d(scaled, append(scaled, Op::FPFixedU32ToDouble,
        {append(scaled, Op::A32GetRegister, {Value{A32::Reg::R1}}), Value{uint8_t(8)}, Value{uint8_t(0)}}));
    CHECK(vita3k::wasmjit::emit_block(scaled).empty());
}

void negate_and_absolute() {
    for (uint64_t input : {0ull, 0x8000000000000000ull, 0x3ff0000000000000ull,
             0xbff0000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
             0x7ff8123456789abcull, 0x0000000000000001ull}) {
        for (bool negate : {false, true}) {
            auto block = blank(0);
            write_d(block, append(block, negate ? Op::FPNeg64 : Op::FPAbs64, {read_d(block)}));
            auto state = base_state(0), expected = state;
            set_d_bits(state, input);
            set_d_bits(expected, negate ? (input ^ 0x8000000000000000ull)
                                        : (input & 0x7fffffffffffffffull));
            run(block, state, &expected);
        }
    }
}

void single_to_double() {
    for (uint32_t input : {0u, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x7f7fffffu,
             0x00000001u, 0x007fffffu, 0x7f800000u, 0xff800000u}) {
        auto block = blank(0);
        write_d(block, append(block, Op::FPSingleToDouble, {read_s(block), Value{uint8_t(0)}}));
        auto state = base_state(0), expected = state;
        set_s_bits(state, input);
        set_d_bits(expected, double_bits(double(float_from_bits(input))));
        run(block, state, &expected);
    }
    // Quiet a signaling NaN deterministically: sign kept, payload widened, quiet
    // bit set, IOC raised.
    for (uint32_t input : {0x7fc00000u, 0xffc00000u, 0x7f800001u, 0xffa00000u}) {
        auto block = blank(0);
        write_d(block, append(block, Op::FPSingleToDouble, {read_s(block), Value{uint8_t(0)}}));
        auto state = base_state(0), expected = state;
        set_s_bits(state, input);
        const uint64_t sign = uint64_t(input & 0x80000000u) << 32;
        set_d_bits(expected, sign | 0x7ff8000000000000ull
            | (uint64_t(input & 0x007fffffu) << 29));
        const bool signaling = (input & 0x00400000u) == 0;
        if (signaling) expected.fpscr = 1;
        run(block, state, &expected);
    }
    // DN canonicalizes every NaN result.
    {
        auto block = blank(fpscr_dn);
        write_d(block, append(block, Op::FPSingleToDouble, {read_s(block), Value{uint8_t(0)}}));
        auto state = base_state(fpscr_dn), expected = state;
        set_s_bits(state, 0xffc00001u);
        set_d_bits(expected, 0x7ff8000000000000ull);
        run(block, state, &expected);
    }
    // FZ flushes a denormal input to a signed zero and raises IDC.
    for (uint32_t input : {0x00000001u, 0x807fffffu}) {
        auto block = blank(fpscr_fz);
        write_d(block, append(block, Op::FPSingleToDouble, {read_s(block), Value{uint8_t(0)}}));
        auto state = base_state(fpscr_fz), expected = state;
        set_s_bits(state, input);
        set_d_bits(expected, uint64_t(input & 0x80000000u) << 32);
        expected.fpscr = fpscr_fz | 0x80;
        run(block, state, &expected);
    }
}

void double_to_single() {
    struct Case { uint64_t input; uint32_t result; uint32_t flags; };
    const Case cases[] = {
        {0x3ff0000000000000ull, 0x3f800000u, 0},                    // 1.0 exact
        {0xbff0000000000000ull, 0xbf800000u, 0},                    // -1.0 exact
        {0x0000000000000000ull, 0x00000000u, 0},                    // +0
        {0x8000000000000000ull, 0x80000000u, 0},                    // -0
        {0x7ff0000000000000ull, 0x7f800000u, 0},                    // +inf
        {0x36a0000000000000ull, 0x00000001u, 0},                    // 2^-149 exact subnormal
        {0x36a0000000000000ull + 1, 0x00000001u, 0x18},             // tiny, inexact
        {0x3ff0000000000001ull, 0x3f800000u, 0x10},                 // rounds to 1.0, inexact
        {0x380fffff00000000ull, 0x007ffffcu, 0},                    // min normal - 4 subnormal ULPs
        {0x380fffffe0000000ull, 0x00800000u, 0x18},                 // tiny tie rounds to even min normal
        {0x3810000000000000ull, 0x00800000u, 0},                    // exact min normal
        {0x3810000000000000ull + 1, 0x00800000u, 0x10},             // least normal, inexact
        {0x47f0000000000000ull, 0x7f800000u, 0x14},                 // overflow: OFC+IXC
        {0xc7f0000000000000ull, 0xff800000u, 0x14},                 // negative overflow
        {0x3690000000000000ull, 0x00000000u, 0x18},                 // tiny, rounds to zero
    };
    for (const auto &c : cases) {
        auto block = blank(0);
        write_s(block, append(block, Op::FPDoubleToSingle, {read_d(block), Value{uint8_t(0)}}));
        auto state = base_state(0), expected = state;
        set_d_bits(state, c.input);
        expected = state;
        set_s_bits(expected, c.result);
        expected.fpscr = c.flags;
        run(block, state, &expected);
    }
    // Without DN, preserve sign and the most significant payload bits, quieting sNaNs.
    for (uint64_t input : {0x7ff8000000000000ull, 0xfff8000000000000ull,
             0x7ff0000000000001ull, 0x7ff4123456789abcull}) {
        auto block = blank(0);
        write_s(block, append(block, Op::FPDoubleToSingle, {read_d(block), Value{uint8_t(0)}}));
        auto state = base_state(0), expected = state;
        set_d_bits(state, input);
        expected = state;
        set_s_bits(expected, uint32_t((input >> 32) & 0x80000000u)
            | 0x7fc00000u | uint32_t((input >> 29) & 0x003fffffu));
        if ((input & 0x0008000000000000ull) == 0) expected.fpscr = 1;
        run(block, state, &expected);
    }
    // DN discards the sign and payload but still reports signaling NaNs.
    for (uint64_t input : {0xfff8123456789abcull, 0xfff0123456789abcull}) {
        auto block = blank(fpscr_dn);
        write_s(block, append(block, Op::FPDoubleToSingle, {read_d(block), Value{uint8_t(0)}}));
        auto state = base_state(fpscr_dn);
        set_d_bits(state, input);
        auto expected = state;
        set_s_bits(expected, 0x7fc00000);
        expected.fpscr |= (input & 0x0008000000000000ull) ? 0 : 1;
        run(block, state, &expected);
    }
    // FZ input flushing raises IDC, not result underflow.
    for (uint64_t input : {1ull, 0x800fffffffffffffull}) {
        auto block = blank(fpscr_fz);
        write_s(block, append(block, Op::FPDoubleToSingle, {read_d(block), Value{uint8_t(0)}}));
        auto state = base_state(fpscr_fz);
        set_d_bits(state, input);
        auto expected = state;
        set_s_bits(expected, uint32_t(input >> 32) & 0x80000000u);
        expected.fpscr |= 0x80;
        run(block, state, &expected);
    }
    // FZ acts before rounding, including a tiny operand that rounds to min normal.
    for (uint64_t input : {0x36a0000000000000ull, 0xb6a0000000000000ull,
             0x380fffffc0000000ull, 0x380fffffe0000000ull, 0x3690000000000000ull}) {
        auto block = blank(fpscr_fz);
        write_s(block, append(block, Op::FPDoubleToSingle, {read_d(block), Value{uint8_t(0)}}));
        auto state = base_state(fpscr_fz), expected = state;
        set_d_bits(state, input);
        expected = state;
        set_s_bits(expected, uint32_t((input >> 32) & 0x80000000u));
        expected.fpscr = fpscr_fz | 0x8;
        run(block, state, &expected);
    }
}

// Float-to-integer conversion (VCVT.S32.F32/F64, fbits == 0). Rounding
// immediates follow Dynarmic::FP::RoundingMode: 0 is nearest-even (VCVTR
// under a default FPSCR), 3 is towards-zero (plain VCVT). Expected values
// below are hand-derived from the exact binary value: saturation with IOC
// for NaN/infinity/overflow, IXC for any dropped fraction, and the
// 2^31 boundary staying exact only for negative signed. Sticky FPSCR bits
// must survive; only the new flags are accumulated.
void float_to_int32() {
    struct Case { Op op; uint64_t input; uint32_t result; uint32_t flags; uint8_t rounding; };
    const Case cases[] = {
        // binary32, towards-zero
        {Op::FPSingleToFixedS32, 0x00000000, 0x00000000u, 0, 3}, // +0 exact
        {Op::FPSingleToFixedS32, 0x80000000, 0x00000000u, 0, 3}, // -0 exact
        {Op::FPSingleToFixedS32, 0x3f800000, 1, 0, 3}, // 1.0
        {Op::FPSingleToFixedS32, 0xbf800000, 0xffffffffu, 0, 3}, // -1.0
        {Op::FPSingleToFixedS32, 0x3fc00000, 1, 0x10, 3}, // 1.5 truncates
        {Op::FPSingleToFixedS32, 0xbfc00000, 0xffffffffu, 0x10, 3}, // -1.5
        {Op::FPSingleToFixedS32, 0x40200000, 2, 0x10, 3}, // 2.5
        {Op::FPSingleToFixedS32, 0xc0200000, 0xfffffffeu, 0x10, 3}, // -2.5
        {Op::FPSingleToFixedS32, 0x3f000000, 0, 0x10, 3}, // 0.5
        {Op::FPSingleToFixedS32, 0xbf000000, 0, 0x10, 3}, // -0.5 -> +0
        {Op::FPSingleToFixedS32, 0x4f000000, 0x7fffffffu, 1, 3}, // 2^31 saturates
        {Op::FPSingleToFixedS32, 0xcf000000, 0x80000000u, 0, 3}, // -2^31 exact
        {Op::FPSingleToFixedS32, 0x4f000001, 0x7fffffffu, 1, 3}, // just above 2^31
        {Op::FPSingleToFixedS32, 0xceffffff, 0x80000080u, 0, 3}, // just above -2^31
        {Op::FPSingleToFixedS32, 0x4b800000, 0x01000000u, 0, 3}, // 2^24
        {Op::FPSingleToFixedS32, 0x4b800001, 0x01000002u, 0, 3}, // 2^24 + 2
        {Op::FPSingleToFixedS32, 0xcb800000, 0xff000000u, 0, 3}, // -2^24
        {Op::FPSingleToFixedS32, 0x33800000, 0, 0x10, 3}, // tiny normal
        {Op::FPSingleToFixedS32, 0x7f800000, 0x7fffffffu, 1, 3}, // +inf
        {Op::FPSingleToFixedS32, 0xff800000, 0x80000000u, 1, 3}, // -inf
        {Op::FPSingleToFixedS32, 0x7fc00000, 0, 1, 3}, // quiet NaN
        {Op::FPSingleToFixedS32, 0x7f800001, 0, 1, 3}, // signaling NaN
        {Op::FPSingleToFixedS32, 0xffc00000, 0, 1, 3}, // negative NaN
        {Op::FPSingleToFixedS32, 0x00000001, 0, 0x10, 3}, // min denormal
        {Op::FPSingleToFixedS32, 0x007fffff, 0, 0x10, 3}, // max denormal
        {Op::FPSingleToFixedS32, 0x00800000, 0, 0x10, 3}, // min normal
        // binary32, nearest-even
        {Op::FPSingleToFixedS32, 0x3f000000, 0, 0x10, 0}, // 0.5 ties to even 0
        {Op::FPSingleToFixedS32, 0x3fc00000, 2, 0x10, 0}, // 1.5 rounds to 2
        {Op::FPSingleToFixedS32, 0x40200000, 2, 0x10, 0}, // 2.5 ties to even 2
        {Op::FPSingleToFixedS32, 0x40200001, 3, 0x10, 0}, // above the tie
        {Op::FPSingleToFixedS32, 0xbf000000, 0, 0x10, 0}, // -0.5 ties to 0
        {Op::FPSingleToFixedS32, 0xbfc00000, 0xfffffffeu, 0x10, 0}, // -1.5 to -2
        {Op::FPSingleToFixedS32, 0xc0200000, 0xfffffffeu, 0x10, 0}, // -2.5 to -2
        {Op::FPSingleToFixedS32, 0x3f800000, 1, 0, 0}, // 1.0 exact
        {Op::FPSingleToFixedS32, 0x3f800001, 1, 0x10, 0}, // 1 + ulp
        {Op::FPSingleToFixedS32, 0x4f000000, 0x7fffffffu, 1, 0}, // overflow
        {Op::FPSingleToFixedS32, 0xcf000000, 0x80000000u, 0, 0}, // -2^31 exact
        {Op::FPSingleToFixedS32, 0x7fc00000, 0, 1, 0}, // NaN
        {Op::FPSingleToFixedS32, 0x7f800000, 0x7fffffffu, 1, 0}, // +inf
        // binary64, towards-zero
        {Op::FPDoubleToFixedS32, 0x3ff0000000000000ull, 1, 0, 3}, // 1.0
        {Op::FPDoubleToFixedS32, 0xbff8000000000000ull, 0xffffffffu, 0x10, 3}, // -1.5
        {Op::FPDoubleToFixedS32, 0xc004000000000000ull, 0xfffffffeu, 0x10, 3}, // -2.5
        {Op::FPDoubleToFixedS32, 0x41e0000000000000ull, 0x7fffffffu, 1, 3}, // 2^31
        {Op::FPDoubleToFixedS32, 0xc1e0000000000000ull, 0x80000000u, 0, 3}, // -2^31 exact
        {Op::FPDoubleToFixedS32, 0x41d0000000000000ull, 0x40000000u, 0, 3}, // 2^30
        {Op::FPDoubleToFixedS32, 0x3ff0000000000001ull, 1, 0x10, 3}, // 1 + ulp
        {Op::FPDoubleToFixedS32, 0x4330000000000000ull, 0x7fffffffu, 1, 3}, // 2^52
        {Op::FPDoubleToFixedS32, 0x7ff0000000000000ull, 0x7fffffffu, 1, 3}, // +inf
        {Op::FPDoubleToFixedS32, 0xfff0000000000000ull, 0x80000000u, 1, 3}, // -inf
        {Op::FPDoubleToFixedS32, 0x7ff8000000000000ull, 0, 1, 3}, // quiet NaN
        {Op::FPDoubleToFixedS32, 0x7ff0000000000001ull, 0, 1, 3}, // signaling NaN
        {Op::FPDoubleToFixedS32, 0x0000000000000001ull, 0, 0x10, 3}, // min denormal
        {Op::FPDoubleToFixedS32, 0x0010000000000000ull, 0, 0x10, 3}, // min normal
        // binary64, nearest-even
        {Op::FPDoubleToFixedS32, 0x3ff8000000000000ull, 2, 0x10, 0}, // 1.5 to 2
        {Op::FPDoubleToFixedS32, 0x4004000000000000ull, 2, 0x10, 0}, // 2.5 to 2
        {Op::FPDoubleToFixedS32, 0x3fe0000000000000ull, 0, 0x10, 0}, // 0.5 to 0
        {Op::FPDoubleToFixedS32, 0x3ff0000000000000ull, 1, 0, 0}, // 1.0 exact
        {Op::FPDoubleToFixedS32, 0xc1e0000000000000ull, 0x80000000u, 0, 0},
        {Op::FPDoubleToFixedS32, 0x7ff8000000000000ull, 0, 1, 0}, // NaN
    };
    for (const auto &c : cases) {
        auto block = blank(0);
        const bool is_double = c.op == Op::FPDoubleToFixedS32;
        const auto value = is_double
            ? append(block, c.op, {read_d(block), Value{uint8_t(0)}, Value{c.rounding}})
            : append(block, c.op, {read_s(block), Value{uint8_t(0)}, Value{c.rounding}});
        write_s(block, value);
        auto state = base_state(0x9000001cu), expected = state;
        if (is_double) set_d_bits(state, c.input);
        else set_s_bits(state, uint32_t(c.input));
        expected = state;
        set_s_bits(expected, c.result);
        expected.fpscr |= c.flags;
        run(block, state, &expected);
    }
    // FZ flushes a denormal input to a signed zero and raises IDC, while a
    // tiny normal input still truncates with IXC.
    struct FlushCase { Op op; uint64_t input; uint32_t result; };
    const FlushCase flushes[] = {
        {Op::FPSingleToFixedS32, 0x00400000, 0},
        {Op::FPSingleToFixedS32, 0x80400000, 0},
        {Op::FPDoubleToFixedS32, 0x0008000000000000ull, 0},
        {Op::FPDoubleToFixedS32, 0x8008000000000000ull, 0},
    };
    for (const auto &c : flushes) {
        auto block = blank(fpscr_fz);
        const bool is_double = c.op == Op::FPDoubleToFixedS32;
        const auto value = is_double
            ? append(block, c.op, {read_d(block), Value{uint8_t(0)}, Value{uint8_t(3)}})
            : append(block, c.op, {read_s(block), Value{uint8_t(0)}, Value{uint8_t(3)}});
        write_s(block, value);
        auto state = base_state(fpscr_fz), expected = state;
        if (is_double) set_d_bits(state, c.input);
        else set_s_bits(state, uint32_t(c.input));
        expected = state;
        set_s_bits(expected, c.result);
        expected.fpscr = fpscr_fz | 0x80;
        run(block, state, &expected);
    }
    for (uint64_t input : {0x00800000ull, 0x80800000ull}) {
        auto block = blank(fpscr_fz);
        write_s(block, append(block, Op::FPSingleToFixedS32,
            {read_s(block), Value{uint8_t(0)}, Value{uint8_t(3)}}));
        auto state = base_state(fpscr_fz), expected = state;
        set_s_bits(state, uint32_t(input));
        expected = state;
        set_s_bits(expected, 0);
        expected.fpscr = fpscr_fz | 0x10;
        run(block, state, &expected);
    }
    // DN has no integer result to canonicalize, so a NaN converts as usual.
    {
        auto block = blank(fpscr_dn);
        write_s(block, append(block, Op::FPSingleToFixedS32,
            {read_s(block), Value{uint8_t(0)}, Value{uint8_t(3)}}));
        auto state = base_state(fpscr_dn), expected = state;
        set_s_bits(state, 0xffc00001u);
        expected = state;
        set_s_bits(expected, 0);
        expected.fpscr = fpscr_dn | 1;
        run(block, state, &expected);
    }
    // More fraction bits than the 32-bit result holds, and ToOdd (not an
    // FPToFixed rounding mode), must fail closed.
    auto scaled = blank(0);
    write_s(scaled, append(scaled, Op::FPSingleToFixedS32,
        {read_s(scaled), Value{uint8_t(33)}, Value{uint8_t(3)}}));
    CHECK(vita3k::wasmjit::emit_block(scaled).empty());
    for (uint8_t rounding : {5, 6}) {
        for (Op op : {Op::FPSingleToFixedS32, Op::FPSingleToFixedU32,
                 Op::FPDoubleToFixedS32, Op::FPDoubleToFixedU32}) {
            auto bad = blank(0);
            const bool is_double = op == Op::FPDoubleToFixedS32 || op == Op::FPDoubleToFixedU32;
            const auto input = is_double ? read_d(bad) : read_s(bad);
            write_s(bad, append(bad, op, {input, Value{uint8_t(0)}, Value{rounding}}));
            CHECK(vita3k::wasmjit::emit_block(bad).empty());
            const A32::LocationDescriptor at{bad.Location()};
            CHECK(vita3k::wasmjit::emit_region({&bad}, {{at.PC(), PSR_DISPATCH_MASK,
                at.CPSR().Value() & PSR_DISPATCH_MASK, 1}}).empty());
        }
    }
}

void float_to_uint32() {
    struct Case { Op op; uint64_t input; uint32_t result; uint32_t flags; uint8_t rounding; };
    const Case cases[] = {
        // binary32, towards-zero. Negative inputs round first: those that
        // round to 0 give 0 with IXC, the others saturate to 0 with IOC.
        {Op::FPSingleToFixedU32, 0x3f800000, 1, 0, 3}, // 1.0
        {Op::FPSingleToFixedU32, 0x3fc00000, 1, 0x10, 3}, // 1.5
        {Op::FPSingleToFixedU32, 0x4f7fffff, 0xffffff00u, 0, 3}, // largest exact
        {Op::FPSingleToFixedU32, 0x4f800000, 0xffffffffu, 1, 3}, // 2^32 saturates
        {Op::FPSingleToFixedU32, 0x4f000000, 0x80000000u, 0, 3}, // 2^31 exact
        {Op::FPSingleToFixedU32, 0xcf800000, 0, 1, 3}, // -1.0
        {Op::FPSingleToFixedU32, 0xbf800000, 0, 1, 3}, // -1.0, not IXC
        {Op::FPSingleToFixedU32, 0xbf000000, 0, 0x10, 3}, // -0.5 truncates to 0
        {Op::FPSingleToFixedU32, 0x80000000, 0, 0, 3}, // -0 exact, no IOC
        {Op::FPSingleToFixedU32, 0x80000001, 0, 0x10, 3}, // negative denormal truncates to 0
        {Op::FPSingleToFixedU32, 0x7fc00000, 0, 1, 3}, // NaN
        {Op::FPSingleToFixedU32, 0x7f800000, 0xffffffffu, 1, 3}, // +inf
        {Op::FPSingleToFixedU32, 0xff800000, 0, 1, 3}, // -inf
        {Op::FPSingleToFixedU32, 0x00000001, 0, 0x10, 3}, // min denormal
        // binary32, nearest-even
        {Op::FPSingleToFixedU32, 0x3fc00000, 2, 0x10, 0},
        {Op::FPSingleToFixedU32, 0x40200000, 2, 0x10, 0},
        {Op::FPSingleToFixedU32, 0x3f000000, 0, 0x10, 0},
        // binary64, towards-zero
        {Op::FPDoubleToFixedU32, 0x3ff8000000000000ull, 1, 0x10, 3}, // 1.5
        {Op::FPDoubleToFixedU32, 0x41efffffffffffffull, 0xffffffffu, 0x10, 3}, // max - ulp
        {Op::FPDoubleToFixedU32, 0xbff0000000000000ull, 0, 1, 3}, // -1.0
        {Op::FPDoubleToFixedU32, 0xc1e0000000000000ull, 0, 1, 3}, // -2^31
        {Op::FPDoubleToFixedU32, 0xfff0000000000000ull, 0, 1, 3}, // -inf
        {Op::FPDoubleToFixedU32, 0x41e0000000000000ull, 0x80000000u, 0, 3}, // 2^31
        {Op::FPDoubleToFixedU32, 0x41f0000000000000ull, 0xffffffffu, 1, 3}, // 2^32
        {Op::FPDoubleToFixedU32, 0x7ff8000000000000ull, 0, 1, 3}, // NaN
        {Op::FPDoubleToFixedU32, 0x7ff0000000000000ull, 0xffffffffu, 1, 3}, // +inf
        // binary64, nearest-even
        {Op::FPDoubleToFixedU32, 0x3ff8000000000000ull, 2, 0x10, 0},
        {Op::FPDoubleToFixedU32, 0x4004000000000000ull, 2, 0x10, 0},
    };
    for (const auto &c : cases) {
        auto block = blank(0);
        const bool is_double = c.op == Op::FPDoubleToFixedU32;
        const auto value = is_double
            ? append(block, c.op, {read_d(block), Value{uint8_t(0)}, Value{c.rounding}})
            : append(block, c.op, {read_s(block), Value{uint8_t(0)}, Value{c.rounding}});
        write_s(block, value);
        auto state = base_state(0x9000001cu), expected = state;
        if (is_double) set_d_bits(state, c.input);
        else set_s_bits(state, uint32_t(c.input));
        expected = state;
        set_s_bits(expected, c.result);
        expected.fpscr |= c.flags;
        run(block, state, &expected);
    }
    // An FZ-flushed negative denormal raises IDC only; without the flush the
    // same input truncates to 0 with IXC.
    for (uint64_t input : {0x80400000ull, 0x80000001ull}) {
        auto block = blank(fpscr_fz);
        write_s(block, append(block, Op::FPSingleToFixedU32,
            {read_s(block), Value{uint8_t(0)}, Value{uint8_t(3)}}));
        auto state = base_state(fpscr_fz), expected = state;
        set_s_bits(state, uint32_t(input));
        expected = state;
        set_s_bits(expected, 0);
        expected.fpscr = fpscr_fz | 0x80;
        run(block, state, &expected);
    }
    {
        auto block = blank(fpscr_fz);
        write_s(block, append(block, Op::FPDoubleToFixedU32,
            {read_d(block), Value{uint8_t(0)}, Value{uint8_t(3)}}));
        auto state = base_state(fpscr_fz), expected = state;
        set_d_bits(state, 0x8008000000000000ull);
        expected = state;
        set_s_bits(expected, 0);
        expected.fpscr = fpscr_fz | 0x80;
        run(block, state, &expected);
    }
}

void compare64() {
    // VCMPSD/VCMPS-style NZCV results from the two operands. C=1,V=1 unordered;
    // Z,C equal; N less; C greater.
    struct Case { uint64_t a, b; uint32_t nzcv; uint32_t flags; };
    const Case cases[] = {
        {0x3ff0000000000000ull, 0x4000000000000000ull, 0x80000000u, 0}, // 1 < 2 -> N
        {0x4000000000000000ull, 0x3ff0000000000000ull, 0x20000000u, 0}, // 2 > 1 -> C
        {0x3ff0000000000000ull, 0x3ff0000000000000ull, 0x60000000u, 0}, // equal
        {0x8000000000000000ull, 0x0000000000000000ull, 0x60000000u, 0}, // -0 == +0
        {0x7ff0000000000000ull, 0x7ff0000000000000ull, 0x60000000u, 0}, // inf == inf
        {0xfff0000000000000ull, 0x7ff0000000000000ull, 0x80000000u, 0}, // -inf < inf
        {0xc000000000000000ull, 0xbff0000000000000ull, 0x80000000u, 0}, // -2 < -1
        {0xbff0000000000000ull, 0xc000000000000000ull, 0x20000000u, 0}, // -1 > -2
        {0x7ff8000000000000ull, 0x3ff0000000000000ull, 0x30000000u, 0}, // quiet NaN unordered
        {0x7ff0000000000001ull, 0x3ff0000000000000ull, 0x30000000u, 1}, // sNaN raises IOC
    };
    for (const auto &c : cases) {
        for (bool signal : {false, true}) {
            auto block = blank(0);
            append(block, Op::A32SetFpscrNZCV, {append(block, Op::FPCompare64,
                {read_d(block, 0), read_d(block, 1), Value{signal}})});
            auto state = base_state(0), expected = state;
            set_d_bits(state, c.a, 0);
            set_d_bits(state, c.b, 1);
            // Only N/Z/C/V are replaced; the rest of FPSCR (including sticky
            // flags) must survive.
            expected = state;
            const bool nan = (c.a & 0x7fffffffffffffffull) > 0x7ff0000000000000ull
                || (c.b & 0x7fffffffffffffffull) > 0x7ff0000000000000ull;
            expected.fpscr = c.nzcv | c.flags | ((signal && nan) ? 1u : 0u);
            run(block, state, &expected);
        }
    }
    // FZ flushes a denormal operand before the comparison and raises IDC.
    {
        auto block = blank(fpscr_fz);
        append(block, Op::A32SetFpscrNZCV, {append(block, Op::FPCompare64,
            {read_d(block, 0), read_d(block, 1), Value{false}})});
        auto state = base_state(fpscr_fz), expected = state;
        set_d_bits(state, 0x0000000000000001ull, 0);
        set_d_bits(state, 0x8000000000000000ull, 1); // flushed to -0 == +0
        expected = state;
        expected.fpscr = fpscr_fz | 0x80 | 0x60000000u;
        run(block, state, &expected);
    }
}
// Trap enables are live state, not part of the location key. Reuse each
// compiled module with every enable and require a bail before architectural writes.
void mode_guards() {
    for (Op opcode : {Op::FPSingleToDouble, Op::FPDoubleToSingle, Op::FPCompare64,
             Op::FPSingleToFixedS32, Op::FPSingleToFixedU32, Op::FPDoubleToFixedS32,
             Op::FPDoubleToFixedU32}) {
        auto block = blank(0);
        if (opcode == Op::FPSingleToDouble)
            write_d(block, append(block, opcode, {read_s(block), Value{uint8_t(0)}}));
        else if (opcode == Op::FPDoubleToSingle)
            write_s(block, append(block, opcode, {read_d(block), Value{uint8_t(0)}}));
        else if (opcode == Op::FPCompare64)
            append(block, Op::A32SetFpscrNZCV, {append(block, opcode,
                {read_d(block), read_d(block, 1), Value{false}})});
        else if (opcode == Op::FPSingleToFixedS32 || opcode == Op::FPSingleToFixedU32)
            write_s(block, append(block, opcode,
                {read_s(block), Value{uint8_t(0)}, Value{uint8_t(3)}}));
        else
            write_s(block, append(block, opcode,
                {read_d(block), Value{uint8_t(0)}, Value{uint8_t(3)}}));
        const A32::LocationDescriptor at{block.Location()};
        for (bool region : {false, true}) {
            const auto bytes = region ? vita3k::wasmjit::emit_region({&block},
                {{at.PC(), PSR_DISPATCH_MASK, at.CPSR().Value() & PSR_DISPATCH_MASK, 1}})
                : vita3k::wasmjit::emit_block(block);
            CHECK(!bytes.empty());
            const int slot = region
                ? vita3k_jit_install_region(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write)
                : vita3k_jit_install(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write);
            CHECK(slot >= 0);
            for (uint32_t trap : {0x100u, 0x200u, 0x400u, 0x800u, 0x1000u, 0x8000u}) {
                auto state = base_state(trap | 0x90000014u);
                const auto initial = state;
                const auto reason = static_cast<Reason>(region
                    ? vita3k_jit_run(slot, reinterpret_cast<uintptr_t>(&state), 1)
                    : vita3k_jit_call(slot, reinterpret_cast<uintptr_t>(&state)));
                CHECK(reason == Reason::Unsupported);
                CHECK(state.fpscr == initial.fpscr);
                CHECK(state.cpsr == initial.cpsr);
                for (unsigned i = 0; i < 64; ++i) CHECK(state.fpu[i] == initial.fpu[i]);
                for (unsigned i = 0; i < 15; ++i) CHECK(state.regs[i] == initial.regs[i]);
            }
            if (region) vita3k_jit_release_region(slot);
            else vita3k_jit_release(slot);
        }
    }
    for (Op opcode : {Op::FPSingleToDouble, Op::FPDoubleToSingle}) {
        for (uint8_t rounding : {1, 2, 3}) {
            auto block = blank(uint32_t(rounding) << 22);
            if (opcode == Op::FPSingleToDouble)
                write_d(block, append(block, opcode, {read_s(block), Value{rounding}}));
            else
                write_s(block, append(block, opcode, {read_d(block), Value{rounding}}));
            CHECK(vita3k::wasmjit::emit_block(block).empty());
        }
    }
}

// Real VCVT encodings through the translator and dispatcher, ARM and Thumb,
// single-block and region. Dest is always s2; f32 sources read s5 and f64
// sources read d7, so overlapping lanes cannot hide a miswired operand.
// The Thumb words are the half-swapped ARM encodings, verified against the
// A32 decoder patterns (VCVT to u32/s32) and a Thumb disassembler.
// VCVT{A,N,P,M}.S32/U32.F32/F64 (VFPv5; rm 0..3 = A, N, P, M) and VCVTR under
// each FPSCR.RMode, through the ARM/Thumb translator. Hand-derived results.
void guest_float_to_int_rounding(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    struct Case { uint32_t input; uint32_t by_mode[4]; }; // A, N, P, M
    const Case cases[] = {
        {0x3fc00000u, {2, 2, 2, 1}}, // 1.5
        {0x40200000u, {3, 2, 3, 2}}, // 2.5
        {0xbfc00000u, {0xfffffffeu, 0xfffffffeu, 0xffffffffu, 0xfffffffeu}}, // -1.5
        {0xc0200000u, {0xfffffffdu, 0xfffffffeu, 0xfffffffeu, 0xfffffffdu}}, // -2.5
        {0x3f000000u, {1, 0, 1, 0}}, // 0.5
        {0xbf000000u, {0xffffffffu, 0, 0, 0xffffffffu}}, // -0.5
        {0x3e800000u, {0, 0, 1, 0}}, // 0.25
        {0xbe800000u, {0, 0, 0, 0xffffffffu}}, // -0.25
    };
    unsigned count = 0;
    for (bool regions : {false, true}) for (bool thumb : {false, true}) for (bool from_double : {false, true})
        for (unsigned rm = 0; rm < 4; ++rm) for (bool vcvtr : {false, true}) for (bool is_unsigned : {false, true})
            for (const auto &c : cases) {
                // VCVTR follows FPSCR.RMode (0 N, 1 P, 2 M, 3 Z); A has no RMode.
                if (vcvtr && rm == 0) continue;
                const uint32_t rmode = rm == 1 ? 0 : rm == 2 ? 1 : 2;
                // Destination s2; source s5 (M=1, Vm=2) or d7 (sz=1, Vm=7).
                const uint32_t source = from_double ? 0x107u : 0x22u;
                const uint32_t opcode = vcvtr
                    ? (is_unsigned ? 0xeebc1a40u : 0xeebd1a40u) | source
                    : (is_unsigned ? 0xfebc1a40u : 0xfebc1ac0u) | (rm << 16) | source;
                jit.set_region_mode(regions);
                if (thumb) put_thumb(mem, jit, {(opcode << 16) | (opcode >> 16), 0xbf00df00u});
                else put(mem, jit, {opcode, 0xef000000u});
                auto before = jit.save_context();
                if (from_double) {
                    const double wide = double(std::bit_cast<float>(c.input));
                    std::memcpy(&before.fpu_registers[14], &wide, 8);
                } else {
                    std::memcpy(&before.fpu_registers[5], &c.input, 4);
                }
                before.fpscr = 0x90000080u | (vcvtr ? rmode << 22 : 0);
                jit.load_context(before);
                if (jit.run() != 0) {
                    std::fprintf(stderr, "vcvt rounding guest failed: %08x (%s): %s\n", opcode,
                        thumb ? "Thumb" : "ARM", jit.get_last_error().c_str());
                    CHECK(false);
                }
                // ARM ARM FPToFixed rounds, then saturates: an unsigned
                // conversion is invalid only when the rounded value is
                // negative (-0.5 to nearest even or towards +inf is 0, IXC).
                const bool invalid = is_unsigned && int32_t(c.by_mode[rm]) < 0;
                const uint32_t want = invalid ? 0 : c.by_mode[rm];
                const uint32_t flags = invalid ? 1 : 0x10;
                const auto after = jit.save_context();
                uint32_t result;
                std::memcpy(&result, &after.fpu_registers[2], 4);
                if (result != want || after.fpscr != (before.fpscr | flags))
                    std::fprintf(stderr, "vcvt rounding op=%08x input=%08x got=%08x/%08x want=%08x/%08x\n",
                        opcode, c.input, result, after.fpscr, want, before.fpscr | flags);
                CHECK(result == want);
                CHECK(after.fpscr == (before.fpscr | flags));
                ++count;
            }
    std::printf("VCVT{A,N,P,M}/VCVTR rounding: %u guest cases passed\n", count);
}

void guest_float_to_int(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    struct Case { uint32_t opcode; bool from_double; uint64_t input; uint32_t result; uint32_t flags; };
    const Case cases[] = {
        {0xeebd1ae2u, false, 0x3fc00000, 1, 0x10}, // VCVT.S32.F32 1.5 -> 1
        {0xeebd1ae2u, false, 0xbfc00000, 0xffffffffu, 0x10}, // -1.5 -> -1
        {0xeebd1ae2u, false, 0x4f000000, 0x7fffffffu, 1}, // 2^31 saturates
        {0xeebd1ae2u, false, 0xcf000000, 0x80000000u, 0}, // -2^31 exact
        {0xeebd1ae2u, false, 0x7fc00001, 0, 1}, // NaN -> 0
        {0xeebd1a62u, false, 0x3fc00000, 2, 0x10}, // VCVTR.S32.F32 1.5 -> 2
        {0xeebd1a62u, false, 0x40200000, 2, 0x10}, // VCVTR 2.5 ties to 2
        {0xeebc1ae2u, false, 0x3fc00000, 1, 0x10}, // VCVT.U32.F32
        {0xeebc1ae2u, false, 0xcf800000, 0, 1}, // -2^32 invalid, not inexact
        {0xeebc1ae2u, false, 0xbf000000, 0, 0x10}, // -0.5 truncates to 0: inexact, not invalid
        {0xeebc1ae2u, false, 0x4f7fffff, 0xffffff00u, 0}, // largest exact u32
        {0xeebc1ae2u, false, 0x4f800000, 0xffffffffu, 1}, // 2^32 saturates
        {0xeebd1bc7u, true, 0xc004000000000000ull, 0xfffffffeu, 0x10}, // VCVT.S32.F64 -2.5
        {0xeebd1bc7u, true, 0x41e0000000000000ull, 0x7fffffffu, 1}, // 2^31
        {0xeebd1bc7u, true, 0xc1e0000000000000ull, 0x80000000u, 0}, // -2^31
        {0xeebd1bc7u, true, 0x7ff8000000000000ull, 0, 1}, // NaN
        {0xeebc1bc7u, true, 0x3ff8000000000000ull, 1, 0x10}, // VCVT.U32.F64 1.5
        {0xeebc1bc7u, true, 0xbff0000000000000ull, 0, 1}, // -1.0 invalid
        {0xeebc1bc7u, true, 0xbfefffffffffffffull, 0, 0x10}, // just above -1.0 truncates to 0
        {0xeebc1bc7u, true, 0x41e0000000000000ull, 0x80000000u, 0}, // 2^31 exact
        {0xeebc1bc7u, true, 0x41f0000000000000ull, 0xffffffffu, 1}, // 2^32
    };
    for (bool regions : {false, true}) for (bool thumb : {false, true}) for (const auto &c : cases) {
        jit.set_region_mode(regions);
        if (thumb) put_thumb(mem, jit, {(c.opcode << 16) | (c.opcode >> 16), 0xbf00df00u});
        else put(mem, jit, {c.opcode, 0xef000000u});
        auto before = jit.save_context();
        if (c.from_double) std::memcpy(&before.fpu_registers[14], &c.input, 8);
        else {
            const uint32_t bits = uint32_t(c.input);
            std::memcpy(&before.fpu_registers[5], &bits, 4);
        }
        before.fpscr = 0x90000080u;
        jit.load_context(before);
        if (jit.run() != 0) {
            std::fprintf(stderr, "vcvt guest failed: %08x (%s): %s\n", c.opcode,
                thumb ? "Thumb" : "ARM", jit.get_last_error().c_str());
            CHECK(false);
        }
        CHECK(parent.svc_called);
        const auto after = jit.save_context();
        uint32_t result;
        std::memcpy(&result, &after.fpu_registers[2], 4);
        if (result != c.result || after.fpscr != (before.fpscr | c.flags))
            std::fprintf(stderr, "vcvt op=%08x input=%llx got=%08x expected=%08x fpscr=%08x expected=%08x\n",
                c.opcode, (unsigned long long)c.input, result, c.result,
                after.fpscr, before.fpscr | c.flags);
        CHECK(result == c.result);
        CHECK(after.fpscr == (before.fpscr | c.flags));
        CHECK(after.cpsr == before.cpsr);
    }
}
// VCVTB/VCVTT.F32.F16 (FPHalfToSingle): all 65536 half inputs under each
// relevant location mode (FZ, DN, AHP) against Dynarmic's FPConvert, which
// the emitted integer lowering never calls. The source register's other
// half holds junk that must be ignored.
void half_to_single() {
    constexpr uint32_t fpscr_ahp = 0x04000000;
    unsigned count = 0;
    for (uint32_t location_fpscr : {0u, fpscr_fz, fpscr_dn, fpscr_ahp, fpscr_ahp | fpscr_dn}) {
        auto block = blank(location_fpscr);
        const auto half = append(block, Op::LeastSignificantHalf, {read_s(block, 1)});
        write_s(block, append(block, Op::FPHalfToSingle, {half, Value{uint8_t(0)}}));
        const A32::LocationDescriptor at{block.Location()};
        for (bool region : {false, true}) {
            const auto bytes = region
                ? vita3k::wasmjit::emit_region({&block}, {{at.PC(), PSR_DISPATCH_MASK,
                    at.CPSR().Value() & PSR_DISPATCH_MASK, 1}})
                : vita3k::wasmjit::emit_block(block);
            CHECK(!bytes.empty());
            if (bytes.empty()) continue;
            const int slot = region
                ? vita3k_jit_install_region(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write)
                : vita3k_jit_install(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write);
            CHECK(slot >= 0);
            for (uint32_t input = 0; input < 0x10000; ++input) {
                Dynarmic::FP::FPSR fpsr{0};
                const uint32_t want = Dynarmic::FP::FPConvert<uint32_t, uint16_t>(uint16_t(input),
                    Dynarmic::FP::FPCR{location_fpscr}, Dynarmic::FP::RoundingMode::ToNearest_TieEven, fpsr);
                auto state = base_state(location_fpscr | 0x90000000u);
                set_s_bits(state, input | 0xdead0000u, 1);
                const auto reason = static_cast<Reason>(region
                    ? vita3k_jit_run(slot, reinterpret_cast<uintptr_t>(&state), 1)
                    : vita3k_jit_call(slot, reinterpret_cast<uintptr_t>(&state)));
                CHECK(reason == (region ? Reason::Miss : Reason::Continue));
                if (s_bits(state) != want || state.fpscr != (location_fpscr | 0x90000000u | (fpsr.Value() & 0x9f)))
                    std::fprintf(stderr, "half->single fpscr=%08x input=%04x got=%08x/%08x want=%08x/%02x\n",
                        location_fpscr, input, s_bits(state), state.fpscr, want, fpsr.Value() & 0x9f);
                CHECK(s_bits(state) == want);
                CHECK(state.fpscr == (location_fpscr | 0x90000000u | (fpsr.Value() & 0x9f)));
                CHECK(s_bits(state, 1) == (input | 0xdead0000u));
                ++count;
            }
            if (region) vita3k_jit_release_region(slot);
            else vita3k_jit_release(slot);
        }
    }
    // A live trap enable must stop before any architectural write.
    auto block = blank(0);
    write_s(block, append(block, Op::FPHalfToSingle,
        {append(block, Op::LeastSignificantHalf, {read_s(block, 1)}), Value{uint8_t(0)}}));
    const auto bytes = vita3k::wasmjit::emit_block(block);
    const int slot = vita3k_jit_install(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write);
    auto state = base_state(0x00000100u);
    set_s_bits(state, 0x7c01u, 1); // signaling NaN
    CHECK(static_cast<Reason>(vita3k_jit_call(slot, reinterpret_cast<uintptr_t>(&state))) == Reason::Unsupported);
    CHECK(s_bits(state) == 0x5a5a5a5au && state.fpscr == 0x00000100u);
    vita3k_jit_release(slot);
    std::printf("half->single: %u oracle cases (FZ/DN/AHP, block+region) passed\n", count);
}

void guest_half_to_single(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    const std::array<uint32_t, 6> inputs{0x3c00c000u, 0x7c00fc00u, 0x7c017e00u, 0x00010400u, 0x83ff8000u, 0x7bff0001u};
    unsigned count = 0;
    for (bool region : {false, true}) {
        jit.set_region_mode(region);
        for (bool thumb : {false, true}) for (bool top : {false, true}) for (uint32_t fpscr : {0u, fpscr_dn}) {
            for (const uint32_t input : inputs) {
                // VCVTB/VCVTT.F32.F16 s0, s1 (identical VFP words in ARM and Thumb)
                const uint32_t opcode = top ? 0xeeb20ae0u : 0xeeb20a60u;
                if (thumb) put_thumb(mem, jit, {(opcode << 16) | (opcode >> 16), 0xbf00df00});
                else put(mem, jit, {opcode, 0xef000000});
                auto context = jit.save_context();
                context.fpu_registers[0] = std::bit_cast<float>(0x5a5a5a5au);
                context.fpu_registers[1] = std::bit_cast<float>(input);
                context.fpscr = fpscr;
                context.cpsr = 0x10 | (thumb ? 0x20 : 0);
                jit.load_context(context);
                if (jit.run() != 0) {
                    std::fprintf(stderr, "VCVTB/T guest failed: %s\n", jit.get_last_error().c_str());
                    CHECK(false);
                }
                Dynarmic::FP::FPSR fpsr{0};
                const uint32_t want = Dynarmic::FP::FPConvert<uint32_t, uint16_t>(uint16_t(top ? input >> 16 : input),
                    Dynarmic::FP::FPCR{fpscr}, Dynarmic::FP::RoundingMode::ToNearest_TieEven, fpsr);
                const auto result = jit.save_context();
                const uint32_t s0 = std::bit_cast<uint32_t>(result.fpu_registers[0]);
                if (s0 != want || result.fpscr != (fpscr | (fpsr.Value() & 0x9f)))
                    std::fprintf(stderr, "VCVT%c %s fpscr=%08x input=%08x got=%08x/%08x want=%08x/%02x\n",
                        top ? 'T' : 'B', thumb ? "Thumb" : "ARM", fpscr, input, s0,
                        result.fpscr, want, fpsr.Value() & 0x9f);
                CHECK(s0 == want);
                CHECK(std::bit_cast<uint32_t>(result.fpu_registers[1]) == input);
                CHECK(result.fpscr == (fpscr | (fpsr.Value() & 0x9f)));
                CHECK(jit.get_pc() == code + (thumb ? 6 : 8));
                ++count;
            }
        }
    }
    std::printf("half->single: %u VCVTB/VCVTT guest cases passed\n", count);
}

// Float-to-integer and fixed-point VCVT (fbits 0..32; Limbo's VCVT.S32.F32
// s15, s15, #16), 32- and 16-bit results. The emitted lowering is pure
// integer code; the expectation is the ARM ARM FPToFixed (arm_fp_to_fixed.h)
// under the location FPSCR. Dynarmic's FPToFixed stays a second oracle: it
// must agree everywhere except the two divergences that header names, and
// both must occur in the sweep. Pinned cases below are hand-derived.
void float_to_fixed_scaled() {
    const Op ops[] = {Op::FPSingleToFixedS32, Op::FPSingleToFixedU32, Op::FPDoubleToFixedS32, Op::FPDoubleToFixedU32,
        Op::FPSingleToFixedS16, Op::FPSingleToFixedU16, Op::FPDoubleToFixedS16, Op::FPDoubleToFixedU16};
    std::vector<uint32_t> singles{0x00000000u, 0x80000000u, 0x00000001u, 0x807fffffu, 0x00800000u,
        0x3f800000u, 0xbf800000u, 0x3fc00000u, 0xbfc00000u, 0x3f000000u, 0x37000000u, 0x37800000u,
        0x37c00000u, 0x46ffffffu, 0x47000000u, 0xc7000000u, 0xc7000001u, 0x4f000000u, 0x4f800000u,
        0x7f7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0x7f800001u, 0x2f800000u, 0x30000000u,
        0x40200000u, 0xc0200000u, 0x3e800000u, 0xbe800000u, 0x2f000000u, 0xaf000000u, 0xceffffffu,
        // -0.5, +-0.75, and +-(2^15 - 1/2, 1/4, 3/4) for the 16-bit limits.
        0xbf000000u, 0x3f400000u, 0xbf400000u, 0x46ffff00u, 0xc6ffff00u, 0xc6fffe80u, 0xc6ffff80u};
    std::vector<uint64_t> doubles{0, 0x8000000000000000ull, 1, 0x800fffffffffffffull, 0x0010000000000000ull,
        0x3ff0000000000000ull, 0xbff8000000000000ull, 0x3ef0000000000000ull, 0x3ee0000000000000ull,
        0x3ef8000000000000ull, 0x40dfffffffffffffull, 0x40e0000000000000ull, 0xc0e0000000000000ull,
        0xc0e0000000000001ull, 0x41f0000000000000ull, 0x7fefffffffffffffull, 0x7ff0000000000000ull,
        0xfff0000000000000ull, 0x7ff8000000000000ull, 0x7ff0000000000001ull, 0x3df0000000000000ull,
        0x41e0000000100000ull, 0xc1e0000000100000ull, 0x41efffffffe00000ull, // overflow after a fraction
        // +-(2^32 - 1/2) and 2^32 - 1/4: rounding up carries out of 32 bits.
        0x41effffffff00000ull, 0xc1effffffff00000ull, 0x41effffffff80000ull,
        // -(2^31 - 1/4, 1/2, 3/4) and -(2^31 + 1/2): SignedIntoRange.
        0xc1dffffffff00000ull, 0xc1dfffffffe00000ull, 0xc1dfffffffd00000ull, 0xc1e0000000100000ull,
        0x41dfffffffe00000ull, 0x41dffffffff00000ull, // 2^31 - 1/2, - 1/4
        // Ties and tiny magnitudes for the nearest and directed modes.
        0x3fe0000000000000ull, 0xbfe0000000000000ull, 0x4004000000000000ull, 0xc004000000000000ull,
        0x3d70000000000000ull, 0xbd70000000000000ull, 0x3fe8000000000000ull, 0xbfe8000000000000ull,
        // -(2^15 - 1/2, 1/4, 3/4): SignedIntoRange for 16-bit results.
        0xc0dfffe000000000ull, 0xc0dfffd000000000ull, 0xc0dffff000000000ull};
    uint32_t rng32 = 0x2545f491u;
    uint64_t rng64 = 0x853c49e6748fea9bull;
    for (unsigned i = 0; i < 400; ++i) {
        rng32 ^= rng32 << 13; rng32 ^= rng32 >> 17; rng32 ^= rng32 << 5;
        rng64 ^= rng64 << 13; rng64 ^= rng64 >> 7; rng64 ^= rng64 << 17;
        // Bias exponents into the interesting range around 2^(31 - fbits).
        singles.push_back((rng32 & 0x807fffffu) | ((0x60u + (rng32 >> 24) % 0x40u) << 23));
        doubles.push_back((rng64 & 0x800fffffffffffffull) | (uint64_t(0x3c0u + (rng64 >> 52) % 0x80u) << 52));
    }
    using arm_reference::Divergence;
    unsigned count = 0;
    std::map<Divergence, unsigned> divergences;
    for (const Op op : ops) for (uint8_t fbits : {0, 1, 8, 16, 31, 32}) for (uint8_t rounding : {0, 1, 2, 3, 4})
        for (uint32_t location_fpscr : {0u, fpscr_fz}) {
            const bool is_double = op == Op::FPDoubleToFixedS32 || op == Op::FPDoubleToFixedU32
                || op == Op::FPDoubleToFixedS16 || op == Op::FPDoubleToFixedU16;
            const bool unsigned_ = op == Op::FPSingleToFixedU32 || op == Op::FPDoubleToFixedU32
                || op == Op::FPSingleToFixedU16 || op == Op::FPDoubleToFixedU16;
            const unsigned ibits = op == Op::FPSingleToFixedS16 || op == Op::FPSingleToFixedU16
                    || op == Op::FPDoubleToFixedS16 || op == Op::FPDoubleToFixedU16
                ? 16 : 32;
            if (fbits > ibits) continue;
            auto block = blank(location_fpscr);
            const auto input = is_double ? read_d(block, 1) : read_s(block, 1);
            auto result = append(block, op, {input, Value{fbits}, Value{rounding}});
            // The translator widens a 16-bit result exactly like this.
            if (ibits == 16)
                result = append(block, unsigned_ ? Op::ZeroExtendHalfToWord : Op::SignExtendHalfToWord, {result});
            write_s(block, result);
            const A32::LocationDescriptor at{block.Location()};
            for (bool region : {false, true}) {
                const auto bytes = region
                    ? vita3k::wasmjit::emit_region({&block}, {{at.PC(), PSR_DISPATCH_MASK,
                        at.CPSR().Value() & PSR_DISPATCH_MASK, 1}})
                    : vita3k::wasmjit::emit_block(block);
                CHECK(!bytes.empty());
                const int slot = region
                    ? vita3k_jit_install_region(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write)
                    : vita3k_jit_install(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write);
                CHECK(slot >= 0);
                const size_t n = is_double ? doubles.size() : singles.size();
                for (size_t i = 0; i < n; ++i) {
                    const uint64_t bits = is_double ? doubles[i] : singles[i];
                    const auto arm = arm_reference::fp_to_fixed(bits, is_double, fbits, unsigned_, rounding,
                        location_fpscr & fpscr_fz, ibits);
                    Dynarmic::FP::FPSR fpsr{0};
                    const auto mode = static_cast<Dynarmic::FP::RoundingMode>(rounding);
                    const uint32_t dynarmic = is_double
                        ? uint32_t(Dynarmic::FP::FPToFixed<uint64_t>(ibits, doubles[i], fbits, unsigned_,
                            Dynarmic::FP::FPCR{location_fpscr}, mode, fpsr))
                        : uint32_t(Dynarmic::FP::FPToFixed<uint32_t>(ibits, singles[i], fbits, unsigned_,
                            Dynarmic::FP::FPCR{location_fpscr}, mode, fpsr));
                    const auto divergence = arm_reference::fp_to_fixed_divergence(arm, dynarmic, fpsr.Value() & 0x9f,
                        unsigned_, ibits);
                    if (divergence == Divergence::Unexplained)
                        std::fprintf(stderr, "tofixed reference %s fbits=%u rounding=%u input=%llx arm=%08x/%02x dynarmic=%08x/%02x\n",
                            IR::GetNameOf(op).c_str(), fbits, rounding, (unsigned long long)bits, arm.bits, arm.flags,
                            dynarmic, fpsr.Value() & 0x9f);
                    CHECK(divergence != Divergence::Unexplained);
                    if (!region) ++divergences[divergence];
                    const uint32_t want = ibits == 32 || unsigned_ ? arm.bits : uint32_t(int32_t(int16_t(arm.bits)));
                    auto state = base_state(location_fpscr | 0x90000000u);
                    if (is_double) set_d_bits(state, doubles[i], 1);
                    else set_s_bits(state, singles[i], 1);
                    const auto reason = static_cast<Reason>(region
                        ? vita3k_jit_run(slot, reinterpret_cast<uintptr_t>(&state), 1)
                        : vita3k_jit_call(slot, reinterpret_cast<uintptr_t>(&state)));
                    CHECK(reason == (region ? Reason::Miss : Reason::Continue));
                    if (s_bits(state) != want || state.fpscr != (location_fpscr | 0x90000000u | arm.flags))
                        std::fprintf(stderr, "tofixed %s fbits=%u rounding=%u fpscr=%08x input=%llx got=%08x/%08x want=%08x/%02x\n",
                            IR::GetNameOf(op).c_str(), fbits, rounding, location_fpscr,
                            (unsigned long long)bits, s_bits(state), state.fpscr, want, arm.flags);
                    CHECK(s_bits(state) == want);
                    CHECK(state.fpscr == (location_fpscr | 0x90000000u | arm.flags));
                    ++count;
                }
                if (region) vita3k_jit_release_region(slot);
                else vita3k_jit_release(slot);
            }
        }
    struct Case { Op op; uint64_t input; uint8_t fbits, rounding; uint32_t result, flags; };
    const Case pinned[] = {
        {Op::FPSingleToFixedS32, 0x3fc00000, 16, 3, 0x00018000u, 0}, // 1.5
        {Op::FPSingleToFixedS32, 0xbfc00000, 16, 3, 0xfffe8000u, 0}, // -1.5
        {Op::FPSingleToFixedU32, 0xbfc00000, 16, 3, 0, 1}, // negative unsigned below -1 unit: IOC
        {Op::FPSingleToFixedS32, 0x46ffffff, 16, 3, 0x7fffff80u, 0}, // just below 2^15
        {Op::FPSingleToFixedS32, 0x47000000, 16, 3, 0x7fffffffu, 1}, // 2^15 overflows signed
        {Op::FPSingleToFixedU32, 0x47000000, 16, 3, 0x80000000u, 0}, // ... but fits unsigned
        {Op::FPSingleToFixedS32, 0xc7000000, 16, 3, 0x80000000u, 0}, // -2^15 exact
        {Op::FPSingleToFixedS32, 0x37800000, 16, 3, 1, 0}, // 2^-16 is one unit
        {Op::FPSingleToFixedS32, 0x37000000, 16, 3, 0, 0x10}, // half a unit truncates
        {Op::FPSingleToFixedS32, 0x37000000, 16, 0, 0, 0x10}, // ... and ties to even 0
        {Op::FPSingleToFixedS32, 0x37c00000, 16, 3, 1, 0x10}, // 1.5 units
        {Op::FPSingleToFixedS32, 0x37c00000, 16, 0, 2, 0x10}, // ... ties to even 2
        {Op::FPSingleToFixedS32, 0x3f000000, 32, 3, 0x7fffffffu, 1}, // 0.5 * 2^32 = 2^31
        {Op::FPSingleToFixedU32, 0x3f000000, 32, 3, 0x80000000u, 0},
        {Op::FPSingleToFixedS32, 0x7fc00000, 16, 3, 0, 1}, // NaN
        {Op::FPDoubleToFixedS32, 0x3ff8000000000000ull, 16, 3, 0x00018000u, 0}, // 1.5
        {Op::FPDoubleToFixedU32, 0x3ef0000000000000ull, 16, 3, 1, 0}, // 2^-16
        {Op::FPDoubleToFixedS32, 0x40e0000000000000ull, 16, 3, 0x7fffffffu, 1}, // 2^15
        // Saturation raises IOC only, even when a fraction was dropped.
        {Op::FPDoubleToFixedS32, 0x41e0000000100000ull, 0, 3, 0x7fffffffu, 1}, // 2^31 + 0.5
        {Op::FPDoubleToFixedS32, 0x40dfffffffe00000ull, 16, 0, 0x7fffffffu, 1}, // 2^31 - 0.5 ties up to 2^31
        // Rounding modes 1 (+inf), 2 (-inf) and 4 (nearest, ties away).
        {Op::FPSingleToFixedS32, 0x3fc00000, 0, 1, 2, 0x10}, // 1.5
        {Op::FPSingleToFixedS32, 0x3fc00000, 0, 2, 1, 0x10},
        {Op::FPSingleToFixedS32, 0x3fc00000, 0, 4, 2, 0x10},
        {Op::FPSingleToFixedS32, 0xc0200000, 0, 1, 0xfffffffeu, 0x10}, // -2.5
        {Op::FPSingleToFixedS32, 0xc0200000, 0, 2, 0xfffffffdu, 0x10},
        {Op::FPSingleToFixedS32, 0xc0200000, 0, 4, 0xfffffffdu, 0x10},
        {Op::FPSingleToFixedS32, 0x40200000, 0, 4, 3, 0x10}, // 2.5 ties away
        {Op::FPSingleToFixedS32, 0x2f000000, 0, 1, 1, 0x10}, // tiny positive
        {Op::FPSingleToFixedS32, 0xaf000000, 0, 2, 0xffffffffu, 0x10}, // tiny negative
        {Op::FPSingleToFixedS32, 0xaf000000, 0, 1, 0, 0x10},
        {Op::FPSingleToFixedU32, 0x2f000000, 0, 1, 1, 0x10},
        // Negative unsigned: 0 with IXC when it rounds to 0 (ARM ARM, not
        // Dynarmic's IOC: UnsignedRoundsToZero), else saturated with IOC.
        {Op::FPSingleToFixedU32, 0xbf000000, 0, 1, 0, 0x10}, // -0.5 towards +inf
        {Op::FPSingleToFixedU32, 0xbf000000, 0, 3, 0, 0x10}, // -0.5 towards zero
        {Op::FPSingleToFixedU32, 0xbf000000, 0, 0, 0, 0x10}, // -0.5 ties to even 0
        {Op::FPSingleToFixedU32, 0xbf000000, 0, 4, 0, 1}, // -0.5 ties away to -1
        {Op::FPSingleToFixedU32, 0xbf000000, 0, 2, 0, 1}, // -0.5 towards -inf
        {Op::FPSingleToFixedU32, 0xbe800000, 0, 4, 0, 0x10}, // -0.25 to nearest
        {Op::FPSingleToFixedU32, 0xbf400000, 0, 0, 0, 1}, // -0.75 to nearest is -1
        {Op::FPDoubleToFixedU32, 0xbfe8000000000000ull, 0, 3, 0, 0x10}, // -0.75 towards zero
        {Op::FPDoubleToFixedU32, 0xbfe0000000000000ull, 16, 3, 0, 1}, // -0.5 * 2^16
        {Op::FPSingleToFixedU32, 0xb7000000, 16, 3, 0, 0x10}, // -2^-17 * 2^16 = -1/2 unit
        {Op::FPSingleToFixedU16, 0xbf000000, 0, 3, 0, 0x10},
        // 16-bit results: saturation at +-2^15 / 2^16 and SignedIntoRange.
        {Op::FPSingleToFixedS16, 0x46ffff00, 0, 3, 0x00007fffu, 0x10}, // 32767.5
        {Op::FPSingleToFixedS16, 0x46ffff00, 0, 0, 0x00007fffu, 1}, // ties up to 2^15
        {Op::FPSingleToFixedS16, 0xc6ffff00, 0, 3, 0xffff8001u, 0x10}, // -32767.5 towards zero
        {Op::FPSingleToFixedS16, 0xc7000080, 0, 3, 0xffff8000u, 0x10}, // -32768.5 truncates into range
        {Op::FPSingleToFixedS16, 0xc7000180, 0, 3, 0xffff8000u, 1}, // -32769.5 saturates
        {Op::FPSingleToFixedU16, 0x477fff00, 0, 3, 0x0000ffffu, 0}, // 65535
        {Op::FPSingleToFixedU16, 0x47800000, 0, 3, 0x0000ffffu, 1}, // 65536 saturates
        {Op::FPSingleToFixedS16, 0x3fc00000, 8, 3, 0x00000180u, 0}, // 1.5 * 2^8
        {Op::FPDoubleToFixedS16, 0xc0dfffe000000000ull, 0, 3, 0xffff8001u, 0x10}, // -32767.5
        {Op::FPDoubleToFixedS32, 0x3d70000000000000ull, 0, 1, 1, 0x10}, // 2^-40
        {Op::FPDoubleToFixedS32, 0xbd70000000000000ull, 0, 2, 0xffffffffu, 0x10},
        // 2^32 - 0.5 rounds up to 2^32: the 32-bit magnitude carries out.
        {Op::FPDoubleToFixedU32, 0x41effffffff00000ull, 0, 0, 0xffffffffu, 1},
        {Op::FPDoubleToFixedU32, 0x41effffffff00000ull, 0, 1, 0xffffffffu, 1},
        {Op::FPDoubleToFixedU32, 0x41effffffff00000ull, 0, 2, 0xffffffffu, 0x10},
        {Op::FPDoubleToFixedS32, 0x41effffffff00000ull, 0, 4, 0x7fffffffu, 1},
        // Towards zero into range from just below -2^31 (ARM ARM, not
        // Dynarmic's IOC: SignedIntoRange).
        {Op::FPDoubleToFixedS32, 0xc1dfffffffe00000ull, 0, 3, 0x80000001u, 0x10}, // -(2^31 - 0.5)
        {Op::FPDoubleToFixedS32, 0xc1dfffffffe00000ull, 0, 1, 0x80000001u, 0x10},
        {Op::FPDoubleToFixedS32, 0xc1dfffffffe00000ull, 0, 2, 0x80000000u, 0x10},
        {Op::FPDoubleToFixedS32, 0xc1dfffffffe00000ull, 0, 4, 0x80000000u, 0x10},
        {Op::FPDoubleToFixedS32, 0xc1dfffffffe00000ull, 0, 0, 0x80000000u, 0x10}, // tie to even
        {Op::FPDoubleToFixedS32, 0xc1dfffffffd00000ull, 0, 0, 0x80000001u, 0x10}, // -(2^31 - 0.75)
    };
    for (const auto &c : pinned) {
        const bool is_double = c.op == Op::FPDoubleToFixedS32 || c.op == Op::FPDoubleToFixedU32
            || c.op == Op::FPDoubleToFixedS16 || c.op == Op::FPDoubleToFixedU16;
        auto block = blank(0);
        const auto input = is_double ? read_d(block) : read_s(block);
        auto result = append(block, c.op, {input, Value{c.fbits}, Value{c.rounding}});
        if (c.op == Op::FPSingleToFixedS16 || c.op == Op::FPDoubleToFixedS16)
            result = append(block, Op::SignExtendHalfToWord, {result});
        else if (c.op == Op::FPSingleToFixedU16 || c.op == Op::FPDoubleToFixedU16)
            result = append(block, Op::ZeroExtendHalfToWord, {result});
        write_s(block, result);
        auto state = base_state(0x9000001cu), expected = state;
        if (is_double) set_d_bits(state, c.input);
        else set_s_bits(state, uint32_t(c.input));
        expected = state;
        set_s_bits(expected, c.result);
        expected.fpscr |= c.flags;
        run(block, state, &expected);
    }
    CHECK(divergences[Divergence::SignedIntoRange] != 0 && divergences[Divergence::UnsignedRoundsToZero] != 0);
    std::printf("fixed-point VCVT: %u ARM ARM FPToFixed cases (S/U 32/16, fbits 0..32, five rounding modes, FZ, "
                "block+region; Dynarmic agrees on %u, diverges on %u SignedIntoRange and %u UnsignedRoundsToZero) "
                "and pinned cases passed\n", count, divergences[Divergence::None],
        divergences[Divergence::SignedIntoRange], divergences[Divergence::UnsignedRoundsToZero]);
}

// Real fixed-point VCVT encodings (ARM and Thumb) through the translator.
void guest_float_to_fixed(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    struct Case { uint32_t opcode; bool is_double; uint64_t input; uint32_t result; uint32_t flags; };
    const Case cases[] = {
        {0xeefe7ac8u, false, 0x3fc00000, 0x00018000u, 0}, // VCVT.S32.F32 s15, s15, #16
        {0xeefe7ac8u, false, 0xc7000001, 0x80000000u, 1}, // below -2^15 saturates
        {0xeefe7ac8u, false, 0x37c00000, 1, 0x10}, // truncates 1.5 units
        {0xeeff7ac8u, false, 0x47000000, 0x80000000u, 0}, // VCVT.U32.F32 #16
        {0xeefe7aefu, false, 0x3fc00000, 3, 0}, // VCVT.S32.F32 #1
        {0xeebe7bc8u, true, 0xbff8000000000000ull, 0xfffe8000u, 0}, // VCVT.S32.F64 d7, #16
        {0xeebf7bc0u, true, 0x3fe0000000000000ull, 0x80000000u, 0}, // VCVT.U32.F64 d7, #32
        {0xeefe7a44u, false, 0x3fc00000, 0x00000180u, 0}, // VCVT.S16.F32 s15, s15, #8
        {0xeefe7a44u, false, 0xc3000000, 0xffff8000u, 0}, // -128 * 2^8 is the minimum
        {0xeefe7a44u, false, 0x43000000, 0x00007fffu, 1}, // 128 * 2^8 saturates
        {0xeeff7a44u, false, 0xbb000000, 0, 0x10}, // VCVT.U16.F32: -2^-9 * 2^8 truncates to 0
        {0xeeff7a44u, false, 0x43800000, 0x0000ffffu, 1}, // 256 * 2^8 saturates
        {0xeebe7b44u, true, 0xbff8000000000000ull, 0xfffffe80u, 0}, // VCVT.S16.F64 d7, #8
        {0xeebf7b44u, true, 0x3ff8000000000000ull, 0x00000180u, 0}, // VCVT.U16.F64 d7, #8
    };
    for (bool regions : {false, true}) for (bool thumb : {false, true}) for (const auto &c : cases) {
        jit.set_region_mode(regions);
        if (thumb) put_thumb(mem, jit, {(c.opcode << 16) | (c.opcode >> 16), 0xbf00df00u});
        else put(mem, jit, {c.opcode, 0xef000000u});
        auto before = jit.save_context();
        if (c.is_double) std::memcpy(&before.fpu_registers[14], &c.input, 8);
        else {
            const uint32_t bits = uint32_t(c.input);
            std::memcpy(&before.fpu_registers[15], &bits, 4);
        }
        before.fpscr = 0x90000080u;
        jit.load_context(before);
        CHECK(jit.run() == 0 && parent.svc_called);
        const auto after = jit.save_context();
        uint32_t result;
        // The 32-bit result lands in the low word of the (s15 / d7) source register.
        std::memcpy(&result, &after.fpu_registers[c.is_double ? 14 : 15], 4);
        CHECK(result == c.result);
        if (c.is_double) {
            uint32_t high;
            std::memcpy(&high, &after.fpu_registers[15], 4);
            const bool is_unsigned = (c.opcode & 0x10000u) != 0;
            CHECK(high == (is_unsigned || !(c.result >> 31) ? 0u : 0xffffffffu)); // extended to 64 bits
        }
        CHECK(after.fpscr == (before.fpscr | c.flags));
    }
    std::puts("fixed-point VCVT guest: ARM/Thumb single+region passed");
}
} // namespace f64_tests
