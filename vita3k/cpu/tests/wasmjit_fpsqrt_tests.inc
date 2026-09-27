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

// Scalar binary64 square root (FPSqrt64, vsqrt.f64) on the exact path: the
// native helper under the live FPSCR, every rounding mode, FZ and DN. The
// oracle takes round-to-nearest bits from the host's correctly rounded sqrt,
// decides exactness with the square in binary128 (106 significant bits fit),
// and derives the directed modes by stepping one ulp towards the right side.
void oracle64(uint64_t a, uint32_t fpscr, uint64_t &result, uint32_t &flags) {
    constexpr uint64_t sign = 0x8000000000000000ull, quiet = 0x0008000000000000ull;
    constexpr uint64_t infinity = 0x7ff0000000000000ull, default_nan = 0x7ff8000000000000ull;
    flags = 0;
    uint64_t magnitude = a & ~sign;
    if (magnitude > infinity) {
        if ((a & quiet) == 0) flags |= 1;
        result = (fpscr & 0x02000000u) ? default_nan : (a | quiet);
        return;
    }
    if ((fpscr & 0x01000000u) && magnitude != 0 && magnitude < 0x0010000000000000ull) {
        flags |= 0x80; // FZ flushes the subnormal input to a signed zero
        a &= sign;
        magnitude = 0;
    }
    if (magnitude == 0) { result = a; return; } // sqrt(+-0) = +-0
    if (a & sign) { flags |= 1; result = default_nan; return; }
    if (magnitude == infinity) { result = a; return; }
    double x;
    std::memcpy(&x, &a, sizeof(x));
    const double root = std::sqrt(x);
    std::memcpy(&result, &root, sizeof(result));
    const long double square = static_cast<long double>(root) * static_cast<long double>(root);
    if (square != static_cast<long double>(x)) {
        flags |= 0x10;
        const bool above = square > static_cast<long double>(x);
        const unsigned mode = (fpscr >> 22) & 3;
        if (mode == 1 && !above) ++result; // towards +infinity
        if ((mode == 2 || mode == 3) && above) --result; // towards -infinity / zero
    }
}

void sweep64() {
    static_assert(sizeof(long double) == 16, "binary128 exactness oracle");
    std::vector<uint64_t> inputs;
    for (uint64_t exp : {0ull, 1ull, 2ull, 0x3feull, 0x3ffull, 0x400ull, 0x7fdull, 0x7feull, 0x7ffull})
        for (uint64_t frac : {0ull, 1ull, 0x0008000000000000ull, 0x0007ffffffffffffull,
                 0x0008000000000001ull, 0x000fffffffffffffull, 0x0006a09e667f3bcdull})
            for (uint64_t sign : {0ull, 0x8000000000000000ull})
                inputs.push_back(sign | (exp << 52) | frac);
    for (double f : {1.0, 4.0, 2.25, 0.25, 9.0, 1e300, 1e-300, 0x1p-1074, 0x1p-1022, 0x1.fffffffffffffp1023}) {
        uint64_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        for (uint64_t near : {bits - 1, bits, bits + 1}) inputs.push_back(near);
    }
    uint64_t rng = 0x9e3779b97f4a7c15ull;
    for (unsigned i = 0; i < 2000; ++i) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        inputs.push_back(rng);
        inputs.push_back(rng & 0x800fffffffffffffull); // subnormals
    }
    uint64_t count = 0;
    for (uint32_t mode = 0; mode < 16; ++mode) {
        const uint32_t location_fpscr = (mode & 3) << 22 | ((mode >> 2) & 1) << 24 | ((mode >> 3) & 1) << 25;
        const A32::LocationDescriptor here{code, A32::PSR{0x10}, A32::FPSCR{location_fpscr}};
        const A32::LocationDescriptor next{code + 4, A32::PSR{0x10}, A32::FPSCR{location_fpscr}};
        IR::Block block{here};
        block.SetEndLocation(next);
        block.SetTerminal(IR::Term::LinkBlock{next});
        block.CycleCount() = 1;
        append(block, Op::A32SetExtendedRegister64, {Value{A32::ExtReg::D1},
            append(block, Op::FPSqrt64, {append(block, Op::A32GetExtendedRegister64, {Value{A32::ExtReg::D3}})})});
        for (bool region : {false, true}) {
            const auto bytes = region
                ? vita3k::wasmjit::emit_region({&block}, {{here.PC(), PSR_DISPATCH_MASK,
                    here.CPSR().Value() & PSR_DISPATCH_MASK, 1}})
                : vita3k::wasmjit::emit_block(block);
            CHECK(!bytes.empty());
            const int slot = region
                ? vita3k_jit_install_region(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write)
                : vita3k_jit_install(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write);
            CHECK(slot >= 0);
            for (const uint64_t a : inputs) {
                uint64_t want_bits;
                uint32_t want_flags;
                oracle64(a, location_fpscr, want_bits, want_flags);
                JitState state = base_state();
                state.fpscr = location_fpscr | 0xa0000000u; // FPSCR NZCV must survive
                state.fpu[6] = uint32_t(a);
                state.fpu[7] = uint32_t(a >> 32);
                state.fpu[2] = state.fpu[3] = 0x5a5a5a5au;
                const auto reason = static_cast<Reason>(region
                    ? vita3k_jit_run(slot, reinterpret_cast<uintptr_t>(&state), 1)
                    : vita3k_jit_call(slot, reinterpret_cast<uintptr_t>(&state)));
                CHECK(reason == (region ? Reason::Miss : Reason::Continue));
                const uint64_t got = uint64_t(state.fpu[2]) | (uint64_t(state.fpu[3]) << 32);
                if (got != want_bits || state.fpscr != (location_fpscr | 0xa0000000u | want_flags))
                    std::fprintf(stderr, "fpsqrt64 region=%d fpscr=%08x a=%016llx got=%016llx/%08x want=%016llx/%02x\n",
                        region, location_fpscr, (unsigned long long)a, (unsigned long long)got, state.fpscr,
                        (unsigned long long)want_bits, want_flags);
                CHECK(got == want_bits);
                CHECK(state.fpscr == (location_fpscr | 0xa0000000u | want_flags));
                CHECK(state.fpu[6] == uint32_t(a) && state.fpu[7] == uint32_t(a >> 32));
                ++count;
            }
            if (region) vita3k_jit_release_region(slot);
            else vita3k_jit_release(slot);
        }
    }
    // Pinned without the host sqrt: sqrt(2) rounds up in RN/RP, down in RZ/RM.
    for (uint32_t mode = 0; mode < 4; ++mode) {
        const auto r = vita3k::wasmjit::fp64_arithmetic(10, 0x4000000000000000ull, 0, mode << 22);
        CHECK(r.bits == (mode == 0 || mode == 1 ? 0x3ff6a09e667f3bcdull : 0x3ff6a09e667f3bccull));
        CHECK(r.flags == 0x10);
    }
    const auto exact = vita3k::wasmjit::fp64_arithmetic(10, 0x4022000000000000ull, 0, 0); // 9.0
    CHECK(exact.bits == 0x4008000000000000ull && exact.flags == 0);
    const auto tiny = vita3k::wasmjit::fp64_arithmetic(10, 1, 0, 0); // 2^-1074 -> 2^-537, exact
    CHECK(tiny.bits == 0x1e60000000000000ull && tiny.flags == 0);
    std::printf("fpsqrt64 sweep: %llu cases (16 FPSCR modes x block+region) passed\n", (unsigned long long)count);
}

// Real VSQRT.F64 d0, d3 (ARM and Thumb) through the translator and the lazy
// JIT, which uses the exact path; trap enables still bail out at run time.
void guest64(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    struct Case { uint64_t input, result; uint32_t flags; };
    const Case cases[] = {
        {0x4010000000000000ull, 0x4000000000000000ull, 0}, // 4 -> 2 exact
        {0x4000000000000000ull, 0x3ff6a09e667f3bcdull, 0x10}, // sqrt(2) inexact
        {0xbff0000000000000ull, 0x7ff8000000000000ull, 1}, // -1 -> default NaN + IOC
        {0x8000000000000000ull, 0x8000000000000000ull, 0}, // -0 -> -0
        {0x7ff4000000000000ull, 0x7ffc000000000000ull, 1}, // sNaN quieted + IOC
    };
    for (bool regions : {false, true}) for (bool thumb : {false, true}) for (const auto &c : cases) {
        jit.set_region_mode(regions);
        constexpr uint32_t vsqrt = 0xeeb10bc3u;
        if (thumb) put_thumb(mem, jit, {(vsqrt << 16) | (vsqrt >> 16), 0xbf00df00u});
        else put(mem, jit, {vsqrt, 0xef000000u});
        auto before = jit.save_context();
        std::memcpy(&before.fpu_registers[6], &c.input, 8);
        before.fpscr = 0x60000004u;
        jit.load_context(before);
        CHECK(jit.run() == 0 && parent.svc_called);
        const auto after = jit.save_context();
        uint64_t result;
        std::memcpy(&result, &after.fpu_registers[0], 8);
        CHECK(result == c.result);
        CHECK(after.fpscr == (before.fpscr | c.flags));
    }
    std::puts("fpsqrt64 guest: VSQRT.F64 ARM/Thumb single+region passed");
}

inline void run() {
    sweep();
    guards();
    sweep64();
}

} // namespace fpsqrt_tests
