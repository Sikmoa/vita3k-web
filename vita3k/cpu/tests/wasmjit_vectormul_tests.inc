// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
// Included by wasmjit_backend_test.cpp after its shared CHECK/put helpers.
// Vector FP multiply (FPVectorMul32, vmul.f32) fixtures, standard-FPSCR form
// only (fpcr_controlled=false, the shape the A32 translator emits for Limbo's
// PC=8103fe76 block). The differential runs each add/sub/mul pattern through
// BOTH the vector op and four scalar ops. Scalar controls come from their
// own location FPSCR, which must be RN/FZ=DN=1 to match the vector's standard
// semantics, not FPSCR{0}. It requires identical FPU words and FPSCR to pin
// lane wiring and accumulation. Independent hand cases pin DN default NaNs,
// input flushing/IDC, invalid 0*inf, and overflow/underflow flags. Mode cases
// keep vector semantics fixed while varying live and location RMode/FZ/DN.
namespace vectormul_tests {
namespace A32 = Dynarmic::A32;
namespace IR = Dynarmic::IR;
using Op = IR::Opcode;
using Value = IR::Value;
using Reason = vita3k::wasmjit::ExitReason;

constexpr uint32_t standard_fpscr = 0x03000000u; // ASIMDStandardValue: RN, FZ=DN=1

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
Value read_s(IR::Block &block, unsigned s) {
    return append(block, Op::A32GetExtendedRegister32, {Value{A32::ExtReg::S0 + s}});
}
void write_s(IR::Block &block, Value value, unsigned s) {
    append(block, Op::A32SetExtendedRegister32, {Value{A32::ExtReg::S0 + s}, value});
}

JitState base_state() {
    JitState state{};
    for (unsigned r = 0; r < 15; ++r) state.regs[r] = 0x51ab0000 + r;
    state.regs[15] = code;
    state.cpsr = 0x10;
    return state;
}

// Vector block: Q2 = Q0 * Q1. Scalar oracle block: S[8..11] = S[0..3] * S[4..7]
// through scalar FPMul32 lowering under an explicit RN/FZ=DN=1 location.
// Scalar controls are location-dependent; setting only live FPSCR is NOT
// sufficient. Same initial words (Q0==S0..S3, Q1==S4..S7), destinations
// (Q2==S8..S11) and matching region keys: end states must be bit-identical.
void run_pair(Op vec_op, Op sc_op, const uint32_t a[4], const uint32_t b[4]) {
    auto vec = blank(code, standard_fpscr);
    write_q(vec, append(vec, vec_op, {read_q(vec, 0), read_q(vec, 1), Value{false}}), 2);
    auto sc = blank(code, standard_fpscr);
    for (unsigned lane = 0; lane < 4; ++lane)
        write_s(sc, append(sc, sc_op, {read_s(sc, lane), read_s(sc, 4 + lane)}), 8 + lane);
    const A32::LocationDescriptor at{vec.Location()};
    for (bool region : {false, true}) {
        const auto vbytes = region
            ? vita3k::wasmjit::emit_region({&vec}, {{at.PC(), PSR_DISPATCH_MASK,
                at.CPSR().Value() & PSR_DISPATCH_MASK, 1}})
            : vita3k::wasmjit::emit_block(vec);
        const auto sbytes = region
            ? vita3k::wasmjit::emit_region({&sc}, {{at.PC(), PSR_DISPATCH_MASK,
                at.CPSR().Value() & PSR_DISPATCH_MASK, 1}})
            : vita3k::wasmjit::emit_block(sc);
        CHECK(!vbytes.empty() && !sbytes.empty());
        const int vslot = region
            ? vita3k_jit_install_region(vbytes.data(), vbytes.size(), checked_memory_read, checked_memory_write)
            : vita3k_jit_install(vbytes.data(), vbytes.size(), checked_memory_read, checked_memory_write);
        const int sslot = region
            ? vita3k_jit_install_region(sbytes.data(), sbytes.size(), checked_memory_read, checked_memory_write)
            : vita3k_jit_install(sbytes.data(), sbytes.size(), checked_memory_read, checked_memory_write);
        CHECK(vslot >= 0 && sslot >= 0);
        JitState initial = base_state();
        initial.fpscr = standard_fpscr;
        for (unsigned lane = 0; lane < 4; ++lane) {
            initial.fpu[lane] = a[lane];
            initial.fpu[4 + lane] = b[lane];
        }
        auto vstate = initial, sstate = initial;
        const auto vreason = static_cast<Reason>(region
            ? vita3k_jit_run(vslot, reinterpret_cast<uintptr_t>(&vstate), 1)
            : vita3k_jit_call(vslot, reinterpret_cast<uintptr_t>(&vstate)));
        const auto sreason = static_cast<Reason>(region
            ? vita3k_jit_run(sslot, reinterpret_cast<uintptr_t>(&sstate), 1)
            : vita3k_jit_call(sslot, reinterpret_cast<uintptr_t>(&sstate)));
        (void)vreason; (void)sreason;
        CHECK(vreason == (region ? Reason::Miss : Reason::Continue));
        CHECK(sreason == (region ? Reason::Miss : Reason::Continue));
        for (unsigned i = 0; i < 64; ++i) {
            if (vstate.fpu[i] != sstate.fpu[i] || vstate.fpscr != sstate.fpscr)
                std::fprintf(stderr, "vectorfp region=%d word=%u vec=%08x sc=%08x vfpscr=%08x sfpscr=%08x a=%08x b=%08x\n",
                    region, i, vstate.fpu[i], sstate.fpu[i], vstate.fpscr, sstate.fpscr,
                    a[i < 4 ? i : 0], b[i < 4 ? i : 0]);
            CHECK(vstate.fpu[i] == sstate.fpu[i]);
        }
        CHECK(vstate.fpscr == sstate.fpscr);
        if (region) { vita3k_jit_release_region(vslot); vita3k_jit_release_region(sslot); }
        else { vita3k_jit_release(vslot); vita3k_jit_release(sslot); }
    }
}

struct Case { uint32_t a, b, result, fpscr; };

void hand_cases_for(Op vec_op, Op sc_op, const char *name, std::initializer_list<Case> cases) {
    for (const auto &item : cases) {
        const uint32_t a[4] = {item.a, item.a, item.a, item.a};
        const uint32_t b[4] = {item.b, item.b, item.b, item.b};
        // Pin the standard-FPSCR contract against hand values even though
        // the live/location FPSCR is zero (FZ/DN clear).
        auto block = blank();
        write_q(block, append(block, vec_op, {read_q(block, 0), read_q(block, 1), Value{false}}), 2);
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
            JitState state = base_state();
            for (unsigned lane = 0; lane < 4; ++lane) {
                state.fpu[lane] = item.a;
                state.fpu[4 + lane] = item.b;
            }
            const auto reason = static_cast<Reason>(region
                ? vita3k_jit_run(slot, reinterpret_cast<uintptr_t>(&state), 1)
                : vita3k_jit_call(slot, reinterpret_cast<uintptr_t>(&state)));
            CHECK(reason == (region ? Reason::Miss : Reason::Continue));
            for (unsigned lane = 0; lane < 4; ++lane) {
                if (state.fpu[8 + lane] != item.result || state.fpscr != item.fpscr)
                    std::fprintf(stderr, "vectorfp hand %s region=%d lane=%u got=%08x/%08x want=%08x/%08x a=%08x b=%08x\n",
                        name, region, lane, state.fpu[8 + lane], state.fpscr, item.result, item.fpscr, item.a, item.b);
                CHECK(state.fpu[8 + lane] == item.result);
            }
            CHECK(state.fpscr == item.fpscr);
            if (region) vita3k_jit_release_region(slot);
            else vita3k_jit_release(slot);
        }
        run_pair(vec_op, sc_op, a, b); // vector-vs-scalar agreement on the same pattern
    }
}

void hand_cases() {
    hand_cases_for(Op::FPVectorMul32, Op::FPMul32, "mul", {
        {0x40000000u, 0x40400000u, 0x40c00000u, 0x00000000u}, // 2*3=6 exact
        {0x00000000u, 0x7f800000u, 0x7fc00000u, 0x00000001u}, // 0*inf: IOC + default NaN
        {0xff800000u, 0x00000000u, 0x7fc00000u, 0x00000001u}, // -inf*+0: IOC + default NaN
        {0x7f800000u, 0x40000000u, 0x7f800000u, 0x00000000u}, // inf*2=inf, no flags
        {0x7f7fffffu, 0x40000000u, 0x7f800000u, 0x00000014u}, // max*2: OFC+IXC
        {0x00800000u, 0x00800000u, 0x00000000u, 0x00000008u}, // min-normal^2: FZ, UFC WITHOUT IXC
        {0x00800000u, 0x3f000000u, 0x00000000u, 0x00000008u}, // exact subnormal also flushes + UFC
        {0x80800000u, 0x3f000000u, 0x80000000u, 0x00000008u}, // negative tiny result -> -0
        {0x00800000u, 0x3f7fffffu, 0x00000000u, 0x00000008u}, // tiny BEFORE RN would round to min-normal
        {0x80800000u, 0x3f7fffffu, 0x80000000u, 0x00000008u}, // negative pre-rounding tininess
        {0x00800000u, 0x3f800000u, 0x00800000u, 0x00000000u}, // min-normal is not tiny
        {0x3fc00000u, 0x3fc00000u, 0x40100000u, 0x00000000u}, // 1.5*1.5=2.25 exact
        {0x3f800001u, 0x3f800001u, 0x3f800002u, 0x00000010u}, // normal inexact product still raises IXC
        {0x7fc00001u, 0x40000000u, 0x7fc00000u, 0x00000000u}, // DN qNaN, no flags
        {0xffc12345u, 0x40000000u, 0x7fc00000u, 0x00000000u}, // discard sign and payload
        {0x40000000u, 0x7f800001u, 0x7fc00000u, 0x00000001u}, // sNaN second: DN + IOC
        {0x7f800001u, 0x7fc00001u, 0x7fc00000u, 0x00000001u}, // sNaN first still raises IOC
        {0x7fc00001u, 0xff800123u, 0x7fc00000u, 0x00000001u}, // qNaN first cannot hide signaling b
        {0x00000001u, 0x3f800000u, 0x00000000u, 0x00000080u}, // input FZ, IDC only
        {0x807fffffu, 0x3f800000u, 0x80000000u, 0x00000080u}, // input FZ preserves -0
        {0x00000001u, 0x7f800000u, 0x7fc00000u, 0x00000081u}, // flushed input makes 0*inf invalid
        {0xff800000u, 0x80000001u, 0x7fc00000u, 0x00000081u}, // either input must flush first
        {0x7fc12345u, 0x00000001u, 0x7fc00000u, 0x00000080u}, // qNaN does not suppress other IDC
        {0x80000001u, 0x7fc12345u, 0x7fc00000u, 0x00000080u},
        {0x7f800001u, 0x80000001u, 0x7fc00000u, 0x00000081u}, // IDC and IOC both accumulate
        {0x80000000u, 0x80000000u, 0x00000000u, 0x00000000u}, // -0*-0=+0 exact, no UFC
        {0x80000000u, 0x3f800000u, 0x80000000u, 0x00000000u}, // -0*1=-0 exact
    });
    hand_cases_for(Op::FPVectorAdd32, Op::FPAdd32, "add", {
        {0x3f800000u, 0x40000000u, 0x40400000u, 0x00000000u}, // 1+2=3 exact
        {0x7f800000u, 0xff800000u, 0x7fc00000u, 0x00000001u}, // inf+(-inf): IOC + default NaN
        {0x7f800000u, 0x7f800000u, 0x7f800000u, 0x00000000u}, // inf+inf=inf, no flags
        {0x7f800000u, 0x3f800000u, 0x7f800000u, 0x00000000u}, // inf+finite: no reverse-sum IXC
        {0x7f7fffffu, 0x7f7fffffu, 0x7f800000u, 0x00000014u}, // max+max: OFC+IXC
        {0x00800000u, 0x80800000u, 0x00000000u, 0x00000000u}, // exact cancellation: no UFC
        {0x00800001u, 0x80800000u, 0x00000000u, 0x00000008u}, // exact tiny difference: UFC, not IXC
        {0x00800000u, 0x80800001u, 0x80000000u, 0x00000008u}, // negative tiny difference -> -0
        {0x7fc00002u, 0x3f800000u, 0x7fc00000u, 0x00000000u}, // DN qNaN, no flags
        {0x3f800000u, 0x7f800002u, 0x7fc00000u, 0x00000001u}, // sNaN second: DN + IOC
        {0x7f800003u, 0x7fc00004u, 0x7fc00000u, 0x00000001u}, // sNaN first: DN + IOC
        {0x7fc12345u, 0x80000001u, 0x7fc00000u, 0x00000080u}, // process input IDC before NaNs
        {0x00000001u, 0xff800123u, 0x7fc00000u, 0x00000081u},
        {0x007fffffu, 0x00800000u, 0x00800000u, 0x00000080u}, // subnormal input, not subnormal result
        {0x80000001u, 0x80000001u, 0x80000000u, 0x00000080u}, // -0 + -0 after input FZ
        {0x00000000u, 0x80000001u, 0x00000000u, 0x00000080u}, // mixed zero signs -> +0 under RN
        {0x3f800000u, 0x33800000u, 0x3f800000u, 0x00000010u}, // 1 + half-ulp: RN ties to even
        {0x3f800000u, 0x00800000u, 0x3f800000u, 0x00000010u}, // normal addend lost even in f64
        {0x80000000u, 0x80000000u, 0x80000000u, 0x00000000u}, // -0 + -0 = -0 exact
        {0x80000000u, 0x00000000u, 0x00000000u, 0x00000000u}, // -0 + +0 = +0 (RN) exact
    });
    hand_cases_for(Op::FPVectorSub32, Op::FPSub32, "sub", {
        {0x40400000u, 0x3f800000u, 0x40000000u, 0x00000000u}, // 3-1=2 exact
        {0x7f800000u, 0x7f800000u, 0x7fc00000u, 0x00000001u}, // inf-inf: IOC + default NaN
        {0x7f800000u, 0xff800000u, 0x7f800000u, 0x00000000u}, // inf-(-inf)=inf, no flags
        {0xff7fffffu, 0x7f7fffffu, 0xff800000u, 0x00000014u}, // -max-max: OFC+IXC
        {0x00800000u, 0x00800000u, 0x00000000u, 0x00000000u}, // exact zero is not flushed
        {0x00800001u, 0x00800000u, 0x00000000u, 0x00000008u}, // exact tiny result: UFC only
        {0x00800000u, 0x00800001u, 0x80000000u, 0x00000008u}, // negative tiny result: signed zero
        {0x7fc00005u, 0x40000000u, 0x7fc00000u, 0x00000000u}, // DN qNaN, no flags
        {0x40000000u, 0x7f800006u, 0x7fc00000u, 0x00000001u}, // sNaN second: DN + IOC
        {0x40000000u, 0xffc12345u, 0x7fc00000u, 0x00000000u}, // default NaN sign stays positive
        {0x7fc12345u, 0x00000001u, 0x7fc00000u, 0x00000080u}, // input IDC survives NaN selection
        {0x80000001u, 0x7f800001u, 0x7fc00000u, 0x00000081u},
        {0x00800000u, 0x007fffffu, 0x00800000u, 0x00000080u}, // flush b BEFORE subtracting
        {0x80000001u, 0x00000000u, 0x80000000u, 0x00000080u}, // -0 - +0 after input FZ
        {0x00000000u, 0x80000001u, 0x00000000u, 0x00000080u}, // +0 - -0 after input FZ
        {0x3f800000u, 0x33000000u, 0x3f800000u, 0x00000010u}, // 1 - half the lower ulp: RN even
        {0x3f800000u, 0x00800000u, 0x3f800000u, 0x00000010u}, // normal subtrahend lost even in f64
        {0x00000000u, 0x80000000u, 0x00000000u, 0x00000000u}, // +0 - -0 = +0 exact
    });
    std::puts("vectorfp hand cases: standard FZ/DN, IDC/NaN/zero/inf/overflow/underflow contracts passed");
}

// Vary all 16 RMode/FZ/DN combinations, matching the location and live key
// so regions actually execute rather than silently testing an entry Miss.
// Distinct lanes pin input FZ, DN, pre-rounding output FZ, and RN arithmetic.
void mode_bits() {
    struct ModeCase { Op op; const char *name; uint32_t a[4], b[4], result[4], flags; };
    const ModeCase cases[] = {
        {Op::FPVectorMul32, "mul",
            {0x80000001u, 0xffc12345u, 0x00800000u, 0x3f800001u},
            {0x3f800000u, 0x3f800000u, 0x3f7fffffu, 0x3f800001u},
            {0x80000000u, 0x7fc00000u, 0x00000000u, 0x3f800002u}, 0x98u},
        {Op::FPVectorAdd32, "add",
            {0x80000001u, 0xffc12345u, 0x00800001u, 0x3f800000u},
            {0x80000000u, 0x3f800000u, 0x80800000u, 0x33800000u},
            {0x80000000u, 0x7fc00000u, 0x00000000u, 0x3f800000u}, 0x98u},
        {Op::FPVectorSub32, "sub",
            {0x80000001u, 0xffc12345u, 0x00800000u, 0x3f800000u},
            {0x00000000u, 0x3f800000u, 0x00800001u, 0x33000000u},
            {0x80000000u, 0x7fc00000u, 0x80000000u, 0x3f800000u}, 0x98u},
    };
    for (const auto &item : cases) {
        for (uint32_t mode = 0; mode < 16; ++mode) {
            const uint32_t scr = mode << 22;
            auto block = blank(code, scr);
            write_q(block, append(block, item.op, {read_q(block, 0), read_q(block, 1), Value{false}}), 2);
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
                auto state = base_state();
                state.fpscr = scr | 0xa8000002u; // preserve NZCV/QC and an unrelated DZC
                for (unsigned i = 0; i < 64; ++i) state.fpu[i] = 0x5aa50000u + i;
                for (unsigned lane = 0; lane < 4; ++lane) {
                    state.fpu[lane] = item.a[lane];
                    state.fpu[4 + lane] = item.b[lane];
                }
                auto expected = state;
                for (unsigned lane = 0; lane < 4; ++lane) expected.fpu[8 + lane] = item.result[lane];
                expected.fpscr |= item.flags;
                const auto reason = static_cast<Reason>(region
                    ? vita3k_jit_run(slot, reinterpret_cast<uintptr_t>(&state), 1)
                    : vita3k_jit_call(slot, reinterpret_cast<uintptr_t>(&state)));
                CHECK(reason == (region ? Reason::Miss : Reason::Continue));
                for (unsigned i = 0; i < 64; ++i) {
                    if (state.fpu[i] != expected.fpu[i] || state.fpscr != expected.fpscr)
                        std::fprintf(stderr, "vectorfp modes %s region=%d mode=%x word=%u got=%08x/%08x want=%08x/%08x\n",
                            item.name, region, mode, i, state.fpu[i], state.fpscr, expected.fpu[i], expected.fpscr);
                    CHECK(state.fpu[i] == expected.fpu[i]);
                }
                CHECK(state.fpscr == expected.fpscr);
                CHECK(state.cpsr == expected.cpsr);
                for (unsigned i = 0; i < 15; ++i) CHECK(state.regs[i] == expected.regs[i]);
                if (region) vita3k_jit_release_region(slot);
                else vita3k_jit_release(slot);
            }
        }
    }
    std::puts("vectorfp modes: all RMode/FZ/DN combinations preserve standard results and cumulative FPSCR");
}

void sweep_for(Op vec_op, Op sc_op, const char *name, uint64_t &count) {
    for (uint32_t ea : {0u, 1u, 0x7eu, 0x7fu, 0x80u, 0xfdu, 0xfeu, 0xffu}) {
        for (uint32_t eb : {0u, 1u, 0x7eu, 0x7fu, 0x80u, 0xfdu, 0xfeu, 0xffu}) {
            for (uint32_t f : {0u, 1u, 0x00400000u, 0x7fffffu}) {
                uint32_t a[4], b[4];
                for (unsigned lane = 0; lane < 4; ++lane) {
                    a[lane] = (ea << 23 | ((f + lane * 0x123457u) & 0x7fffffu)) ^ (lane & 1 ? 0x80000000u : 0u);
                    b[lane] = (eb << 23 | ((f * 3 + lane * 0x1b3c5du) & 0x7fffffu)) ^ (lane & 2 ? 0x80000000u : 0u);
                }
                run_pair(vec_op, sc_op, a, b);
                ++count;
            }
        }
    }
    uint32_t rng = 0x8b9d4c71;
    for (unsigned i = 0; i < 1500; ++i) {
        uint32_t a[4], b[4];
        for (unsigned lane = 0; lane < 4; ++lane) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            a[lane] = rng;
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            b[lane] = rng;
        }
        run_pair(vec_op, sc_op, a, b);
        ++count;
    }
}

void sweep() {
    uint64_t count = 0;
    sweep_for(Op::FPVectorMul32, Op::FPMul32, "mul", count);
    sweep_for(Op::FPVectorAdd32, Op::FPAdd32, "add", count);
    sweep_for(Op::FPVectorSub32, Op::FPSub32, "sub", count);
    std::printf("vectorfp sweep: %llu patterns x (vector+scalar) x (block+region) passed\n",
        (unsigned long long)count);
}

// Limbo's exact shape at PC=8103fe76: D-form (d1 = d1 * d2), two lanes.
void limbo_shape() {
    auto block = blank();
    const auto d1 = append(block, Op::A32GetVector, {Value{A32::ExtReg::D1}});
    const auto d2 = append(block, Op::A32GetVector, {Value{A32::ExtReg::D2}});
    append(block, Op::A32SetVector, {Value{A32::ExtReg::D1},
        append(block, Op::FPVectorMul32, {d1, d2, Value{false}})});
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
        JitState state = base_state();
        state.fpu[2] = 0x40000000u; state.fpu[3] = 0x40400000u; // d1 = (2, 3)
        state.fpu[4] = 0x40800000u; state.fpu[5] = 0x40a00000u; // d2 = (4, 5)
        state.fpu[6] = 0xdeadbeefu; state.fpu[7] = 0xdeadbeefu; // neighbour: untouched
        const auto reason = static_cast<Reason>(region
            ? vita3k_jit_run(slot, reinterpret_cast<uintptr_t>(&state), 1)
            : vita3k_jit_call(slot, reinterpret_cast<uintptr_t>(&state)));
        CHECK(reason == (region ? Reason::Miss : Reason::Continue));
        CHECK(state.fpu[2] == 0x41000000u); // 2*4=8
        CHECK(state.fpu[3] == 0x41700000u); // 3*5=15
        CHECK(state.fpu[6] == 0xdeadbeefu && state.fpu[7] == 0xdeadbeefu);
        CHECK(state.fpscr == 0);
        if (region) vita3k_jit_release_region(slot);
        else vita3k_jit_release(slot);
    }
    std::puts("vectormul limbo shape: D-form d1=d1*d2 passed");
}

void trap_guards_for(Op vec_op, const char *name) {
    auto block = blank();
    write_q(block, append(block, vec_op, {read_q(block, 0), read_q(block, 1), Value{false}}), 2);
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
            if (reason != Reason::Unsupported)
                std::fprintf(stderr, "vectorfp trap %s region=%d trap=%05x reason=%d\n",
                    name, region, trap, static_cast<int>(reason));
            CHECK(reason == Reason::Unsupported);
            CHECK(state.fpscr == initial.fpscr);
            for (unsigned i = 0; i < 64; ++i) CHECK(state.fpu[i] == initial.fpu[i]);
        }
        if (region) vita3k_jit_release_region(slot);
        else vita3k_jit_release(slot);
    }
    // fpcr_controlled=true is not emitted yet: live rounding/flush/default-NaN
    // would need threading through every lane.
    auto controlled = blank();
    write_q(controlled, append(controlled, vec_op, {read_q(controlled, 0), read_q(controlled, 1), Value{true}}), 2);
    CHECK(vita3k::wasmjit::emit_block(controlled).empty());
}

void trap_guards() {
    trap_guards_for(Op::FPVectorMul32, "mul");
    trap_guards_for(Op::FPVectorAdd32, "add");
    trap_guards_for(Op::FPVectorSub32, "sub");
    std::puts("vectorfp trap guards: exception enables and fpcr_controlled=true reject");
}

inline void run() {
    hand_cases();
    mode_bits();
    sweep();
    limbo_shape();
    trap_guards();
}

} // namespace vectormul_tests
