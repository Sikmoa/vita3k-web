// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
// Included by wasmjit_backend_test.cpp after its shared CHECK/put helpers.
// VCEQ/VCGT/VCGE.f32: independent float-comparison oracle versus integer-key
// Wasm lowering. A32 StandardFPSCRValue is RN/FZ=DN=1, NOT FPCR{0}.
// All NaNs compare false. GT/GE signal IOC for any NaN; EQ only for sNaNs.
namespace vectorfp_compare_tests {
namespace A32 = Dynarmic::A32;
namespace IR = Dynarmic::IR;
using Op = IR::Opcode;
using Value = IR::Value;
using Reason = vita3k::wasmjit::ExitReason;
using vector_tests::append;
using vector_tests::read_q;
using vector_tests::write_q;

constexpr std::array operations{Op::FPVectorEqual32, Op::FPVectorGreater32, Op::FPVectorGreaterEqual32};
struct Mode {
    bool region;
    vita3k::wasmjit::RegionStateOptions options;
    const char *name;
};
constexpr Mode modes[] = {
    {false, {}, "block"}, {true, {false, false}, "A"},
    {true, {true, false}, "P"}, {true, {false, true}, "K"},
    {true, {true, true}, "PK"},
};

IR::Block blank(uint32_t fpscr = 0, uint32_t pc = code, uint32_t cpsr = 0x10) {
    const A32::LocationDescriptor here{pc, A32::PSR{cpsr}, A32::FPSCR{fpscr}};
    const auto next = here.SetPC(pc + 4);
    IR::Block block{here};
    block.SetEndLocation(next);
    block.SetTerminal(IR::Term::LinkBlock{next});
    block.CycleCount() = 1;
    return block;
}
std::vector<uint8_t> emit(const IR::Block &block, Mode mode) {
    const A32::LocationDescriptor at{block.Location()};
    return mode.region
        ? vita3k::wasmjit::emit_region({&block}, {{at.PC(), PSR_DISPATCH_MASK,
            at.CPSR().Value() & PSR_DISPATCH_MASK, 1}}, mode.options)
        : vita3k::wasmjit::emit_block(block);
}
struct Compiled {
    Mode mode;
    int slot;
    Compiled(const std::vector<uint8_t> &bytes, Mode mode) : mode(mode) {
        CHECK(!bytes.empty());
        slot = mode.region
            ? vita3k_jit_install_region(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write)
            : vita3k_jit_install(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write);
        CHECK(slot >= 0);
    }
    Compiled(const IR::Block &block, Mode mode) : Compiled(emit(block, mode), mode) {}
    ~Compiled() {
        if (mode.region) vita3k_jit_release_region(slot);
        else vita3k_jit_release(slot);
    }
    Compiled(const Compiled &) = delete;
    Compiled &operator=(const Compiled &) = delete;
    Reason call(JitState &state, uint32_t budget = 1) const {
        return static_cast<Reason>(mode.region
            ? vita3k_jit_run(slot, reinterpret_cast<uintptr_t>(&state), budget)
            : vita3k_jit_call(slot, reinterpret_cast<uintptr_t>(&state)));
    }
};
JitState initial(uint32_t fpscr = 0xa8000014) {
    JitState state{};
    for (unsigned r = 0; r < 15; ++r) state.regs[r] = 0xabc00000 + r;
    for (unsigned i = 0; i < 64; ++i) state.fpu[i] = 0x3f010203u + 0x10001u * i;
    for (unsigned i = 0; i < 4; ++i) state.memory_value[i] = 0xdead0000u + i;
    state.regs[15] = code;
    state.cpsr = 0xa80f0010; // NZCV, Q and GE must survive.
    state.fpscr = fpscr; // NZCV/QC and existing exception flags must survive.
    return state;
}
void architecture(const JitState &actual, const JitState &expected) {
    for (unsigned r = 0; r < 16; ++r) CHECK(actual.regs[r] == expected.regs[r]);
    for (unsigned i = 0; i < 64; ++i) {
        if (actual.fpu[i] != expected.fpu[i])
            std::fprintf(stderr, "vector FP compare word=%u got=%08x want=%08x a=%08x,%08x,%08x,%08x b=%08x,%08x,%08x,%08x fpscr=%08x/%08x\n",
                i, actual.fpu[i], expected.fpu[i], actual.fpu[0], actual.fpu[1], actual.fpu[2], actual.fpu[3],
                actual.fpu[4], actual.fpu[5], actual.fpu[6], actual.fpu[7], actual.fpscr, expected.fpscr);
        CHECK(actual.fpu[i] == expected.fpu[i]);
    }
    CHECK(actual.cpsr == expected.cpsr);
    CHECK(actual.fpscr == expected.fpscr);
    for (unsigned i = 0; i < 4; ++i) CHECK(actual.memory_value[i] == expected.memory_value[i]);
}
void verify(const Compiled &compiled, const JitState &input, JitState expected, unsigned ticks = 1) {
    auto state = input;
    CHECK(compiled.call(state, ticks) == (compiled.mode.region ? Reason::Miss : Reason::Continue));
    expected.regs[15] += 4 * ticks;
    architecture(state, expected);
    CHECK(state.executed == ticks);
}

// The oracle uses host FP ordering, not the emitter's bit-pattern sort keys.
// Classify/flush in integer form first so signaling-NaN bits cannot be lost.
uint32_t oracle(Op operation, uint32_t a, uint32_t b, uint32_t &flags) {
    flags = 0;
    for (uint32_t *operand : {&a, &b}) {
        const uint32_t magnitude = *operand & 0x7fffffffu;
        if (magnitude != 0 && magnitude < 0x00800000u) {
            *operand &= 0x80000000u;
            flags |= 0x80;
        }
    }
    const bool nan_a = (a & 0x7fffffffu) > 0x7f800000u;
    const bool nan_b = (b & 0x7fffffffu) > 0x7f800000u;
    if (nan_a || nan_b) {
        if (operation != Op::FPVectorEqual32 || (nan_a && !(a & 0x00400000u))
            || (nan_b && !(b & 0x00400000u)))
            flags |= 1;
        return 0;
    }
    float fa, fb;
    std::memcpy(&fa, &a, sizeof(a));
    std::memcpy(&fb, &b, sizeof(b));
    const bool result = operation == Op::FPVectorEqual32 ? fa == fb
        : operation == Op::FPVectorGreater32 ? fa > fb : fa >= fb;
    return result ? UINT32_MAX : 0;
}
void expected_vector(JitState &state, Op operation, unsigned dst = 8, unsigned a = 0, unsigned b = 4, unsigned words = 4) {
    const auto input = state;
    for (unsigned i = 0; i < words; ++i) {
        uint32_t flags;
        state.fpu[dst + i] = oracle(operation, input.fpu[a + i], input.fpu[b + i], flags);
        state.fpscr |= flags;
    }
}
IR::Block comparison(Op operation, uint32_t fpscr = 0) {
    auto block = blank(fpscr);
    write_q(block, append(block, operation, {read_q(block, 0), read_q(block, 1), Value{false}}), 2);
    return block;
}

void hand_cases() {
    struct Case { uint32_t a, b; bool eq, gt, ge; uint32_t eq_flags, ordered_flags; };
    const Case cases[] = {
        {0x00000000, 0x80000000, true, false, true, 0, 0},
        {0x80000000, 0x00000000, true, false, true, 0, 0},
        {0x00000001, 0x00000000, true, false, true, 0x80, 0x80},
        {0x807fffff, 0x007fffff, true, false, true, 0x80, 0x80},
        {0x00800000, 0x007fffff, false, true, true, 0x80, 0x80},
        {0x80800000, 0x80000001, false, false, false, 0x80, 0x80},
        {0x3f800000, 0xbf800000, false, true, true, 0, 0},
        {0xbf800000, 0xc0000000, false, true, true, 0, 0},
        {0x3f800000, 0x3f800001, false, false, false, 0, 0},
        {0x7f800000, 0x7f800000, true, false, true, 0, 0},
        {0xff800000, 0xff800000, true, false, true, 0, 0},
        {0x7f800000, 0xff800000, false, true, true, 0, 0},
        {0x7fc00001, 0x3f800000, false, false, false, 0, 1},
        {0x3f800000, 0xffc12345, false, false, false, 0, 1},
        {0x7fc00001, 0x7fc00001, false, false, false, 0, 1},
        {0x7f800001, 0x3f800000, false, false, false, 1, 1},
        {0x3f800000, 0xffbfffff, false, false, false, 1, 1},
        {0x7fc00001, 0xff800001, false, false, false, 1, 1},
        {0x00000001, 0x7fc00000, false, false, false, 0x80, 0x81},
        {0x7f800001, 0x80000001, false, false, false, 0x81, 0x81},
    };
    for (Op operation : operations) {
        auto block = comparison(operation);
        for (Mode mode : modes) {
            Compiled compiled(block, mode);
            for (const auto &item : cases) for (unsigned lane = 0; lane < 4; ++lane) {
                // Only one exceptional lane: every lane must raise flags,
                // while later ordinary lanes must not inherit its NaN mask.
                auto state = initial();
                for (unsigned i = 0; i < 4; ++i) state.fpu[i] = state.fpu[4 + i] = 0x3f800000;
                state.fpu[lane] = item.a;
                state.fpu[4 + lane] = item.b;
                auto expected = state;
                const bool predicate = operation == Op::FPVectorEqual32 ? item.eq
                    : operation == Op::FPVectorGreater32 ? item.gt : item.ge;
                for (unsigned i = 0; i < 4; ++i)
                    expected.fpu[8 + i] = (i == lane ? predicate : operation != Op::FPVectorGreater32) ? UINT32_MAX : 0;
                expected.fpscr |= operation == Op::FPVectorEqual32 ? item.eq_flags : item.ordered_flags;
                verify(compiled, state, expected);
            }
        }
    }
    std::puts("vector FP compare: pinned NaN, FZ, signed-zero and sticky-flag contracts passed");
}

void differential() {
    struct Case { std::array<uint32_t, 4> a, b; };
    std::vector<Case> cases;
    const uint32_t edges[] = {
        0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x807fffff,
        0x00800000, 0x80800000, 0x00800001, 0x80800001,
        0x3f7fffff, 0x3f800000, 0x3f800001, 0xbf7fffff, 0xbf800000, 0xbf800001,
        0x4f000000, 0xcf000000, 0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000,
        0x7fc00000, 0xffc00000, 0x7fffffff, 0xffffffff,
        0x7f800001, 0xff800001, 0x7fbfffff, 0xffbfffff,
    };
    for (uint32_t a : edges) for (uint32_t b : edges)
        cases.push_back({{a, b, a, b}, {b, a, a, b}});
    for (uint32_t exponent = 0; exponent < 256; ++exponent) {
        for (uint32_t fraction : {0u, 1u, 0x3fffffu, 0x400000u, 0x400001u, 0x7fffffu}) {
            const uint32_t a = exponent << 23 | fraction;
            cases.push_back({{a, a | 0x80000000u, a, a + 1},
                {a + 1, (a + 1) | 0x80000000u, a | 0x80000000u, a}});
        }
    }
    uint32_t rng = 0xd31f2501;
    const auto random = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
    for (unsigned iteration = 0; iteration < 4096; ++iteration) {
        Case item;
        for (unsigned lane = 0; lane < 4; ++lane) { item.a[lane] = random(); item.b[lane] = random(); }
        cases.push_back(item);
    }
    for (Op operation : operations) {
        auto block = comparison(operation);
        for (Mode mode : modes) {
            // Compile once, execute many inputs: exercises stale-local and
            // cumulative-state bugs without thousands of redundant modules.
            Compiled compiled(block, mode);
            for (const auto &item : cases) {
                auto state = initial();
                std::copy(item.a.begin(), item.a.end(), state.fpu);
                std::copy(item.b.begin(), item.b.end(), state.fpu + 4);
                auto expected = state;
                expected_vector(expected, operation);
                auto actual = state;
                CHECK(compiled.call(actual) == (mode.region ? Reason::Miss : Reason::Continue));
                if (actual.fpscr != expected.fpscr || std::memcmp(actual.fpu + 8, expected.fpu + 8, 16))
                    std::fprintf(stderr, "vector FP compare %s %s a0=%08x b0=%08x got=%08x/%08x want=%08x/%08x\n",
                        IR::GetNameOf(operation).c_str(), mode.name, item.a[0], item.b[0],
                        actual.fpu[8], actual.fpscr, expected.fpu[8], expected.fpscr);
                expected.regs[15] += 4;
                architecture(actual, expected);
                CHECK(actual.executed == 1);
            }
        }
    }
    std::printf("vector FP compare: %zu four-lane cases x 3 ops x (block+A/P/K/PK) passed\n", cases.size());
}

void guards() {
    for (Op operation : operations) {
        for (bool dynamic : {false, true}) {
            auto block = blank();
            const auto control = dynamic ? append(block, Op::A32GetCFlag) : Value{true};
            write_q(block, append(block, operation, {read_q(block), read_q(block, 1), control}), 2);
            for (Mode mode : modes) CHECK(emit(block, mode).empty());
        }
        auto block = comparison(operation);
        for (Mode mode : modes) {
            Compiled compiled(block, mode);
            for (uint32_t trap : {0x100u, 0x200u, 0x400u, 0x800u, 0x1000u, 0x8000u, 0x9f00u}) {
                auto state = initial(0xa8000014u | trap);
                state.fpu[0] = 0x7f800001;
                state.fpu[4] = 1;
                const auto before = state;
                CHECK(compiled.call(state) == Reason::Unsupported);
                architecture(state, before);
                CHECK(state.executed == 0);
            }
            for (unsigned rounding = 0; rounding < 4; ++rounding) for (unsigned fzdn = 0; fzdn < 4; ++fzdn) {
                const uint32_t scr = (rounding << 22) | (fzdn << 24);
                auto state = initial(0xa8000014u | scr);
                state.fpu[0] = 1; state.fpu[4] = 0;
                state.fpu[1] = 0xffc12345; state.fpu[5] = 0x3f800000;
                state.fpu[2] = 0x7f800001; state.fpu[6] = 0x80000001;
                auto expected = state;
                expected_vector(expected, operation);
                // A matching nondefault location must RUN in region mode;
                // otherwise the fixture would only test clean dispatch Miss.
                auto matching = comparison(operation, scr);
                Compiled matched(matching, mode);
                verify(matched, state, expected);
                if (mode.region && scr) {
                    const auto before = state;
                    CHECK(compiled.call(state) == Reason::Miss);
                    architecture(state, before);
                    CHECK(state.executed == 0);
                } else {
                    // Block emission itself cannot depend on live mode bits.
                    verify(compiled, state, expected);
                }
            }
        }
    }
    for (Op operation : {Op::FPVectorEqual16, Op::FPVectorEqual64, Op::FPVectorGreater64, Op::FPVectorGreaterEqual64}) {
        auto block = comparison(operation);
        for (Mode mode : modes) CHECK(emit(block, mode).empty());
    }
    std::puts("vector FP compare: immediate, live-trap and standard-FPSCR mode guards passed");
}

void composition() {
    for (Mode mode : modes) {
        auto block = blank();
        const auto a = read_q(block), b = read_q(block, 1);
        write_q(block, append(block, Op::FPVectorEqual32, {a, b, Value{false}}));
        write_q(block, append(block, Op::FPVectorGreater32, {a, b, Value{false}}), 2);
        write_q(block, append(block, Op::FPVectorGreaterEqual32, {a, b, Value{false}}), 1);
        auto state = initial();
        state.fpu[0] = 0x7f800001; state.fpu[4] = 1;
        state.fpu[1] = 0x40000000; state.fpu[5] = 0xbf800000;
        state.fpu[2] = 0x80000000; state.fpu[6] = 0;
        state.fpu[3] = 0xff800000; state.fpu[7] = 0x7f800000;
        auto expected = state;
        for (unsigned op = 0; op < operations.size(); ++op) for (unsigned lane = 0; lane < 4; ++lane) {
            uint32_t flags;
            const unsigned dst = op == 0 ? 0 : op == 1 ? 8 : 4;
            expected.fpu[dst + lane] = oracle(operations[op], state.fpu[lane], state.fpu[4 + lane], flags);
            expected.fpscr |= flags;
        }
        Compiled compiled(block, mode);
        verify(compiled, state, expected);

        // Constant-vector producers exercise both packed operand words.
        auto constants = blank();
        const auto packed = append(constants, Op::Pack2x64To1x128,
            {Value{uint64_t(0x8000000100000001)}, Value{uint64_t(0x7f8000017fc00000)}});
        const auto zero = append(constants, Op::Pack2x64To1x128,
            {Value{uint64_t(0)}, Value{uint64_t(0)}});
        write_q(constants, append(constants, Op::FPVectorEqual32, {packed, zero, Value{false}}), 2);
        state = initial(); expected = state;
        expected.fpu[8] = expected.fpu[9] = UINT32_MAX;
        expected.fpu[10] = expected.fpu[11] = 0;
        expected.fpscr |= 0x81;
        Compiled constant_compiled(constants, mode);
        verify(constant_compiled, state, expected);

        if (!mode.region) continue;
        auto first = comparison(Op::FPVectorEqual32);
        auto second = blank(0, code + 4);
        write_q(second, append(second, Op::FPVectorGreaterEqual32,
            {read_q(second, 3), read_q(second, 4), Value{false}}), 5);
        const auto bytes = vita3k::wasmjit::emit_region({&first, &second},
            {{code, PSR_DISPATCH_MASK, 0, 1}, {code + 4, PSR_DISPATCH_MASK, 0, 1}}, mode.options);
        Compiled chain(bytes, mode);
        state = initial();
        state.fpu[0] = 0x7f800001; state.fpu[4] = 1;
        for (unsigned i = 0; i < 4; ++i) state.fpu[12 + i] = state.fpu[16 + i] = 0;
        expected = state;
        expected_vector(expected, Op::FPVectorEqual32);
        expected_vector(expected, Op::FPVectorGreaterEqual32, 20, 12, 16);
        verify(chain, state, expected, 2);
    }
    std::puts("vector FP compare: SSA aliases, constant inputs and cross-member local reuse passed");
}

void guest_comparisons(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    struct Guest { uint32_t word; Op op; unsigned d, n, m, lanes; bool zero, swapped; };
    // Encodings independently assembled from jit_vector_fp_compare_encodings.S.
    const Guest cases[] = {
        {0xf3250e01, Op::FPVectorGreater32, 0, 5, 1, 2, false, false}, // Limbo PC 811b006c
        {0xf3050e01, Op::FPVectorGreaterEqual32, 0, 5, 1, 2, false, false},
        {0xf2050e01, Op::FPVectorEqual32, 0, 5, 1, 2, false, false},
        {0xf36eeec2, Op::FPVectorGreater32, 30, 30, 2, 4, false, false},
        {0xf342ee6e, Op::FPVectorGreaterEqual32, 30, 2, 30, 4, false, false},
        {0xf24eeeee, Op::FPVectorEqual32, 30, 30, 30, 4, false, false},
        {0xf3f9f42f, Op::FPVectorGreater32, 31, 31, 0, 2, true, false},
        {0xf3f9f4af, Op::FPVectorGreaterEqual32, 31, 31, 0, 2, true, false},
        {0xf3f9f52f, Op::FPVectorEqual32, 31, 31, 0, 2, true, false},
        {0xf3f9f5af, Op::FPVectorGreaterEqual32, 31, 31, 0, 2, true, true},
        {0xf3f9f62f, Op::FPVectorGreater32, 31, 31, 0, 2, true, true},
    };
    const uint32_t inputs[] = {0, 0x80000000, 1, 0x80000001, 0x00800000, 0x80800000,
        0x3f800000, 0xbf800000, 0x7f800000, 0xff800000, 0x7fc12345, 0xff800001};
    CHECK(vector_tests::thumb_word(cases[0].word) == 0x0e01ff25);
    for (bool region : {false, true}) for (bool thumb : {false, true}) {
        jit.set_region_mode(region);
        for (const auto &item : cases) for (unsigned input = 0; input < std::size(inputs); ++input) {
            if (thumb) put_thumb(mem, jit, {vector_tests::thumb_word(item.word), 0xbf00df00});
            else put(mem, jit, {item.word, 0xef000000});
            auto state = initial(0xabc00014); // non-RN live FPSCR, FZ=DN=1
            // Fill non-operands with sNaNs: D-form upper lanes must be zero,
            // not the neighbouring D register's values or stale Q lanes.
            std::fill_n(state.fpu, 64, 0x7f800001);
            for (unsigned lane = 0; lane < item.lanes; ++lane) {
                state.fpu[2 * item.n + lane] = inputs[(input + lane) % std::size(inputs)];
                if (!item.zero && item.m != item.n)
                    state.fpu[2 * item.m + lane] = inputs[(input + lane + 3) % std::size(inputs)];
            }
            auto context = jit.save_context();
            std::copy_n(state.regs, 15, context.cpu_registers.begin());
            std::memcpy(context.fpu_registers.data(), state.fpu, sizeof(state.fpu));
            context.cpsr = state.cpsr | (thumb ? 0x20u : 0);
            context.fpscr = state.fpscr;
            jit.load_context(context);
            auto expected = state;
            for (unsigned lane = 0; lane < item.lanes; ++lane) {
                uint32_t flags;
                const uint32_t a = state.fpu[2 * item.n + lane];
                const uint32_t b = item.zero ? 0 : state.fpu[2 * item.m + lane];
                expected.fpu[2 * item.d + lane] = oracle(item.op, item.swapped ? b : a, item.swapped ? a : b, flags);
                expected.fpscr |= flags;
            }
            parent.svc_called = false;
            if (jit.run() != 0) {
                std::fprintf(stderr, "vector FP compare guest %08x (%s): %s\n", item.word,
                    thumb ? "Thumb" : "ARM", jit.get_last_error().c_str());
                CHECK(false);
            }
            CHECK(parent.svc_called);
            const auto result = jit.save_context();
            CHECK(std::memcmp(result.fpu_registers.data(), expected.fpu, sizeof(expected.fpu)) == 0);
            for (unsigned r = 0; r < 15; ++r) CHECK(result.cpu_registers[r] == expected.regs[r]);
            CHECK(result.fpscr == expected.fpscr);
            CHECK(result.cpsr == context.cpsr);
            CHECK(jit.get_pc() == code + (thumb ? 6 : 8));
        }
    }
    std::puts("vector FP compare: Limbo VCGT encoding, D/Q aliases and zero LT/LE (ARM+Thumb) passed");
}

inline void run(MemState &mem) {
    hand_cases();
    differential();
    guards();
    composition();
    guest_comparisons(mem);
}
} // namespace vectorfp_compare_tests
