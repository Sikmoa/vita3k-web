// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
// Included by wasmjit_backend_test.cpp after its shared CHECK/put helpers.
// Vector float-to-int VCVT (FPVectorToSignedFixed32/FPVectorToUnsignedFixed32)
// fixtures. The native helper delegates to the vendored Dynarmic FPToFixed
// implementation, so the differential oracle here IS that same library
// called directly: the fixtures prove the wiring (immediate-shape gating,
// lane packing, helper dispatch, FPSCR flag OR-in, register writeback)
// rather than re-deriving the conversion. Independent hand-pinned cases
// check zero-with-IOC for NaNs/negative unsigned inputs, saturation with IOC
// for overflow, and IXC for discarded fractions. FZ input flushing happens
// first: either sign of subnormal becomes zero with IDC only, even unsigned.
//
// The helper executes the standard VCVT shape only (fbits=0, TowardsZero
// rounding, fpcr_controlled=false) under ASIMDStandardValue() (FZ=1, DN=1;
// explicit rounding overrides RN). Every other immediate shape must reject.
// Mode fixtures vary live RMode/FZ/DN and match region keys to prove these
// bits cannot perturb the standard-FPSCR conversion.
#include <dynarmic/common/fp/op/FPToFixed.h>
#include <dynarmic/common/fp/rounding_mode.h>
namespace tofixed_tests {
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
                    std::fprintf(stderr, "tofixed region=%d word=%u got=%08x expected=%08x q0=%08x\n",
                        region, i, state.fpu[i], expected->fpu[i], initial.fpu[0]);
                CHECK(state.fpu[i] == expected->fpu[i]);
            }
            for (unsigned i = 0; i < 15; ++i) CHECK(state.regs[i] == expected->regs[i]);
            CHECK(state.fpscr == expected->fpscr);
        }
        if (region) vita3k_jit_release_region(slot);
        else vita3k_jit_release(slot);
    }
}

uint32_t oracle_tofixed(uint32_t lane, bool unsigned_, uint32_t &flags) {
    const auto fpcr = Dynarmic::FP::FPCR{0}.ASIMDStandardValue();
    Dynarmic::FP::FPSR fpsr{0};
    const uint64_t result = Dynarmic::FP::FPToFixed<uint32_t>(
        32, lane, 0, unsigned_, fpcr, Dynarmic::FP::RoundingMode::TowardsZero, fpsr);
    flags = fpsr.Value() & 0x9f;
    return uint32_t(result & 0xffffffffu);
}

// These expectations do not call the vendored oracle. In particular, FZ
// must replace the old denormal IXC/unsigned IOC outcomes with IDC only.
void hand_cases() {
    struct Case { uint32_t lane, signed_result, signed_flags, unsigned_result, unsigned_flags; };
    const Case cases[] = {
        {0x00000000u, 0, 0x00u, 0, 0x00u}, // signed zeros convert exactly
        {0x80000000u, 0, 0x00u, 0, 0x00u},
        {0x00000001u, 0, 0x80u, 0, 0x80u}, // min subnormal: IDC, no IXC/UFC
        {0x80000001u, 0, 0x80u, 0, 0x80u}, // negative subnormal: no unsigned IOC
        {0x007fffffu, 0, 0x80u, 0, 0x80u}, // max subnormal still flushes
        {0x807fffffu, 0, 0x80u, 0, 0x80u},
        {0x00800000u, 0, 0x10u, 0, 0x10u}, // min normal does NOT flush: IXC
        {0x80800000u, 0, 0x10u, 0, 0x01u}, // negative normal is unsigned invalid
        {0x7fc12345u, 0, 0x01u, 0, 0x01u}, // qNaN -> integer zero + IOC
        {0xffc12345u, 0, 0x01u, 0, 0x01u}, // NaN sign/payload cannot survive conversion
        {0x7f812345u, 0, 0x01u, 0, 0x01u}, // sNaN -> integer zero + IOC
        {0xff812345u, 0, 0x01u, 0, 0x01u},
        {0x7f800000u, 0x7fffffffu, 0x01u, 0xffffffffu, 0x01u}, // +inf saturates
        {0xff800000u, 0x80000000u, 0x01u, 0, 0x01u}, // -inf saturates
        {0x3f800000u, 1, 0x00u, 1, 0x00u}, // +1 exact
        {0xbf800000u, 0xffffffffu, 0x00u, 0, 0x01u}, // -1 exact signed, unsigned IOC
        {0x3f7fffffu, 0, 0x10u, 0, 0x10u}, // truncate, never round to 1
        {0xbf7fffffu, 0, 0x10u, 0, 0x01u},
        {0x3fc00000u, 1, 0x10u, 1, 0x10u}, // 1.5 -> 1 + IXC, regardless of RMode
        {0xbfc00000u, 0xffffffffu, 0x10u, 0, 0x01u}, // -1.5 -> -1, unsigned IOC only
        {0x4effffffu, 0x7fffff80u, 0x00u, 0x7fffff80u, 0x00u}, // last f32 below 2^31
        {0x4f000000u, 0x7fffffffu, 0x01u, 0x80000000u, 0x00u}, // +2^31 fits unsigned
        {0xcf000000u, 0x80000000u, 0x00u, 0, 0x01u}, // INT_MIN fits signed
        {0xcf000001u, 0x80000000u, 0x01u, 0, 0x01u}, // below INT_MIN saturates
        {0x4f7fffffu, 0x7fffffffu, 0x01u, 0xffffff00u, 0x00u}, // last f32 below 2^32
        {0x4f800000u, 0x7fffffffu, 0x01u, 0xffffffffu, 0x01u}, // 2^32 overflows both
        {0x7f7fffffu, 0x7fffffffu, 0x01u, 0xffffffffu, 0x01u}, // no OFC/IXC on saturation
        {0xff7fffffu, 0x80000000u, 0x01u, 0, 0x01u},
    };
    for (const auto &item : cases) {
        for (uint32_t operation : {6u, 7u}) {
            const bool unsigned_ = operation == 7;
            const uint32_t bits = unsigned_ ? item.unsigned_result : item.signed_result;
            const uint32_t flags = unsigned_ ? item.unsigned_flags : item.signed_flags;
            for (uint32_t mode = 0; mode < 16; ++mode) {
                const auto result = vita3k::wasmjit::fp64_arithmetic(
                    operation, item.lane, 0, (mode << 22) | 0xa8000004u);
                if (result.bits != bits || result.flags != flags)
                    std::fprintf(stderr, "tofixed hand op=%u mode=%x lane=%08x got=%08x/%02x want=%08x/%02x\n",
                        operation, mode, item.lane, uint32_t(result.bits), result.flags, bits, flags);
                CHECK(result.bits == bits);
                CHECK(result.flags == flags); // return only newly raised flags
            }
            for (uint32_t scr : {0u, 0x03c00000u}) {
                auto block = blank(code, scr);
                write_q(block, append(block, unsigned_ ? Op::FPVectorToUnsignedFixed32 : Op::FPVectorToSignedFixed32,
                    {read_q(block), Value{uint8_t(0)}, Value{uint8_t(3)}, Value{false}}), 1);
                auto state = base_state();
                state.fpscr = scr | 0xa8000004u; // retain NZCV/QC and preexisting OFC
                for (unsigned lane = 0; lane < 4; ++lane) state.fpu[lane] = item.lane;
                auto expected = state;
                for (unsigned lane = 0; lane < 4; ++lane) expected.fpu[4 + lane] = bits;
                expected.fpscr |= flags;
                run(block, state, &expected); // both matching-key modes must execute
            }
        }
    }
    std::puts("tofixed hand cases: FZ/IDC, NaNs, saturation, truncation and mode independence passed");
}

// Direct helper differential: sampled bit patterns, one lane at a time,
// against the same Dynarmic entry point the helper calls. Seed the sweep
// with NaN payloads, infinities, signed zeros, overflow boundaries at
// 2^31/2^32, INT_MIN and fractions alongside the independent cases above.
void helper_differential() {
    using vita3k::wasmjit::fp64_arithmetic;
    const uint32_t edges[] = {
        0x00000000u, 0x80000000u, // zeros
        0x7f800000u, 0xff800000u, // infinities
        0x7fc00000u, 0xffc00001u, 0x7f800001u, 0xff800001u, 0x7fffffffu, // NaN classes
        0x3f800000u, 0xbf800000u, // +/-1
        0x3f7fffffu, 0xbf7fffffu, // just below 1 (IXC, truncate to 0)
        0x3fc00000u, 0xbfc00000u, // +/-1.5 (IXC)
        0x4f000000u, 0xcf000000u, // +/-2^31 (signed boundary; +2^31 fits unsigned)
        0x4f000001u, 0xcf000001u,
        0x4f800000u, // 2^32 (both overflow)
        0xcf800000u, // -2^32
        0x7f7fffffu, // FLT_MAX
        0xff7fffffu, // -FLT_MAX
        0x00800000u, 0x80800000u, // smallest normals
        0x007fffffu, 0x807fffffu, // largest denormals
        0x00000001u, 0x80000001u, // smallest denormals
        0x3b800000u, // 1/16384 (Wwise-style gain fraction)
        0x42c80000u, 0xc2c80000u, // +/-100
        0x447a0000u, // 1000
    };
    uint32_t rng = 0x51ab3c7d;
    for (uint64_t iteration = 0; iteration < 200000; ++iteration) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        const uint32_t lane = iteration < sizeof(edges) / sizeof(edges[0]) ? edges[iteration] : rng;
        for (uint32_t operation : {6u, 7u}) {
            const auto result = fp64_arithmetic(operation, lane, 0, 0);
            uint32_t oracle_flags = 0;
            const uint32_t oracle = oracle_tofixed(lane, operation == 7, oracle_flags);
            if (result.bits != oracle || result.flags != oracle_flags) {
                std::fprintf(stderr, "tofixed differential op=%u lane=%08x bits=%08x/%08x flags=%02x/%02x\n",
                    operation, lane, uint32_t(result.bits), oracle, result.flags, oracle_flags);
            }
            CHECK(result.bits == oracle);
            CHECK(result.flags == oracle_flags);
        }
    }
    // The first selector past VSQRT.F64 (10) must still reject.
    const auto invalid = fp64_arithmetic(11, 0, 0, 0);
    CHECK(invalid.bits == 0x7ff8000000000000ull && invalid.flags == 1);
    std::puts("tofixed helper differential: 200000 patterns x 2 operations passed");
}

// Exhaustive input-class coverage through the emitted IR, both operations
// and both execution modes. Lanes carry distinct values per word to prove
// per-lane packing (a lane-0-only publish would fail lanes 1-3).
void ir_lanes() {
    struct Case { uint32_t lanes[4]; };
    std::vector<Case> cases;
    for (uint32_t exponent : {0u, 1u, 0x7du, 0x7eu, 0x7fu, 0x80u, 0x81u, 0xcdu, 0xceu, 0xcfu, 0xfdu, 0xfeu, 0xffu}) {
        for (uint32_t fraction : {0u, 1u, 0x3fffffu, 0x400000u, 0x400001u, 0x7fffffu}) {
            Case c;
            for (unsigned lane = 0; lane < 4; ++lane)
                c.lanes[lane] = (exponent << 23 | ((fraction ^ (lane * 0x101001u)) & 0x7fffffu)) | (lane & 1 ? 0x80000000u : 0u);
            cases.push_back(c);
        }
    }
    uint32_t rng = 0x1b873593;
    for (unsigned i = 0; i < 1000; ++i) {
        Case c;
        for (unsigned lane = 0; lane < 4; ++lane) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            c.lanes[lane] = rng;
        }
        cases.push_back(c);
    }
    for (const auto &item : cases) {
        for (bool unsigned_ : {false, true}) {
            auto block = blank();
            const auto q0 = read_q(block);
            write_q(block, append(block, unsigned_ ? Op::FPVectorToUnsignedFixed32 : Op::FPVectorToSignedFixed32,
                {q0, Value{uint8_t(0)}, Value{uint8_t(3)}, Value{false}}), 1);
            JitState state = base_state();
            for (unsigned word = 0; word < 4; ++word)
                state.fpu[word] = item.lanes[word];
            JitState expected = state;
            uint32_t fpscr = 0;
            for (unsigned lane = 0; lane < 4; ++lane) {
                uint32_t flags = 0;
                expected.fpu[4 + lane] = oracle_tofixed(state.fpu[lane], unsigned_, flags);
                fpscr |= flags;
            }
            expected.fpscr = fpscr;
            run(block, state, &expected);
        }
    }
    std::printf("tofixed ir lanes: %zu cases x 2 ops x (block+region) passed\n", cases.size());
}

// Only the standard VCVT shape may emit: fbits must be 0, rounding must be
// TowardsZero (3), and fpcr_controlled must be false. Every other immediate
// combination rejects the block in both modes with zero writes.
void immediate_guards() {
    const uint8_t fbits_values[] = {0, 1, 31};
    const uint8_t rounding_values[] = {0, 1, 2, 3, 4};
    for (bool unsigned_ : {false, true}) {
        for (uint8_t fbits : fbits_values) {
            for (uint8_t rounding : rounding_values) {
                for (bool controlled : {false, true}) {
                    const bool accepted = fbits == 0 && rounding == 3 && !controlled;
                    auto block = blank();
                    const auto q0 = read_q(block);
                    write_q(block, append(block, unsigned_ ? Op::FPVectorToUnsignedFixed32 : Op::FPVectorToSignedFixed32,
                        {q0, Value{fbits}, Value{rounding}, Value{controlled}}), 1);
                    const A32::LocationDescriptor at{block.Location()};
                    for (bool region : {false, true}) {
                        const auto bytes = region
                            ? vita3k::wasmjit::emit_region({&block}, {{at.PC(), PSR_DISPATCH_MASK,
                                at.CPSR().Value() & PSR_DISPATCH_MASK, 1}})
                            : vita3k::wasmjit::emit_block(block);
                        if (!accepted) {
                            // Static-shape rejection happens at translation:
                            // the emitter returns no bytes at all.
                            CHECK(bytes.empty());
                            continue;
                        }
                        CHECK(!bytes.empty());
                        const int slot = region
                            ? vita3k_jit_install_region(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write)
                            : vita3k_jit_install(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write);
                        CHECK(slot >= 0);
                        JitState state = base_state();
                        for (unsigned word = 0; word < 4; ++word)
                            state.fpu[word] = 0x40400040u + word;
                        const auto reason = static_cast<Reason>(region
                            ? vita3k_jit_run(slot, reinterpret_cast<uintptr_t>(&state), 1)
                            : vita3k_jit_call(slot, reinterpret_cast<uintptr_t>(&state)));
                        CHECK(reason == (region ? Reason::Miss : Reason::Continue));
                        if (region) vita3k_jit_release_region(slot);
                        else vita3k_jit_release(slot);
                    }
                }
            }
        }
    }
    std::puts("tofixed immediate guards: only fbits=0/ToZero/uncontrolled emits");
}

// The conversion runs under the standard FPSCR value regardless of the live
// FPSCR: RMode, FZ and DN cannot perturb it when fpcr_controlled=false, so
// block mode still executes with identical results. Only live exception
// enables must be clear (the shared helper convention).
void scr_guards() {
    // 0x200 (bit 9) is an exception enable, not a mode bit: it belongs with
    // the trap group below, which must bail in both modes.
    for (uint32_t scr : {0x00400000u, 0x00800000u, 0x00c00000u, 0x01000000u, 0x02000000u}) {
        auto block = blank();
        write_q(block, append(block, Op::FPVectorToUnsignedFixed32,
            {read_q(block), Value{uint8_t(0)}, Value{uint8_t(3)}, Value{false}}), 1);
        JitState state = base_state();
        state.fpscr = scr;
        // Exact small integers: the conversion raises no cumulative flags.
        for (unsigned word = 0; word < 4; ++word)
            state.fpu[word] = 0x40400000u + 0x00400000u * word; // 3.0, 4.0, 6.0, 8.0
        // Oracle: the standard-FPSCR conversion, identical for every live FPSCR.
        JitState expected = state;
        for (unsigned lane = 0; lane < 4; ++lane) {
            uint32_t flags = 0;
            expected.fpu[4 + lane] = oracle_tofixed(state.fpu[lane], true, flags);
            CHECK(flags == 0); // exact small integers raise no cumulative flags
        }
        // Block mode executes: RMode/FZ/DN cannot perturb the conversion.
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
        // zero architectural writes and the host re-dispatches. The
        // block-mode half above proves the emitter itself ignores the mode
        // bits per fpcr_controlled=false.
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
    std::puts("tofixed SCR guards: RMode/FZ/DN do not perturb standard-FPSCR conversions (block runs, region clean-Misses)");

    // Trap enables are live state: every enable must reject before any write.
    auto block = blank();
    write_q(block, append(block, Op::FPVectorToSignedFixed32,
        {read_q(block), Value{uint8_t(0)}, Value{uint8_t(3)}, Value{false}}), 1);
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
    std::puts("tofixed SCR guards: rejected exception-enable blocks");
}

// The exact shape Limbo executes at PC=811b001e (Thumb-2 VCVT.f32.u32 in the
// Wwise bank loader), to prove the rejected block now compiles and runs.
void limbo_shape() {
    auto block = blank();
    const auto q0 = read_q(block);
    write_q(block, append(block, Op::FPVectorToUnsignedFixed32,
        {q0, Value{uint8_t(0)}, Value{uint8_t(3)}, Value{false}}), 1);
    JitState state = base_state();
    state.fpu[0] = 0x3b800000u; // 1/16384: Wwise-style fractional gain
    state.fpu[1] = 0x447a0000u; // 1000.0
    state.fpu[2] = 0x4f000000u; // 2^31: exactly representable unsigned, no IOC
    state.fpu[3] = 0xbf800000u; // -1.0: unsigned invalid, zero with IOC
    JitState expected = state;
    uint32_t fpscr = 0;
    for (unsigned lane = 0; lane < 4; ++lane) {
        uint32_t flags = 0;
        expected.fpu[4 + lane] = oracle_tofixed(state.fpu[lane], true, flags);
        fpscr |= flags;
    }
    expected.fpscr = fpscr;
    run(block, state, &expected);
    std::puts("tofixed limbo shape: PC=811b001e VCVT pattern passed");
}

inline void run() {
    hand_cases();
    helper_differential();
    ir_lanes();
    immediate_guards();
    scr_guards();
    limbo_shape();
}

} // namespace tofixed_tests
