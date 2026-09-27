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
#include <kernel/relocation.h>

#include <algorithm>
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
// A second AOT range (Thumb): movs r0,#7; itttt ne; movne r1,#1;
// movne r2,#2; udfne #0; nopne. The UDF block translates
// (A32ExceptionRaised) and must fail at run time exactly like the lazy JIT,
// with the UDF's own ITSTATE (0x1c) published.
constexpr uint32_t kTrap = 0x81050000;
constexpr uint8_t kTrapProgram[] = {0x07, 0x20, 0x1f, 0xbf, 0x01, 0x21, 0x02, 0x22, 0x00, 0xde, 0x00, 0xbf};
constexpr uint32_t kTrapUdf = kTrap + 8, kTrapCpsr = 0x30 | (0x1cu << 8);
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
        CHECK(alloc_at(mem, kTrap, 0x1000, "aot-trap") == kTrap);
        CHECK(mem_write(mem, kTrap, kTrapProgram, sizeof(kTrapProgram)));
        CHECK(mem_set_permissions(mem, kTrap, 0x1000, MemPerm::ReadExecute));
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
    spec.code.push_back({kTrap, sizeof(kTrapProgram)});
    spec.function_roots.push_back(WasmJitCPU::aot_location(kTrap | 1));
    std::vector<uint8_t> image;
    std::string report;
    // The C ablation (alone, or in F/G) drops the backward-edge budget checks
    // that bound AOT loops (the program's loops never call out): refused.
    for (const uint32_t flags : {2u, 7u, 30u}) {
        vita3k::wasmjit::set_ablate_flags(flags);
        CHECK(!WasmJitCPU::build_aot(fixture.mem, spec, image, report));
        CHECK(image.empty() && report.find("VITA3K_ABLATE") != std::string::npos);
    }
    vita3k::wasmjit::set_ablate_flags(0);
    CHECK(WasmJitCPU::build_aot(fixture.mem, spec, image, report));
    std::printf("AOT module: %s\n", report.c_str());
    // An image for the other memory width is refused by name before the
    // runtime would fail to link it (memory64 cannot import as memory32).
    std::vector<uint8_t> other = image;
    const uint8_t magic[] = { 'V', 'A', 'O', 'T' };
    const auto header = std::search(other.begin(), other.end(), std::begin(magic), std::end(magic));
    CHECK(header != other.end() && header[8] == aot_memory_bits);
    header[8] = aot_memory_bits == 64 ? 32 : 64;
    aot_test_supply(other.data(), other.size());
    CHECK(WasmJitCPU::load_aot(fixture.mem, report) == -1);
    CHECK(report.find("this runtime uses wasm") != std::string::npos);
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
// Rewrites guest code at `at` and checks that the AOT function covering it
// retires: the run must match the interpreter on the rewritten program. A
// launch-time rewrite (title patch) retires through the static entry point
// before any CPU exists; a runtime one through a CPU's cache invalidation.
void rewrite_retires_aot(uint32_t at, const std::array<uint8_t, 2> &code, bool before_cpu) {
    Final oracle;
    {
        Fixture fixture;
        CHECK(mem_set_permissions(fixture.mem, kCode, 0x1000, MemPerm::ReadWrite));
        CHECK(mem_write(fixture.mem, at, code.data(), code.size()));
        CHECK(mem_set_permissions(fixture.mem, kCode, 0x1000, MemPerm::ReadExecute));
        CPUState parent{};
        parent.mem = &fixture.mem;
        InterpreterCPU cpu(&parent, 0);
        reset(cpu, fixture.mem);
        parent.svc_called = false;
        CHECK(cpu.run() == 0 && parent.svc_called);
        oracle = capture(cpu, fixture.mem, cpu.instructions_executed());
    }
    CHECK(oracle.regs[0] != run_interpreter().regs[0]); // the rewrite is observable
    const uint64_t retired = g_aot.invalidated_functions;
    Fixture fixture;
    CHECK(mem_set_permissions(fixture.mem, kCode, 0x1000, MemPerm::ReadWrite));
    CHECK(mem_write(fixture.mem, at, code.data(), code.size()));
    CHECK(mem_set_permissions(fixture.mem, kCode, 0x1000, MemPerm::ReadExecute));
    if (before_cpu)
        WasmJitCPU::retire_aot(at, code.size());
    CPUState parent{};
    parent.mem = &fixture.mem;
    WasmJitCPU cpu(&parent, 0);
    cpu.set_region_mode(true);
    if (!before_cpu)
        cpu.invalidate_jit_cache(at, code.size());
    CHECK(g_aot.invalidated_functions > retired);
    reset(cpu, fixture.mem);
    run_to_svc(cpu, parent, 0);
    check_same(oracle, capture(cpu, fixture.mem, cpu.instructions_executed()));
    CHECK(cpu.regions_formed() > 0); // the retired function ran lazily
}
void invalidation_retires_aot() {
    // square: muls r0,r0 -> adds r0,r0,r0 (doubled, not squared)
    rewrite_retires_aot(kCode + 0x36, {0x00, 0x18}, false);
    // tail: adds r0,#3 -> adds r0,#4, as a title patch applied at launch
    rewrite_retires_aot(kCode + 0x5e, {0x04, 0x30}, true);
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
// AOT pointer roots come from the absolute values a module's relocations
// write (vita_app.cpp build_aot_image): ABS32/TARGET1 words and MOVW/MOVT
// pairs, once per pair; relative and prel31 relocations are not pointers.
void relocation_pointers() {
    constexpr uint32_t kText = 0x81070000, kData = 0x81071000;
    MemState mem{};
    CHECK(init(mem, true));
    CHECK(alloc_at(mem, kText, 0x1000, "reloc-text") == kText);
    CHECK(alloc_at(mem, kData, 0x1000, "reloc-data") == kData);
    const SegmentInfosForReloc segments{{0, {kText, kText, 0x1000}}, {1, {kData, kData, 0x1000}}};
    // Format 0: format, symbol segment, code, patch segment (code2 = 0); addend; offset.
    const auto format0 = [](uint32_t code, uint32_t addend, uint32_t offset) {
        return std::array<uint32_t, 3>{0u | 0u << 4 | code << 8 | 1u << 16, addend, offset};
    };
    std::vector<uint32_t> entries;
    for (const auto &entry : {format0(2, 0x41, 0), format0(3, 0x81, 4), format0(38, 0x80, 8), format0(42, 0x101, 12)})
        entries.insert(entries.end(), entry.begin(), entry.end());
    // Format 3: Thumb MOVW at g_offset + 4 (16), MOVT 4 bytes later, symbol segment 0.
    entries.push_back(3u | 0u << 4 | 1u << 8 | 4u << 9 | 4u << 27);
    entries.push_back(0x1235);
    std::vector<Address> pointers;
    CHECK(relocate(entries.data(), static_cast<uint32_t>(entries.size() * 4), segments, mem, false, 0, &pointers));
    CHECK((pointers == std::vector<Address>{kText + 0x41, kText + 0x80, kText + 0x1235}));
    uint32_t word = 0;
    CHECK(mem_read(mem, kData, &word, sizeof(word)) && word == kText + 0x41);
    CHECK(mem_read(mem, kData + 4, &word, sizeof(word)) && word == kText + 0x81 - (kData + 4));
    // Without a sink the same relocations still apply.
    CHECK(relocate(entries.data(), static_cast<uint32_t>(entries.size() * 4), segments, mem));
    deinit_mem(mem);
    std::puts("AOT pointer roots: absolute relocation values recorded, relative ones not");
}
// Lazy (before the module loads) and AOT runs of the trap program must fail
// the same way: the emission-time rejection error, PC on the UDF, and the
// MOVS before it executed.
struct TrapOutcome {
    std::string error;
    uint32_t pc = 0, cpsr = 0, r0 = 0, r1 = 0, r2 = 0;
    uint64_t regions = 0;
    bool operator==(const TrapOutcome &) const = default;
};
TrapOutcome run_trap() {
    Fixture fixture;
    CPUState parent{};
    parent.mem = &fixture.mem;
    WasmJitCPU cpu(&parent, 0);
    cpu.set_region_mode(true);
    cpu.set_reg(0, 0);
    cpu.set_pc(kTrap | 1);
    cpu.set_cpsr(0x30);
    CHECK(cpu.run() < 0);
    const TrapOutcome first{cpu.get_last_error(), cpu.get_pc(), cpu.get_cpsr(), cpu.get_reg(0),
        cpu.get_reg(1), cpu.get_reg(2), cpu.regions_formed()};
    CHECK(cpu.run() < 0); // a retry raises again at the same state
    CHECK((TrapOutcome{cpu.get_last_error(), cpu.get_pc(), cpu.get_cpsr(), cpu.get_reg(0),
        cpu.get_reg(1), cpu.get_reg(2), first.regions}) == first);
    return first;
}
} // namespace

// A module's code ends at its .ARM.exidx end-of-code sentinel (a final
// EXIDX_CANTUNWIND entry); the rest of the text segment is data and must not
// become AOT code. Tables without that sentinel keep the whole segment.
void code_size_from_exidx() {
    constexpr uint32_t kText = 0x81060000, kSize = 0x1000, kTable = kText + 0x800;
    MemState mem{};
    CHECK(init(mem, true));
    CHECK(alloc_at(mem, kText, kSize, "aot-exidx") == kText);
    // prel31 word from an entry to a function start (negative: code below).
    const auto entry = [](uint32_t at, uint32_t function, uint32_t second) {
        return std::array<uint32_t, 2>{(function - at) & 0x7fffffffu, second};
    };
    const auto table = [&](std::initializer_list<std::array<uint32_t, 3>> rows) {
        uint32_t at = kTable;
        for (const auto &[function, second, unused] : rows) {
            (void)unused;
            const auto words = entry(at, function, second);
            CHECK(mem_write(mem, at, words.data(), sizeof(words)));
            at += 8;
        }
        return at;
    };
    // No module info in the segment (0 here) bounds nothing.
    const auto code_size = [&](uint32_t end, uint32_t module_info = 0) {
        return WasmJitCPU::aot_code_size(mem, kText, kSize, kTable, end, module_info);
    };
    // Thumb functions (one mid-table EXIDX_CANTUNWIND), 16-byte ARM import
    // stubs, then the sentinel at the first byte past the last stub.
    uint32_t end = table({{kText | 1, 0x80b0b0b0u, 0}, {kText + 0x101, 1, 0}, {kText + 0x200, 0x80b0b0b0u, 0},
        {kText + 0x210, 0x80b0b0b0u, 0}, {kText + 0x220, 1, 0}});
    CHECK(code_size(end) == 0x220);
    // The module info never extends the code past the sentinel.
    CHECK(code_size(end, kText + 0x300) == 0x220);
    // The same table without the sentinel: the last stub's end is unknown,
    // unless the module info follows it (SceLibft2, SceSysmodule).
    CHECK(code_size(end - 8) == kSize);
    CHECK(code_size(end - 8, kText + 0x220) == 0x220);
    // Module info below the last function start, at the segment end or
    // outside the segment is not an end of code.
    CHECK(code_size(end - 8, kText + 0x210) == kSize);
    CHECK(code_size(end - 8, kText + kSize) == kSize);
    CHECK(code_size(end - 8, kText - 0x100) == kSize);
    // A final Thumb or out-of-segment CANTUNWIND target is not a sentinel;
    // the table then ends with a function that the module info can bound
    // (SceLibPvf, SceLibSsl end with an unwound Thumb function).
    end = table({{kText | 1, 0x80b0b0b0u, 0}, {kText + 0x221, 1, 0}});
    CHECK(code_size(end) == kSize);
    CHECK(code_size(end, kText + 0x300) == 0x300);
    end = table({{kText | 1, 0x80b0b0b0u, 0}, {kText + kSize + 4, 1, 0}});
    CHECK(code_size(end) == kSize);
    CHECK(code_size(end, kText + 0x300) == kSize);
    // Empty, single-entry and misaligned tables keep the whole segment.
    CHECK(code_size(kTable, kText + 0x300) == kSize);
    CHECK(code_size(kTable + 8, kText + 0x300) == kSize);
    CHECK(code_size(kTable + 20, kText + 0x300) == kSize);
    // Function starts: the Thumb bit as recorded, and restored for an entry
    // that lacks it at a halfword-aligned start or at a conditional ARM
    // word (SceLibc 8035b520: push {r0,r4,lr}; subs r2,#32 reads as ARM
    // 0x3a20b511, "bcc"). AL and unconditional-space words stay ARM, and
    // an inline-unwind first word (bit 31) is not a function.
    const std::array<uint32_t, 4> starts{0xe92d4010u, 0x3a20b511u, 0xf5d1f000u, 0x0000bf00u};
    CHECK(mem_write(mem, kText + 0x400, starts.data(), sizeof(starts)));
    end = table({{kText | 1, 0x80b0b0b0u, 0}, {kText + 0x400, 1, 0}, {kText + 0x404, 1, 0}, {kText + 0x408, 1, 0},
        {kText + 0x40e, 1, 0}});
    const uint32_t skipped = 0x80000000u;
    CHECK(mem_write(mem, end, &skipped, sizeof(skipped)));
    CHECK((WasmJitCPU::aot_exidx_functions(mem, kTable, end + 8)
        == std::vector<uint32_t>{kText | 1, kText + 0x400, kText + 0x405, kText + 0x408, kText + 0x40f}));
    CHECK(WasmJitCPU::aot_exidx_functions(mem, kTable, kTable).empty());
    CHECK(WasmJitCPU::aot_exidx_functions(mem, kTable + 8, kTable).empty());
    deinit_mem(mem);
    std::puts("AOT code size: the exidx end-of-code sentinel and the module info bound the text segment; exidx Thumb bits restored");
}

int main() {
    code_size_from_exidx();
    relocation_pointers();
    const Final oracle = run_interpreter();
    // Lazy slices must fit the largest block (64 instructions); smaller
    // ones make no progress by contract.
    for (const uint64_t slice : {0u, 64u, 65u, 71u, 97u, 128u})
        check_same(oracle, run_lazy(slice));
    write_epochs_recorded(false);
    const TrapOutcome lazy_trap = run_trap();
    CHECK(lazy_trap.error == "unsupported Dynarmic IR or terminal (no fallback)");
    CHECK(lazy_trap.pc == kTrapUdf && lazy_trap.cpsr == kTrapCpsr && lazy_trap.regions != 0);
    CHECK(lazy_trap.r0 == 7 && lazy_trap.r1 == 1 && lazy_trap.r2 == 2);
    build_and_load();
    TrapOutcome aot_trap = run_trap();
    CHECK(aot_trap.regions == 0); // the AOT function raised, not a lazy region
    aot_trap.regions = lazy_trap.regions;
    CHECK(aot_trap == lazy_trap);
    check_same(oracle, run_aot(0));
    for (const uint64_t slice : {1u, 2u, 3u, 7u, 50u, 1000u})
        check_same(oracle, run_aot(slice));
    write_epochs_recorded(true);
    vector_min_max_flags();
    tls_addr_intrinsic();
    invalidation_retires_aot();
    std::printf("AOT module: %u checks passed (interpreter oracle, lazy JIT, AOT with slices, write epochs, invalidation, VMIN/VMAX flags, TLS intrinsic, UDF trap)\n", checks);
    return 0;
}
