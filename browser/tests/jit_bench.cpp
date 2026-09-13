// Matched to jit_bench.S with VitaSDK as + objdump. Register-only loop:
// this benchmark is NOT a homebrew/display speedup claim.
#include <cpu/functions.h>
#include <cpu/impl/interpreter_cpu.h>
#include <cpu/impl/wasm_jit_cpu.h>
#include <cpu/state.h>
#include <mem/functions.h>
#include <chrono>
#include <cstdio>
#include <stdexcept>

int main() {
    MemState mem;
    if (!init(mem, true)) return 1;
    constexpr Address pc = 0x81000000;
    constexpr uint16_t code[] = {0x2000, 0x3001, 0x3901, 0xd1fc, 0xdf42};
    if (try_alloc_at(mem, pc, 4096, "JIT benchmark") != pc || !mem_write(mem, pc, code, sizeof(code))) return 2;
    {
        auto a = init_cpu(false, 1, 0, mem);
        auto b = init_cpu(false, 2, 0, mem);
        b->cpu = std::make_unique<WasmJitCPU>(b.get(), 0);
        auto &jit = static_cast<WasmJitCPU &>(*b->cpu);
        auto &reference = static_cast<InterpreterCPU &>(*a->cpu);
        constexpr uint32_t iterations = 200000;
        constexpr uint64_t instructions = uint64_t(iterations) * 3 + 2;
        jit.set_instruction_budget(instructions);
        reference.set_instruction_budget(instructions);
        const auto sample = [&](CPUState &cpu, const char *name, unsigned round) {
            CPUContext start{};
            start.cpsr = 0x30;
            start.cpu_registers[15] = pc;
            start.cpu_registers[1] = iterations;
            load_context(cpu, start);
            const auto before = std::chrono::steady_clock::now();
            if (run(cpu) != 0 || !cpu.svc_called || cpu.svc != 0x42
                || read_reg(cpu, 0) != iterations || read_reg(cpu, 1) != 0
                || read_pc(cpu) != pc + sizeof(code) || read_cpsr(cpu) != 0x60000030)
                throw std::runtime_error("benchmark architectural check failed");
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count();
            std::printf("M14 BENCH %s round=%u instructions=%llu ms=%.3f MIPS=%.3f\n", name, round,
                (unsigned long long)instructions, ms, double(instructions) / ms / 1000.0);
        };
        for (unsigned i = 0; i < 5; ++i) {
            if (i & 1) { sample(*b, "jit", i); sample(*a, "interpreter", i); }
            else { sample(*a, "interpreter", i); sample(*b, "jit", i); }
            const auto x = save_context(*a), y = save_context(*b);
            if (x.cpu_registers != y.cpu_registers || x.cpsr != y.cpsr) return 3;
        }
        std::printf("M14 BENCH compiled=%llu cache_hits=%llu invalidated=%llu (no fallback)\n",
            (unsigned long long)jit.compiled_blocks(), (unsigned long long)jit.cache_hits(),
            (unsigned long long)jit.invalidated_blocks());
    }
    deinit_mem(mem);
    return 0;
}
