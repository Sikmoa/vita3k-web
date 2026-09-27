#pragma once

#include <cpu/impl/interface.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

// Cooperative ARMv7 backend for the observed VitaSDK crt0 instruction subset.
// Unsupported instructions and runaway execution fail explicitly; this is not a
// complete ARM emulator.
class InterpreterCPU final : public CPUInterface {
public:
    InterpreterCPU(CPUState *state, std::size_t processor_id);
    int run() override;
    void stop() override;
    uint32_t get_reg(uint8_t idx) override;
    void set_reg(uint8_t idx, uint32_t value) override;
    uint32_t get_sp() override; void set_sp(uint32_t value) override;
    uint32_t get_pc() noexcept override; void set_pc(uint32_t value) override;
    uint32_t get_lr() override; void set_lr(uint32_t value) override;
    uint32_t get_cpsr() override; void set_cpsr(uint32_t value) override;
    uint32_t get_tpidruro() override; void set_tpidruro(uint32_t value) override;
    float get_float_reg(uint8_t idx) override; void set_float_reg(uint8_t idx, float value) override;
    uint32_t get_fpscr() override; void set_fpscr(uint32_t value) override;
    CPUContext save_context() override; void load_context(const CPUContext &ctx) override;
    bool is_thumb_mode() override; int step() override;
    bool hit_breakpoint() noexcept override; void trigger_breakpoint() override;
    void set_log_code(bool value) override; void set_log_mem(bool value) override;
    bool get_log_code() override; bool get_log_mem() override;
    void clear_exclusive() noexcept override;
    std::size_t processor_id() const override;
    void invalidate_jit_cache(Address, size_t) override {}

    // Budget applies to each run() (SVC remains a cooperative return boundary).
    void set_instruction_budget(uint64_t value) { instruction_budget = value; }
    const std::string &get_last_error() const { return last_error; }
    // Total guest instructions executed by this backend (benchmarking).
    uint64_t instructions_executed() const { return executed_instructions; }

private:
    int thumb16(uint16_t op, uint32_t pc, bool in_it);
    int thumb32(uint16_t hi, uint16_t lo, uint32_t pc);
    int arm(uint32_t op, uint32_t pc);
    uint32_t operand(unsigned reg, uint32_t pc, bool thumb) const;
    void nz(uint32_t value);
    void nzc(uint32_t value, bool carry);
    uint32_t add(uint32_t a, uint32_t b, bool carry, bool flags);
    bool condition(unsigned cond) const;
    uint8_t itstate() const;
    void set_itstate(uint8_t value);
    void advance_it();
    int fail(uint32_t pc, uint32_t opcode, const char *reason);
    CPUState *parent;
    std::array<uint32_t, 16> regs{};
    std::array<uint32_t, 64> float_regs{};
    uint32_t cpsr = 0, fpscr = 0, tpidruro = 0;
    std::size_t core_id;
    std::atomic<bool> stopped{false};
    bool breakpoint = false, log_code = false, log_mem = false;
    // Effectively unlimited: production hosts run guest threads for whole
    // frames. Tests that want budget exhaustion call set_instruction_budget.
    uint64_t instruction_budget = 1'000'000'000'000;
    uint64_t executed_instructions = 0;
    std::string last_error;
};
