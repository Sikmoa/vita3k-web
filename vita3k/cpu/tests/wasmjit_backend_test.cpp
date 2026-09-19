// Focused wasm32/Memory64 backend integration tests. Compile this TU INSTEAD OF
// wasm_jit_cpu.cpp so the anonymous checked helpers can also be tested directly.
// Uses production MemState and Dynarmic translation; no interpreter or mocks.
#include "../src/wasm_jit_cpu.cpp"
#include <mem/ptr.h>
#include <dynarmic/frontend/A32/a32_types.h>
#include <dynarmic/ir/opcodes.h>
#include <dynarmic/common/fp/op.h>
#include <dynarmic/common/fp/fpcr.h>
#include <dynarmic/common/fp/fpsr.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {
unsigned checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); std::abort(); } } while (false)
constexpr Address code = 0x81000000, data = 0x82000000;
constexpr uint32_t page = 4096;

void helpers(MemState &mem) {
    JitState state{};
    // Non-observer helper ABI: architectural/cache-owned fields must not be
    // mutated even when helpers fault. Their memory copy may be stale during
    // a promoted region invocation, and a helper must not make it authoritative.
    for (unsigned i = 0; i < 16; ++i) state.regs[i] = 0xabc00000 + i;
    state.cpsr = 0xf80f00d0;
    state.executed = 0xfffffff0;
    state.next_pc = 0x81001234;
    state.dispatches = 37;
    const auto architectural = state;
    state.memory_cookie = reinterpret_cast<uintptr_t>(&mem);
    const std::array<uint32_t, 4> lanes{0x76543210, 0xfedcba98, 0x89abcdef, 0x01234567};
    // Each valid size is deliberately unaligned; wider accesses cross pages.
    for (uint32_t bytes : {1, 2, 4, 8, 16}) {
        const Address address = data + page - 1;
        std::copy(lanes.begin(), lanes.end(), state.memory_value);
        CHECK(checked_memory_write(&state, address, bytes) == 0);
        std::fill_n(state.memory_value, 4, UINT32_MAX);
        CHECK(checked_memory_read(&state, address, bytes) == 0);
        for (unsigned i = 0; i < 16; ++i) {
            const uint8_t actual = state.memory_value[i / 4] >> ((i % 4) * 8);
            const uint8_t expected = i < bytes ? lanes[i / 4] >> ((i % 4) * 8) : 0;
            CHECK(actual == expected);
        }
    }
    for (uint32_t bytes : {0u, 3u, 5u, 15u, 17u, UINT32_MAX}) {
        CHECK(checked_memory_read(&state, data, bytes) == 2);
        CHECK(state.fault_address == data && state.fault_write == 0);
        CHECK(checked_memory_write(&state, data, bytes) == 2);
        CHECK(state.fault_address == data && state.fault_write == 1);
    }
    for (Address address : {0u, 0x83000000u, UINT32_MAX}) {
        CHECK(checked_memory_read(&state, address, 4) == 2);
        CHECK(state.fault_address == address && state.fault_write == 0);
        CHECK(checked_memory_write(&state, address, 4) == 2);
        CHECK(state.fault_address == address && state.fault_write == 1);
    }
    CHECK(mem_set_permissions(mem, data + page, page, MemPerm::ReadOnly));
    std::array<uint8_t, 16> before{}, after{};
    CHECK(mem_read(mem, data + page - 3, before.data(), before.size()));
    std::fill_n(state.memory_value, 4, 0x55555555);
    CHECK(checked_memory_write(&state, data + page - 3, 16) == 2);
    CHECK(mem_read(mem, data + page - 3, after.data(), after.size()));
    CHECK(before == after); // WHOLE access checked before even the first write.
    CHECK(mem_set_permissions(mem, data + page, page, MemPerm::WriteOnly));
    const auto saved = state;
    CHECK(checked_memory_read(&state, data + page - 3, 16) == 2);
    CHECK(std::memcmp(saved.memory_value, state.memory_value, sizeof(state.memory_value)) == 0);
    CHECK(mem_set_permissions(mem, data + page, page, MemPerm::ReadWrite));
    state.memory_cookie = 0;
    CHECK(checked_memory_read(&state, data, 4) == 2);
    CHECK(checked_memory_write(&state, data, 4) == 2);
    CHECK(std::memcmp(state.regs, architectural.regs, sizeof(state.regs)) == 0);
    CHECK(state.cpsr == architectural.cpsr && state.executed == architectural.executed);
    CHECK(state.next_pc == architectural.next_pc && state.dispatches == architectural.dispatches);
}

void put(MemState &mem, WasmJitCPU &jit, std::initializer_list<uint32_t> words) {
    CHECK(mem_write(mem, code, words.begin(), words.size() * sizeof(uint32_t)));
    jit.invalidate_jit_cache(code, page);
    jit.set_cpsr(0x10);
    jit.set_pc(code);
}
void put_thumb(MemState &mem, WasmJitCPU &jit, std::initializer_list<uint32_t> words) {
    CHECK(mem_write(mem, code, words.begin(), words.size() * sizeof(uint32_t)));
    jit.invalidate_jit_cache(code, page);
    jit.set_cpsr(0x10);
    jit.set_pc(code | 1); // T bit from the low PC bit, descriptor PC aligned
}
void equal_context(const CPUContext &a, const CPUContext &b) {
    CHECK(a.cpu_registers == b.cpu_registers);
    CHECK(a.cpsr == b.cpsr && a.fpscr == b.fpscr);
    CHECK(std::memcmp(a.fpu_registers.data(), b.fpu_registers.data(), sizeof(a.fpu_registers)) == 0);
}

#include "wasmjit_vector_tests.inc"
#include "wasmjit_vector_integer_tests.inc"
#include "wasmjit_vector_compare_tests.inc"
#include "wasmjit_vectorfp_compare_tests.inc"
#include "wasmjit_byte_reverse_tests.inc"
#include "wasmjit_packed_saturate_tests.inc"
#include "wasmjit_f64_tests.inc"
#include "fp64_helper_tests.inc"
#include "wasmjit_recip_tests.inc"
#include "wasmjit_tofixed_tests.inc"
#include "wasmjit_vectormul_tests.inc"
#include "wasmjit_fpsqrt_tests.inc"
#include "wasmjit_exclusive_tests.inc"
#include "wasmjit_inline_mutex_tests.inc"

void tls_read(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    for (bool regions : {false, true}) {
        jit.set_region_mode(regions);
        // MRC p15,0,r0,c13,c0,3; SVC #0. Same Thumb instruction
        // that stopped retail libc, with no proprietary code or data.
        for (uint32_t tls : {0x87654321u, 0x12345000u}) {
            jit.set_tpidruro(tls);
            put_thumb(mem, jit, {0x0f70ee1d, 0xbf00df00});
            CHECK(jit.run() == 0);
            CHECK(parent.svc_called);
            CHECK(jit.get_reg(0) == tls);
            CHECK(jit.get_tpidruro() == tls);
            put(mem, jit, {0xee1d0f70, 0xef000000});
            CHECK(jit.run() == 0);
            CHECK(parent.svc_called);
            CHECK(jit.get_reg(0) == tls);
        }
    }
}

void memory_barriers(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    for (bool regions : {false, true}) {
        jit.set_region_mode(regions);
        for (uint32_t option : {0x5fu, 0x4fu, 0x6fu}) {
        // DMB/DSB/ISB SY; NOP; SVC #0, in Thumb then ARM. Barrier
        // lowering must preserve registers and reach the following SVC.
        jit.set_reg(0, 0xdeadbeefu);
        put_thumb(mem, jit, {0x8f00f3bfu | (option << 16), 0xdf00bf00});
        CHECK(jit.run() == 0);
        CHECK(parent.svc_called);
        CHECK(jit.get_reg(0) == 0xdeadbeefu);
        parent.svc_called = false;
        put(mem, jit, {0xf57ff000u | option, 0xe1a00000, 0xef000000});
        CHECK(jit.run() == 0);
        CHECK(parent.svc_called);
        CHECK(jit.get_reg(0) == 0xdeadbeefu);
        parent.svc_called = false;
        }
    }
}

// A scheduler must be able to preempt a polling guest without calling it
// finished or losing its continuation. Two independent CPUs share only memory.
void cooperative_slices(MemState &mem) {
    for (bool regions : {false, true}) {
        CPUState parent{}, child{};
        parent.mem = child.mem = &mem;
        WasmJitCPU a(&parent, 0), b(&child, 0);
        a.set_region_mode(regions);
        b.set_region_mode(regions);
        // Parent: while (*r1) {}; SVC. Child: *r1 = 0; SVC.
        put(mem, a, {0xe5910000, 0xe3500000, 0x1afffffc, 0xef000000});
        const uint32_t worker[] = {0xe3a00000, 0xe5810000, 0xef000000};
        CHECK(mem_write(mem, code + 0x100, worker, sizeof(worker)));
        b.set_cpsr(0x10);
        b.set_pc(code + 0x100);
        a.set_reg(1, data);
        b.set_reg(1, data);
        a.set_tpidruro(0x11110000);
        b.set_tpidruro(0x22220000);
        uint32_t flag = 1;
        CHECK(mem_write(mem, data, &flag, sizeof(flag)));
        const auto before = a.instructions_executed();
        CHECK(a.run_slice(64) == WasmJitCPU::slice_yield);
        CHECK(a.instructions_executed() > before);
        CHECK(a.instructions_executed() - before <= 64);
        CHECK(!parent.svc_called);
        CHECK(a.get_last_error().empty());
        CHECK(a.get_pc() >= code && a.get_pc() < code + 12);
        for (unsigned slice = 0; slice < 3; ++slice) {
            const auto tick = a.instructions_executed();
            CHECK(a.run_slice(64) == WasmJitCPU::slice_yield);
            CHECK(a.instructions_executed() > tick);
            CHECK(a.instructions_executed() - tick <= 64);
            CHECK(!parent.svc_called);
        }
        const auto paused = a.save_context();
        const auto tick = a.instructions_executed();
        CHECK(a.run_slice(0) == WasmJitCPU::slice_yield);
        equal_context(a.save_context(), paused);
        CHECK(a.instructions_executed() == tick);
        CHECK(b.run_slice(64) == 0);
        CHECK(child.svc_called);
        CHECK(mem_read(mem, data, &flag, sizeof(flag)) && flag == 0);
        equal_context(a.save_context(), paused);
        CHECK(a.run_slice(64) == 0);
        CHECK(parent.svc_called);
        CHECK(a.get_reg(0) == 0);
        CHECK(a.get_tpidruro() == 0x11110000);
        CHECK(b.get_tpidruro() == 0x22220000);
        // A scheduler slice must not turn the existing runaway guard into
        // successful completion, or hide an instruction fault.
        put(mem, a, {0xeafffffe});
        a.set_instruction_budget(64);
        CHECK(a.run_slice(16) == WasmJitCPU::slice_yield);
        const auto guarded = a.instructions_executed();
        CHECK(a.run() < 0);
        CHECK(a.instructions_executed() - guarded == 64);
        CHECK(a.get_last_error().find("instruction budget exhausted") != std::string::npos);
        a.set_pc(0x83000000);
        CHECK(a.run_slice(64) < 0);
    }
}

void leading_zeros(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    for (bool regions : {false, true}) {
        jit.set_region_mode(regions);
        for (const auto &[input, expected] : std::array<std::pair<uint32_t, uint32_t>, 6>{{
                 {0, 32}, {1, 31}, {0x80000000, 0}, {0xffffffff, 0}, {0x10000, 15}, {0x1234, 19}}}) {
            for (bool thumb : {false, true}) {
                // CLZ r4,r3 followed by SVC; both ARM encodings tested.
                if (thumb) put_thumb(mem, jit, {0xf483fab3, 0xbf00df00});
                else put(mem, jit, {0xe16f4f13, 0xef000000});
                jit.set_reg(3, input);
                jit.set_cpsr(jit.get_cpsr() | 0xa0000000);
                const auto flags = jit.get_cpsr();
                CHECK(jit.run() == 0);
                CHECK(parent.svc_called);
                CHECK(jit.get_reg(4) == expected);
                CHECK(jit.get_reg(3) == input);
                CHECK(jit.get_cpsr() == flags);
            }
        }
    }
}

void unsigned_long_multiply(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    for (bool regions : {false, true}) {
        jit.set_region_mode(regions);
        for (const auto &[a, b] : std::array<std::pair<uint32_t, uint32_t>, 5>{{
                 {0, 0xffffffff}, {1, 7}, {0xffffffff, 0xffffffff},
                 {0x80000000, 2}, {0x12345678, 0xabcdef01}}}) {
            // UMULL r1,r0,r5,r0: destination overlaps a source.
            put_thumb(mem, jit, {0x1000fba5, 0xbf00df00});
            jit.set_reg(5, a);
            jit.set_reg(0, b);
            const auto flags = jit.get_cpsr();
            CHECK(jit.run() == 0);
            const uint64_t product = uint64_t(a) * b;
            CHECK(jit.get_reg(1) == uint32_t(product));
            CHECK(jit.get_reg(0) == uint32_t(product >> 32));
            CHECK(jit.get_reg(5) == a);
            CHECK(jit.get_cpsr() == flags);
        }
    }
}

void signed_long_multiply(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    for (bool regions : {false, true}) {
        jit.set_region_mode(regions);
        for (const auto &[a, b] : std::array<std::pair<int32_t, int32_t>, 7>{{
                 {0, -1}, {1, 7}, {-1, -1}, {-1, 7},
                 {INT32_MIN, 2}, {INT32_MIN, INT32_MIN}, {INT32_MAX, INT32_MIN}}}) {
            for (bool thumb : {false, true}) {
                // SMULL r2,r1,r1,r0: high destination overlaps a source.
                if (thumb) put_thumb(mem, jit, {0x2100fb81, 0xbf00df00});
                else put(mem, jit, {0xe0c12091, 0xef000000});
                jit.set_reg(1, uint32_t(a));
                jit.set_reg(0, uint32_t(b));
                jit.set_cpsr(jit.get_cpsr() | 0xa0000000);
                const auto flags = jit.get_cpsr();
                CHECK(jit.run() == 0);
                CHECK(parent.svc_called);
                const uint64_t product = uint64_t(int64_t(a) * int64_t(b));
                CHECK(jit.get_reg(2) == uint32_t(product));
                CHECK(jit.get_reg(1) == uint32_t(product >> 32));
                CHECK(jit.get_reg(0) == uint32_t(b));
                CHECK(jit.get_cpsr() == flags);
            }
        }
    }
}

void scalar_fp_mode_guards(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    for (bool regions : {false, true}) {
        jit.set_region_mode(regions);
        for (bool thumb : {false, true}) for (uint32_t opcode :
             {0xee200a20u, 0xee300a20u, 0xee300a60u, 0xee800a20u, 0xeeb80a61u, 0xeeb80ae1u}) {
            if (thumb) put_thumb(mem, jit, {(opcode << 16) | (opcode >> 16), 0xbf00df00});
            else put(mem, jit, {opcode, 0xef000000});
            jit.set_fpscr(0);
            CHECK(jit.run() == 0); // populate the compiled-code cache
            for (uint32_t mode : {0x100u, 0x200u, 0x400u, 0x800u, 0x1000u, 0x8000u,
                     0x00400000u, 0x00800000u, 0x00c00000u}) {
                jit.set_pc(code);
                jit.set_cpsr(thumb ? 0x30 : 0x10);
                jit.set_fpscr(mode);
                parent.svc_called = false;
                const auto before = jit.save_context();
                CHECK(jit.run() < 0);
                const auto after = jit.save_context();
                CHECK(!parent.svc_called);
                CHECK(after.fpscr == mode);
                CHECK(std::memcmp(&before.fpu_registers, &after.fpu_registers, sizeof(before.fpu_registers)) == 0);
            }
        }
    }
}

void scalar_binary32_batch(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    struct Case { uint32_t opcode, a, b, result, flags; };
    // All instructions use s0 = s0 op s1, exercising source/dest overlap.
    const Case cases[] = {
        {0xee200a20, 0x3fc00000, 0x40000000, 0x40400000, 0}, // multiply
        {0xee200a20, 0x7f800000, 0, 0x7fc00000, 1},
        {0xee200a20, 0x80000000, 0x40000000, 0x80000000, 0},
        {0xee200a20, 0x7f7fffff, 0x40000000, 0x7f800000, 0x14},
        {0xee200a20, 0x00800000, 0x3f000000, 0x00400000, 0},
        {0xee200a20, 0x00800001, 0x3f000000, 0x00400000, 0x18},
        {0xee200a20, 0x3f800001, 0x3f800001, 0x3f800002, 0x10},
        {0xee300a20, 0x3f800000, 0x40000000, 0x40400000, 0}, // add
        {0xee300a20, 0x7f800000, 0xff800000, 0x7fc00000, 1},
        {0xee300a20, 0x7f7fffff, 0x7f7fffff, 0x7f800000, 0x14},
        {0xee300a20, 0x3f800000, 0x33800000, 0x3f800000, 0x10}, // tie
        {0xee300a20, 0x3f800000, 0x00800000, 0x3f800000, 0x10}, // wide lost addend
        {0xee300a20, 0x80000000, 0x80000000, 0x80000000, 0},
        {0xee300a20, 0x3f800000, 0xbf800000, 0, 0}, // exact cancellation
        {0xee300a60, 0x40400000, 0x3f800000, 0x40000000, 0}, // subtract
        {0xee300a60, 0x7f800000, 0x7f800000, 0x7fc00000, 1},
        {0xee300a60, 0x00800001, 0x00800000, 1, 0},
        {0xee300a60, 0x3f800000, 0x00800000, 0x3f800000, 0x10},
        {0xee300a60, 0x3f800000, 0x3f800000, 0, 0},
        {0xee200a20, 0xffc12345, 0x3f800000, 0xffc12345, 0},
        {0xee200a20, 0x7fc12345, 0xff812345, 0xffc12345, 1},
        {0xee300a20, 0x7f812345, 0xffc12345, 0x7fc12345, 1},
        {0xee300a60, 0x3f800000, 0xffc12345, 0xffc12345, 0},
        {0xee300a60, 0xff812345, 0x7f812345, 0xffc12345, 1},
        {0xee200a20, 1, 0x3f800000, 1, 0},
        {0xee200a20, 0x80000001, 0x3f800000, 0x80000001, 0},
        {0xee300a20, 1, 0x00800000, 0x00800001, 0},
        {0xee300a60, 0x00800000, 1, 0x007fffff, 0},
        {0xee200a20, 0x00800000, 0x3f7fffff, 0x00800000, 0x18}, // tiny before rounding

    };
    for (bool regions : {false, true}) {
        jit.set_region_mode(regions);
        for (bool thumb : {false, true}) for (bool flush : {false, true})
            for (bool default_nan : {false, true}) {
            for (const auto &c : cases) {
                if (thumb) put_thumb(mem, jit, {(c.opcode << 16) | (c.opcode >> 16), 0xbf00df00});
                else put(mem, jit, {c.opcode, 0xef000000});
                auto before = jit.save_context();
                std::memcpy(&before.fpu_registers[0], &c.a, 4);
                std::memcpy(&before.fpu_registers[1], &c.b, 4);
                before.fpscr = 0xa0000002 | (flush ? 0x01000000 : 0)
                    | (default_nan ? 0x02000000 : 0);
                jit.load_context(before);
                CHECK(jit.run() == 0);
                const auto after = jit.save_context();
                uint32_t actual; std::memcpy(&actual, &after.fpu_registers[0], 4);
                const bool tiny = (c.result & 0x7fffffff) != 0 && (c.result & 0x7fffffff) < 0x00800000;
                auto expected = default_nan && (c.result & 0x7fffffff) > 0x7f800000
                    ? 0x7fc00000u : flush && tiny ? c.result & 0x80000000 : c.result;
                auto flags = flush && tiny ? 8u : c.flags;
                if (flush && (c.a == 1 || c.a == 0x80000001)) {
                    expected = c.opcode == 0xee200a20 ? c.a & 0x80000000 : c.b;
                    flags = 0x80;
                }
                if (flush && c.b == 1) { expected = c.a; flags = 0x80; }
                if (flush && c.a == 0x00800000 && c.b == 0x3f7fffff) { expected = 0; flags = 8; }
                if (actual != expected || after.fpscr != (before.fpscr | flags))
                    std::fprintf(stderr, "binary FP op=%08x a=%08x b=%08x got=%08x expected=%08x fpscr=%08x expected=%08x\n", c.opcode, c.a, c.b, actual, expected, after.fpscr, before.fpscr | flags);
                CHECK(actual == expected);
                CHECK(after.fpscr == (before.fpscr | flags));
                CHECK(after.cpsr == before.cpsr);
                CHECK(parent.svc_called);
            }
        }
    }
}

void float_divide32(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    struct Case { uint32_t a, b, expected, flags; };
    // s15 = s0/s15 nearest-even. Covers div-by-zero, inf/inf -> NaN, exact
    // and inexact quotients, signed zeros, NaN propagation.
    const std::array<Case, 28> cases{{
        {0x3f800000, 0x40000000, 0x3f000000, 0},
        {0x40000000, 0x3f800000, 0x40000000, 0},
        {0x3f800000, 0, 0x7f800000, 2},
        {0xbf800000, 0, 0xff800000, 2},
        {0x3f800000, 0x80000000, 0xff800000, 2},
        {0x7f800000, 0x3f800000, 0x7f800000, 0},
        {0x7f800000, 0x7f800000, 0x7fc00000, 1},
        {0x7fc00000, 0x3f800000, 0x7fc00000, 0},
        {0, 0x40000000, 0, 0},
        {0x80000000, 0x40000000, 0x80000000, 0},
        {0x7f7fffff, 0x40000000, 0x7effffff, 0},
        {0x3f800000, 0x3f000000, 0x40000000, 0},
        {0, 0, 0x7fc00000, 1},
        {0x7f800001, 0x3f800000, 0x7fc00000, 1},
        {0x3f800000, 0x40400000, 0x3eaaaaab, 0x10},
        {0x7f7fffff, 0x3f000000, 0x7f800000, 0x14},
        {0x00800000, 0x40000000, 0, 8},
        {0x80800000, 0x40000000, 0x80000000, 8},
        {1, 0x3f800000, 0, 0x80},
        {0x3f800000, 1, 0x7f800000, 0x82},
        {0x7f800000, 0, 0x7f800000, 0},
        {0xffc12345, 0x3f800000, 0x7fc00000, 0},
        {0x7fc12345, 0xff812345, 0x7fc00000, 1},
        {0x00800000, 0x40400000, 0, 8},
        {0x00800001, 0x40000000, 0, 8},
        {0x80000001, 0x3f800000, 0x80000000, 0x80},
        {0x3f800000, 0x7f800000, 0, 0},
        {0xbf800000, 0x7f800000, 0x80000000, 0},
    }};
    for (bool regions : {false, true}) {
        jit.set_region_mode(regions);
        for (bool thumb : {false, true}) for (bool flush : {false, true})
            for (bool default_nan : {false, true}) for (const auto &c : cases) {
            uint32_t expected = c.expected, flags = c.flags;
            if (!flush) {
                if (c.a == 0x00800000 && c.b == 0x40000000) { expected = 0x00400000; flags = 0; }
                if (c.a == 0x80800000) { expected = 0x80400000; flags = 0; }
                if (c.a == 1 || c.a == 0x80000001) { expected = c.a; flags = 0; }
                if (c.b == 1) { expected = 0x7f800000; flags = 0x14; }
                if (c.a == 0x00800000 && c.b == 0x40400000) { expected = 0x002aaaab; flags = 0x18; }
                if (c.a == 0x00800001) { expected = 0x00400000; flags = 0x18; }
            }
            if (!default_nan) {
                if (c.a == 0x7f800001) expected = 0x7fc00001;
                if (c.a == 0xffc12345) expected = c.a;
                if (c.b == 0xff812345) expected = 0xffc12345;
            }
            if (thumb) put_thumb(mem, jit, {0x7a27eec0, 0xbf00df00});
            else put(mem, jit, {0xeec07a27, 0xef000000});
            auto context = jit.save_context();
            std::memcpy(&context.fpu_registers[0], &c.a, 4);
            std::memcpy(&context.fpu_registers[15], &c.b, 4);
            context.fpscr = 0xf0000004 | (flush ? 0x01000000 : 0)
                | (default_nan ? 0x02000000 : 0); // existing OFC must remain sticky
            jit.load_context(context);
            CHECK(jit.run() == 0);
            CHECK(parent.svc_called);
            const auto after = jit.save_context();
            uint32_t result;
            std::memcpy(&result, &after.fpu_registers[15], 4);
            CHECK(result == expected);
            CHECK(after.fpscr == (context.fpscr | flags));
            CHECK(after.cpsr == context.cpsr);
        }
    }
}

void unsigned_integer_to_float32(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    struct Case { uint32_t input, expected, flags; };
    const Case cases[] = {{0, 0, 0}, {1, 0x3f800000, 0},
        {0xffffffff, 0x4f800000, 0x10}, {0x80000000, 0x4f000000, 0},
        {16777217, 0x4b800000, 0x10}, {16777219, 0x4b800002, 0x10}};
    for (bool regions : {false, true}) {
        jit.set_region_mode(regions);
        for (bool thumb : {false, true}) for (const auto &c : cases) {
            // VCVT.F32.U32 s0,s3 with FPSCR round-to-nearest.
            if (thumb) put_thumb(mem, jit, {0x0a61eeb8, 0xbf00df00});
            else put(mem, jit, {0xeeb80a61, 0xef000000});
            auto before = jit.save_context();
            std::memcpy(&before.fpu_registers[3], &c.input, 4);
            before.fpscr = 0xa3000001;
            jit.load_context(before);
            CHECK(jit.run() == 0);
            const auto after = jit.save_context();
            uint32_t result; std::memcpy(&result, &after.fpu_registers[0], 4);
            if (result != c.expected || after.fpscr != (before.fpscr | c.flags))
                std::fprintf(stderr, "VCVT.U32 input=%08x result=%08x expected=%08x FPSCR=%08x expected=%08x differing-bits=%08x\n",
                    c.input, result, c.expected, after.fpscr, before.fpscr | c.flags,
                    after.fpscr ^ (before.fpscr | c.flags));
            CHECK(result == c.expected);
            CHECK(after.fpscr == (before.fpscr | c.flags));
            CHECK(after.cpsr == before.cpsr);
        }
    }
}

void integer_to_float32(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    const std::array<std::pair<uint32_t, uint32_t>, 8> cases{{
        {0, 0}, {1, 0x3f800000}, {0xffffffff, 0xbf800000},
        {0x80000000, 0xcf000000}, {0x7fffffff, 0x4f000000},
        {16777217, 0x4b800000}, {16777219, 0x4b800002},
        {uint32_t(-16777217), 0xcb800000},
    }};
    for (bool regions : {false, true}) {
        jit.set_region_mode(regions);
        for (bool thumb : {false, true}) for (const auto &[input, expected] : cases) {
            // VCVT.F32.S32 s0,s3 with FPSCR round-to-nearest, ties-to-even.
            if (thumb) put_thumb(mem, jit, {0x0ae1eeb8, 0xbf00df00});
            else put(mem, jit, {0xeeb80ae1, 0xef000000});
            auto context = jit.save_context();
            std::memcpy(&context.fpu_registers[3], &input, 4);
            context.fpscr = 0xa3000081;
            jit.load_context(context);
            CHECK(jit.run() == 0);
            CHECK(parent.svc_called);
            const auto after = jit.save_context();
            uint32_t result;
            std::memcpy(&result, &after.fpu_registers[0], 4);
            CHECK(result == expected);
            const bool inexact = input == 0x7fffffff || input == 16777217
                || input == 16777219 || input == uint32_t(-16777217);
            CHECK(after.fpscr == (context.fpscr | (inexact ? 0x10u : 0u)));
            CHECK(after.cpsr == context.cpsr);
        }
    }
}

void floating_abs32(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    for (bool regions : {false, true}) {
        jit.set_region_mode(regions);
        for (bool thumb : {false, true}) for (bool negate : {false, true}) {
            for (uint32_t bits : {0u, 0x80000000u, 0xbf800000u, 0xff800000u,
                     0xff800001u, 0xffc12345u, 0x80000001u, 0x3f800000u}) {
                // VABS/VNEG.F32 s0,s0 preserve NaN payloads and subnormals.
                if (thumb) put_thumb(mem, jit, {negate ? 0x0a40eeb1u : 0x0ac0eeb0u, 0xbf00df00});
                else put(mem, jit, {negate ? 0xeeb10a40u : 0xeeb00ac0u, 0xef000000});
                auto context = jit.save_context();
                std::memcpy(&context.fpu_registers[0], &bits, 4);
                context.fpscr = 0xf3000091;
                jit.load_context(context);
                CHECK(jit.run() == 0);
                CHECK(parent.svc_called);
                const auto after = jit.save_context();
                uint32_t result;
                std::memcpy(&result, &after.fpu_registers[0], 4);
                CHECK(result == (negate ? bits ^ 0x80000000u : bits & 0x7fffffffu));
                CHECK(after.fpscr == context.fpscr);
                CHECK(after.cpsr == context.cpsr);
            }
        }
    }
}

void floating_compare32(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    struct Case { uint32_t a, b, nzcv, ioc; };
    const std::array<Case, 10> cases{{
        {0, 0x80000000, 0x60000000, 0},
        {0xbf800000, 0, 0x80000000, 0},
        {0x3f800000, 0, 0x20000000, 0},
        {0x7f800000, 0x7f800000, 0x60000000, 0},
        {0xff800000, 0x7f800000, 0x80000000, 0},
        {0x7fc00001, 0, 0x30000000, 0},
        {0, 0xffc00001, 0x30000000, 0},
        {0x7f800001, 0, 0x30000000, 1},
        {0, 0xff800001, 0x30000000, 1},
        {1, 0, 0x20000000, 0},
    }};
    for (bool regions : {false, true}) {
        jit.set_region_mode(regions);
        for (bool thumb : {false, true}) for (bool zero : {false, true}) {
            for (bool signal : {false, true}) for (bool flush : {false, true}) {
                for (const auto &c : cases) {
                    if (zero && c.b != 0) continue;
                    const uint32_t instruction = (zero ? 0xeef50a40u : 0xeef40a41u)
                        | (signal ? 0x80u : 0u);
                    // VCMP[E].F32 s1,s2/#0; VMRS APSR_nzcv,FPSCR; SVC.
                    if (thumb) put_thumb(mem, jit, {(instruction << 16) | (instruction >> 16), 0xfa10eef1, 0xbf00df00});
                    else put(mem, jit, {instruction, 0xeef1fa10, 0xef000000});
                    auto context = jit.save_context();
                    std::memcpy(&context.fpu_registers[1], &c.a, 4);
                    std::memcpy(&context.fpu_registers[2], &c.b, 4);
                    jit.load_context(context);
                    const uint32_t control = (flush ? 1u << 24 : 0) | (1u << 25) | 0x10;
                    jit.set_fpscr(control | 0xf0000000);
                    const uint32_t psr = jit.get_cpsr() & 0x0fffffff;
                    CHECK(jit.run() == 0);
                    CHECK(parent.svc_called);
                    const bool denormal = c.a == 1;
                    const auto nzcv = flush && denormal ? 0x60000000u : c.nzcv;
                    const auto ioc = c.ioc | (signal && c.nzcv == 0x30000000 ? 1u : 0u);
                    CHECK(jit.get_fpscr() == (control | nzcv | ioc | (flush && denormal ? 0x80u : 0u)));
                    CHECK(jit.get_cpsr() == (psr | nzcv));
                }
            }
        }
    }
}

void floating_compare_trap_guard(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    for (bool regions : {false, true}) {
        jit.set_region_mode(regions);
        put_thumb(mem, jit, {0x0a40eef5, 0xbf00df00});
        jit.set_fpscr(0);
        CHECK(jit.run() == 0);
        // Reuse the compiled code: exception enables aren't in its cache key.
        jit.set_pc(code);
        jit.set_cpsr(0x30);
        jit.set_fpscr(0x100);
        parent.svc_called = false;
        CHECK(jit.run() < 0);
        CHECK(!parent.svc_called);
        CHECK(jit.get_fpscr() == 0x100);
    }
}

void multiply32(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    for (bool regions : {false, true}) {
        jit.set_region_mode(regions);
        // MUL r0,r2,r0 (Thumb T2 00 fb 02 f0); flags never written.
        // Same instruction that stopped Limbo, with overlap dest/source.
        for (const auto &[a, b] : std::array<std::pair<uint32_t, uint32_t>, 5>{{
                 {0, 0}, {5, 7}, {1, 0xffffffffu},
                 {0x12345678u, 0x9abcdef0u}, {0xffffffffu, 0xffffffffu}}}) {
            for (bool thumb : {false, true}) {
                if (thumb) put_thumb(mem, jit, {0xf002fb00, 0xbf00df00});
                else put(mem, jit, {0xe0000290, 0xef000000});
                jit.set_reg(0, a);
                jit.set_reg(2, b);
                jit.set_cpsr(jit.get_cpsr() | 0xf0000000);
                const auto flags = jit.get_cpsr();
                CHECK(jit.run() == 0);
                CHECK(parent.svc_called);
                CHECK(jit.get_reg(0) == static_cast<uint32_t>(static_cast<uint64_t>(a) * b));
                CHECK(jit.get_reg(2) == b);
                CHECK(jit.get_cpsr() == flags);
            }
        }
    }
}

void backend(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 3);
    jit.set_region_mode(false); // this section verifies single-block semantics
    jit.set_instruction_budget(128);
    CHECK(jit.processor_id() == 3);
    // Full context, including payload NaNs and both halves of every D register.
    CPUContext initial{};
    for (unsigned i = 0; i < 16; ++i) initial.cpu_registers[i] = 0x11100000 + i;
    for (unsigned i = 0; i < 64; ++i) {
        const uint32_t bits = 0x7f800001 + i;
        std::memcpy(&initial.fpu_registers[i], &bits, sizeof(bits));
    }
    initial.cpsr = 0x800f0010;
    initial.fpscr = 0x01400010;
    jit.set_tpidruro(0x87654321);
    jit.load_context(initial);
    equal_context(jit.save_context(), initial);
    CHECK(jit.get_tpidruro() == 0x87654321);
    jit.set_fpscr(0);

    // Multi-instruction translation is retried at limit=1 for memory IR; the
    // requested limit remains the cache key. Repeated run must NOT recompile.
    put(mem, jit, {0xe5910000, 0xe2800001, 0xe5810000, 0xef000042});
    uint32_t value = 40;
    CHECK(mem_write(mem, data, &value, sizeof(value)));
    jit.set_reg(1, data);
    auto executed = jit.instructions_executed();
    CHECK(jit.run() == 0 && parent.svc_called && parent.svc == 0x42);
    CHECK(jit.get_reg(0) == 41 && jit.get_pc() == code + 16);
    CHECK(jit.instructions_executed() == executed + 4);
    const auto compiled = jit.compiled_blocks(), hits = jit.cache_hits();
    jit.set_pc(code);
    CHECK(jit.run() == 0 && parent.svc_called);
    CHECK(jit.get_reg(0) == 42);
    CHECK(jit.compiled_blocks() == compiled && jit.cache_hits() > hits);
    CHECK(mem_read(mem, data, &value, sizeof(value)) && value == 42);

    // Host Ptr modifications must invalidate a hot code block even without an
    // explicit invalidate_jit_cache call.
    auto invalidated = jit.invalidated_blocks();
    *Ptr<uint32_t>(code + 4).get(mem) = 0xe2800002; // ADD R0, R0, #2
    jit.set_pc(code);
    CHECK(jit.run() == 0 && jit.get_reg(0) == 44);
    CHECK(jit.invalidated_blocks() > invalidated);

    // A generated store replaces the next instruction before its cached block
    // can execute. Every entry's byte snapshot must observe the changed opcode.
    put(mem, jit, {0xe5810000, 0xe3a02001, 0xef000042});
    jit.set_reg(0, 0xe3a0202a); // MOV R2, #42
    jit.set_reg(1, code + 4);
    CHECK(jit.run() == 0 && jit.get_reg(2) == 42);
    invalidated = jit.invalidated_blocks();
    jit.set_pc(code);
    jit.set_reg(0, 0xe3a0202b); // MOV R2, #43
    CHECK(jit.run() == 0 && jit.get_reg(2) == 43);
    CHECK(jit.invalidated_blocks() > invalidated);

    // Faulting LDM changes R0 before its second load fails. The backend must
    // restore ALL CPU state, leave PC at the instruction, and retain metadata.
    put(mem, jit, {0xe8b10005, 0xef000042}); // LDMIA R1!, {R0,R2}
    jit.set_reg(1, data + 2 * page - 4);
    auto before = jit.save_context();
    executed = jit.instructions_executed();
    CHECK(jit.step() == -1);
    equal_context(jit.save_context(), before);
    CHECK(jit.get_tpidruro() == 0x87654321);
    CHECK(jit.instructions_executed() == executed && !parent.svc_called);
    CHECK(jit.get_fault_address() == data + 2 * page && !jit.get_fault_write());
    CHECK(jit.get_last_error().find("guest memory read fault") != std::string::npos);

    // Earlier stores in one guest STM instruction commit before a later fault;
    // CPU state rolls back, but memory intentionally is not transactional.
    put(mem, jit, {0xe8a10005, 0xef000042}); // STMIA R1!, {R0,R2}
    jit.set_reg(0, 0x12345678);
    jit.set_reg(1, data + 2 * page - 4);
    before = jit.save_context();
    CHECK(jit.step() == -1);
    equal_context(jit.save_context(), before);
    CHECK(jit.get_fault_address() == data + 2 * page && jit.get_fault_write());
    CHECK(mem_read(mem, data + 2 * page - 4, &value, sizeof(value)) && value == 0x12345678);

    // Permission denial also faults through the imported checked helper.
    put(mem, jit, {0xe5810000, 0xef000042});
    jit.set_reg(1, data);
    CHECK(mem_set_permissions(mem, data, page, MemPerm::ReadOnly));
    before = jit.save_context();
    CHECK(jit.step() == -1);
    equal_context(jit.save_context(), before);
    CHECK(jit.get_fault_address() == data && jit.get_fault_write());
    CHECK(mem_set_permissions(mem, data, page, MemPerm::ReadWrite));

    // The fixture's NEON memset loop (0x81000e24..0x81000e36) with its own
    // encodings, in Thumb: vdup.32 q8,lr; then vst1.32 {d16-d17},[ip]! /
    // cmp r3,ip / bne back to the vst1, run twice; svc terminates. Proves
    // the Q8/D16/D17 word mapping, guest stores and ip writeback through
    // the real backend and a taken branch into a 32-bit instruction.
    put_thumb(mem, jit, {0xeb90eea0, // vdup.32 q8, lr
        0x0a8df94c,                 // vst1.32 {d16-d17}, [ip]!
        0xd1fb4563,                 // cmp r3, ip; bne -10 -> vst1
        0xbf00df33});               // svc 0x33; nop
    jit.set_reg(14, 0x5a5a5a5a);
    jit.set_reg(3, data + 32);
    jit.set_reg(12, data);
    executed = jit.instructions_executed();
    CHECK(jit.run() == 0 && parent.svc_called && parent.svc == 0x33);
    CHECK(jit.get_pc() == code + 14);
    CHECK(jit.get_reg(12) == data + 32 && jit.get_reg(3) == data + 32);
    CHECK(jit.get_cpsr() == 0x60000030); // final cmp equal: Z+C set
    CHECK(jit.instructions_executed() == executed + 8);
    const auto neon = jit.save_context();
    for (unsigned i = 32; i < 36; ++i) { // every q8 lane, through the context
        uint32_t lane = 0;
        std::memcpy(&lane, &neon.fpu_registers[i], sizeof(lane));
        CHECK(lane == 0x5a5a5a5a);
    }
    for (unsigned i = 0; i < 32 / 4; ++i) {
        uint32_t word = 0;
        CHECK(mem_read(mem, data + i * 4, &word, sizeof(word)));
        CHECK(word == 0x5a5a5a5a);
    }

    // A speculative fetch beyond the last mapped instruction must not discard
    // that instruction. Retrying run also reuses the shortened cached block.
    for (const bool thumb : {false, true}) {
        const uint32_t last_word = thumb ? 0x3001bf00 : 0xe2800001; // ADD r0,#1
        CHECK(mem_write(mem, code + page - 4, &last_word, sizeof(last_word)));
        jit.invalidate_jit_cache(code, page);
        const Address entry = code + page - (thumb ? 2 : 4);
        uint64_t compiled_after_first = 0;
        for (unsigned run = 0; run < 2; ++run) {
            jit.set_cpsr(0x10);
            jit.set_pc(entry | (thumb ? 1 : 0));
            jit.set_reg(0, 41);
            const auto count = jit.instructions_executed();
            const auto old_hits = jit.cache_hits();
            CHECK(jit.run() == -1 && !parent.svc_called);
            CHECK(jit.get_reg(0) == 42 && jit.get_pc() == code + page);
            CHECK(jit.instructions_executed() == count + 1);
            CHECK(jit.get_last_error().find("instruction fetch failed") != std::string::npos);
            if (run == 0) compiled_after_first = jit.compiled_blocks();
            else {
                CHECK(jit.compiled_blocks() == compiled_after_first);
                CHECK(jit.cache_hits() > old_hits);
            }
        }
        const auto at_fault = jit.save_context();
        const auto count = jit.instructions_executed();
        CHECK(jit.run() == -1); // The first instruction itself is now unmapped.
        equal_context(jit.save_context(), at_fault);
        CHECK(jit.instructions_executed() == count);
    }
}
// M14c region formation: DFS membership, one-block-per-PC, PSR metadata,
// tick accounting and sorted output on a real Thumb CFG with a loop, an
// unconditional branch over dead code, and a SVC-terminated block.
void formation(MemState &mem) {
    // 0x00 movs r0,#0; 0x02 adds r0,#1; 0x04 cmp r0,#3; 0x06 bne 0x02;
    // 0x08 b 0x0c; 0x0a nop (unreachable); 0x0c svc 0x42
    const std::array<uint32_t, 4> words{0x30012000, 0xd1fc2803, 0xbf00e000, 0xbf00df42};
    CHECK(mem_write(mem, code, words.data(), words.size() * sizeof(uint32_t)));
    Region region;
    std::vector<Dynarmic::IR::Block> ir;
    CHECK(form_region(mem, code, 0x30, 0, region, ir)); // Thumb, user mode
    CHECK(region.blocks.size() == 4); // entry, loop body, b, svc
    CHECK(ir.size() == region.blocks.size());
    std::vector<uint32_t> pcs;
    for (const auto &block : region.blocks) {
        pcs.push_back(block.pc);
        CHECK(block.psr_mask == PSR_DISPATCH_MASK);
        CHECK(block.psr_value == 0x20); // T bit, no IT/E
        CHECK(block.ticks >= 1);
        CHECK(!block.original.empty());
    }
    CHECK(std::is_sorted(pcs.begin(), pcs.end()));
    CHECK(std::adjacent_find(pcs.begin(), pcs.end()) == pcs.end()); // one per PC
    CHECK(std::find(pcs.begin(), pcs.end(), code) != pcs.end());
    CHECK(std::find(pcs.begin(), pcs.end(), code + 2) != pcs.end()); // loop body
    CHECK(std::find(pcs.begin(), pcs.end(), code + 8) != pcs.end()); // b
    CHECK(std::find(pcs.begin(), pcs.end(), code + 0xc) != pcs.end()); // svc
    CHECK(std::find(pcs.begin(), pcs.end(), code + 0xa) == pcs.end()); // dead nop
    uint64_t sum = 0;
    for (const auto &block : region.blocks) sum += block.ticks;
    CHECK(region.total_ticks == sum);
    CHECK(region.total_ticks < REGION_MAX_TICKS && region.blocks.size() < REGION_MAX_BLOCKS);
    CHECK(region.page_begin == code / 4096 && region.page_end == (code + 0x10 + 4095) / 4096);
    // An unmapped entry cannot form a region.
    Region bad;
    std::vector<Dynarmic::IR::Block> bad_ir;
    CHECK(!form_region(mem, 0x83000000, 0x30, 0, bad, bad_ir) && bad.blocks.empty());
    // Formation is deterministic.
    Region again;
    std::vector<Dynarmic::IR::Block> again_ir;
    CHECK(form_region(mem, code, 0x30, 0, again, again_ir));
    CHECK(again.blocks.size() == region.blocks.size());
    for (size_t i = 0; i < again.blocks.size(); ++i) {
        CHECK(again.blocks[i].pc == region.blocks[i].pc);
        CHECK(again.blocks[i].ticks == region.blocks[i].ticks);
        CHECK(again.blocks[i].original == region.blocks[i].original);
        CHECK(Dynarmic::A32::LocationDescriptor(again_ir[i].Location()).PC() == again.blocks[i].pc);
    }
    // The emitted region module is valid Wasm with the run(state,budget) export.
    std::vector<const Dynarmic::IR::Block *> ptrs;
    std::vector<vita3k::wasmjit::RegionBlockMeta> meta;
    for (size_t i = 0; i < region.blocks.size(); ++i) {
        ptrs.push_back(&ir[i]);
        meta.push_back({region.blocks[i].pc, region.blocks[i].psr_mask,
            region.blocks[i].psr_value, region.blocks[i].ticks});
    }
    const auto module = vita3k::wasmjit::emit_region(ptrs, meta);
    CHECK(!module.empty());
    // Mismatched meta is rejected: wrong ticks, wrong PSR, unsorted, dup PCs.
    auto bad_meta = meta;
    bad_meta[1].ticks += 1;
    CHECK(vita3k::wasmjit::emit_region(ptrs, bad_meta).empty());
    bad_meta = meta;
    bad_meta[2].entry_pc = bad_meta[1].entry_pc; // duplicate PC
    CHECK(vita3k::wasmjit::emit_region(ptrs, bad_meta).empty());
    CHECK(vita3k::wasmjit::emit_region({}, {}).empty());
}

// Region-mode execution: the same guest programs run through REGION modules
// with in-Wasm dispatch, chaining, budget and fault semantics.
void region_exec(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0); // region mode is the default
    jit.set_instruction_budget(4096);

    // The loop CFG: blocks chain inside ONE region; only the SVC exits.
    // 0x00 movs r0,#0; 0x02 adds r0,#1; 0x04 cmp r0,#3; 0x06 bne 0x02;
    // 0x08 b 0x0c; 0x0a nop (unreachable); 0x0c svc 0x42
    // 0x3001 = adds r0,#1 (GAS-verified; 0x3008 would decode as adds r0,#8).
    const std::array<uint32_t, 4> words{0x30012000, 0xd1fc2803, 0xbf00e000, 0xbf00df42};
    CHECK(mem_write(mem, code, words.data(), words.size() * sizeof(uint32_t)));
    jit.set_cpsr(0x30);
    jit.set_pc(code | 1);
    CHECK(jit.run() == 0 && parent.svc_called && parent.svc == 0x42);
    CHECK(jit.get_reg(0) == 3); // movs r0,#1, then adds runs twice (1->2->3)
    CHECK(jit.get_pc() == code + 0xe); // PC past the 2-byte svc (Thumb)
    CHECK(jit.compiled_blocks() == 0); // no single-block modules were built
    CHECK(jit.get_last_error().empty());

    // Host-side code patch: the cached region must be dropped and rebuilt.
    const std::array<uint32_t, 2> patch{0xdf432000, 0}; // movs r0,#0; svc 0x43
    CHECK(mem_write(mem, code, patch.data(), patch.size() * sizeof(uint32_t)));
    jit.set_pc(code | 1);
    CHECK(jit.run() == 0 && parent.svc_called && parent.svc == 0x43);
    CHECK(jit.get_reg(0) == 0);
    CHECK(jit.invalidated_blocks() > 0);

    // Budget exhaustion: block 0x00 costs 4 ticks (movs+adds+cmp+bne); with a
    // 5-tick budget it completes, then dispatch refuses the 3-tick successor.
    CHECK(mem_write(mem, code, words.data(), words.size() * sizeof(uint32_t)));
    jit.set_cpsr(0x30);
    jit.set_pc(code | 1);
    jit.set_instruction_budget(5);
    const uint64_t executed_before = jit.instructions_executed();
    CHECK(jit.run() == 0); // cannot-fit slice boundary: clean return, no error
    CHECK(jit.instructions_executed() - executed_before == 4);
    CHECK(jit.get_pc() == code + 2); // stopped at the successor entry

    // Memory fault mid-region: fault_address is the guest address; executed
    // counts only blocks completed before the faulting instruction.
    const std::array<uint32_t, 2> faultprog{0x68012007, 0xbf00df42}; // movs r0,#7; ldr r1,[r0]; svc
    CHECK(mem_write(mem, code, faultprog.data(), faultprog.size() * sizeof(uint32_t)));
    jit.set_cpsr(0x30);
    jit.set_pc(code | 1);
    jit.set_instruction_budget(4096);
    const uint64_t fault_executed_before = jit.instructions_executed();
    CHECK(jit.run() == -1);
    CHECK(jit.get_fault_address() == 7 && !jit.get_fault_write());
    CHECK(jit.instructions_executed() - fault_executed_before == 0); // ticks count only COMPLETED blocks (REGION_ABI); the movs' block faulted at the ldr
    CHECK(jit.get_pc() == code + 2); // faulting ldr, not the block entry
    CHECK(jit.get_last_error().find("guest memory read fault") != std::string::npos);

    // Budget fully consumed is an error in BOTH modes (the interpreter-oracle
    // runaway-guard contract); only the cannot-fit slice boundary returns 0.
    // Restore the loop CFG first: the fault program overwrote it.
    CHECK(mem_write(mem, code, words.data(), words.size() * sizeof(uint32_t)));
    jit.set_instruction_budget(4); // exactly block A's 4 ticks
    jit.set_cpsr(0x30);
    jit.set_pc(code | 1);
    const uint64_t exhausted_before = jit.instructions_executed();
    CHECK(jit.run() == -1);
    CHECK(jit.get_last_error().find("budget") != std::string::npos);
    CHECK(jit.instructions_executed() - exhausted_before == 4);
    jit.set_region_mode(false);
    jit.set_instruction_budget(2); // the program cannot finish in 2 instructions
    jit.set_cpsr(0x30);
    jit.set_pc(code | 1);
    CHECK(jit.run() == -1);
    CHECK(jit.get_last_error().find("budget") != std::string::npos);
}

void region_budget_continuations(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(1);

    // A dynamic branch consumes the last tick and returns Miss, not Budget.
    put(mem, jit, {0xe12fff11, 0xef000042}); // BX r1; SVC 0x42
    jit.set_reg(1, code + 4);
    auto count = jit.instructions_executed();
    CHECK(jit.run() == -1 && !parent.svc_called);
    CHECK(jit.instructions_executed() == count + 1 && jit.get_pc() == code + 4);
    CHECK(jit.get_last_error().find("budget") != std::string::npos);

    // SMC wins over the dispatch budget check. Exhaustion must still be an
    // error after invalidating code, before executing the modified instruction.
    put(mem, jit, {0xe5810000, 0xe3a02001, 0xef000042});
    jit.set_reg(0, 0xe3a0202a); // Replace MOV r2,#1 with MOV r2,#42.
    jit.set_reg(1, code + 4);
    jit.set_reg(2, 9);
    count = jit.instructions_executed();
    const auto invalidated = jit.invalidated_blocks();
    CHECK(jit.run() == -1 && !parent.svc_called);
    CHECK(jit.instructions_executed() == count + 1 && jit.get_pc() == code + 4);
    CHECK(jit.get_reg(2) == 9 && jit.invalidated_blocks() > invalidated);
    CHECK(jit.get_last_error().find("budget") != std::string::npos);
    jit.set_instruction_budget(2);
    CHECK(jit.run() == 0 && parent.svc_called && jit.get_reg(2) == 42);

    // A completed SVC still succeeds when it consumes exactly the last tick.
    put(mem, jit, {0xef000042});
    jit.set_instruction_budget(1);
    CHECK(jit.run() == 0 && parent.svc_called);
    jit.set_pc(code);
    jit.set_instruction_budget(0);
    count = jit.instructions_executed();
    CHECK(jit.run() == -1 && !parent.svc_called);
    CHECK(jit.instructions_executed() == count && jit.get_pc() == code);
    CHECK(jit.get_last_error().find("budget") != std::string::npos);
}

void region_regressions(MemState &mem, vita3k::wasmjit::RegionStateOptions options) {
    namespace A32 = Dynarmic::A32;
    namespace IR = Dynarmic::IR;
    using Op = IR::Opcode;
    using Value = IR::Value;
    using Reason = vita3k::wasmjit::ExitReason;
    const auto loc = [](uint32_t pc) {
        return A32::LocationDescriptor{pc, A32::PSR{0x10}, A32::FPSCR{0}};
    };
    const auto blank = [&](uint32_t pc) {
        IR::Block block{loc(pc)};
        block.SetEndLocation(loc(pc + 4));
        block.SetTerminal(IR::Term::LinkBlock{loc(pc + 4)});
        block.CycleCount() = 1;
        return block;
    };
    const auto append = [](IR::Block &block, Op op,
                            std::initializer_list<Value> args) {
        block.AppendNewInst(op, args);
        return Value{&block.back()};
    };
    const auto emit = [&](std::initializer_list<const IR::Block *> input) {
        std::vector<const IR::Block *> blocks(input);
        std::vector<vita3k::wasmjit::RegionBlockMeta> meta;
        for (const auto *block : blocks) {
            const A32::LocationDescriptor at(block->Location());
            meta.push_back({at.PC(), PSR_DISPATCH_MASK,
                at.CPSR().Value() & PSR_DISPATCH_MASK,
                static_cast<uint32_t>(block->CycleCount()
                    + block->ConditionFailedCycleCount())});
        }
        return vita3k::wasmjit::emit_region(blocks, meta, options);
    };
    const auto run = [](const std::vector<uint8_t> &bytes, JitState &state,
                         uint32_t budget) {
        CHECK(!bytes.empty());
        const int slot = vita3k_jit_install_region(bytes.data(), bytes.size(),
            checked_memory_read, checked_memory_write);
        CHECK(slot >= 0);
        const auto reason = vita3k_jit_run(slot,
            reinterpret_cast<uintptr_t>(&state), budget);
        vita3k_jit_release_region(slot);
        return static_cast<Reason>(reason);
    };

    // Rejection after a valid prefix must discard the entire body.
    auto bad = blank(code);
    append(bad, Op::A32SetRegister, {Value{A32::Reg::R0}, Value{uint32_t(7)}});
    append(bad, Op::Breakpoint, {});
    CHECK(emit({&bad}).empty());

    // First SSA slot: a wide producer in A, then zero-extension in B.
    auto a = blank(code), b = blank(code + 4);
    const auto wide = append(a, Op::Pack2x32To1x64,
        {Value{uint32_t(0)}, Value{uint32_t(0xdeadbeef)}});
    append(a, Op::A32SetExtendedRegister64, {Value{A32::ExtReg::D0}, wide});
    const auto narrow = append(b, Op::ZeroExtendWordToLong, {Value{uint32_t(1)}});
    append(b, Op::A32SetExtendedRegister64, {Value{A32::ExtReg::D1}, narrow});
    append(b, Op::A32SetRegister,
        {Value{static_cast<A32::Reg>(15)}, Value{uint32_t(code + 8)}});
    append(b, Op::A32CallSupervisor, {Value{uint32_t(0x66)}});
    b.ReplaceTerminal(IR::Term::ReturnToDispatch{});
    const auto module = emit({&a, &b});
    JitState state{};
    state.regs[15] = code;
    state.cpsr = 0x10;
    CHECK(run(module, state, 2) == Reason::Svc);
    CHECK(state.fpu[1] == 0xdeadbeef);
    CHECK(state.fpu[2] == 1 && state.fpu[3] == 0);
    CHECK(state.next_pc == code + 8 && state.executed == 2);

    // Flags produced in one member feed a condition in another; an NZ-only
    // update then preserves C/V into a third member's full CPSR read and SVC.
    // Exhaustion before that SVC must publish exactly the pending member PC.
    auto flags_a = blank(code), flags_b = blank(code + 4), flags_c = blank(code + 8);
    append(flags_a, Op::A32SetCpsrNZCVRaw,
        {append(flags_a, Op::A32GetRegister, {Value{A32::Reg::R0}})});
    flags_b.SetCondition(IR::Cond::EQ);
    flags_b.SetConditionFailedLocation(loc(code + 8));
    flags_b.ConditionFailedCycleCount() = 1;
    append(flags_b, Op::A32SetCpsrNZ,
        {append(flags_b, Op::GetNZFromOp, {Value{uint32_t(0x80000000)}})});
    append(flags_c, Op::A32SetRegister, {Value{A32::Reg::R3},
        append(flags_c, Op::A32GetCpsr, {})});
    append(flags_c, Op::A32SetRegister,
        {Value{static_cast<A32::Reg>(15)}, Value{uint32_t(code + 12)}});
    append(flags_c, Op::A32CallSupervisor, {Value{uint32_t(0x42)}});
    flags_c.ReplaceTerminal(IR::Term::ReturnToDispatch{});
    const auto flags_module = emit({&flags_a, &flags_b, &flags_c});
    for (uint32_t flags = 0; flags < 16; ++flags) {
        for (uint32_t budget = 0; budget <= 4; ++budget) {
            state = JitState{};
            state.regs[0] = flags << 28;
            state.regs[3] = 0xbeef;
            state.regs[15] = code;
            state.cpsr = 0x080f00d0 | ((flags ^ 15) << 28); // preserve Q/GE/I/F/mode
            state.executed = 0xfffffffe; // wrapping accumulated counter
            const auto before_cpsr = state.cpsr;
            CHECK(run(flags_module, state, budget) == (budget >= 3 ? Reason::Svc : Reason::Budget));
            const uint32_t completed = budget >= 3 ? 3 : budget == 0 ? 0 : 1;
            CHECK(counter_delta(0xfffffffe, state.executed) == completed);
            uint32_t expected_flags = flags << 28;
            if (budget >= 3 && (flags & 4)) expected_flags = (expected_flags & 0x30000000) | 0x80000000;
            CHECK(state.cpsr == (completed ? (before_cpsr & 0x0fffffff) | expected_flags : before_cpsr));
            CHECK(state.regs[15] == code + 4 * completed && state.next_pc == state.regs[15]);
            CHECK(state.regs[3] == (budget >= 3 ? state.cpsr : 0xbeefu));
        }
    }

    // No side effects after SVC, including dead/invalidated IR markers.
    append(b, Op::Void, {});
    CHECK(emit({&b}).empty());

    for (const auto reason : {Reason::Stop, Reason::Smc}) {
        state = JitState{};
        state.regs[15] = code + 4;
        state.cpsr = 0x10;
        state.executed = 17;
        state.stop_flag = reason == Reason::Stop;
        state.smc_dirty = reason == Reason::Smc;
        CHECK(run(module, state, 2) == reason);
        CHECK(state.next_pc == code + 4 && state.executed == 17);
    }

    auto bx = blank(code);
    append(bx, Op::A32BXWritePC, {Value{uint32_t(code + 0x21)}});
    bx.ReplaceTerminal(IR::Term::ReturnToDispatch{});
    state = JitState{};
    state.regs[15] = code;
    state.cpsr = 0x10;
    CHECK(run(emit({&bx}), state, 1) == Reason::Miss);
    CHECK(state.regs[15] == code + 0x20 && state.next_pc == code + 0x20);
    CHECK((state.cpsr & 0x20) != 0);

    // Real region execution across both 32-bit counter wrap boundaries.
    auto loop = blank(code);
    const auto r0 = append(loop, Op::A32GetRegister, {Value{A32::Reg::R0}});
    const auto sum = append(loop, Op::Add32,
        {r0, Value{uint32_t(1)}, Value{false}});
    append(loop, Op::A32SetRegister, {Value{A32::Reg::R0}, sum});
    loop.ReplaceTerminal(IR::Term::LinkBlock{loc(code)});
    state = JitState{};
    state.regs[15] = code;
    state.cpsr = 0x10;
    state.executed = state.dispatches = 0xfffffff0;
    CHECK(run(emit({&loop}), state, 32) == Reason::Budget);
    CHECK(state.regs[0] == 32 && state.executed == 0x10);
    CHECK(counter_delta(0xfffffff0, state.executed) == 32);
    // Light dispatch path: the loop-back edge pre-checks the next block's
    // budget (REGION_ABI.md v1.2), so the failing iteration is NOT counted;
    // the old search-leaf check consumed one extra dispatch-loop trip (33).
    CHECK(counter_delta(0xfffffff0, state.dispatches) == 32);

    // Registers written in A survive B, including its condition-failed edge.
    // B's write-only register must retain the host value when B is skipped.
    auto cached_a = blank(code), cached_b = blank(code + 4);
    const auto cached_r0 = append(cached_a, Op::A32GetRegister, {Value{A32::Reg::R0}});
    const auto increment = append(cached_a, Op::Add32,
        {cached_r0, Value{uint32_t(1)}, Value{false}});
    append(cached_a, Op::A32SetRegister, {Value{A32::Reg::R0}, increment});
    append(cached_a, Op::A32SetRegister, {Value{A32::Reg::R1}, Value{uint32_t(123)}});
    cached_b.SetCondition(IR::Cond::EQ);
    cached_b.SetConditionFailedLocation(loc(code));
    cached_b.ConditionFailedCycleCount() = 1;
    const auto from_a = append(cached_b, Op::A32GetRegister, {Value{A32::Reg::R0}});
    append(cached_b, Op::A32SetRegister, {Value{A32::Reg::R2}, from_a});
    cached_b.ReplaceTerminal(IR::Term::LinkBlockFast{loc(code)});
    const auto cached_module = emit({&cached_a, &cached_b});
    for (const bool pass : {false, true}) {
        state = JitState{};
        state.regs[0] = 7;
        state.regs[1] = 0xbeef;
        state.regs[2] = 0x87654321;
        state.regs[15] = code;
        state.cpsr = 0x10 | (pass ? 0x40000000 : 0);
        state.executed = 17;
        state.dispatches = 29;
        // A, B, A fit; the conservative cost of the next B is two ticks.
        CHECK(run(cached_module, state, 4) == Reason::Budget);
        CHECK(state.regs[0] == 9 && state.regs[1] == 123);
        CHECK(state.regs[2] == (pass ? 8u : 0x87654321u));
        CHECK(state.executed == 20 && state.dispatches == 32);
        CHECK(state.regs[15] == code + 4 && state.next_pc == code + 4);
    }
    // Every entry initializes the cache even when no member body executes:
    // stop/SMC, generic budget failure, PC miss, CPSR mismatch, FPSCR mismatch.
    for (unsigned entry = 0; entry < 6; ++entry) {
        state = JitState{};
        state.regs[0] = 71;
        state.regs[1] = 72;
        state.regs[2] = 73;
        state.regs[15] = entry == 3 ? code + 8 : code;
        state.cpsr = entry == 4 ? 0x30 : 0x10;
        state.fpscr = entry == 5 ? 0x01000000 : 0;
        state.stop_flag = entry == 0;
        state.smc_dirty = entry == 1;
        state.executed = 17;
        state.dispatches = 29;
        const auto expected = entry == 0 ? Reason::Stop : entry == 1 ? Reason::Smc
            : entry == 2 ? Reason::Budget : Reason::Miss;
        CHECK(run(cached_module, state, entry == 2 ? 0 : 4) == expected);
        CHECK(state.regs[0] == 71 && state.regs[1] == 72 && state.regs[2] == 73);
        CHECK(state.executed == 17 && state.dispatches == 30);
        CHECK(state.next_pc == state.regs[15]);
    }
    // Enter a member directly with fresh host registers, then publish both
    // blocks' writes on the next budget exit.
    state = JitState{};
    state.regs[0] = 100;
    state.regs[15] = code + 4;
    state.cpsr = 0x40000010;
    CHECK(run(cached_module, state, 2) == Reason::Budget);
    CHECK(state.regs[0] == 101 && state.regs[1] == 123 && state.regs[2] == 100);
    CHECK(state.executed == 2 && state.dispatches == 2);

    // A fault in B must publish A's writes and preserve B's unexecuted
    // destination. Exercise both the disabled and permission-probe fallbacks.
    auto fault_a = blank(code);
    append(fault_a, Op::A32SetRegister, {Value{A32::Reg::R0}, Value{uint32_t(42)}});
    const uint32_t ldr = 0xe5912000; // LDR r2,[r1]
    CHECK(mem_write(mem, code + 4, &ldr, sizeof(ldr)));
    auto fault_b = vita3k::wasmjit::translate_block(mem, code + 4, 0x10, 1);
    const auto fault_module = emit({&fault_a, &fault_b});
    for (const bool probes : {false, true}) {
        state = JitState{};
        state.regs[1] = 0x83000000; // unallocated page
        state.regs[2] = 77;
        state.regs[15] = code;
        state.cpsr = 0x10;
        state.executed = 17;
        state.memory_cookie = reinterpret_cast<uintptr_t>(&mem);
        if (probes) {
            state.page_table_base = reinterpret_cast<uintptr_t>(mem.page_table.get());
            state.page_perms_base = reinterpret_cast<uintptr_t>(mem.page_permissions.get());
            state.code_pages_base = reinterpret_cast<uintptr_t>(g_code_pages.data());
        }
        CHECK(run(fault_module, state, 2) == Reason::Fault);
        CHECK(state.regs[0] == 42 && state.regs[2] == 77);
        CHECK(state.fault_pc == code + 4 && state.fault_address == 0x83000000);
        CHECK(state.executed == 18 && state.dispatches == 2);
    }

    // Sorted, overlapping blocks can produce page fragments in the order
    // P, P+1, P. Revalidation must still check each block's original bytes.
    CHECK(mem_set_permissions(mem, data, 2 * page, MemPerm::ReadWriteExecute));
    Region overlapping;
    for (const auto address : {data + page - 8, data + page - 4}) {
        RegionBlock block;
        block.pc = address;
        block.original.resize(address == data + page - 8 ? 16 : 4);
        CHECK(mem_fetch(mem, address, block.original.data(), block.original.size()));
        overlapping.blocks.push_back(std::move(block));
    }
    collect_code_pages(overlapping);
    CHECK(overlapping.validation_pages.size() == 2);
    CHECK(region_unchanged(overlapping, mem));
    overlapping.blocks[1].original[0] ^= 1;
    CHECK(!region_unchanged(overlapping, mem));
    overlapping.blocks[1].original[0] ^= 1;
    const uint8_t changed = overlapping.blocks[0].original[8] ^ 1;
    CHECK(mem_write(mem, data + page, &changed, 1));
    CHECK(!region_unchanged(overlapping, mem));
    CHECK(mem_write(mem, data + page, &overlapping.blocks[0].original[8], 1));
    CHECK(region_unchanged(overlapping, mem));
    CHECK(mem_set_permissions(mem, data + page, page, MemPerm::ReadWrite));
    CHECK(!region_unchanged(overlapping, mem));
    CHECK(mem_set_permissions(mem, data, 2 * page, MemPerm::ReadWrite));

    // Reference counts, cross-page writes, and full-width address rounding.
    Region tracked;
    RegionBlock tracked_block;
    tracked_block.pc = data + page;
    tracked_block.original.resize(4);
    tracked.blocks.push_back(std::move(tracked_block));
    collect_code_pages(tracked);
    CHECK(g_code_pages[data / page] == 0);
    CHECK(g_code_pages[data / page + 1] == 0);
    for (unsigned i = 0; i < 256; ++i) mark_code_pages(tracked, +1);
    CHECK(g_code_pages[data / page + 1] == 256);
    state = JitState{};
    state.memory_cookie = reinterpret_cast<uintptr_t>(&mem);
    state.memory_value[0] = 0x12345678;
    CHECK(checked_memory_write(&state, data + page - 2, 4) == 0);
    CHECK(state.smc_dirty == 1);
    for (unsigned i = 0; i < 256; ++i) mark_code_pages(tracked, -1);
    CHECK(g_code_pages[data / page + 1] == 0);
    tracked.blocks[0].pc = 0xfffff000;
    collect_code_pages(tracked);
    CHECK(tracked.page_begin == 0xfffff && tracked.page_end == 0x100000);

    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(4096);

    // A guest store changes the next instruction before it executes.
    put(mem, jit, {0xe5810000, 0xe3a02001, 0xef000042});
    jit.set_reg(0, 0xe3a0202a);
    jit.set_reg(1, code + 4);
    CHECK(jit.run() == 0 && parent.svc_called && jit.get_reg(2) == 42);
    CHECK(jit.invalidated_blocks() != 0);

    // 64 ARM instructions need a 256-byte snapshot.
    std::array<uint32_t, 65> long_code{};
    long_code.fill(0xe2800001);
    long_code.back() = 0xef000042;
    CHECK(mem_write(mem, code, long_code.data(), sizeof(long_code)));
    jit.invalidate_jit_cache(code, page);
    jit.set_cpsr(0x10);
    jit.set_pc(code);
    jit.set_reg(0, 0);
    CHECK(jit.run() == 0 && parent.svc_called && jit.get_reg(0) == 64);

    // BNE targets UDF, but Z=1 takes the supported SVC fallthrough.
    put(mem, jit, {0x1a000000, 0xef000042, 0xe7f000f0});
    jit.set_cpsr(0x40000010);
    CHECK(jit.run() == 0 && parent.svc_called && parent.svc == 0x42);

    // LRU eviction releases compiled regions and their page references.
    std::vector<uint32_t> calls(REGION_CACHE_LIMIT + 1, 0xef000042);
    const uint32_t cache_bytes = static_cast<uint32_t>(calls.size() * sizeof(uint32_t));
    // The enlarged production cache no longer fits this fixture in one page.
    const Address cache_code = alloc(mem, cache_bytes, "LRU eviction fixture");
    CHECK(cache_code != 0);
    CHECK(mem_write(mem, cache_code, calls.data(), cache_bytes));
    jit.set_cpsr(0x10);
    for (size_t i = 0; i < calls.size(); ++i) {
        jit.set_pc(cache_code + static_cast<uint32_t>(i * 4));
        CHECK(jit.run() == 0 && parent.svc_called);
    }
    const auto formed = jit.regions_formed();
    jit.set_pc(cache_code);
    CHECK(jit.run() == 0 && parent.svc_called);
    CHECK(jit.regions_formed() == formed + 1);
    jit.invalidate_jit_cache(cache_code, cache_bytes);
    free(mem, cache_code);
}
void region_store_continuations(MemState &mem, vita3k::wasmjit::RegionStateOptions options) {
    using Reason = vita3k::wasmjit::ExitReason;
    using Location = Dynarmic::A32::LocationDescriptor;
    Region region;
    std::vector<Dynarmic::IR::Block> ir;
    std::vector<vita3k::wasmjit::RegionBlockMeta> meta;
    // Pass an explicit cap so these tests exercise the side-exit machinery
    // end to end regardless of the production default.
    const auto install = [&](std::initializer_list<uint32_t> words, uint32_t cpsr = 0x10,
                            MemoryFunction write_memory = checked_memory_write) {
        CHECK(mem_write(mem, code, words.begin(), words.size() * sizeof(uint32_t)));
        CHECK(form_region(mem, code, cpsr, 0, region, ir, 64));
        std::vector<const Dynarmic::IR::Block *> blocks;
        meta.clear();
        for (size_t i = 0; i < ir.size(); ++i) {
            blocks.push_back(&ir[i]);
            const auto &block = region.blocks[i];
            meta.push_back({block.pc, block.psr_mask, block.psr_value, block.ticks,
                block.store_continuations});
        }
        const auto bytes = vita3k::wasmjit::emit_region(blocks, meta, options);
        CHECK(!bytes.empty());
        const int slot = vita3k_jit_install_region(bytes.data(), bytes.size(),
            checked_memory_read, write_memory);
        CHECK(slot >= 0);
        mark_code_pages(region, +1);
        return slot;
    };
    const auto release = [&](int slot) {
        vita3k_jit_release_region(slot);
        mark_code_pages(region, -1);
    };
    const auto initial = [&](bool fast) {
        JitState state{};
        state.regs[0] = 0x11;
        state.regs[1] = data;
        state.regs[2] = 0x22;
        state.regs[3] = 0xcafe;
        state.regs[15] = code;
        state.cpsr = 0x10;
        state.executed = 17;
        state.dispatches = 29;
        state.memory_cookie = reinterpret_cast<uintptr_t>(&mem);
        if (fast) {
            state.page_table_base = reinterpret_cast<uintptr_t>(mem.page_table.get());
            state.page_perms_base = reinterpret_cast<uintptr_t>(mem.page_permissions.get());
            state.code_pages_base = reinterpret_cast<uintptr_t>(g_code_pages.data());
        }
        return state;
    };
    const auto run = [&](int slot, JitState &state, uint32_t budget) {
        return static_cast<Reason>(vita3k_jit_run(slot,
            reinterpret_cast<uintptr_t>(&state), budget));
    };

    // STR r0,[r1],#4; STR r2,[r1],#4; ADD r3,r0,r2; SVC.
    // Both stores and all following work now fit in ONE dispatch body.
    int slot = install({0xe4810004, 0xe4812004, 0xe0803002, 0xef000042});
    CHECK(region.blocks.size() == 1 && meta[0].store_continuations.size() == 2);
    CHECK(meta[0].ticks == 4);
    CHECK(meta[0].store_continuations[0].completed_ticks == 1);
    CHECK(meta[0].store_continuations[1].completed_ticks == 2);
    for (const bool fast : {false, true}) {
        for (uint32_t budget = 0; budget <= 4; ++budget) {
            const std::array<uint32_t, 2> sentinels{0xaaaa, 0xbbbb};
            CHECK(mem_write(mem, data, sentinels.data(), sizeof(sentinels)));
            auto state = initial(fast);
            CHECK(run(slot, state, budget) == (budget == 4 ? Reason::Svc : Reason::Budget));
            const uint32_t completed = budget == 4 ? 4 : std::min(budget, 2u);
            const uint32_t stores = std::min(completed, 2u);
            CHECK(state.executed == 17 + completed && state.dispatches == 30);
            CHECK(state.regs[1] == data + 4 * stores); // post-index writeback
            CHECK(state.regs[3] == (budget == 4 ? 0x33u : 0xcafeu));
            CHECK(state.regs[15] == code + 4 * completed && state.next_pc == state.regs[15]);
            std::array<uint32_t, 2> actual{};
            CHECK(mem_read(mem, data, actual.data(), sizeof(actual)));
            CHECK(actual[0] == (stores >= 1 ? 0x11u : sentinels[0]));
            CHECK(actual[1] == (stores >= 2 ? 0x22u : sentinels[1]));
            CHECK(!state.smc_dirty);
        }
    }
    // The emitter must reject malformed boundaries before producing a module.
    auto invalid = meta[0].store_continuations;
    invalid[0].ir_offset = 0;
    CHECK(!vita3k::wasmjit::validate_region_block(ir[0], invalid));
    invalid = meta[0].store_continuations;
    invalid[1].completed_ticks = invalid[0].completed_ticks;
    CHECK(!vita3k::wasmjit::validate_region_block(ir[0], invalid));
    // Legacy translation still ends after the first complete store.
    auto legacy = vita3k::wasmjit::translate_block(mem, code, 0x10, 64);
    CHECK(legacy.CycleCount() == 1 && Location(legacy.EndLocation()).PC() == code + 4);
    release(slot);

    // Request stop from a successful checked store. Its base writeback must
    // complete before the continuation exits, and the next store must not run.
    const MemoryFunction stop_after_write = +[](JitState *state, uint32_t address, uint32_t bytes) noexcept {
        const auto result = checked_memory_write(state, address, bytes);
        if (!result)
            state->stop_flag = 1;
        return result;
    };
    slot = install({0xe4810004, 0xe4812004, 0xe0803002, 0xef000042}, 0x10, stop_after_write);
    auto stopped = initial(false);
    CHECK(run(slot, stopped, 4) == Reason::Stop);
    CHECK(stopped.regs[1] == data + 4 && stopped.regs[3] == 0xcafe);
    CHECK(stopped.executed == 18 && stopped.next_pc == code + 4);
    release(slot);

    // Flag production before a continuation must be visible on Stop/SMC/
    // Budget and remain authoritative after a successful checked helper.
    for (unsigned exit = 0; exit < 3; ++exit) {
        slot = install({0xe0900002, 0xe4810004, 0xe3a03063, 0xef000042},
            0x10, exit == 1 ? stop_after_write : checked_memory_write); // ADDS; STR!; MOV; SVC
        auto flags = initial(false);
        flags.regs[0] = 0x7fffffff; flags.regs[2] = 1;
        flags.cpsr = 0x680f00d0;
        if (exit == 2) flags.regs[1] = code + 8;
        CHECK(run(slot, flags, 2) == (exit == 1 ? Reason::Stop : exit == 2 ? Reason::Smc : Reason::Budget));
        CHECK(flags.cpsr == 0x980f00d0 && flags.executed == 19);
        CHECK(flags.regs[15] == code + 8 && flags.next_pc == code + 8);
        CHECK(flags.regs[1] == (exit == 2 ? code + 12 : data + 4) && flags.regs[3] == 0xcafe);
        release(slot);
    }

    // A self-modifying post-index store exits only after writeback, before
    // the overwritten next instruction. SMC also wins at a budget boundary.
    for (const uint32_t budget : {1u, 4u}) {
        slot = install({0xe4810004, 0xe4812004, 0xe0803002, 0xef000042});
        auto state = initial(true);
        state.regs[0] = 0xe3a0202a; // patch the following instruction
        state.regs[1] = code + 4;
        CHECK(run(slot, state, budget) == Reason::Smc);
        CHECK(state.regs[1] == code + 8 && state.regs[2] == 0x22);
        CHECK(state.regs[3] == 0xcafe && state.executed == 18);
        CHECK(state.regs[15] == code + 4 && state.next_pc == code + 4);
        CHECK(state.smc_dirty && state.smc_page == code / page);
        release(slot);
    }

    // STMIA r1!,{r0,r2}; MOV r3,#99; SVC. Both store elements and the
    // base writeback must finish before the SMC side exit.
    slot = install({0xe8a10005, 0xe3a03063, 0xef000042});
    CHECK(meta[0].store_continuations.size() == 1);
    auto state = initial(true);
    state.regs[1] = code + 4;
    CHECK(run(slot, state, 3) == Reason::Smc);
    CHECK(state.regs[1] == code + 12 && state.regs[3] == 0xcafe);
    CHECK(state.executed == 18 && state.next_pc == code + 4);
    std::array<uint32_t, 2> stored{};
    CHECK(mem_read(mem, code + 4, stored.data(), sizeof(stored)));
    CHECK(stored[0] == 0x11 && stored[1] == 0x22);
    release(slot);

    // Faults after an earlier store retain its writeback and tick, without
    // accounting or executing the faulting segment or subsequent MOV/SVC.
    for (const bool write_fault : {false, true}) {
        slot = install({0xe4810004, write_fault ? 0xe5842000u : 0xe5942000u,
            0xe3a03063, 0xef000042}); // STR/LDR r2,[r4]
        state = initial(true);
        state.regs[4] = 0x83000000;
        CHECK(run(slot, state, 4) == Reason::Fault);
        CHECK(state.regs[1] == data + 4 && state.regs[3] == 0xcafe);
        CHECK(state.executed == 18 && state.fault_pc == code + 4);
        CHECK(state.fault_address == 0x83000000 && state.fault_write == write_fault);
        release(slot);
    }

    // Thumb continuations use the actual instruction width. A 32-bit store
    // to the following halfword may modify two instructions; neither runs.
    slot = install({0x604a6008, 0xdf422363}, 0x30); // STR; STR; MOVS; SVC
    CHECK(region.blocks.size() == 1 && meta[0].store_continuations.size() == 2);
    state = initial(true);
    state.cpsr = 0x30;
    state.regs[1] = code + 2;
    CHECK(run(slot, state, 4) == Reason::Smc);
    CHECK(state.regs[15] == code + 2 && state.next_pc == code + 2);
    CHECK(state.cpsr == 0x30 && state.executed == 18);
    release(slot);

    // Predicated stores retain their existing block/IT boundary, even when
    // callers request continuations. Also verify output from a prior call
    // does not leak into a single-instruction retry.
    const uint32_t thumb_stores = 0x604a6008;
    CHECK(mem_write(mem, code, &thumb_stores, sizeof(thumb_stores)));
    std::vector<vita3k::wasmjit::StoreContinuation> points{{1, 0, 1}};
    auto predicated = vita3k::wasmjit::translate_block(mem, code, 0x40000430, 64, 0, &points);
    CHECK(predicated.GetCondition() == Dynarmic::IR::Cond::EQ && predicated.CycleCount() == 1);
    CHECK(points.empty() && Location(predicated.EndLocation()).IT().Value() == 0x08);
    auto single = vita3k::wasmjit::translate_block(mem, code, 0x30, 1, 0, &points);
    CHECK(single.CycleCount() == 1 && points.empty());

    // A conditional split discovered AFTER recording a store boundary makes
    // that store terminal again; its now-final continuation must be removed.
    const std::array<uint32_t, 3> split{0xe5810000, 0x02822001, 0xef000042};
    CHECK(mem_write(mem, code, split.data(), sizeof(split)));
    auto before_cond = vita3k::wasmjit::translate_block(mem, code, 0x10, 64, 0, &points);
    CHECK(before_cond.CycleCount() == 1 && points.empty());
    CHECK(Location(before_cond.EndLocation()).PC() == code + 4);

    // Continuing into UDF must preserve the supported prefix rather than
    // reducing its arithmetic instructions to separate one-tick blocks.
    const std::array<uint32_t, 3> suffix{0xe2800001, 0xe5810000, 0xe7f000f0};
    CHECK(mem_write(mem, code, suffix.data(), sizeof(suffix)));
    CHECK(form_region(mem, code, 0x10, 0, region, ir));
    CHECK(region.blocks.front().ticks == 2 && region.blocks.front().store_continuations.empty());

    // The default cap is 2: four stores yield two continuations and the block
    // ends after the third store-delimited segment. An explicit cap of 0
    // restores legacy store-ending blocks; unlimited merges all four stores.
    const std::array<uint32_t, 5> many{0xe4810004, 0xe4812004, 0xe4813004,
        0xe4810004, 0xef000042};
    CHECK(mem_write(mem, code, many.data(), sizeof(many)));
    auto capped = vita3k::wasmjit::translate_block(mem, code, 0x10, 64, 0, &points);
    CHECK(points.size() == 2);
    CHECK(Location(capped.EndLocation()).PC() == code + 12);
    auto legacy_stores = vita3k::wasmjit::translate_block(mem, code, 0x10, 64, 0, &points, 0);
    CHECK(points.empty());
    CHECK(Location(legacy_stores.EndLocation()).PC() == code + 4);
    auto merged = vita3k::wasmjit::translate_block(mem, code, 0x10, 64, 0, &points, 64);
    CHECK(points.size() == 4);
    CHECK(Location(merged.EndLocation()).PC() == code + 20);
}
// M16 Wasm-side dispatch pump: inter-region transfers without host exits.
// Region A (LDR r1,[pc]; BX r1) Miss-exits with next_pc=codeB; region B adds
// and SVCs. Loop region C and Thumb loop region T exercise slice/budget and
// the location-hash key paths. All expectations mirror host semantics.
void dispatch_pump(MemState &mem, vita3k::wasmjit::RegionStateOptions options) {
    using Reason = vita3k::wasmjit::ExitReason;
    using Location = Dynarmic::A32::LocationDescriptor;
    static bool installed = false;
    if (!installed) {
        const auto dbytes = vita3k::wasmjit::emit_dispatch();
        CHECK(!dbytes.empty());
        CHECK(vita3k_jit_install_dispatch(dbytes.data(), dbytes.size()) == 0);
        installed = true;
    }
    constexpr uint32_t codeB = code + 0x100, codeC = code + 0x200, codeT = code + 0x300;
    std::vector<Region> kept;
    std::vector<int> slots;
    const auto install_region = [&](uint32_t entry, uint32_t cpsr,
                                   std::optional<vita3k::wasmjit::RegionStateOptions> selected = std::nullopt) {
        Region region;
        std::vector<Dynarmic::IR::Block> ir;
        CHECK(form_region(mem, entry, cpsr, 0, region, ir));
        std::vector<const Dynarmic::IR::Block *> blocks;
        std::vector<vita3k::wasmjit::RegionBlockMeta> meta;
        uint32_t ticks = 0;
        for (size_t i = 0; i < ir.size(); ++i) {
            blocks.push_back(&ir[i]);
            const auto &b = region.blocks[i];
            meta.push_back({b.pc, b.psr_mask, b.psr_value, b.ticks, b.store_continuations});
            ticks += b.ticks;
        }
        const auto bytes = vita3k::wasmjit::emit_region(blocks, meta, selected.value_or(options));
        CHECK(!bytes.empty());
        const int slot = vita3k_jit_install_region(bytes.data(), bytes.size(),
            checked_memory_read, checked_memory_write);
        CHECK(slot >= 0);
        mark_code_pages(region, +1);
        slots.push_back(slot);
        const uint64_t key = Location(entry, Dynarmic::A32::PSR{cpsr}, Dynarmic::A32::FPSCR{0}).UniqueHash();
        CHECK(dispatch_map_insert(0, key, static_cast<uint32_t>(slot)));
        kept.push_back(std::move(region));
        return ticks;
    };
    const auto fresh = [&](uint32_t entry, uint32_t cpsr) {
        JitState state{};
        state.regs[15] = entry;
        state.cpsr = cpsr;
        state.memory_cookie = reinterpret_cast<uintptr_t>(&mem);
        state.page_table_base = reinterpret_cast<uintptr_t>(mem.page_table.get());
        state.page_perms_base = reinterpret_cast<uintptr_t>(mem.page_permissions.get());
        state.code_pages_base = reinterpret_cast<uintptr_t>(g_code_pages.data());
        return state;
    };
    const auto drun = [&](JitState &state, uint32_t remaining) {
        return static_cast<Reason>(vita3k_jit_run_dispatch(
            reinterpret_cast<uintptr_t>(&state),
            remaining, dispatch_map_base(0), dispatch_epoch_addr()));
    };
    // A: LDR r1,[pc,#4]; BX r1; NOP; .word codeB (literal at code+12).
    CHECK(mem_write(mem, code, std::array<uint32_t, 4>{0xe59f1004, 0xe12fff31, 0xe1a00000, codeB}.data(), 16));
    const uint32_t aTicks = install_region(code, 0x10);
    // B: ADD r0,r0,#1; SVC. Ends Svc with r0 == 1.
    CHECK(mem_write(mem, codeB, std::array<uint32_t, 2>{0xe2800001, 0xef000042}.data(), 8));
    const uint32_t bTicks = install_region(codeB, 0x10);
    CHECK(bTicks == 2);
    // A -> B -> Svc in one dispatcher call: exactly one in-Wasm transfer.
    auto state = fresh(code, 0x10);
    CHECK(drun(state, 100) == Reason::Svc);
    CHECK(state.regs[0] == 1 && state.executed == aTicks + bTicks);
    CHECK(state.tx_wasm == 1 && state.next_pc == codeB + 8);
    // SVC wins over budget at an exact boundary (remaining == A+B cost).
    state = fresh(code, 0x10);
    CHECK(drun(state, aTicks + bTicks) == Reason::Svc);
    CHECK(state.executed == aTicks + bTicks);
    // Transfer immediately before exhaustion: A completes, B cannot start.
    // B re-resolves once with zero progress, then Budget surfaces.
    state = fresh(code, 0x10);
    CHECK(drun(state, aTicks + 1) == Reason::Budget);
    CHECK(state.executed == aTicks && state.tx_wasm == 2);
    CHECK(state.next_pc == codeB);
    // C: ADD r0,r0,#1; B C. Ticks per iteration drive slice accounting.
    CHECK(mem_write(mem, codeC, std::array<uint32_t, 2>{0xe2800001, 0xeafffffd}.data(), 8));
    const uint32_t cTicks = install_region(codeC, 0x10);
    CHECK(cTicks == 2);
    // remaining = 0: Budget with no work, matching the host loop top.
    state = fresh(codeC, 0x10);
    CHECK(drun(state, 0) == Reason::Budget);
    CHECK(state.executed == 0 && state.tx_wasm == 0);
    // remaining < one iteration: entry check fails, no progress -> Budget.
    state = fresh(codeC, 0x10);
    CHECK(drun(state, 1) == Reason::Budget);
    CHECK(state.executed == 0 && state.next_pc == codeC);
    // Exact single iteration then exhaustion: full consumption -> Budget.
    state = fresh(codeC, 0x10);
    CHECK(drun(state, cTicks) == Reason::Budget);
    CHECK(state.executed == cTicks && state.regs[0] == 1);
    // Non-divisible total: two iterations plus a fruitless third slice.
    state = fresh(codeC, 0x10);
    CHECK(drun(state, 2 * cTicks + 1) == Reason::Budget);
    CHECK(state.executed == 2 * cTicks && state.regs[0] == 2);
    CHECK(state.tx_wasm == 1);
    // stop_flag short-circuits the pump.
    state = fresh(codeC, 0x10);
    state.stop_flag = 1;
    CHECK(drun(state, 100) == Reason::Stop);
    CHECK(state.executed == 0);
    // Unknown PC: Miss with next_pc published, nothing executed.
    state = fresh(0x83000000, 0x10);
    CHECK(drun(state, 100) == Reason::Miss);
    CHECK(state.next_pc == 0x83000000 && state.executed == 0);
    // Stale epoch: previously mapped entry now Misses; re-insert recovers.
    dispatch_bump_epoch();
    state = fresh(code, 0x10);
    CHECK(drun(state, 100) == Reason::Miss);
    CHECK(state.next_pc == code && state.executed == 0);
    const uint64_t akey = Location(code, Dynarmic::A32::PSR{0x10}, Dynarmic::A32::FPSCR{0}).UniqueHash();
    const uint64_t bkey = Location(codeB, Dynarmic::A32::PSR{0x10}, Dynarmic::A32::FPSCR{0}).UniqueHash();
    const uint64_t ckey = Location(codeC, Dynarmic::A32::PSR{0x10}, Dynarmic::A32::FPSCR{0}).UniqueHash();
    CHECK(dispatch_map_insert(0, akey, static_cast<uint32_t>(slots[0])));
    CHECK(dispatch_map_insert(0, bkey, static_cast<uint32_t>(slots[1])));
    CHECK(dispatch_map_insert(0, ckey, static_cast<uint32_t>(slots[2])));
    state = fresh(code, 0x10);
    CHECK(drun(state, 100) == Reason::Svc);
    CHECK(state.regs[0] == 1 && state.tx_wasm == 1);
    // Thumb entry resolve exercises T-bit key computation end to end.
    CHECK(mem_write(mem, codeT, std::array<uint32_t, 1>{0xe7fd3001}.data(), 4));
    const uint32_t tTicks = install_region(codeT, 0x30);
    CHECK(tTicks == 2);
    state = fresh(codeT, 0x30);
    CHECK(drun(state, 2 * tTicks) == Reason::Budget);
    CHECK(state.executed == 2 * tTicks && state.regs[0] == 2);
    CHECK(state.regs[15] == codeT);

    // Mixed-policy region calls must communicate through materialized state:
    // F produces carry/zero, G consumes carry after call_indirect. Then remove
    // G's mapping to exercise a true Miss with the same produced flags/PC.
    constexpr uint32_t codeF = code + 0x400, codeG = code + 0x500;
    CHECK(mem_write(mem, codeF, std::array<uint32_t, 2>{0xe2900001, 0xe12fff11}.data(), 8)); // ADDS; BX r1
    CHECK(mem_write(mem, codeG, std::array<uint32_t, 2>{0xe2a22000, 0xef000042}.data(), 8)); // ADC r2,r2,#0; SVC
    CHECK(install_region(codeF, 0x10) == 2);
    CHECK(install_region(codeG, 0x10, vita3k::wasmjit::RegionStateOptions{
        !options.promote_flags, !options.promote_accounting}) == 2);
    const auto flags_input = [&] {
        auto input = fresh(codeF, 0x980f00d0);
        input.regs[0] = UINT32_MAX;
        input.regs[1] = codeG;
        return input;
    };
    state = flags_input();
    CHECK(drun(state, 4) == Reason::Svc);
    CHECK(state.executed == 4 && state.tx_wasm == 1 && state.regs[2] == 1);
    CHECK(state.cpsr == 0x680f00d0 && state.next_pc == codeG + 8);
    dispatch_bump_epoch();
    const uint64_t fkey = Location(codeF, Dynarmic::A32::PSR{0x10}, Dynarmic::A32::FPSCR{0}).UniqueHash();
    const uint64_t gkey = Location(codeG, Dynarmic::A32::PSR{0x10}, Dynarmic::A32::FPSCR{0}).UniqueHash();
    CHECK(dispatch_map_insert(0, fkey, static_cast<uint32_t>(slots[slots.size() - 2])));
    state = flags_input();
    CHECK(drun(state, 4) == Reason::Miss);
    CHECK(state.executed == 2 && state.next_pc == codeG && state.regs[15] == codeG);
    CHECK(state.cpsr == 0x680f00d0 && state.regs[2] == 0);
    CHECK(dispatch_map_insert(0, gkey, static_cast<uint32_t>(slots.back())));
    CHECK(drun(state, 2) == Reason::Svc);
    CHECK(state.executed == 4 && state.regs[2] == 1 && state.cpsr == 0x680f00d0);
    for (int slot : slots)
        vita3k_jit_release_region(slot);
    for (auto &r : kept)
        mark_code_pages(r, -1);
}
} // namespace

int main() {
    MemState mem;
    CHECK(init(mem, true));
    CHECK(try_alloc_at(mem, code, page, "JIT backend tests") == code);
    CHECK(try_alloc_at(mem, data, 2 * page, "JIT checked memory") == data);
    inline_mutex_tests::run(mem);
    vector_tests::ir_vector_int_to_float();
    vector_tests::ir_shift32_frontier();
    vector_tests::ir_immediate_shifts();
    vector_tests::ir_data_movement();
    vector_tests::region_slot_reuse();
    vector_tests::guest_data_movement(mem);
    vector_tests::guest_structure_lanes(mem);
    vector_tests::guest_structure_multiple(mem);
    vector_integer_tests::ir_arithmetic();
    vector_integer_tests::guest_arithmetic(mem);
    vector_compare_tests::ir_comparisons();
    vector_compare_tests::guest_comparisons(mem);
    vectorfp_compare_tests::run(mem);
    byte_reverse_tests::ir_reversal();
    byte_reverse_tests::guest_reversal(mem);
    packed_saturate_tests::ir_saturation();
    packed_saturate_tests::guest_saturation(mem);
    exclusive_tests::guest_exclusive(mem);
    f64_tests::integer_to_double();
    f64_tests::float_to_int32();
    f64_tests::float_to_uint32();
    f64_tests::negate_and_absolute();
    f64_tests::single_to_double();
    f64_tests::double_to_single();
    f64_tests::compare64();
    f64_tests::mode_guards();
    f64_tests::integer_widen_shift();
    f64_tests::binary_arithmetic();
    f64_tests::guest_multiply(mem);
    f64_tests::guest_float_to_int(mem);
    f64_tests::guest_tls_write(mem);
    fp64_helper_tests::run();
    recip_tests::run();
    tofixed_tests::run();
    vectormul_tests::run();
    fpsqrt_tests::run();
    helpers(mem);
    tls_read(mem);
    memory_barriers(mem);
    cooperative_slices(mem);
    leading_zeros(mem);
    multiply32(mem);
    unsigned_long_multiply(mem);
    signed_long_multiply(mem);
    floating_compare32(mem);
    floating_abs32(mem);
    unsigned_integer_to_float32(mem);
    integer_to_float32(mem);
    float_divide32(mem);
    scalar_binary32_batch(mem);
    scalar_fp_mode_guards(mem);
    floating_compare_trap_guard(mem);
    backend(mem);
    formation(mem);
    region_exec(mem);
    region_budget_continuations(mem);
    // Direct-emitter matrix is independent of the process environment. CPU
    // integration cases above use the process's selected representation;
    // run this executable in fresh processes to cover that layer too
    // (PROMOTE_FLAGS=0/1 x PROMOTE_ACCOUNTING unset/1 covers all four
    // process defaults; the matrix below additionally crosses the
    // assume_fast_bases guard shape in every mode).
    for (unsigned mode = 0; mode < 8; ++mode) {
        const vita3k::wasmjit::RegionStateOptions options{(mode & 1) != 0, (mode & 2) != 0, (mode & 4) != 0};
        dispatch_bump_epoch(); // released slots from prior cases must not resolve
        region_regressions(mem, options);
        region_store_continuations(mem, options);
        dispatch_bump_epoch();
        dispatch_pump(mem, options);
    }
    deinit_mem(mem);
    std::printf("WasmJit backend: %u checks passed (real memory, no interpreter)\n", checks);
}
