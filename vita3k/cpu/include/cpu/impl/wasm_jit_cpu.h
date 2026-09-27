// Experimental browser-only Dynarmic-IR -> WebAssembly CPU backend.
// Deliberately opt-in; unsupported translations fail without interpreter fallback.
#pragma once
#include <cpu/impl/interface.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vita3k::wasmjit { struct InlineMutexTable; }
struct MemState;

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
    uint32_t get_pc() noexcept override; void set_pc(uint32_t value) override;
    uint32_t get_lr() override; void set_lr(uint32_t value) override;
    uint32_t get_cpsr() override; void set_cpsr(uint32_t value) override;
    uint32_t get_tpidruro() override; void set_tpidruro(uint32_t value) override;
    float get_float_reg(uint8_t idx) override; void set_float_reg(uint8_t idx, float value) override;
    uint32_t get_fpscr() override; void set_fpscr(uint32_t value) override;
    CPUContext save_context() override; void load_context(const CPUContext &ctx) override;
    bool is_thumb_mode() override;
    bool hit_breakpoint() noexcept override; void trigger_breakpoint() override;
    void set_log_code(bool value) override; void set_log_mem(bool value) override;
    bool get_log_code() override; bool get_log_mem() override;
    void clear_exclusive() noexcept override;
    std::size_t processor_id() const override;
    // Guest code in [start, start+length) changed: drops lazy regions and
    // disables every AOT function whose code overlaps the range.
    void invalidate_jit_cache(Address, size_t) override;
    // Releases this CPU's lazy code caches without implying a code change
    // (the loaded AOT module is unaffected). Used when a guest thread exits.
    void release_code_caches();

    void set_instruction_budget(uint64_t value);
    // Explicit scheduler boundary, distinct from halt (1), SVC (0), and
    // fault (<0). Ordinary run() retains its fatal runaway-budget contract.
    static constexpr int slice_yield = 2;
    // Translation failures are reported through the result like other CPU
    // errors, never thrown.
    int run_slice(uint64_t instructions);
    // Region modules (many blocks, in-Wasm dispatch) vs single-block modules.
    // Default: region mode (M14c production path).
    void set_region_mode(bool value);
    // Cooperative runtime only: borrowed host table (nullptr disables).
    // The caller must commit dirty entries before any HLE/scheduler observer.
    static bool inline_mutex_fast_paths_enabled();
    void set_inline_mutex_table(vita3k::wasmjit::InlineMutexTable *table) noexcept;
    // Diagnostic: false keeps this CPU on the lazy JIT even where the loaded
    // AOT module has code (VITA3K_AOT_EXCLUDE_THREADS).
    void set_aot_enabled(bool enabled);
    // Diagnostic: stop entering the loaded AOT module in every thread
    // (VITA3K_AOT_UNTIL); execution continues on the lazy JIT.
    static void disable_aot();
    const std::string &get_last_error() const;
    // Valid after a generated memory-fault exit. CPU state is restored to the
    // faulting instruction's entry; earlier stores in that instruction may
    // already be visible (memory accesses are checked, not transactional).
    uint32_t get_fault_address() const;
    bool get_fault_write() const;
    uint64_t instructions_executed() const;
    // Wall milliseconds spent inside generated guest code (run_js_ms in get_profile).
    double run_ms() const;
    uint64_t compiled_blocks() const;
    // Region-mode metric: successfully installed code regions (a region
    // batches many basic blocks into one WebAssembly.Module).
    uint64_t regions_formed() const;
    // AOT-1: compile+install the region closure for the CURRENT pc/cpsr/fpscr
    // through the standard form/emit/install/table path without executing
    // guest code. Guest-address semantics are unchanged and the installed
    // entry holds no host addresses, table slots, JS identities, allocator
    // pointers, or cross-MemState data (the dispatch slot is resolved per
    // entry at execution, exactly as on the lazy path). Dynamic fallback is
    // retained: run()/run_slice() behave identically with or without a
    // precompile call. Set the entry pc/cpsr/fpscr first; fails closed
    // (ok=false) on unsupported IR, unmapped code, or rejected Wasm.
    struct PrecompileResult {
        bool ok = false;
        bool cache_hit = false;
        std::string error;
        uint32_t entry_pc = 0;
        size_t blocks = 0;
        uint32_t ticks = 0;
        size_t wasm_bytes = 0;
        double emit_ms = 0;
        double install_ms = 0;
    };
    PrecompileResult precompile_region();
    // One-line phase profile: emit/install/run ms, dispatch and helper counts.
    std::string get_profile() const;
    // Step-2 dispatch telemetry for the alternating-vs-single-CPU measurement.
    // Same values as the host_entries=... tail of get_profile(), in a form
    // tests can difference without parsing. All counters are per-CPU except
    // the process-global memory helper tallies (not included here).
    struct PumpCounters {
        uint64_t host_entries = 0, post_hle_entries = 0;
        uint64_t version_syncs = 0, version_bumps = 0;
        uint64_t entry_scanned = 0, entry_evicted = 0;
        uint64_t select_checks = 0, select_stale = 0, capacity_evictions = 0;
        uint64_t js_calls = 0, host_miss = 0, tx_wasm = 0;
    };
    PumpCounters pump_counters() const;
    uint64_t cache_hits() const;
    // Writes the location keys of every block translated so far (one hex
    // Dynarmic LocationDescriptor value per line) when VITA3K_AOT_SEEDS_OUT
    // enabled recording. Input for the AOT builder's root set.
    static bool dump_aot_seeds(const char *path);
    // Ahead-of-time whole-program module (vita3k/cpu/src/wasmjit/AOT.md).
    // Locations are Dynarmic A32 LocationDescriptor values (UniqueHash).
    struct AotBuildSpec {
        struct Range {
            uint32_t base = 0, size = 0; // executable guest bytes
        };
        std::vector<Range> code;
        std::vector<uint64_t> function_roots; // known function entries
        std::vector<uint64_t> extra_entries;  // must be covered (e.g. JIT seeds)
    };
    static constexpr uint32_t aot_magic = 0x544f4156; // "VAOT"
    // Root location for a code address (bit 0 = Thumb) in the default
    // execution state: IT/E clear and the default FPSCR mode.
    static uint64_t aot_location(uint32_t address);
    // Bytes of guest code at the start of a module's text segment
    // [text, text + size): up to the .ARM.exidx end-of-code sentinel when the
    // table [exidx_begin, exidx_end) ends with one, and never past the module
    // info (read-only data) when every function in the table starts below it.
    static uint32_t aot_code_size(MemState &mem, uint32_t text, uint32_t size, uint32_t exidx_begin, uint32_t exidx_end,
        uint32_t module_info);
    // Function starts (bit 0 = Thumb) of the .ARM.exidx table
    // [exidx_begin, exidx_end). An entry without the Thumb bit whose first
    // ARM instruction is conditional is a Thumb function (see the definition).
    static std::vector<uint32_t> aot_exidx_functions(MemState &mem, uint32_t exidx_begin, uint32_t exidx_end);
    // Bump when the module ABI changes: JitState layout, imports or the
    // fp64 helper operations generated code calls (fp64.h).
    static constexpr uint32_t aot_version = 4;
    // Translates and emits the module from the currently loaded guest code.
    static bool build_aot(MemState &mem, const AotBuildSpec &spec, std::vector<uint8_t> &out, std::string &report);
    // Loads the module the host supplied (Module.vita3kAotModule or the Node
    // VITA3K_AOT file) after verifying it against loaded guest code.
    // Returns 1 loaded, 0 none supplied, -1 rejected (lazy JIT only).
    static int load_aot(MemState &mem, std::string &report);
    // Retires the loaded AOT functions covering [start, start + length).
    static void retire_aot(Address start, size_t length);
    uint64_t invalidated_blocks() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
