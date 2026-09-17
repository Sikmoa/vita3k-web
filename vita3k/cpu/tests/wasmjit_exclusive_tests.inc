// Included by wasmjit_backend_test.cpp. Exclusive load/store semantics.
//
// The JitState reservation is a single (address, width, value) monitor. A
// STREX-family instruction publishes 0 only when the reservation still names
// the same address and width AND memory still holds the value the exclusive
// read observed; otherwise it publishes 1 and memory is untouched. That is the
// compare-and-swap Dynarmic's native backend performs through ExclusiveMonitor
// plus MemoryWriteExclusive, and it is what makes a lost update between LDREX
// and STREX observable to the guest.
namespace exclusive_tests {
using namespace vector_tests;

constexpr uint32_t address = data + page + 0x40;

// ARM encodings, checked against dynarmic's decoder patterns in
// frontend/A32/decoder/arm.inc. All conditional fields are AL.
constexpr uint32_t arm_ldrex(unsigned n, unsigned t) { return 0xE1900F9Fu | (n << 16) | (t << 12); }
constexpr uint32_t arm_strex(unsigned d, unsigned t, unsigned n) { return 0xE1800F90u | (n << 16) | (d << 12) | t; }
constexpr uint32_t arm_ldrexb(unsigned n, unsigned t) { return 0xE1D00F9Fu | (n << 16) | (t << 12); }
constexpr uint32_t arm_strexb(unsigned d, unsigned t, unsigned n) { return 0xE1C00F90u | (n << 16) | (d << 12) | t; }
constexpr uint32_t arm_ldrexh(unsigned n, unsigned t) { return 0xE1F00F9Fu | (n << 16) | (t << 12); }
constexpr uint32_t arm_strexh(unsigned d, unsigned t, unsigned n) { return 0xE1E00F90u | (n << 16) | (d << 12) | t; }
constexpr uint32_t arm_ldrexd(unsigned n, unsigned t) { return 0xE1B00F9Fu | (n << 16) | (t << 12); }
constexpr uint32_t arm_strexd(unsigned d, unsigned t, unsigned n) { return 0xE1A00F90u | (n << 16) | (d << 12) | t; }
constexpr uint32_t arm_clrex = 0xF57FF01Fu;
constexpr uint32_t arm_svc = 0xEF000000u;
// ADD/STR/MOV with plain 8-bit immediates (no rotation needed for these).
constexpr uint32_t arm_add_imm(unsigned d, unsigned n, unsigned imm) { return 0xE2800000u | (n << 16) | (d << 12) | (imm & 0xff); }
constexpr uint32_t arm_mov_imm(unsigned d, unsigned imm) { return 0xE3A00000u | (d << 12) | (imm & 0xff); }
constexpr uint32_t arm_str(unsigned t, unsigned n) { return 0xE5800000u | (n << 16) | (t << 12); }

// Thumb-32 halfword pairs packed little-endian into one word
// (frontend/A32/decoder/thumb32.inc). The LDREX spelling is the one the retail
// run rejected: halfwords E856 0F00 = "ldrex r0, [r6]".
constexpr uint32_t thumb_ldrex(unsigned n, unsigned t) { return (((t << 12) | 0x0F00u) << 16) | (0xE850u | n); }
constexpr uint32_t thumb_strex(unsigned d, unsigned t, unsigned n) { return ((((t << 12) | (d << 8))) << 16) | (0xE840u | n); }
constexpr uint32_t thumb_svc = 0x0000DF00u;

// Run one guest program through the real translator in both region and
// single-block mode. `setup` prepares inputs, `expect` turns the state into the
// expected post-state, `after` re-checks memory once the program has run.
template <typename Setup, typename Expect, typename After>
void program_case(MemState &mem, WasmJitCPU &jit, CPUState &parent, bool thumb,
    std::initializer_list<uint32_t> program, Setup setup, Expect expect, After after) {
    CHECK(mem_write(mem, code, program.begin(), program.size() * sizeof(uint32_t)));
    jit.invalidate_jit_cache(code, page);
    jit.set_cpsr(thumb ? 0x30 : 0x10);
    jit.set_pc(thumb ? (code | 1) : code);
    auto state = initial(2);
    auto context = jit.save_context();
    setup(state);
    std::copy_n(state.regs, 15, context.cpu_registers.begin());
    std::memcpy(context.fpu_registers.data(), state.fpu, sizeof(state.fpu));
    context.cpsr = state.cpsr | (thumb ? 0x20 : 0);
    context.fpscr = state.fpscr;
    jit.load_context(context);
    expect(state);
    if (jit.run() != 0) {
        std::fprintf(stderr, "Exclusive guest failed (%s): %s\n",
            thumb ? "Thumb" : "ARM", jit.get_last_error().c_str());
        CHECK(false);
        return;
    }
    CHECK(parent.svc_called);
    const auto result = jit.save_context();
    for (unsigned i = 0; i < 15; ++i) CHECK(result.cpu_registers[i] == state.regs[i]);
    CHECK(result.cpsr == context.cpsr && result.fpscr == context.fpscr);
    after();
}

uint32_t read_word(MemState &mem, uint32_t at) {
    uint32_t value = 0;
    CHECK(mem_read(mem, at, &value, sizeof(value)));
    return value;
}

void guest_exclusive(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    for (bool region : {false, true}) {
        jit.set_region_mode(region);

        // LDREX/STREX round trip: the store succeeds and publishes 0.
        program_case(mem, jit, parent, false,
            {arm_ldrex(1, 2), arm_add_imm(2, 2, 1), arm_strex(3, 2, 1), arm_svc},
            [&](auto &s) {
                s.regs[1] = address;
                const uint32_t value = 0x11223344;
                CHECK(mem_write(mem, address, &value, sizeof(value)));
            },
            [&](auto &s) { s.regs[2] = 0x11223345; s.regs[3] = 0; },
            [&]() { CHECK(read_word(mem, address) == 0x11223345); });

        // A plain store between LDREX and STREX must make STREX fail: the
        // reservation is only valid while memory still holds the read value.
        program_case(mem, jit, parent, false,
            {arm_ldrex(1, 2), arm_mov_imm(4, 0x99), arm_str(4, 1), arm_strex(3, 2, 1), arm_svc},
            [&](auto &s) {
                s.regs[1] = address;
                const uint32_t value = 0x11223344;
                CHECK(mem_write(mem, address, &value, sizeof(value)));
            },
            [&](auto &s) { s.regs[2] = 0x11223344; s.regs[3] = 1; s.regs[4] = 0x99; },
            [&]() { CHECK(read_word(mem, address) == 0x99); });

        // STREX without any LDREX fails and leaves memory alone.
        program_case(mem, jit, parent, false,
            {arm_clrex, arm_mov_imm(2, 0x55), arm_strex(3, 2, 1), arm_svc},
            [&](auto &s) {
                s.regs[1] = address;
                const uint32_t value = 0xAABBCCDD;
                CHECK(mem_write(mem, address, &value, sizeof(value)));
            },
            [&](auto &s) { s.regs[2] = 0x55; s.regs[3] = 1; },
            [&]() { CHECK(read_word(mem, address) == 0xAABBCCDD); });

        // CLREX drops a live reservation, so the following STREX fails.
        program_case(mem, jit, parent, false,
            {arm_ldrex(1, 2), arm_clrex, arm_strex(3, 2, 1), arm_svc},
            [&](auto &s) {
                s.regs[1] = address;
                const uint32_t value = 0x01020304;
                CHECK(mem_write(mem, address, &value, sizeof(value)));
            },
            [&](auto &s) { s.regs[2] = 0x01020304; s.regs[3] = 1; },
            [&]() { CHECK(read_word(mem, address) == 0x01020304); });

        // Byte exclusive round trip: the loaded byte is zero-extended, the
        // store writes only the low byte and succeeds.
        program_case(mem, jit, parent, false,
            {arm_ldrexb(1, 2), arm_strexb(3, 2, 1), arm_svc},
            [&](auto &s) {
                s.regs[1] = address;
                const uint8_t value = 0xAB;
                CHECK(mem_write(mem, address, &value, sizeof(value)));
            },
            [&](auto &s) { s.regs[2] = 0xAB; s.regs[3] = 0; },
            [&]() { CHECK((read_word(mem, address) & 0xff) == 0xAB); });

        // Halfword exclusive round trip.
        program_case(mem, jit, parent, false,
            {arm_ldrexh(1, 2), arm_strexh(3, 2, 1), arm_svc},
            [&](auto &s) {
                s.regs[1] = address;
                const uint16_t value = 0xBEEF;
                CHECK(mem_write(mem, address, &value, sizeof(value)));
            },
            [&](auto &s) { s.regs[2] = 0xBEEF; s.regs[3] = 0; },
            [&]() { CHECK((read_word(mem, address) & 0xffff) == 0xBEEF); });

        // Doubleword exclusive: LDREXD fills Rt/Rt+1, STREXD stores both words
        // and publishes success to a THIRD register.
        program_case(mem, jit, parent, false,
            {arm_ldrexd(1, 2), arm_strexd(0, 2, 1), arm_svc},
            [&](auto &s) {
                s.regs[1] = address;
                const uint64_t value = 0x0123456789ABCDEFull;
                CHECK(mem_write(mem, address, &value, sizeof(value)));
            },
            [&](auto &s) { s.regs[2] = 0x89ABCDEF; s.regs[3] = 0x01234567; s.regs[0] = 0; },
            [&]() {
                const uint64_t stored = uint64_t(read_word(mem, address))
                    | (uint64_t(read_word(mem, address + 4)) << 32);
                CHECK(stored == 0x0123456789ABCDEFull);
            });

        // A 64-bit lost update must fail the pair as well (word 0 changed).
        program_case(mem, jit, parent, false,
            {arm_ldrexd(1, 2), arm_mov_imm(4, 0x11), arm_str(4, 1), arm_strexd(0, 2, 1), arm_svc},
            [&](auto &s) {
                s.regs[1] = address;
                const uint64_t value = 0x0123456789ABCDEFull;
                CHECK(mem_write(mem, address, &value, sizeof(value)));
            },
            [&](auto &s) { s.regs[2] = 0x89ABCDEF; s.regs[3] = 0x01234567; s.regs[4] = 0x11; s.regs[0] = 1; },
            [&]() { CHECK(read_word(mem, address) == 0x11); });

        // The retail blocker itself: Thumb-32 LDREX r0,[r6] + STREX r2,r0,[r1].
        program_case(mem, jit, parent, true,
            {thumb_ldrex(6, 0), thumb_strex(2, 0, 1), thumb_svc},
            [&](auto &s) {
                s.regs[1] = address;
                s.regs[6] = address;
                const uint32_t value = 0x55667788;
                CHECK(mem_write(mem, address, &value, sizeof(value)));
            },
            [&](auto &s) { s.regs[0] = 0x55667788; s.regs[2] = 0; },
            [&]() { CHECK(read_word(mem, address) == 0x55667788); });

        std::printf("Exclusive load/store: %s mode passed\n", region ? "region" : "single-block");
    }
}
} // namespace exclusive_tests
