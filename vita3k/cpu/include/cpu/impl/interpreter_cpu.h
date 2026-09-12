#pragma once

#include <cpu/impl/interface.h>

#include <array>
#include <cstdint>

// Small cooperative ARM backend for browser bring-up. It deliberately supports
// only the instructions needed to exercise ThreadState's existing run/SVC seam.
class InterpreterCPU final : public CPUInterface {
public:
    InterpreterCPU(CPUState *state, std::size_t processor_id);
    int run() override;
    void stop() override;
    uint32_t get_reg(uint8_t idx) override;
    void set_reg(uint8_t idx, uint32_t value) override;
    uint32_t get_sp() override; void set_sp(uint32_t value) override;
    uint32_t get_pc() override; void set_pc(uint32_t value) override;
    uint32_t get_lr() override; void set_lr(uint32_t value) override;
    uint32_t get_cpsr() override; void set_cpsr(uint32_t value) override;
    uint32_t get_tpidruro() override; void set_tpidruro(uint32_t value) override;
    float get_float_reg(uint8_t idx) override; void set_float_reg(uint8_t idx, float value) override;
    uint32_t get_fpscr() override; void set_fpscr(uint32_t value) override;
    CPUContext save_context() override; void load_context(const CPUContext &ctx) override;
    bool is_thumb_mode() override; int step() override;
    bool hit_breakpoint() override; void trigger_breakpoint() override;
    void set_log_code(bool value) override; void set_log_mem(bool value) override;
    bool get_log_code() override; bool get_log_mem() override;
    void clear_exclusive() override;
    std::size_t processor_id() const override;
    void invalidate_jit_cache(Address, size_t) override {}

private:
    CPUState *parent;
    std::array<uint32_t, 16> regs{};
    std::array<float, 64> float_regs{};
    uint32_t cpsr = 0, fpscr = 0, tpidruro = 0;
    std::size_t core_id;
    bool stopped = false, breakpoint = false, log_code = false, log_mem = false;
};
