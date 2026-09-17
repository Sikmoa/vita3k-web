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
            CHECK(state.fpscr == expected->fpscr);
        }
        if (region) vita3k_jit_release_region(slot);
        else vita3k_jit_release(slot);
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
    for (Op opcode : {Op::FPSingleToDouble, Op::FPDoubleToSingle, Op::FPCompare64}) {
        auto block = blank(0);
        if (opcode == Op::FPSingleToDouble)
            write_d(block, append(block, opcode, {read_s(block), Value{uint8_t(0)}}));
        else if (opcode == Op::FPDoubleToSingle)
            write_s(block, append(block, opcode, {read_d(block), Value{uint8_t(0)}}));
        else
            append(block, Op::A32SetFpscrNZCV, {append(block, opcode,
                {read_d(block), read_d(block, 1), Value{false}})});
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
} // namespace f64_tests
