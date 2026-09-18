// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
// Included by wasmjit_backend_test.cpp after its shared CHECK/put helpers.
// Vector RECPE/VRECPS (FPVectorRecipEstimate32/FPVectorRecipStepFused32)
// fixtures. The native helper delegates to the vendored Dynarmic FP
// implementation, so the differential oracle here IS that same library
// called directly: the fixtures therefore prove the wiring (lane packing,
// helper dispatch, FPSCR flag OR-in, register writeback) rather than
// re-deriving the algorithm. Hand-pinned special cases independently check
// input flushing, default NaNs, underflow and newly raised flags, so using
// the same library for the differential cannot hide a wrong control value.
//
// The helper executes under ASIMDStandardValue() (RN, FZ=1, DN=1) per the
// A32 translator's fpcr_controlled=false call sites. Mode fixtures prove
// live RMode/FZ/DN cannot perturb results, including matching region keys.
namespace recip_tests {
namespace A32 = Dynarmic::A32;
namespace IR = Dynarmic::IR;
using Op = IR::Opcode;
using Value = IR::Value;
using Reason = vita3k::wasmjit::ExitReason;

IR::Block blank(uint32_t pc = code, uint32_t fpscr = 0) {
    const A32::LocationDescriptor here{pc, A32::PSR{0x10}, A32::FPSCR{fpscr}};
    const A32::LocationDescriptor next{pc + 4, A32::PSR{0x10}, A32::FPSCR{fpscr}};
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
Value read_q(IR::Block &block, unsigned q = 0) {
    return append(block, Op::A32GetVector, {Value{A32::ExtReg::Q0 + q}});
}
void write_q(IR::Block &block, Value value, unsigned q = 0) {
    append(block, Op::A32SetVector, {Value{A32::ExtReg::Q0 + q}, value});
}

JitState base_state() {
    JitState state{};
    for (unsigned r = 0; r < 15; ++r) state.regs[r] = 0xabc00000 + r;
    state.regs[15] = code;
    state.cpsr = 0x10;
    return state;
}

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
                    std::fprintf(stderr, "recip region=%d word=%u got=%08x expected=%08x q0=%08x q2=%08x\n",
                        region, i, state.fpu[i], expected->fpu[i], initial.fpu[0], initial.fpu[8]);
                CHECK(state.fpu[i] == expected->fpu[i]);
            }
            for (unsigned i = 0; i < 15; ++i) CHECK(state.regs[i] == expected->regs[i]);
            CHECK(state.fpscr == expected->fpscr);
        }
        if (region) vita3k_jit_release_region(slot);
        else vita3k_jit_release(slot);
    }
}

// Independent standard-FPSCR expectations: in particular, denormals are
// zeros BEFORE estimating (IDC+DZC, not overflow), and DN discards NaN signs
// and payloads. Check both the helper and the emitted block/region wiring.
void hand_cases() {
    struct Case { uint32_t operation, a, b, result, flags; };
    const Case cases[] = {
        {4, 0x00000000u, 0, 0x7f800000u, 0x02u}, // +0 -> +inf, DZC
        {4, 0x80000000u, 0, 0xff800000u, 0x02u}, // -0 -> -inf, DZC
        {4, 0x00000001u, 0, 0x7f800000u, 0x82u}, // FZ: +denormal -> +0, IDC+DZC
        {4, 0x80000001u, 0, 0xff800000u, 0x82u}, // FZ preserves the input sign
        {4, 0x007fffffu, 0, 0x7f800000u, 0x82u}, // largest denormal also flushes
        {4, 0x807fffffu, 0, 0xff800000u, 0x82u},
        {4, 0x7f800000u, 0, 0x00000000u, 0x00u}, // +inf -> +0, no UFC/DZC
        {4, 0xff800000u, 0, 0x80000000u, 0x00u}, // -inf -> -0
        {4, 0x7fc12345u, 0, 0x7fc00000u, 0x00u}, // DN qNaN, no IOC
        {4, 0xffc12345u, 0, 0x7fc00000u, 0x00u}, // default NaN is positive
        {4, 0x7f812345u, 0, 0x7fc00000u, 0x01u}, // DN sNaN still raises IOC
        {4, 0xff812345u, 0, 0x7fc00000u, 0x01u},
        {4, 0x3f800000u, 0, 0x3f7f8000u, 0x00u}, // RECPE(1): 511/512, no IXC
        {4, 0x00800000u, 0, 0x7e7f8000u, 0x00u}, // min-normal is NOT flushed
        {4, 0x7e800000u, 0, 0x00000000u, 0x08u}, // tiny estimate flushes, UFC only
        {4, 0xff7fffffu, 0, 0x80000000u, 0x08u}, // negative tiny estimate -> -0
        {5, 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x00u}, // 2 - 1*1 = 1
        {5, 0x3f800000u, 0x40000000u, 0x00000000u, 0x00u}, // exact cancellation is +0 (RN)
        {5, 0x00000000u, 0x7f800000u, 0x40000000u, 0x00u}, // special 0*inf -> 2, no IOC
        {5, 0x80000001u, 0x7f800000u, 0x40000000u, 0x80u}, // flushed 0*inf -> 2, IDC only
        {5, 0xff800000u, 0x007fffffu, 0x40000000u, 0x80u}, // either operand flushes
        {5, 0x00000001u, 0x3f800000u, 0x40000000u, 0x80u},
        {5, 0xffc12345u, 0x3f800000u, 0x7fc00000u, 0x00u}, // DN also covers negated a
        {5, 0x3f800000u, 0xff812345u, 0x7fc00000u, 0x01u}, // signaling b -> IOC
        {5, 0x7fc12345u, 0x00000001u, 0x7fc00000u, 0x80u}, // unpack BOTH before NaNs
        {5, 0x80000001u, 0x7f812345u, 0x7fc00000u, 0x81u}, // IDC survives IOC
        {5, 0x7f800000u, 0x3f800000u, 0xff800000u, 0x00u}, // infinite result, no OFC/IXC
        {5, 0x7f7fffffu, 0x40000000u, 0xff800000u, 0x14u}, // finite overflow, OFC+IXC
        {5, 0x00800000u, 0x00800000u, 0x40000000u, 0x10u}, // tiny PRODUCT, normal fused result: IXC, not UFC
        {5, 0x3f800001u, 0x3ffffffeu, 0x29000000u, 0x00u}, // exact fused residual 2^-45
    };
    for (const auto &item : cases) {
        for (uint32_t mode = 0; mode < 16; ++mode) {
            const auto result = vita3k::wasmjit::fp64_arithmetic(
                item.operation, item.a, item.b, (mode << 22) | 0xa8000004u);
            if (result.bits != item.result || result.flags != item.flags)
                std::fprintf(stderr, "recip hand op=%u mode=%x a=%08x b=%08x got=%08x/%02x want=%08x/%02x\n",
                    item.operation, mode, item.a, item.b, uint32_t(result.bits), result.flags, item.result, item.flags);
            CHECK(result.bits == item.result);
            CHECK(result.flags == item.flags); // never return preexisting flags/mode bits
        }
        for (uint32_t scr : {0u, 0x03c00000u}) {
            auto block = blank(code, scr);
            const auto a = read_q(block);
            const auto result = item.operation == 4
                ? append(block, Op::FPVectorRecipEstimate32, {a, Value{false}})
                : append(block, Op::FPVectorRecipStepFused32, {a, read_q(block, 2), Value{false}});
            write_q(block, result, 1);
            auto state = base_state();
            state.fpscr = scr | 0xa8000004u; // preserve NZCV/QC and a preexisting OFC
            for (unsigned lane = 0; lane < 4; ++lane) {
                state.fpu[lane] = item.a;
                state.fpu[8 + lane] = item.b;
            }
            auto expected = state;
            for (unsigned lane = 0; lane < 4; ++lane) expected.fpu[4 + lane] = item.result;
            expected.fpscr |= item.flags;
            run(block, state, &expected); // matching non-RN region keys must execute too
        }
    }
    std::puts("recip hand cases: standard FZ/DN, fused results, flags and mode independence passed");
}

// Direct helper differential: sampled bit patterns, one lane at a time,
// against the same Dynarmic entry points the helper calls. This proves the
// FP64Result packing (bits + newly-raised flags only), not the algorithm.
void helper_differential() {
    using vita3k::wasmjit::fp64_arithmetic;
    uint32_t rng = 0x9e3779b9;
    for (uint64_t iteration = 0; iteration < 400000; ++iteration) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        const uint32_t lane_a = iteration < 8 ? uint32_t(0xff000000 * iteration) : rng;
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        const uint32_t lane_b = iteration < 8 ? uint32_t(0x00800000 * iteration) : rng;
        for (uint32_t operation : {4u, 5u}) {
            const auto result = fp64_arithmetic(operation, lane_a, lane_b, 0);
            const auto fpcr = Dynarmic::FP::FPCR{0}.ASIMDStandardValue();
            Dynarmic::FP::FPSR fpsr{0};
            const uint32_t oracle = operation == 4
                ? Dynarmic::FP::FPRecipEstimate<uint32_t>(lane_a, fpcr, fpsr)
                : Dynarmic::FP::FPRecipStepFused<uint32_t>(lane_a, lane_b, fpcr, fpsr);
            const uint32_t oracle_flags = fpsr.Value() & 0x9f;
            if (result.bits != oracle || result.flags != oracle_flags) {
                std::fprintf(stderr, "recip differential op=%u a=%08x b=%08x bits=%08x/%08x flags=%02x/%02x\n",
                    operation, lane_a, lane_b, uint32_t(result.bits), oracle, result.flags, oracle_flags);
            }
            CHECK(result.bits == oracle);
            CHECK(result.flags == oracle_flags);
        }
    }
    // The binary64 operations must still reject out-of-range selectors
    // (vector float-to-int VCVT now occupies 6/7).
    const auto invalid = fp64_arithmetic(8, 0, 0, 0);
    CHECK(invalid.bits == 0x7ff8000000000000ull && invalid.flags == 1);
    std::puts("recip helper differential: 400000 patterns x 2 operations passed");
}

// Exhaustive input-class coverage through the emitted IR, in Q/D and both
// execution modes: zeros, infinities, all-NaN classes, boundary exponents,
// randomized mantissas and the denormal shapes RECPE is known to flush.
void ir_lanes() {
    struct Case { uint32_t a, b; };
    std::vector<Case> cases;
    for (uint32_t exponent : {0u, 1u, 0x7eu, 0x7fu, 0x80u, 0xfdu, 0xfeu, 0xffu}) {
        for (uint32_t fraction : {0u, 1u, 0x3fffffu, 0x400000u, 0x400001u, 0x7fffffu}) {
            cases.push_back({exponent << 23 | fraction, 0x3f800000u | (fraction >> 1)});
            cases.push_back({(exponent << 23 | fraction) | 0x80000000u, 0xbf800000u | (fraction >> 1)});
        }
    }
    uint32_t rng = 0x2545f491;
    for (unsigned i = 0; i < 2000; ++i) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        cases.push_back({rng, rng ^ 0x3f800000u});
    }
    for (const auto &item : cases) {
        auto block = blank();
        const auto q0 = read_q(block);
        write_q(block, append(block, Op::FPVectorRecipEstimate32, {q0, Value{false}}), 1);
        write_q(block, append(block, Op::FPVectorRecipStepFused32, {q0, read_q(block, 2), Value{false}}), 3);
        JitState state = base_state();
        for (unsigned word = 0; word < 4; ++word) {
            state.fpu[word] = item.a; // Q0: recpe source
            // Distinct Q2 lanes per word: exercises the emitter's per-lane
            // b-operand publish (a single lane-0 publish would fail lanes 1-3).
            state.fpu[8 + word] = item.b ^ (word * 0x01010101u);
        }
        // Oracle: call the vendored implementation per lane, standard FPSCR.
        JitState expected = state;
        uint32_t fpscr = 0;
        for (unsigned lane = 0; lane < 4; ++lane) {
            const auto fpcr = Dynarmic::FP::FPCR{0}.ASIMDStandardValue();
            Dynarmic::FP::FPSR fpsr{0};
            const uint32_t a = state.fpu[lane], b = state.fpu[8 + lane];
            expected.fpu[4 + lane] = Dynarmic::FP::FPRecipEstimate<uint32_t>(a, fpcr, fpsr);
            fpsr = Dynarmic::FP::FPSR{fpsr.Value() & 0x9f};
            expected.fpu[12 + lane] = Dynarmic::FP::FPRecipStepFused<uint32_t>(a, b, fpcr, fpsr);
            fpscr |= fpsr.Value() & 0x9f;
        }
        expected.fpscr = fpscr;
        run(block, state, &expected);
    }
    std::printf("recip ir lanes: %zu cases x 2 ops x (block+region) passed\n", cases.size());
}

// Only an immediate fpcr_controlled=false is supported. A runtime U1
// (even one whose live value would be false) must not bypass the shape guard.
void immediate_guards() {
    for (Op op : {Op::FPVectorRecipEstimate32, Op::FPVectorRecipStepFused32}) {
        for (unsigned shape : {0u, 1u, 2u}) {
            auto block = blank();
            const auto controlled = shape == 2 ? append(block, Op::A32GetCFlag) : Value{shape == 1};
            const auto a = read_q(block);
            const auto result = op == Op::FPVectorRecipEstimate32
                ? append(block, op, {a, controlled})
                : append(block, op, {a, read_q(block, 2), controlled});
            write_q(block, result, 1);
            const A32::LocationDescriptor at{block.Location()};
            for (bool region : {false, true}) {
                const auto bytes = region
                    ? vita3k::wasmjit::emit_region({&block}, {{at.PC(), PSR_DISPATCH_MASK,
                        at.CPSR().Value() & PSR_DISPATCH_MASK, 1}})
                    : vita3k::wasmjit::emit_block(block);
                CHECK(bytes.empty() == (shape != 0));
            }
        }
    }
    std::puts("recip immediate guards: controlled=true and non-immediate U1 reject in block/region modes");
}

// The estimate runs under the standard FPSCR value regardless of the live
// FPSCR: RMode, FZ and DN cannot perturb vrecpe/vrecps when
// fpcr_controlled=false, so such blocks still emit with identical results.
// Only live exception enables must be clear (the shared helper convention).
void scr_guards() {
    // 0x200 (bit 9) is an exception enable, not a mode bit: it belongs with
    // the trap group below, which must bail in both modes.
    for (uint32_t scr : {0x00400000u, 0x00800000u, 0x00c00000u, 0x01000000u, 0x02000000u}) {
        auto block = blank();
        write_q(block, append(block, Op::FPVectorRecipEstimate32, {read_q(block), Value{false}}), 1);
        JitState state = base_state();
        state.fpscr = scr;
        for (unsigned word = 0; word < 4; ++word)
            state.fpu[word] = 0x3e999000u + 0x00010001u * word;
        // Oracle: the standard-FPSCR estimate, identical for every live FPSCR.
        JitState expected = state;
        for (unsigned lane = 0; lane < 4; ++lane) {
            const auto fpcr = Dynarmic::FP::FPCR{0}.ASIMDStandardValue();
            Dynarmic::FP::FPSR fpsr{0};
            expected.fpu[4 + lane] = Dynarmic::FP::FPRecipEstimate<uint32_t>(state.fpu[lane], fpcr, fpsr);
            CHECK(fpsr.Value() == 0); // normal inputs raise no cumulative flags
        }
        // Block mode executes: RMode/FZ/DN cannot perturb the estimate.
        {
            const auto bytes = vita3k::wasmjit::emit_block(block);
            CHECK(!bytes.empty());
            const int slot = vita3k_jit_install(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write);
            CHECK(slot >= 0);
            auto actual = state;
            CHECK(static_cast<Reason>(vita3k_jit_call(slot, reinterpret_cast<uintptr_t>(&actual))) == Reason::Continue);
            for (unsigned i = 0; i < 64; ++i) CHECK(actual.fpu[i] == expected.fpu[i]);
            CHECK(actual.fpscr == expected.fpscr);
            vita3k_jit_release(slot);
        }
        // Region mode Misses cleanly: FPSCR mode bits are part of the
        // block's translation key, so the dispatch leaf refuses entry with
        // zero architectural writes and the host re-dispatches with the
        // matching key. The hand cases above also execute matching non-RN
        // regions; the block-mode half here proves the emitter itself
        // ignores the mode bits per fpcr_controlled=false.
        {
            const A32::LocationDescriptor at{block.Location()};
            const auto bytes = vita3k::wasmjit::emit_region({&block}, {{at.PC(), PSR_DISPATCH_MASK,
                at.CPSR().Value() & PSR_DISPATCH_MASK, 1}});
            CHECK(!bytes.empty());
            const int slot = vita3k_jit_install_region(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write);
            CHECK(slot >= 0);
            auto actual = state;
            CHECK(static_cast<Reason>(vita3k_jit_run(slot, reinterpret_cast<uintptr_t>(&actual), 1)) == Reason::Miss);
            CHECK(actual.fpscr == state.fpscr);
            CHECK(actual.cpsr == state.cpsr);
            for (unsigned i = 0; i < 64; ++i) CHECK(actual.fpu[i] == state.fpu[i]);
            for (unsigned i = 0; i < 15; ++i) CHECK(actual.regs[i] == state.regs[i]);
            vita3k_jit_release_region(slot);
        }
    }
    std::puts("recip SCR guards: RMode/FZ/DN do not perturb standard-FPSCR estimates (block runs, region clean-Misses)");

    // Trap enables are live state: every enable must reject before any write.
    auto block = blank();
    write_q(block, append(block, Op::FPVectorRecipEstimate32, {read_q(block), Value{false}}), 1);
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
        for (uint32_t trap : {0x100u, 0x200u, 0x400u, 0x800u, 0x1000u, 0x8000u}) {
            JitState state = base_state();
            state.fpscr = trap;
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
    std::puts("recip SCR guards: rejected exception-enable blocks");
}

// The exact shape Limbo executes at PC=8103fe6a/8103fe6e (Thumb VCVT then
// VRECPE through d0/d1), chained in one block like the real translator
// produces, to prove the two operations compose.
void limbo_sequence() {
    auto block = blank();
    const auto q0 = read_q(block);
    // VCVT (integer) then VRECPE then the fused step against Q2.
    write_q(block, append(block, Op::FPVectorFromSignedFixed32,
        {q0, Value{uint8_t(0)}, Value{uint8_t(0)}, Value{false}}), 1);
    const auto cvt = read_q(block, 1);
    write_q(block, append(block, Op::FPVectorRecipEstimate32, {cvt, Value{false}}), 1);
    const auto est = read_q(block, 1);
    write_q(block, append(block, Op::FPVectorRecipStepFused32, {est, read_q(block, 2), Value{false}}), 3);
    JitState state = base_state();
    for (unsigned word = 0; word < 4; ++word) {
        state.fpu[word] = 0x40400040 + 0x01010101u * word;
        state.fpu[8 + word] = 0x3e999000u;
    }
    JitState expected = state;
    uint32_t fpscr = 0;
    for (unsigned lane = 0; lane < 4; ++lane) {
        const auto fpcr = Dynarmic::FP::FPCR{0}.ASIMDStandardValue();
        Dynarmic::FP::FPSR fpsr{0};
        const uint32_t raw = state.fpu[lane];
        // VCVT fbits=0: plain i32 -> f32 via exact double widening.
        const float converted = static_cast<float>(static_cast<int32_t>(raw));
        uint32_t bits;
        std::memcpy(&bits, &converted, sizeof(bits));
        expected.fpu[4 + lane] = bits;
        expected.fpu[4 + lane] = Dynarmic::FP::FPRecipEstimate<uint32_t>(bits, fpcr, fpsr);
        fpsr = Dynarmic::FP::FPSR{fpsr.Value() & 0x9f};
        expected.fpu[12 + lane] = Dynarmic::FP::FPRecipStepFused<uint32_t>(
            expected.fpu[4 + lane], state.fpu[8 + lane], fpcr, fpsr);
        fpscr |= fpsr.Value() & 0x9f;
    }
    expected.fpscr = fpscr;
    run(block, state, &expected);
    std::puts("recip limbo sequence: VCVT -> VRECPE -> VRECPS chain passed");
}

inline void run() {
    hand_cases();
    helper_differential();
    ir_lanes();
    immediate_guards();
    scr_guards();
    limbo_sequence();
}

} // namespace recip_tests
