// Experimental browser-only Dynarmic-IR -> WebAssembly CPU backend.
// Deliberately opt-in; unsupported translations fail without interpreter fallback.
#pragma once
#include <cpu/impl/interface.h>
#include <cstdint>
#include <memory>
#include <string>

class WasmJitCPU final : public CPUInterface {
public:
    WasmJitCPU(CPUState *state, std::size_t processor_id);
    ~WasmJitCPU() override;
    int run() override;
    int step() override;
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
    bool is_thumb_mode() override;
    bool hit_breakpoint() override; void trigger_breakpoint() override;
    void set_log_code(bool value) override; void set_log_mem(bool value) override;
    bool get_log_code() override; bool get_log_mem() override;
    void clear_exclusive() override;
    std::size_t processor_id() const override;
    void invalidate_jit_cache(Address, size_t) override;

    void set_instruction_budget(uint64_t value);
    // Region modules (many blocks, in-Wasm dispatch) vs single-block modules.
    // Default: region mode (M14c production path).
    void set_region_mode(bool value);
    const std::string &get_last_error() const;
    // Valid after a generated memory-fault exit. CPU state is restored to the
    // faulting instruction's entry; earlier stores in that instruction may
    // already be visible (memory accesses are checked, not transactional).
    uint32_t get_fault_address() const;
    bool get_fault_write() const;
    uint64_t instructions_executed() const;
    uint64_t compiled_blocks() const;
    // Region-mode metric: successfully installed code regions (a region
    // batches many basic blocks into one WebAssembly.Module).
    uint64_t regions_formed() const;
    // One-line phase profile: emit/install/run ms, dispatch and helper counts.
    std::string get_profile() const;
    uint64_t cache_hits() const;
    uint64_t invalidated_blocks() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
