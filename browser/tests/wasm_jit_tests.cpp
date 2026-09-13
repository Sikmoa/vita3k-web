// M14: real InterpreterCPU oracle + opt-in WasmJitCPU, never a mock CPU.
// Native builds exercise the reference/architectural checks only; execution of
// generated Wasm is tested ONLY under Emscripten (Node and a browser Worker).
#include <cpu/functions.h>
#include <cpu/impl/interpreter_cpu.h>
#include <cpu/state.h>
#ifdef __EMSCRIPTEN__
#include <cpu/impl/wasm_jit_cpu.h>
#include <emscripten/emscripten.h>
#include <emscripten/heap.h>
#endif
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <mem/ptr.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
#include "jit_probe.inc"

constexpr Address code_address = 0x81000000;
constexpr size_t mapped_size = 0x2000;
constexpr uint32_t nzcv_mask = 0xf0000000;
constexpr uint32_t initial_cpsr = 0x080f0010; // Q, GE, user mode; no IT state.
constexpr uint32_t initial_fpscr = 0x01400010;
constexpr uint32_t initial_svc = 0xabcdef;
constexpr uint32_t tls_marker = 0x81001a00;
constexpr uint64_t instruction_budget = 128;

#define REQUIRE(expr) do { if (!(expr)) throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #expr); } while (false)

struct Memory {
    MemState state;
    Memory() {
        REQUIRE(::init(state, true));
        if (try_alloc_at(state, code_address, mapped_size, "M14 JIT probes") != code_address) {
            deinit_mem(state);
            throw std::runtime_error("M14 code allocation failed");
        }
    }
    ~Memory() { deinit_mem(state); }
};

struct Snapshot {
    CPUContext context;
    uint32_t tpidruro;
    bool svc_called;
    uint32_t svc;
};

Snapshot snapshot(CPUState &cpu) {
    return { save_context(cpu), read_tpidruro(cpu), cpu.svc_called, cpu.svc };
}

void equal_u32(const std::string &label, uint32_t actual, uint32_t expected) {
    if (actual == expected) return;
    char text[128];
    std::snprintf(text, sizeof(text), ": got 0x%08x, expected 0x%08x", actual, expected);
    throw std::runtime_error(label + text);
}

void equal_state(const Snapshot &actual, const Snapshot &expected, const std::string &label) {
    for (unsigned r = 0; r < 16; ++r)
        equal_u32(label + " r" + std::to_string(r), actual.context.cpu_registers[r], expected.context.cpu_registers[r]);
    equal_u32(label + " CPSR", actual.context.cpsr, expected.context.cpsr);
    equal_u32(label + " FPSCR", actual.context.fpscr, expected.context.fpscr);
    equal_u32(label + " TPIDRURO", actual.tpidruro, expected.tpidruro);
    equal_u32(label + " svc_called", actual.svc_called, expected.svc_called);
    equal_u32(label + " svc", actual.svc, expected.svc);
    REQUIRE(std::memcmp(actual.context.fpu_registers.data(), expected.context.fpu_registers.data(),
        sizeof(actual.context.fpu_registers)) == 0);
}

Snapshot initial(bool thumb, uint32_t flags = 0) {
    Snapshot result{};
    for (unsigned r = 0; r < 16; ++r)
        result.context.cpu_registers[r] = 0x10203000 + r * 0x101;
    for (unsigned r = 0; r < 64; ++r)
        result.context.fpu_registers[r] = float(r) + 0.25f;
    result.context.set_pc(code_address | unsigned(thumb));
    result.context.set_sp(code_address + mapped_size - 32);
    result.context.cpsr |= initial_cpsr | flags;
    result.context.fpscr = initial_fpscr;
    result.tpidruro = tls_marker;
    result.svc = initial_svc;
    return result;
}

void restore(CPUState &cpu, const Snapshot &value) {
    load_context(cpu, value.context);
    write_tpidruro(cpu, value.tpidruro);
    cpu.svc_called = value.svc_called;
    cpu.svc = value.svc;
}

Snapshot at_svc(Snapshot value, size_t length) {
    value.context.cpu_registers[15] = code_address + uint32_t(length);
    value.svc_called = true;
    value.svc = 0x42;
    return value;
}

using Bytes = std::vector<uint8_t>;
Bytes read_bytes(MemState &mem, Address address, size_t size) {
    Bytes bytes(size);
    REQUIRE(mem_read(mem, address, bytes.data(), bytes.size()));
    return bytes;
}

struct Fixture {
    Memory memory; // CPUs must die before MemState.
    CPUStatePtr reference;
#ifdef __EMSCRIPTEN__
    CPUStatePtr generated;
    WasmJitCPU &jit() { return static_cast<WasmJitCPU &>(*generated->cpu); }
#endif
    InterpreterCPU &interpreter() { return static_cast<InterpreterCPU &>(*reference->cpu); }

    Fixture() {
        reference = init_cpu(false, 1, 0, memory.state);
        REQUIRE(reference != nullptr);
        // Explicitly select the oracle even if the native build defaults to Dynarmic.
        reference->cpu = std::make_unique<InterpreterCPU>(reference.get(), 0);
        interpreter().set_instruction_budget(instruction_budget);
#ifdef __EMSCRIPTEN__
        generated = init_cpu(false, 2, 0, memory.state);
        REQUIRE(generated != nullptr);
        generated->cpu = std::make_unique<WasmJitCPU>(generated.get(), 0);
        jit().set_instruction_budget(instruction_budget);
#endif
    }

    void install(const uint8_t *bytes, size_t size) {
        Bytes sentinel(mapped_size);
        for (size_t i = 0; i < sentinel.size(); ++i) sentinel[i] = uint8_t(i * 37 + 0x59);
        REQUIRE(mem_write(memory.state, code_address, sentinel.data(), sentinel.size()));
        REQUIRE(mem_write(memory.state, code_address, bytes, size));
#ifdef __EMSCRIPTEN__
        invalidate_jit_cache(*generated, code_address, mapped_size);
#endif
    }

    Snapshot run_reference(const Snapshot &start) {
        restore(*reference, start);
        const auto bytes = read_bytes(memory.state, code_address, mapped_size);
        const int result = run(*reference);
        if (result != 0) throw std::runtime_error(interpreter().get_last_error());
        REQUIRE(reference->svc_called);
        REQUIRE(bytes == read_bytes(memory.state, code_address, mapped_size));
        return snapshot(*reference);
    }

#ifdef __EMSCRIPTEN__
    Snapshot run_jit(const Snapshot &start) {
        restore(*generated, start);
        const auto bytes = read_bytes(memory.state, code_address, mapped_size);
        const auto instructions = jit().instructions_executed();
        const int result = run(*generated);
        if (result != 0) throw std::runtime_error(jit().get_last_error());
        REQUIRE(jit().get_last_error().empty());
        REQUIRE(generated->svc_called);
        REQUIRE(jit().compiled_blocks() > 0);
        REQUIRE(jit().instructions_executed() > instructions);
        REQUIRE(bytes == read_bytes(memory.state, code_address, mapped_size));
        return snapshot(*generated);
    }
#endif

    void compare(const Snapshot &start, const Snapshot &expected) {
        const auto oracle = run_reference(start);
        equal_state(oracle, expected, "Interpreter vs architecture");
#ifdef __EMSCRIPTEN__
        const auto actual = run_jit(start);
        equal_state(actual, expected, "WasmJit vs architecture");
        equal_state(actual, oracle, "WasmJit vs Interpreter");
        // Re-enter with identical state: require an actual hot cache entry, not
        // just another translation producing the same answer.
        const auto compiled = jit().compiled_blocks();
        const auto hits = jit().cache_hits();
        equal_state(run_jit(start), oracle, "hot WasmJit vs Interpreter");
        REQUIRE(jit().compiled_blocks() == compiled);
        REQUIRE(jit().cache_hits() > hits);
#endif
    }
};

struct Suite {
    unsigned passed = 0, failed = 0, oracle_gaps = 0;
    void test(const std::string &label, const std::function<void()> &body) {
        try {
            body();
            ++passed;
            std::printf("M14 PASS %s\n", label.c_str());
        } catch (const std::exception &error) {
            ++failed;
            std::fprintf(stderr, "M14 FAIL %s: %s\n", label.c_str(), error.what());
        }
    }
};

void loops_and_conditions(Suite &suite, Fixture &fixture) {
    suite.test("Thumb MOV/ADD/SUB/CMP/BNE taken+fallthrough loop", [&] {
        fixture.install(thumb_loop, sizeof(thumb_loop));
        const auto start = initial(true, 0x90000000);
        auto expected = at_svc(start, sizeof(thumb_loop));
        expected.context.cpu_registers[0] = 21;
        expected.context.cpu_registers[1] = 0;
        expected.context.cpsr = (start.context.cpsr & ~nzcv_mask) | 0x60000000;
        fixture.compare(start, expected);
    });
    for (bool taken : { false, true }) {
        suite.test(taken ? "ARM BNE taken" : "ARM BNE fallthrough", [&] {
            fixture.install(arm_conditional, sizeof(arm_conditional));
            const auto start = initial(false, taken ? 0x90000000 : 0xd0000000);
            auto expected = at_svc(start, sizeof(arm_conditional));
            expected.context.cpu_registers[0] = 7;
            expected.context.cpu_registers[1] = 5;
            expected.context.cpu_registers[2] = 12;
            if (!taken) expected.context.cpu_registers[3] = 99;
            fixture.compare(start, expected);
        });
    }
    suite.test("ARM full loop (independent expectation; oracle availability checked)", [&] {
        fixture.install(arm_loop, sizeof(arm_loop));
        const auto start = initial(false, 0x90000000);
        auto expected = at_svc(start, sizeof(arm_loop));
        expected.context.cpu_registers[0] = 21;
        expected.context.cpu_registers[1] = 0;
        expected.context.cpu_registers[2] = 7;
        expected.context.cpsr = (start.context.cpsr & ~nzcv_mask) | 0x60000000;
        restore(*fixture.reference, start);
        const auto bytes = read_bytes(fixture.memory.state, code_address, mapped_size);
        const int result = run(*fixture.reference);
        REQUIRE(bytes == read_bytes(fixture.memory.state, code_address, mapped_size));
        if (result == 0) {
            equal_state(snapshot(*fixture.reference), expected, "ARM loop oracle vs architecture");
        } else {
            // Current real InterpreterCPU::arm has no SUB-immediate or CMP.
            // Verify the actual supported prefix; NEVER invent its final state.
            REQUIRE(result < 0);
            REQUIRE(fixture.interpreter().get_last_error().find("unsupported") != std::string::npos);
            auto prefix = start;
            prefix.context.cpu_registers[0] = 7;
            prefix.context.cpu_registers[1] = 3;
            prefix.context.cpu_registers[2] = 7;
            prefix.context.cpu_registers[15] = code_address + 16;
            equal_state(snapshot(*fixture.reference), prefix, "known ARM oracle boundary");
            ++suite.oracle_gaps;
            std::puts("M14 ORACLE GAP: ARM full loop stops at SUB; final differential NOT RUN");
        }
#ifdef __EMSCRIPTEN__
        equal_state(fixture.run_jit(start), expected, "ARM loop JIT vs architecture");
        const auto compiled = fixture.jit().compiled_blocks();
        const auto hits = fixture.jit().cache_hits();
        equal_state(fixture.run_jit(start), expected, "ARM loop hot JIT vs architecture");
        REQUIRE(fixture.jit().compiled_blocks() == compiled);
        REQUIRE(fixture.jit().cache_hits() > hits);
#else
        std::puts("M14 NOT RUN: ARM full-loop final execution requires Emscripten JIT");
#endif
    });
}

void arithmetic(Suite &suite, Fixture &fixture) {
    // Explicit ARMv7 NZCV truth-table values, not values computed by the oracle.
    struct Edge { const char *name; uint32_t a, b, result, flags; };
    constexpr Edge additions[] = {
        { "unsigned carry/zero", 0xffffffff, 1, 0, 0x60000000 },
        { "signed positive overflow", 0x7fffffff, 1, 0x80000000, 0x90000000 },
        { "signed negative overflow/carry", 0x80000000, 0x80000000, 0, 0x70000000 },
        { "negative no overflow", 0xffffffff, 0xffffffff, 0xfffffffe, 0xa0000000 },
        { "clears stale NZCV", 1, 2, 3, 0 },
    };
    constexpr Edge subtractions[] = {
        { "borrow", 0, 1, 0xffffffff, 0x80000000 },
        { "equal/no borrow", 0x80000000, 0x80000000, 0, 0x60000000 },
        { "signed negative overflow", 0x80000000, 1, 0x7fffffff, 0x30000000 },
        { "signed positive overflow", 0x7fffffff, 0xffffffff, 0x80000000, 0x90000000 },
        { "no borrow/no overflow", 5, 3, 2, 0x20000000 },
    };
    for (bool thumb : { false, true }) {
        for (const auto &edge : additions) {
            suite.test(std::string(thumb ? "Thumb ADDS " : "ARM ADDS ") + edge.name, [&] {
                const size_t size = thumb ? sizeof(thumb_add) : sizeof(arm_add);
                fixture.install(thumb ? thumb_add : arm_add, size);
                auto start = initial(thumb, nzcv_mask);
                start.context.cpu_registers[0] = edge.a;
                start.context.cpu_registers[1] = edge.b;
                auto expected = at_svc(start, size);
                expected.context.cpu_registers[2] = edge.result;
                expected.context.cpsr = (start.context.cpsr & ~nzcv_mask) | edge.flags;
                fixture.compare(start, expected);
            });
        }
    }
    for (bool compare_only : { false, true }) {
        for (const auto &edge : subtractions) {
            suite.test(std::string(compare_only ? "Thumb CMP " : "Thumb SUBS ") + edge.name, [&] {
                const size_t size = compare_only ? sizeof(thumb_cmp) : sizeof(thumb_sub);
                fixture.install(compare_only ? thumb_cmp : thumb_sub, size);
                auto start = initial(true, nzcv_mask);
                start.context.cpu_registers[0] = edge.a;
                start.context.cpu_registers[1] = edge.b;
                auto expected = at_svc(start, size);
                if (!compare_only) expected.context.cpu_registers[2] = edge.result;
                expected.context.cpsr = (start.context.cpsr & ~nzcv_mask) | edge.flags;
                fixture.compare(start, expected);
            });
        }
    }
}

void cache_mutations(Suite &suite, Fixture &fixture) {
    for (bool thumb : { false, true }) {
        suite.test(thumb ? "Thumb cache invalidation and code writes" : "ARM cache invalidation and code writes", [&] {
            const size_t size = thumb ? sizeof(thumb_patch) : sizeof(arm_patch);
            const size_t width = thumb ? 2 : 4;
            fixture.install(thumb ? thumb_patch : arm_patch, size);
            const auto start = initial(thumb, nzcv_mask);
            auto check = [&](uint32_t immediate) {
                auto expected = at_svc(start, size);
                expected.context.cpu_registers[0] = immediate;
                if (thumb) expected.context.cpsr &= ~0xc0000000u; // MOVS clears NZ, preserves CV.
                fixture.compare(start, expected);
            };
            check(7);
#ifdef __EMSCRIPTEN__
            auto compiled = fixture.jit().compiled_blocks();
            auto invalidated = fixture.jit().invalidated_blocks();
            invalidate_jit_cache(*fixture.generated, code_address, width);
            REQUIRE(fixture.jit().invalidated_blocks() > invalidated);
            check(7); // Identical bytes must still recompile after explicit invalidation.
            REQUIRE(fixture.jit().compiled_blocks() > compiled);
            compiled = fixture.jit().compiled_blocks();
#endif
            REQUIRE(mem_write(fixture.memory.state, code_address,
                thumb ? thumb_patch_11 : arm_patch_11, width));
            check(11); // Deliberately no invalidate_jit_cache: checked guest write.
#ifdef __EMSCRIPTEN__
            REQUIRE(fixture.jit().compiled_blocks() > compiled);
            compiled = fixture.jit().compiled_blocks();
#endif
            auto *trusted = Ptr<uint8_t>(code_address).get(fixture.memory.state);
            REQUIRE(trusted != nullptr);
            std::memcpy(trusted, thumb ? thumb_patch_13 : arm_patch_13, width);
            check(13); // Trusted Ptr bypass: entry-time source-byte validation is required.
#ifdef __EMSCRIPTEN__
            REQUIRE(fixture.jit().compiled_blocks() > compiled);
#endif
            std::memcpy(trusted, thumb ? thumb_patch_17 : arm_patch_17, width);
#ifdef __EMSCRIPTEN__
            invalidate_jit_cache(*fixture.generated, code_address, width);
#endif
            check(17);
#ifdef __EMSCRIPTEN__
            // Cached code must not bypass a later execute-permission removal.
            compiled = fixture.jit().compiled_blocks();
            invalidated = fixture.jit().invalidated_blocks();
            REQUIRE(mem_set_permissions(fixture.memory.state, code_address, size, MemPerm::ReadWrite));
            restore(*fixture.generated, start);
            const int denied = run(*fixture.generated);
            REQUIRE(mem_set_permissions(fixture.memory.state, code_address, size, MemPerm::ReadWriteExecute));
            REQUIRE(denied < 0);
            equal_state(snapshot(*fixture.generated), start, "NX cached entry unchanged");
            REQUIRE(fixture.jit().invalidated_blocks() > invalidated);
            check(17);
            REQUIRE(fixture.jit().compiled_blocks() > compiled);

            // Generated modules share the SAME Memory object across growth.
            REQUIRE(emscripten_resize_heap(emscripten_get_heap_size() + 65536));
            compiled = fixture.jit().compiled_blocks();
            check(17);
            REQUIRE(fixture.jit().compiled_blocks() == compiled);

            // Unmapping a hot page must fail, not execute stale translated code.
            const auto saved_bytes = read_bytes(fixture.memory.state, code_address, mapped_size);
            free(fixture.memory.state, code_address);
            restore(*fixture.generated, start);
            const int unmapped = run(*fixture.generated);
            REQUIRE(try_alloc_at(fixture.memory.state, code_address, mapped_size, "remapped JIT test") == code_address);
            REQUIRE(mem_write(fixture.memory.state, code_address, saved_bytes.data(), saved_bytes.size()));
            REQUIRE(unmapped < 0);
            equal_state(snapshot(*fixture.generated), start, "unmapped cached entry unchanged");
            check(17);
#endif
        });
    }
}

void failure_boundaries(Suite &suite, Fixture &fixture) {
    for (bool thumb : { false, true }) {
        suite.test(thumb ? "Thumb unsupported instruction is atomic" : "ARM unsupported instruction is atomic", [&] {
            fixture.install(thumb ? thumb_unsupported : arm_unsupported,
                thumb ? sizeof(thumb_unsupported) : sizeof(arm_unsupported));
            const auto start = initial(thumb, nzcv_mask);
            const auto bytes = read_bytes(fixture.memory.state, code_address, mapped_size);
            restore(*fixture.reference, start);
            REQUIRE(run(*fixture.reference) < 0);
            REQUIRE(!fixture.interpreter().get_last_error().empty());
            equal_state(snapshot(*fixture.reference), start, "unsupported Interpreter unchanged");
            REQUIRE(bytes == read_bytes(fixture.memory.state, code_address, mapped_size));
#ifdef __EMSCRIPTEN__
            restore(*fixture.generated, start);
            REQUIRE(run(*fixture.generated) < 0);
            REQUIRE(!fixture.jit().get_last_error().empty());
            equal_state(snapshot(*fixture.generated), start, "unsupported WasmJit unchanged");
            REQUIRE(bytes == read_bytes(fixture.memory.state, code_address, mapped_size));
#endif
        });
        suite.test(thumb ? "Thumb checked load matches oracle" : "ARM checked load matches oracle", [&] {
            // M14b lowers this through checked MemState helpers. Successful
            // loads must match the oracle, without modifying guest memory.
            const size_t size = thumb ? sizeof(thumb_memory_unsupported) : sizeof(arm_memory_unsupported);
            fixture.install(thumb ? thumb_memory_unsupported : arm_memory_unsupported, size);
            auto start = initial(thumb, nzcv_mask);
            start.context.cpu_registers[0] = code_address + 0x1100;
            const uint32_t payload = 0x13579bdf;
            REQUIRE(mem_write(fixture.memory.state, start.context.cpu_registers[0], &payload, sizeof(payload)));
            const auto bytes = read_bytes(fixture.memory.state, code_address, mapped_size);
            auto expected = at_svc(start, size);
            expected.context.cpu_registers[2] = payload;
            fixture.compare(start, expected);
#ifdef __EMSCRIPTEN__
            start.context.cpu_registers[0] = 0x20000000; // deliberately unmapped
            restore(*fixture.generated, start);
            REQUIRE(run(*fixture.generated) < 0);
            REQUIRE(!fixture.jit().get_last_error().empty());
            equal_state(snapshot(*fixture.generated), start, "faulting load leaves CPU state unchanged");
#endif
            REQUIRE(bytes == read_bytes(fixture.memory.state, code_address, mapped_size));
        });
#ifdef __EMSCRIPTEN__
        suite.test(thumb ? "Thumb valid prefix executes before unsupported instruction" : "ARM valid prefix executes before unsupported instruction", [&] {
            fixture.install(thumb ? thumb_prefix_unsupported : arm_prefix_unsupported,
                thumb ? sizeof(thumb_prefix_unsupported) : sizeof(arm_prefix_unsupported));
            const auto start = initial(thumb, nzcv_mask);
            const auto bytes = read_bytes(fixture.memory.state, code_address, mapped_size);
            restore(*fixture.generated, start);
            REQUIRE(run(*fixture.generated) < 0);
            REQUIRE(!fixture.jit().get_last_error().empty());
            auto stopped = start;
            stopped.context.cpu_registers[3] = 99;
            stopped.context.cpu_registers[15] += thumb ? 2 : 4;
            if (thumb) stopped.context.cpsr &= ~0xc0000000u; // MOVS prefix
            equal_state(snapshot(*fixture.generated), stopped, "valid single-instruction prefix commits before fault");
            REQUIRE(bytes == read_bytes(fixture.memory.state, code_address, mapped_size));
        });
#endif
        suite.test(thumb ? "Thumb runaway budget" : "ARM runaway budget", [&] {
            fixture.install(thumb ? thumb_budget : arm_budget,
                thumb ? sizeof(thumb_budget) : sizeof(arm_budget));
            const auto start = initial(thumb);
            const auto bytes = read_bytes(fixture.memory.state, code_address, mapped_size);
            restore(*fixture.reference, start);
            REQUIRE(run(*fixture.reference) < 0);
            REQUIRE(fixture.interpreter().get_last_error().find("budget") != std::string::npos);
            equal_state(snapshot(*fixture.reference), start, "budget Interpreter state");
#ifdef __EMSCRIPTEN__
            restore(*fixture.generated, start);
            const auto count = fixture.jit().instructions_executed();
            REQUIRE(run(*fixture.generated) < 0);
            REQUIRE(fixture.jit().get_last_error().find("budget") != std::string::npos);
            REQUIRE(fixture.jit().instructions_executed() - count <= instruction_budget);
            equal_state(snapshot(*fixture.generated), start, "budget WasmJit state");
#endif
            REQUIRE(bytes == read_bytes(fixture.memory.state, code_address, mapped_size));
        });
    }
}

struct Runtime {
    Memory memory;
    KernelState kernel;
    ThreadStatePtr thread;
    bool initialized = false;
    ~Runtime() {
        if (thread) kernel.threads.erase(thread->id);
        thread.reset();
        if (initialized) kernel.deinit(memory.state);
    }
};

Snapshot runtime_probe(bool use_jit) {
    Runtime runtime;
    REQUIRE(mem_write(runtime.memory.state, code_address, arm_runtime, sizeof(arm_runtime)));
    unsigned imports = 0;
    uint32_t seen_nid = 0;
    SceUID seen_thread = 0;
    Snapshot seen{};
    REQUIRE(runtime.kernel.init(runtime.memory.state, [&](CPUState &cpu, uint32_t nid, SceUID thread_id) {
        ++imports;
        seen_nid = nid;
        seen_thread = thread_id;
        seen = snapshot(cpu);
        runtime.thread->exit_delete(false); // Stop BEFORE the return slot; not Vita HLE.
    }, false));
    runtime.initialized = true;
    runtime.thread = std::make_shared<ThreadState>(runtime.kernel.get_next_uid(), runtime.kernel, runtime.memory.state);
    auto &thread = *runtime.thread;
    REQUIRE(thread.init("M14 preimport probe", Ptr<const void>(code_address), SCE_KERNEL_DEFAULT_PRIORITY,
        SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_DEFAULT, nullptr) == 0);
    REQUIRE(thread.cpu != nullptr);
    const auto original_context = snapshot(*thread.cpu);
    const auto core_id = get_processor_id(*thread.cpu);
#ifdef __EMSCRIPTEN__
    if (use_jit) {
        auto backend = std::make_unique<WasmJitCPU>(thread.cpu.get(), core_id);
        backend->set_instruction_budget(instruction_budget);
        thread.cpu->cpu = std::move(backend);
    } else
#else
    REQUIRE(!use_jit);
#endif
    {
        auto backend = std::make_unique<InterpreterCPU>(thread.cpu.get(), core_id);
        backend->set_instruction_budget(instruction_budget);
        thread.cpu->cpu = std::move(backend);
    }
    restore(*thread.cpu, original_context); // Including TPIDRURO initialized by ThreadState.
    runtime.kernel.threads.emplace(thread.id, runtime.thread);
    REQUIRE(thread.start(0, Ptr<void>(0)) == SCE_KERNEL_OK);
    auto expected = at_svc(snapshot(*thread.cpu), 8);
    expected.context.cpu_registers[0] = 42;
    const auto code_before = read_bytes(runtime.memory.state, code_address, mapped_size);
    const auto stack_before = read_bytes(runtime.memory.state, thread.stack.get(), thread.stack_size);
    thread.run_loop(true);
    REQUIRE(imports == 1);
    equal_u32("preimport NID", seen_nid, 0x12345678);
    REQUIRE(seen_thread == thread.id);
    equal_state(seen, expected, "preimport architectural state");
    equal_state(snapshot(*thread.cpu), expected, "run_loop stopped before return slot");
    REQUIRE(thread.status == ThreadStatus::dormant);
    REQUIRE(get_current_cpu_state() == nullptr);
    REQUIRE(code_before == read_bytes(runtime.memory.state, code_address, mapped_size));
    REQUIRE(stack_before == read_bytes(runtime.memory.state, thread.stack.get(), thread.stack_size));
#ifdef __EMSCRIPTEN__
    if (use_jit) REQUIRE(static_cast<WasmJitCPU &>(*thread.cpu->cpu).compiled_blocks() > 0);
#endif
    return seen;
}
} // namespace

extern "C" {
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int vita3k_web_jit_tests() {
    Suite suite;
    try {
        Fixture fixture;
        loops_and_conditions(suite, fixture);
        arithmetic(suite, fixture);
        cache_mutations(suite, fixture);
        failure_boundaries(suite, fixture);
    } catch (const std::exception &error) {
        ++suite.failed;
        std::fprintf(stderr, "M14 fixture failure: %s\n", error.what());
    }
    suite.test("real KernelState ThreadState::run_loop(true) preimport", [&] {
        const auto oracle = runtime_probe(false);
#ifdef __EMSCRIPTEN__
        equal_state(runtime_probe(true), oracle, "preimport JIT vs Interpreter");
#else
        (void)oracle;
#endif
    });
#ifdef __EMSCRIPTEN__
    constexpr const char *mode = "emscripten-jit";
#else
    constexpr const char *mode = "native-reference-only";
    std::puts("M14 NOT RUN: native cannot execute WasmJitCPU; no JIT/fallback claimed");
#endif
    std::printf("M14 SUMMARY mode=%s passed=%u failed=%u oracle_gaps=%u\n",
        mode, suite.passed, suite.failed, suite.oracle_gaps);
    return suite.failed ? 1 : 0;
}
}

#ifndef VITA3K_JIT_TEST_NO_MAIN
int main() { return vita3k_web_jit_tests(); }
#endif
