// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
// Included by wasmjit_backend_test.cpp after its shared CHECK/put helpers.
// Scalar binary32 square-root (FPSqrt32, vsqrts) fixtures. The result oracle
// is the host's correctly-rounded sqrtf (bit-exact with Wasm f32.sqrt under
// RN); the flag oracle re-derives IEEE semantics in plain C++ (NaN priority,
// negative-input IOC, f64-square exactness for IXC, tininess for UFC), so a
// transcription error in the Wasm lowering cannot hide behind a shared model.
namespace fpsqrt_tests {
namespace A32 = Dynarmic::A32;
namespace IR = Dynarmic::IR;
using Op = IR::Opcode;
using Value = IR::Value;
using Reason = vita3k::wasmjit::ExitReason;

IR::Block blank(uint32_t pc = code) {
    const A32::LocationDescriptor here{pc, A32::PSR{0x10}, A32::FPSCR{0}};
    const A32::LocationDescriptor next{pc + 4, A32::PSR{0x10}, A32::FPSCR{0}};
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

// Independent oracle: bits from sqrtf, flags from first principles.
void oracle(uint32_t a, uint32_t &result, uint32_t &flags) {
    const uint32_t aa = a & 0x7fffffffu;
    flags = 0;
    if (aa > 0x7f800000u) { // NaN
        if ((a & 0x00400000u) == 0) flags |= 1; // signaling: IOC
        result = a | 0x00400000u; // DN=0: propagate quieted
        return;
    }
    if (a & 0x80000000u) { // negative
        if (aa == 0) { result = a; return; } // sqrt(-0) = -0, exact
        flags |= 1;
        result = 0x7fc00000u; // default NaN
        return;
    }
    float fa;
    std::memcpy(&fa, &a, sizeof(fa));
    const float fr = std::sqrt(fa);
    std::memcpy(&result, &fr, sizeof(result));
    const double w = static_cast<double>(fr);
    if (w * w != static_cast<double>(fa)) {
        flags |= 0x10; // IXC
        const uint32_t ra = result & 0x7fffffffu;
        if (ra < 0x00800000u && ra != 0) flags |= 8; // UFC: tiny and inexact
    }
}

JitState base_state() {
    JitState state{};
    for (unsigned r = 0; r < 15; ++r) state.regs[r] = 0x59ab0000 + r;
    state.regs[15] = code;
    state.cpsr = 0x10;
    return state;
}

void run_case(uint32_t a) {
    uint32_t want_bits, want_flags;
    oracle(a, want_bits, want_flags);
    auto block = blank();
    append(block, Op::A32SetExtendedRegister32, {Value{A32::ExtReg::S0},
        append(block, Op::FPSqrt32, {append(block, Op::A32GetExtendedRegister32, {Value{A32::ExtReg::S0}})})});
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
        state.fpu[0] = a;
        const auto reason = static_cast<Reason>(region
            ? vita3k_jit_run(slot, reinterpret_cast<uintptr_t>(&state), 1)
            : vita3k_jit_call(slot, reinterpret_cast<uintptr_t>(&state)));
        CHECK(reason == (region ? Reason::Miss : Reason::Continue));
        if (state.fpu[0] != want_bits || state.fpscr != want_flags)
            std::fprintf(stderr, "fpsqrt region=%d a=%08x got=%08x/%02x want=%08x/%02x\n",
                region, a, state.fpu[0], state.fpscr, want_bits, want_flags);
        CHECK(state.fpu[0] == want_bits);
        CHECK(state.fpscr == want_flags);
        if (region) vita3k_jit_release_region(slot);
        else vita3k_jit_release(slot);
    }
}

void sweep() {
    uint64_t count = 0;
    for (uint32_t exp : {0u, 1u, 2u, 0x7eu, 0x7fu, 0x80u, 0xfdu, 0xfeu, 0xffu}) {
        for (uint32_t frac : {0u, 1u, 0x00400000u, 0x3fffffu, 0x400000u, 0x400001u, 0x7fffffu}) {
            run_case(exp << 23 | frac);
            run_case((exp << 23 | frac) | 0x80000000u);
            count += 2;
        }
    }
    uint32_t rng = 0x51ed1c77;
    for (unsigned i = 0; i < 3000; ++i) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        run_case(rng);
        ++count;
    }
    // Perfect squares (exact, no IXC) and near misses.
    for (float f : {0.0f, 1.0f, 2.25f, 4.0f, 0.25f, 9.0f, 16.0f, 100.0f, 1.44f}) {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        run_case(bits);
        run_case(bits + 1);
        run_case(bits - 1);
        count += 3;
    }
    std::printf("fpsqrt sweep: %llu patterns x (block+region) passed\n", (unsigned long long)count);
}

void guards() {
    // Live exception enables must reject before any write, both modes.
    auto block = blank();
    append(block, Op::A32SetExtendedRegister32, {Value{A32::ExtReg::S0},
        append(block, Op::FPSqrt32, {append(block, Op::A32GetExtendedRegister32, {Value{A32::ExtReg::S0}})})});
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
            state.fpu[0] = 0x40000000u;
            const auto reason = static_cast<Reason>(region
                ? vita3k_jit_run(slot, reinterpret_cast<uintptr_t>(&state), 1)
                : vita3k_jit_call(slot, reinterpret_cast<uintptr_t>(&state)));
            CHECK(reason == Reason::Unsupported);
            CHECK(state.fpscr == trap && state.fpu[0] == 0x40000000u);
        }
        if (region) vita3k_jit_release_region(slot);
        else vita3k_jit_release(slot);
    }
    // Non-RN location FPSCR refuses emission (Wasm sqrt is RN-only).
    for (uint32_t rmode : {0x00400000u, 0x00800000u, 0x00c00000u}) {
        const A32::LocationDescriptor here{code, A32::PSR{0x10}, A32::FPSCR{rmode}};
        const A32::LocationDescriptor next{code + 4, A32::PSR{0x10}, A32::FPSCR{rmode}};
        IR::Block rb{here};
        rb.SetEndLocation(next);
        rb.SetTerminal(IR::Term::LinkBlock{next});
        rb.CycleCount() = 1;
        append(rb, Op::FPSqrt32, {append(rb, Op::A32GetExtendedRegister32, {Value{A32::ExtReg::S0}})});
        CHECK(vita3k::wasmjit::emit_block(rb).empty());
    }
    std::puts("fpsqrt guards: trap enables reject at runtime, non-RN locations refuse emission");
}

inline void run() {
    sweep();
    guards();
}

} // namespace fpsqrt_tests
