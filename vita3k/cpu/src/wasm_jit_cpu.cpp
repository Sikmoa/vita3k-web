// Opt-in M14 proof: Dynarmic frontend/IR -> generated Wasm -> call_indirect.
// No interpreter fallback. The display application's default backend is unchanged.
#include <cpu/impl/wasm_jit_cpu.h>
#include <cpu/state.h>
#include <mem/functions.h>
#include "wasmjit/frontend.h"
#include "wasmjit/emit_wasm.h"
#include <dynarmic/frontend/A32/a32_location_descriptor.h>
#include <dynarmic/ir/basic_block.h>
#include <emscripten.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

// Compilation alone crosses JS. Cache-hit execution is a Wasm function-pointer
// call through the same table, not a JS function wrapping each guest block.
EM_JS(int, vita3k_jit_install, (const uint8_t *bytes, unsigned length), {
    let slot = -1;
    const freeSlots = Module['vita3kJitFreeSlots'] || (Module['vita3kJitFreeSlots'] = []);
    try {
        const module = new WebAssembly.Module(HEAPU8.slice(bytes, bytes + length));
        const instance = new WebAssembly.Instance(module, {env: {memory: wasmMemory}});
        slot = freeSlots.length ? freeSlots.pop() : wasmTable.grow(1);
        setWasmTableEntry(slot, instance.exports.block);
        return slot;
    } catch (error) {
        if (slot >= 0) { setWasmTableEntry(slot, null); freeSlots.push(slot); }
        console.error('Vita3K JIT compilation failed:', error);
        return -1;
    }
});
EM_JS(void, vita3k_jit_release, (int slot), {
    setWasmTableEntry(slot, null);
    (Module['vita3kJitFreeSlots'] || (Module['vita3kJitFreeSlots'] = [])).push(slot);
});

struct WasmJitCPU::Impl {
    using State = vita3k::wasmjit::JitState;
    using Key = std::pair<uint64_t, uint32_t>;
    struct Block {
        Address pc;
        std::vector<uint8_t> original;
        int table_index;
    };
    CPUState *parent;
    std::size_t core;
    State state{};
    std::array<uint32_t, 64> floats{};
    uint32_t tpidruro = 0;
    std::atomic<bool> stopped{false};
    bool breakpoint = false, log_code = false, log_mem = false;
    uint64_t budget = 1'000'000, executed = 0, compiled = 0, hits = 0, invalidated = 0;
    std::string error;
    std::map<Key, Block> cache;

    Impl(CPUState *parent, std::size_t core) : parent(parent), core(core) {}
    ~Impl() { clear(); }
    void clear() {
        for (const auto &[key, block] : cache) vita3k_jit_release(block.table_index);
        invalidated += cache.size();
        cache.clear();
    }
    int fail(const char *reason) {
        error = reason;
        std::fprintf(stderr, "WasmJitCPU PC=%08x: %s\n", state.regs[15], reason);
        return -1;
    }
    bool unchanged(const Block &block) const {
        // M14.0 conservative coherence policy: revalidate executable bytes on
        // EVERY entry. This catches even trusted Ptr/HLE writes, remapping, and
        // permission changes without altering MemState or the reference CPU.
        // Replace with per-page generations only after every write path is tracked.
        std::array<uint8_t, 128> bytes{};
        return mem_fetch(*parent->mem, block.pc, bytes.data(), block.original.size())
            && std::equal(block.original.begin(), block.original.end(), bytes.begin());
    }
    int execute(uint32_t limit) {
        using namespace Dynarmic::A32;
        const LocationDescriptor location(state.regs[15], PSR{state.cpsr}, FPSCR{state.fpscr});
        const Key key{location.UniqueHash(), limit};
        try {
            auto found = cache.find(key);
            if (found != cache.end() && !unchanged(found->second)) {
                vita3k_jit_release(found->second.table_index);
                cache.erase(found);
                ++invalidated;
                found = cache.end();
            }
            if (found == cache.end()) {
                if (cache.size() >= 1024) clear(); // bounded prototype cache
                const auto ir = vita3k::wasmjit::translate_block(*parent->mem,
                    state.regs[15], state.cpsr, limit, state.fpscr);
                // EndLocation is the sequential end of decoded bytes, not a
                // branch target. Reject wraparound/empty or unexpectedly large blocks.
                const uint64_t end = LocationDescriptor(ir.EndLocation()).PC();
                const uint64_t start = state.regs[15];
                if (end <= start || end - start > 128)
                    return fail("unsupported translated code span");
                Block block{state.regs[15], std::vector<uint8_t>(end - start), -1};
                if (!mem_fetch(*parent->mem, block.pc, block.original.data(), block.original.size()))
                    return fail("guest code unavailable at compilation");
                auto bytes = vita3k::wasmjit::emit_block(ir);
                if (bytes.empty()) return fail("unsupported Dynarmic IR or terminal (no fallback)");
                // Allocate cache entry before installing so allocation failure
                // cannot leak a function-table slot.
                found = cache.emplace(key, std::move(block)).first;
                found->second.table_index = vita3k_jit_install(bytes.data(), bytes.size());
                if (found->second.table_index < 0) {
                    cache.erase(found);
                    return fail("browser rejected generated Wasm");
                }
                ++compiled;
            } else ++hits;
            if (log_code) std::printf("JIT block PC=%08x table=%d\n", state.regs[15], found->second.table_index);
            state.executed = 0;
            state.exit_reason = 0;
            // Generated modules do not throw C++ exceptions. noexcept is vital:
            // otherwise Emscripten lowers this to invoke_ii (a JS trampoline),
            // not a direct Wasm call_indirect. Guest faults use return reasons.
            using Function = uint32_t (*)(State *) noexcept;
            const auto function = reinterpret_cast<Function>(static_cast<uintptr_t>(found->second.table_index));
            const uint32_t reason = function(&state); // compiled as call_indirect
            executed += state.executed;
            if (!state.executed || state.executed > limit) return fail("invalid generated instruction count");
            if (reason == static_cast<uint32_t>(vita3k::wasmjit::ExitReason::Svc)) {
                parent->svc = state.svc;
                parent->svc_called = true;
                return 0;
            }
            if (reason != 0) return fail("generated block returned unsupported/fault exit");
            return 0;
        } catch (const std::exception &e) {
            return fail(e.what());
        }
    }
};

WasmJitCPU::WasmJitCPU(CPUState *parent, std::size_t core) : impl(std::make_unique<Impl>(parent, core)) {}
WasmJitCPU::~WasmJitCPU() = default;
int WasmJitCPU::run() {
    impl->stopped = false;
    impl->parent->svc_called = false;
    impl->error.clear();
    const auto start = impl->executed;
    while (impl->executed - start < impl->budget) {
        if (impl->stopped || impl->breakpoint) return 1;
        const uint32_t limit = std::min<uint64_t>(32, impl->budget - (impl->executed - start));
        const int result = impl->execute(limit);
        if (result || impl->parent->svc_called) return result;
    }
    return impl->fail("instruction budget exhausted");
}
int WasmJitCPU::step() {
    impl->parent->svc_called = false;
    impl->error.clear();
    if (impl->breakpoint) return 1;
    return impl->execute(1);
}
void WasmJitCPU::stop() { impl->stopped = true; }
uint32_t WasmJitCPU::get_reg(uint8_t i) { return impl->state.regs[i & 15]; }
void WasmJitCPU::set_reg(uint8_t i, uint32_t v) { impl->state.regs[i & 15] = v; }
uint32_t WasmJitCPU::get_sp() { return get_reg(13); }
void WasmJitCPU::set_sp(uint32_t v) { set_reg(13, v); }
uint32_t WasmJitCPU::get_lr() { return get_reg(14); }
void WasmJitCPU::set_lr(uint32_t v) { set_reg(14, v); }
uint32_t WasmJitCPU::get_pc() { return get_reg(15); }
void WasmJitCPU::set_pc(uint32_t v) {
    impl->state.cpsr = (impl->state.cpsr & ~0x20u) | ((v & 1) ? 0x20u : 0);
    set_reg(15, v & ((v & 1) ? ~1u : ~3u));
}
uint32_t WasmJitCPU::get_cpsr() { return impl->state.cpsr; }
void WasmJitCPU::set_cpsr(uint32_t v) { impl->state.cpsr = v; }
uint32_t WasmJitCPU::get_fpscr() { return impl->state.fpscr; }
void WasmJitCPU::set_fpscr(uint32_t v) { impl->state.fpscr = v; }
uint32_t WasmJitCPU::get_tpidruro() { return impl->tpidruro; }
void WasmJitCPU::set_tpidruro(uint32_t v) { impl->tpidruro = v; }
float WasmJitCPU::get_float_reg(uint8_t i) { return std::bit_cast<float>(impl->floats[i & 63]); }
void WasmJitCPU::set_float_reg(uint8_t i, float v) { impl->floats[i & 63] = std::bit_cast<uint32_t>(v); }
CPUContext WasmJitCPU::save_context() {
    CPUContext context{};
    std::copy_n(impl->state.regs, 16, context.cpu_registers.begin());
    for (unsigned i = 0; i < 64; ++i) context.fpu_registers[i] = std::bit_cast<float>(impl->floats[i]);
    context.cpsr = impl->state.cpsr;
    context.fpscr = impl->state.fpscr;
    return context;
}
void WasmJitCPU::load_context(const CPUContext &c) {
    std::copy(c.cpu_registers.begin(), c.cpu_registers.end(), impl->state.regs);
    for (unsigned i = 0; i < 64; ++i) impl->floats[i] = std::bit_cast<uint32_t>(c.fpu_registers[i]);
    impl->state.cpsr = c.cpsr;
    impl->state.fpscr = c.fpscr;
}
void WasmJitCPU::invalidate_jit_cache(Address start, size_t length) {
    if (!length) return;
    const uint64_t end = uint64_t(start) + std::min<uint64_t>(length, 0x100000000ULL - start);
    // Invalidate every block touching any page in the changed range.
    const uint64_t first_page = start / 4096, end_page = (end + 4095) / 4096;
    for (auto it = impl->cache.begin(); it != impl->cache.end();) {
        const auto &b = it->second;
        const uint64_t b_end = (uint64_t(b.pc) + b.original.size() + 4095) / 4096;
        if (b.pc / 4096 < end_page && first_page < b_end) {
            vita3k_jit_release(b.table_index);
            it = impl->cache.erase(it);
            ++impl->invalidated;
        } else ++it;
    }
}
bool WasmJitCPU::is_thumb_mode() { return impl->state.cpsr & 0x20; }
bool WasmJitCPU::hit_breakpoint() { return impl->breakpoint; }
void WasmJitCPU::trigger_breakpoint() { impl->breakpoint = true; stop(); }
void WasmJitCPU::set_log_code(bool v) { impl->log_code = v; }
void WasmJitCPU::set_log_mem(bool v) { impl->log_mem = v; }
bool WasmJitCPU::get_log_code() { return impl->log_code; }
bool WasmJitCPU::get_log_mem() { return impl->log_mem; }
void WasmJitCPU::clear_exclusive() { /* Exclusive IR is rejected; no monitor is acquired. */ }
std::size_t WasmJitCPU::processor_id() const { return impl->core; }
void WasmJitCPU::set_instruction_budget(uint64_t v) { impl->budget = v; }
const std::string &WasmJitCPU::get_last_error() const { return impl->error; }
uint64_t WasmJitCPU::instructions_executed() const { return impl->executed; }
uint64_t WasmJitCPU::compiled_blocks() const { return impl->compiled; }
uint64_t WasmJitCPU::cache_hits() const { return impl->hits; }
uint64_t WasmJitCPU::invalidated_blocks() const { return impl->invalidated; }
