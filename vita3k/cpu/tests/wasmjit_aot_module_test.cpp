// AOT module equivalence (vita3k/cpu/src/wasmjit/AOT.md).
//
// A small Thumb-2 program (calls, returns through BX LR and POP {PC}, a loop,
// an IT block, a tail jump, loads and stores) runs on the interpreter oracle,
// on the lazy region JIT and on an AOT module built from the same guest
// memory. Final registers, the full CPSR and FPSCR, memory and instruction
// counts must agree, including when the AOT run is cut into tiny scheduler
// slices so every re-entry goes through the lookup table into the middle of a
// function.
#include "../src/wasm_jit_cpu.cpp"
#include <cpu/impl/interpreter_cpu.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
unsigned checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); std::abort(); } } while (false)

constexpr uint32_t kCode = 0x81000000, kData = 0x81010000, kStack = 0x81020000;
constexpr uint32_t kStackTop = kStack + 0x1000;
// arm-vita-eabi-as -mcpu=cortex-a9 of:
//   main:     push {r4,r5,lr}; movs r4,#0; movs r5,#0
//   loop:     mov r0,r4; bl square
//             cmp r4,#50; itt gt; addgt r0,r0,#1; addgt r5,r5,#3
//             ldr r1,=0x81010000; lsls r2,r4,#2; str r0,[r1,r2]
//             adds r4,#1; cmp r4,#100; blt loop
//             bl sum; mov r4,r0; movs r0,#7; bl classify
//             add r0,r0,r5; add r0,r0,r4; pop {r4,r5,lr}; b done
//   square:   muls r0,r0; bx lr
//   sum:      push {r4,lr}; ldr r1,=0x81010000; movs r0,#0; movs r2,#0
//   1:        ldr.w r3,[r1,r2,lsl #2]; adds r0,r0,r3; adds r2,#1; cmp r2,#100; bne 1b
//             pop {r4,pc}
//   classify: cmp r0,#5; ite gt; movgt r1,#1; movle r1,#2
//             add.w r0,r0,r1,lsl #4; b tail
//   tail:     adds r0,#3; bx lr
//   done:     svc #0
//             .ltorg (0x81010000)
// The ITT block inside the loop is a conditional block entered with IT != 0
// whose end mode differs from its entry mode: exits right after it must
// publish IT = 0 (see Emitter::location_member).
constexpr uint8_t kProgram[] = {
    0x30, 0xb5, 0x00, 0x24, 0x00, 0x25, 0x20, 0x46, 0x00, 0xf0, 0x15, 0xf8,
    0x32, 0x2c, 0xc4, 0xbf, 0x01, 0x30, 0x03, 0x35, 0x13, 0x49, 0xa2, 0x00,
    0x88, 0x50, 0x01, 0x34, 0x64, 0x2c, 0xf2, 0xdb, 0x00, 0xf0, 0x0b, 0xf8,
    0x04, 0x46, 0x07, 0x20, 0x00, 0xf0, 0x12, 0xf8, 0x28, 0x44, 0x20, 0x44,
    0xbd, 0xe8, 0x30, 0x40, 0x15, 0xe0, 0x40, 0x43, 0x70, 0x47, 0x10, 0xb5,
    0x09, 0x49, 0x00, 0x20, 0x00, 0x22, 0x51, 0xf8, 0x22, 0x30, 0xc0, 0x18,
    0x01, 0x32, 0x64, 0x2a, 0xf9, 0xd1, 0x10, 0xbd, 0x05, 0x28, 0xcc, 0xbf,
    0x01, 0x21, 0x02, 0x21, 0x00, 0xeb, 0x01, 0x10, 0xff, 0xe7, 0x03, 0x30,
    0x70, 0x47, 0x00, 0xdf, 0x00, 0x00, 0x01, 0x81,
};
constexpr uint32_t kDone = kCode + 0x62;
// classify(7) + 3 * 49 + sum(i*i + (i > 50)) for i < 100
constexpr uint32_t kResult = 26 + 147 + 328350 + 49;

// Non-default entry state the program never writes: the Q flag, GE bits and
// user mode in the CPSR, and FPSCR NZCV plus every cumulative exception flag.
// None of these is part of the location key (IT, T, E and the FPSCR mode are),
// so the AOT lookup still matches, and every run must hand them back as-is.
constexpr uint32_t kEntryCpsr = (1u << 27) | (0x5u << 16) | 0x20 | 0x10;
constexpr uint32_t kEntryFpscr = 0xa0000000u | 0x9f;

struct Fixture {
    MemState mem{};
    Fixture() {
        CHECK(init(mem, true));
        CHECK(alloc_at(mem, kCode, 0x1000, "aot-code") == kCode);
        CHECK(alloc_at(mem, kData, 0x1000, "aot-data") == kData);
        CHECK(alloc_at(mem, kStack, 0x1000, "aot-stack") == kStack);
        CHECK(mem_write(mem, kCode, kProgram, sizeof(kProgram)));
        CHECK(mem_set_permissions(mem, kCode, 0x1000, MemPerm::ReadExecute));
    }
    ~Fixture() { deinit_mem(mem); }
};

struct Final {
    std::array<uint32_t, 16> regs{};
    uint32_t cpsr = 0, fpscr = 0; // full architectural words
    std::array<uint32_t, 100> table{};
    uint64_t executed = 0;
};

template <typename Cpu>
void reset(Cpu &cpu, MemState &mem) {
    for (unsigned r = 0; r < 16; ++r)
        cpu.set_reg(r, 0x1000 + r);
    cpu.set_sp(kStackTop);
    cpu.set_lr(0xdeadbeef);
    cpu.set_pc(kCode);
    cpu.set_cpsr(kEntryCpsr);
    cpu.set_fpscr(kEntryFpscr);
    std::array<uint32_t, 100> zero{};
    CHECK(mem_write(mem, kData, zero.data(), sizeof(zero)));
}

template <typename Cpu>
Final capture(Cpu &cpu, MemState &mem, uint64_t executed) {
    Final out;
    for (unsigned r = 0; r < 16; ++r)
        out.regs[r] = cpu.get_reg(r);
    out.cpsr = cpu.get_cpsr();
    out.fpscr = cpu.get_fpscr();
    CHECK(mem_read(mem, kData, out.table.data(), sizeof(out.table)));
    out.executed = executed;
    return out;
}

void check_same(const Final &a, const Final &b) {
    for (unsigned r = 0; r < 16; ++r) {
        if (a.regs[r] != b.regs[r])
            std::fprintf(stderr, "r%u: %08x vs %08x\n", r, a.regs[r], b.regs[r]);
        CHECK(a.regs[r] == b.regs[r]);
    }
    if (a.cpsr != b.cpsr || a.fpscr != b.fpscr)
        std::fprintf(stderr, "cpsr %08x vs %08x, fpscr %08x vs %08x\n", a.cpsr, b.cpsr, a.fpscr, b.fpscr);
    CHECK(a.cpsr == b.cpsr);
    CHECK(a.fpscr == b.fpscr);
    CHECK(a.table == b.table);
    CHECK(a.executed == b.executed);
}

Final run_interpreter() {
    Fixture fixture;
    CPUState parent{};
    parent.mem = &fixture.mem;
    InterpreterCPU cpu(&parent, 0);
    reset(cpu, fixture.mem);
    parent.svc_called = false;
    CHECK(cpu.run() == 0 && parent.svc_called);
    CHECK(cpu.get_pc() == kDone + 2);
    const Final out = capture(cpu, fixture.mem, cpu.instructions_executed());
    CHECK(out.regs[0] == kResult && out.regs[5] == 0x1005); // main restores r4/r5
    CHECK((out.cpsr & 0x0fffffffu) == kEntryCpsr && out.fpscr == kEntryFpscr);
    for (uint32_t i = 0; i < 100; ++i)
        CHECK(out.table[i] == i * i + (i > 50));
    return out;
}

// Drives run() or scheduler slices until the terminating SVC.
void run_to_svc(WasmJitCPU &cpu, CPUState &parent, uint64_t slice) {
    parent.svc_called = false;
    if (!slice) {
        CHECK(cpu.run() == 0 && parent.svc_called);
        return;
    }
    for (unsigned slices = 0;; ++slices) {
        const int rc = cpu.run_slice(slice);
        if (rc == 0 && parent.svc_called)
            return;
        CHECK(rc == WasmJitCPU::slice_yield);
        CHECK(slices < 1000000);
    }
}

// Lazy region JIT; small slices exit at every chained edge, which is where a
// stale IT state would be published.
Final run_lazy(uint64_t slice) {
    Fixture fixture;
    CPUState parent{};
    parent.mem = &fixture.mem;
    WasmJitCPU cpu(&parent, 0);
    cpu.set_region_mode(true);
    reset(cpu, fixture.mem);
    run_to_svc(cpu, parent, slice);
    return capture(cpu, fixture.mem, cpu.instructions_executed());
}

EM_JS(void, aot_test_supply, (const uint8_t *bytes, size_t length), {
    Module['vita3kAotModule'] = new WebAssembly.Module(Module["vita3kHostBytes"](bytes, Number(length)).slice());
});

// Builds the module once from a fixture's memory; the loaded module is
// process-wide, so later fixtures must map identical code at the same place.
void build_and_load() {
    Fixture fixture;
    WasmJitCPU::AotBuildSpec spec;
    spec.code.push_back({kCode, sizeof(kProgram) & ~1u});
    spec.function_roots.push_back(WasmJitCPU::aot_location(kCode | 1));
    std::vector<uint8_t> image;
    std::string report;
    CHECK(WasmJitCPU::build_aot(fixture.mem, spec, image, report));
    std::printf("AOT module: %s\n", report.c_str());
    aot_test_supply(image.data(), image.size());
    CHECK(WasmJitCPU::load_aot(fixture.mem, report) == 1);
    std::printf("%s\n", report.c_str());
}

// Budgeted AOT run: `slice` > 0 cuts execution into scheduler slices.
Final run_aot(uint64_t slice) {
    Fixture fixture;
    CPUState parent{};
    parent.mem = &fixture.mem;
    WasmJitCPU cpu(&parent, 0);
    cpu.set_region_mode(true);
    reset(cpu, fixture.mem);
    run_to_svc(cpu, parent, slice);
    // Everything ran as AOT: the lazy path formed no region.
    CHECK(cpu.regions_formed() == 0);
    return capture(cpu, fixture.mem, cpu.instructions_executed());
}
// VMIN/VMAX.F32 under the standard FPSCR on the lazy (exact FP) path:
// signaling NaN -> default NaN + IOC, denormal input flushed + IDC.
//   vldr d1, lit1; vldr d2, lit2; vmin.f32 d0,d1,d2; vmax.f32 d3,d1,d2
//   vstr d0,[r0]; vstr d3,[r0,#8]; vmrs r1,fpscr; svc #0
//   lit1: 0x7f800001, 1.0f   lit2: 2.0f, 0x00000001
void vector_min_max_flags() {
    constexpr uint32_t kNeon = 0x81030000;
    constexpr uint8_t kNeonProgram[] = {
        0x06, 0x1b, 0x9f, 0xed, 0x07, 0x2b, 0x9f, 0xed, 0x02, 0x0f, 0x21, 0xf2,
        0x02, 0x3f, 0x01, 0xf2, 0x00, 0x0b, 0x80, 0xed, 0x02, 0x3b, 0x80, 0xed,
        0x10, 0x1a, 0xf1, 0xee, 0x00, 0x00, 0x00, 0xef, 0x01, 0x00, 0x80, 0x7f,
        0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00,
    };
    Fixture fixture;
    CHECK(alloc_at(fixture.mem, kNeon, 0x1000, "aot-neon") == kNeon);
    CHECK(mem_write(fixture.mem, kNeon, kNeonProgram, sizeof(kNeonProgram)));
    CHECK(mem_set_permissions(fixture.mem, kNeon, 0x1000, MemPerm::ReadExecute));
    CPUState parent{};
    parent.mem = &fixture.mem;
    WasmJitCPU cpu(&parent, 0);
    cpu.set_region_mode(true);
    cpu.set_reg(0, kData);
    cpu.set_pc(kNeon);
    cpu.set_cpsr(0x10);
    cpu.set_fpscr(0);
    run_to_svc(cpu, parent, 0);
    std::array<uint32_t, 4> lanes{};
    CHECK(mem_read(fixture.mem, kData, lanes.data(), sizeof(lanes)));
    CHECK(lanes[0] == 0x7fc00000u && lanes[1] == 0x00000000u); // vmin
    CHECK(lanes[2] == 0x7fc00000u && lanes[3] == 0x3f800000u); // vmax
    CHECK((cpu.get_reg(1) & 0x81u) == 0x81u);                   // IDC | IOC
}

// Guest code that changes after the AOT module loaded must not keep running
// from the module: invalidate_jit_cache retires the overlapping functions.
// square's MULS r0,r0 (+0x36) becomes ADDS r0,r0,r0.
void invalidation_retires_aot() {
    constexpr uint32_t kSquare = kCode + 0x36;
    constexpr uint8_t kAdds[] = {0x00, 0x18};
    Final oracle;
    {
        Fixture fixture;
        CHECK(mem_set_permissions(fixture.mem, kCode, 0x1000, MemPerm::ReadWrite));
        CHECK(mem_write(fixture.mem, kSquare, kAdds, sizeof(kAdds)));
        CHECK(mem_set_permissions(fixture.mem, kCode, 0x1000, MemPerm::ReadExecute));
        CPUState parent{};
        parent.mem = &fixture.mem;
        InterpreterCPU cpu(&parent, 0);
        reset(cpu, fixture.mem);
        parent.svc_called = false;
        CHECK(cpu.run() == 0 && parent.svc_called);
        oracle = capture(cpu, fixture.mem, cpu.instructions_executed());
        CHECK(oracle.table[3] == 6); // doubled, not squared
    }
    Fixture fixture;
    CHECK(mem_set_permissions(fixture.mem, kCode, 0x1000, MemPerm::ReadWrite));
    CHECK(mem_write(fixture.mem, kSquare, kAdds, sizeof(kAdds)));
    CHECK(mem_set_permissions(fixture.mem, kCode, 0x1000, MemPerm::ReadExecute));
    CPUState parent{};
    parent.mem = &fixture.mem;
    WasmJitCPU cpu(&parent, 0);
    cpu.set_region_mode(true);
    cpu.invalidate_jit_cache(kSquare, sizeof(kAdds));
    reset(cpu, fixture.mem);
    run_to_svc(cpu, parent, 0);
    check_same(oracle, capture(cpu, fixture.mem, cpu.instructions_executed()));
    CHECK(cpu.regions_formed() > 0); // the retired function ran lazily
}
// sceKernelGetTLSAddr intrinsic: an in-range key is answered in Wasm
// (TPIDRURO - 0x800 + 4*key, no SVC); an out-of-range key takes the SVC.
//   mov r0,#5; bl stub; mov r4,r0; mov r0,#0x200; bl stub; svc #1
//   stub: svc #0; mov pc,lr; .word 0xB295EB61
void tls_addr_intrinsic() {
    constexpr uint32_t kTls = 0x81040000, kTpidruro = 0x81123800;
    constexpr uint8_t kTlsProgram[] = {
        0x05, 0x00, 0xa0, 0xe3, 0x03, 0x00, 0x00, 0xeb, 0x00, 0x40, 0xa0, 0xe1,
        0x02, 0x0c, 0xa0, 0xe3, 0x00, 0x00, 0x00, 0xeb, 0x01, 0x00, 0x00, 0xef,
        0x00, 0x00, 0x00, 0xef, 0x0e, 0xf0, 0xa0, 0xe1, 0x61, 0xeb, 0x95, 0xb2,
    };
    Fixture fixture;
    CHECK(alloc_at(fixture.mem, kTls, 0x1000, "aot-tls") == kTls);
    CHECK(mem_write(fixture.mem, kTls, kTlsProgram, sizeof(kTlsProgram)));
    CHECK(mem_set_permissions(fixture.mem, kTls, 0x1000, MemPerm::ReadExecute));
    CPUState parent{};
    parent.mem = &fixture.mem;
    WasmJitCPU cpu(&parent, 0);
    cpu.set_region_mode(true);
    cpu.set_pc(kTls);
    cpu.set_cpsr(0x10);
    cpu.set_tpidruro(kTpidruro);
    parent.svc_called = false;
    CHECK(cpu.run() == 0 && parent.svc_called);
    // The in-range call completed in Wasm; the first SVC is the 0x200 one.
    CHECK(parent.svc == 0 && cpu.get_pc() == kTls + 0x1c);
    CHECK(cpu.get_reg(4) == kTpidruro - 0x800 + 4 * 5);
    CHECK(cpu.get_reg(0) == 0x200);
}
// Write tracking (mem_mark_written): guest stores through lazy regions and
// through the AOT module record the current write epoch on the pages they
// touch (the data table, the stack) and leave the code page alone.
void write_epochs_recorded(bool aot) {
    Fixture fixture;
    CPUState parent{};
    parent.mem = &fixture.mem;
    WasmJitCPU cpu(&parent, 0);
    cpu.set_region_mode(true);
    reset(cpu, fixture.mem);
    fixture.mem.write_epoch = 77; // reset's own mem_write carries the old epoch
    run_to_svc(cpu, parent, 0);
    CHECK(fixture.mem.write_epochs[kData >> 12] == 77);
    CHECK(fixture.mem.write_epochs[(kStackTop - 4) >> 12] == 77);
    CHECK(fixture.mem.write_epochs[kCode >> 12] != 77);
    CHECK((cpu.regions_formed() == 0) == aot);
}
} // namespace

int main() {
    const Final oracle = run_interpreter();
    // Lazy slices must fit the largest block (64 instructions); smaller
    // ones make no progress by contract.
    for (const uint64_t slice : {0u, 64u, 65u, 71u, 97u, 128u})
        check_same(oracle, run_lazy(slice));
    write_epochs_recorded(false);
    build_and_load();
    check_same(oracle, run_aot(0));
    for (const uint64_t slice : {1u, 2u, 3u, 7u, 50u, 1000u})
        check_same(oracle, run_aot(slice));
    write_epochs_recorded(true);
    vector_min_max_flags();
    tls_addr_intrinsic();
    invalidation_retires_aot();
    std::printf("AOT module: %u checks passed (interpreter oracle, lazy JIT, AOT with slices, write epochs, invalidation, VMIN/VMAX flags, TLS intrinsic)\n", checks);
    return 0;
}
