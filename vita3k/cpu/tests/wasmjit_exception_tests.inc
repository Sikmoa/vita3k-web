// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
// Included by wasmjit_backend_test.cpp after its shared CHECK/put helpers.
// A32ExceptionRaised (UDF, BKPT, undefined/unpredictable encodings) lowers to
// ExitReason::Exception at the raising instruction: the block stays
// translatable, the instructions before it run, and the host fails with the
// same error as for a block the emitter rejects. Expected values come from
// the guest programs themselves (the prefix writes r0 = 7, the raising
// instruction sits right after it), not from the emitter.
namespace exception_tests {
namespace A32 = Dynarmic::A32;
using Reason = vita3k::wasmjit::ExitReason;

struct Program {
    const char *name;
    bool thumb;
    std::vector<uint32_t> words;
    uint32_t raise_offset; // byte offset of the raising instruction
};
const Program programs[] = {
    {"ARM UDF", false, {0xe3a00007, 0xe7f000f0}, 4}, // mov r0,#7; udf #0
    {"ARM BKPT", false, {0xe3a00007, 0xe1200071}, 4}, // mov r0,#7; bkpt #1
    {"ARM BKPTNE", false, {0xe3a00007, 0x11200071}, 4}, // conditional BKPT is unpredictable
    {"Thumb UDF", true, {0xde002007}, 2}, // movs r0,#7; udf #0
    {"Thumb BKPT", true, {0xbe012007}, 2}, // movs r0,#7; bkpt #1
    {"ARM UDF first", false, {0xe7f000f0}, 0},
};

void emitted(MemState &mem) {
    unsigned cases = 0;
    for (const auto &program : programs) {
        CHECK(mem_write(mem, code, program.words.data(), program.words.size() * 4));
        const uint32_t cpsr = program.thumb ? 0x30 : 0x10;
        const auto ir = vita3k::wasmjit::translate_block(mem, code, cpsr, 64, 0);
        bool raises = false;
        for (const auto &inst : ir)
            raises |= inst.GetOpcode() == Dynarmic::IR::Opcode::A32ExceptionRaised;
        CHECK(raises);
        const A32::LocationDescriptor at{ir.Location()};
        for (unsigned mode = 0; mode < 3; ++mode) {
            const bool region = mode != 0;
            vita3k::wasmjit::RegionStateOptions options{};
            options.promote_flags = mode == 2;
            const auto bytes = region
                ? vita3k::wasmjit::emit_region({&ir}, {{at.PC(), PSR_DISPATCH_MASK,
                    at.CPSR().Value() & PSR_DISPATCH_MASK, uint32_t(ir.CycleCount())}}, options)
                : vita3k::wasmjit::emit_block(ir);
            CHECK(!bytes.empty());
            CHECK(vita3k::wasmjit::validate_region_block(ir, {}, nullptr, options));
            const int slot = region
                ? vita3k_jit_install_region(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write)
                : vita3k_jit_install(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write);
            CHECK(slot >= 0);
            JitState state{};
            state.regs[15] = code;
            state.cpsr = cpsr | 0x60000000;
            // Thumb MOVS clears Z and keeps C; ARM MOV keeps both.
            const uint32_t want_cpsr = cpsr | (program.thumb ? 0x20000000 : 0x60000000);
            state.fault_pc = 0xdeadbeef;
            const auto reason = static_cast<Reason>(region
                ? vita3k_jit_run(slot, reinterpret_cast<uintptr_t>(&state), 64)
                : vita3k_jit_call(slot, reinterpret_cast<uintptr_t>(&state)));
            if (reason != Reason::Exception || state.fault_pc != code + program.raise_offset
                || state.cpsr != want_cpsr)
                std::fprintf(stderr, "%s mode=%u: reason=%u fault_pc=%08x cpsr=%08x\n", program.name, mode,
                    static_cast<unsigned>(reason), state.fault_pc, state.cpsr);
            CHECK(reason == Reason::Exception);
            CHECK(state.fault_pc == code + program.raise_offset);
            CHECK(state.regs[0] == (program.raise_offset ? 7u : 0u));
            CHECK(state.executed == 0); // like a memory fault: no completed store segment
            CHECK(state.cpsr == want_cpsr);
            if (region) vita3k_jit_release_region(slot);
            else vita3k_jit_release(slot);
            ++cases;
        }
    }
    std::printf("ExceptionRaised: %u emitted fixtures exit with Exception at the raising PC\n", cases);
}

// The CPU layer fails closed with the emission-time rejection error, with the
// PC on the raising instruction, and keeps failing on every retry.
void guest(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(64);
    for (bool regions : {false, true}) {
        jit.set_region_mode(regions);
        for (const auto &program : programs) {
            std::vector<uint32_t> words = program.words;
            words.push_back(program.thumb ? 0xdf00df00u : 0xef000000u); // unreachable SVC
            CHECK(mem_write(mem, code, words.data(), words.size() * 4));
            jit.invalidate_jit_cache(code, page);
            jit.set_cpsr(0x10);
            jit.set_pc(program.thumb ? code | 1 : code);
            jit.set_reg(0, 0);
            parent.svc_called = false;
            for (unsigned attempt = 0; attempt < 2; ++attempt) {
                CHECK(jit.run() < 0);
                CHECK(!parent.svc_called);
                CHECK(jit.get_last_error() == "unsupported Dynarmic IR or terminal (no fallback)");
                CHECK(jit.get_pc() == code + program.raise_offset);
                CHECK(jit.get_reg(0) == (program.raise_offset ? 7u : 0u));
                CHECK(((jit.get_cpsr() & 0x20) != 0) == program.thumb);
            }
        }
    }
    // A store before the raising instruction has committed, and the ticks
    // of the completed store segment are counted (single-block mode ends
    // the block at the store; regions continue past it).
    for (bool regions : {false, true}) {
        jit.set_region_mode(regions);
        put(mem, jit, {0xe3a00007, 0xe5810000, 0xe7f000f0, 0xef000000}); // mov; str r0,[r1]; udf; svc
        const uint32_t zero = 0;
        CHECK(mem_write(mem, data, &zero, sizeof(zero)));
        jit.set_reg(1, data);
        const auto before = jit.instructions_executed();
        CHECK(jit.run() < 0);
        uint32_t stored = 0;
        CHECK(mem_read(mem, data, &stored, sizeof(stored)) && stored == 7);
        CHECK(jit.get_pc() == code + 8);
        CHECK(jit.instructions_executed() - before == 2);
    }
    std::puts("ExceptionRaised: guest UDF/BKPT fail closed at the raising PC (single+region)");
}
} // namespace exception_tests
