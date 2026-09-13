// Opt-in M14 proof: Dynarmic frontend/IR -> generated Wasm -> call_indirect.
// No interpreter fallback. The display application's default backend is unchanged.
#include <cpu/impl/wasm_jit_cpu.h>

#include <mem/functions.h>
#include <cpu/state.h>
#include "wasmjit/frontend.h"
#include "wasmjit/emit_wasm.h"
#include <dynarmic/frontend/A32/a32_location_descriptor.h>
#include <dynarmic/ir/basic_block.h>
#include <emscripten.h>
#include <emscripten/emscripten.h>
#include <emscripten/heap.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
using JitState = vita3k::wasmjit::JitState;
using MemoryFunction = uint32_t (*)(JitState *, uint32_t, uint32_t) noexcept;

uint32_t memory_fault(JitState &state, uint32_t address, bool write) noexcept {
    state.fault_address = address;
    state.fault_write = write;
    return static_cast<uint32_t>(vita3k::wasmjit::ExitReason::Fault);
}

bool valid_memory_size(uint32_t bytes) noexcept {
    return bytes == 1 || bytes == 2 || bytes == 4 || bytes == 8 || bytes == 16;
}

EMSCRIPTEN_KEEPALIVE uint32_t checked_memory_read(JitState *state, uint32_t address, uint32_t bytes) noexcept {
    if (!valid_memory_size(bytes) || !state->memory_cookie)
        return memory_fault(*state, address, false);
    const auto *mem = reinterpret_cast<const MemState *>(static_cast<uintptr_t>(state->memory_cookie));
    std::array<uint8_t, 16> value{};
    if (!mem_read(*mem, address, value.data(), bytes))
        return memory_fault(*state, address, false);
    // Explicit little-endian lanes, zero-extending narrow reads. Publish only
    // after the entire checked access succeeds, including cross-page accesses.
    std::fill_n(state->memory_value, 4, 0);
    for (uint32_t i = 0; i < bytes; ++i)
        state->memory_value[i / 4] |= uint32_t(value[i]) << ((i % 4) * 8);
    return 0;
}

EMSCRIPTEN_KEEPALIVE uint32_t checked_memory_write(JitState *state, uint32_t address, uint32_t bytes) noexcept {
    if (!valid_memory_size(bytes) || !state->memory_cookie)
        return memory_fault(*state, address, true);
    auto *mem = reinterpret_cast<MemState *>(static_cast<uintptr_t>(state->memory_cookie));
    std::array<uint8_t, 16> value{};
    for (uint32_t i = 0; i < bytes; ++i)
        value[i] = static_cast<uint8_t>(state->memory_value[i / 4] >> ((i % 4) * 8));
    if (!mem_write(*mem, address, value.data(), bytes))
        return memory_fault(*state, address, true);
    return 0;
}
} // namespace

// Compilation alone crosses JS. Cache-hit execution and checked memory helpers
// are native Wasm calls, not JS wrappers. Function pointers arrive as table
// indices; wasmTable.get supplies the actual Wasm function references as imports.
EM_JS(int, vita3k_jit_install, (const uint8_t *bytes, unsigned length,
    MemoryFunction read_memory, MemoryFunction write_memory), {
    let slot = -1;
    const freeSlots = Module['vita3kJitFreeSlots'] || (Module['vita3kJitFreeSlots'] = []);
    try {
        const raw = HEAPU8.slice(bytes, bytes + length);
        if (typeof process !== 'undefined' && process.env?.VITA3K_DUMP_JIT) require('fs').writeFileSync('/tmp/jit-module.wasm', raw);
        const module = new WebAssembly.Module(raw);
        const instance = new WebAssembly.Instance(module, {env: {
            memory: wasmMemory,
            mem_read: wasmTable.get(read_memory),
            mem_write: wasmTable.get(write_memory)
        }});
        slot = freeSlots.length ? freeSlots.pop() : wasmTable.grow(1);
        setWasmTableEntry(slot, instance.exports.block);
        return slot;
    } catch (error) {
        if (slot >= 0) { setWasmTableEntry(slot, null); freeSlots.push(slot); }
        console.error('Vita3K JIT compilation failed:', error);
        return -1;
    }
});
EM_JS(uint32_t, vita3k_jit_call, (int slot, uint32_t state), {
    const fn = wasmTable.get(slot);
    return fn(state);
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
        uint32_t instruction_limit;
    };
    CPUState *parent;
    std::size_t core;
    State state{};
    std::atomic<bool> stopped{false};
    bool breakpoint = false, log_code = false, log_mem = false;
    uint64_t budget = 1'000'000'000'000, executed = 0, compiled = 0, hits = 0, invalidated = 0;
    std::string error;
    std::map<Key, Block> cache;

    Impl(CPUState *parent, std::size_t core) : parent(parent), core(core) {
        static_assert(sizeof(uintptr_t) == sizeof(uint32_t), "JIT memory cookie requires wasm32");
        state.memory_cookie = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(parent->mem));
    }
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
    int reject(const Dynarmic::IR::Block &ir) {
        std::array<uint8_t, 4> opcode{};
        const bool fetched = mem_fetch(*parent->mem, state.regs[15], opcode.data(), opcode.size());
        std::fprintf(stderr, "WasmJitCPU rejected %s PC=%08x opcode(bytes LE)=%02x %02x %02x %02x%s\n%s\n",
            (state.cpsr & 0x20) ? "Thumb" : "ARM", state.regs[15],
            opcode[0], opcode[1], opcode[2], opcode[3], fetched ? "" : " (unavailable)",
            Dynarmic::IR::DumpBlock(ir).c_str());
        return fail("unsupported Dynarmic IR or terminal (no fallback)");
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
                uint32_t translation_limit = limit;
                auto ir = vita3k::wasmjit::translate_block(*parent->mem,
                    state.regs[15], state.cpsr, translation_limit, state.fpscr);
                auto bytes = vita3k::wasmjit::emit_block(ir);
                if (bytes.empty() && translation_limit > 1) {
                    // Memory IR is initially safe only in a single guest
                    // instruction: precise CPU rollback and SMC revalidation
                    // must happen before entering the next guest instruction.
                    translation_limit = 1;
                    ir = vita3k::wasmjit::translate_block(*parent->mem,
                        state.regs[15], state.cpsr, translation_limit, state.fpscr);
                    bytes = vita3k::wasmjit::emit_block(ir);
                }
                if (bytes.empty()) return reject(ir);
                // Cache under the REQUESTED limit, even for a one-instruction
                // retry, so subsequent run() entries reuse the smaller block.
                // EndLocation is the sequential end of decoded bytes, not a
                // branch target. Reject wraparound/empty or unexpectedly large blocks.
                const uint64_t end = LocationDescriptor(ir.EndLocation()).PC();
                const uint64_t start = state.regs[15];
                if (end <= start || end - start > 128)
                    return fail("unsupported translated code span");
                Block block{state.regs[15], std::vector<uint8_t>(end - start), -1, translation_limit};
                if (!mem_fetch(*parent->mem, block.pc, block.original.data(), block.original.size()))
                    return fail("guest code unavailable at compilation");
                // Allocate cache entry before installing so allocation failure
                // cannot leak a function-table slot.
                found = cache.emplace(key, std::move(block)).first;
                found->second.table_index = vita3k_jit_install(bytes.data(), bytes.size(),
                    checked_memory_read, checked_memory_write);
                if (found->second.table_index < 0) {
                    cache.erase(found);
                    return fail("browser rejected generated Wasm");
                }
                ++compiled;
            } else ++hits;
            if (log_code)
                std::fprintf(stderr, "JIT block PC=%08x table=%d executed=%llu\n", state.regs[15], found->second.table_index, (unsigned long long)executed);
            state.executed = 0;
            state.exit_reason = 0;
            state.svc = 0;
            state.fault_address = 0;
            state.fault_write = 0;
            state.memory_cookie = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(parent->mem));
            const State before = state;
            // Generated modules do not throw C++ exceptions. noexcept is vital:
            // otherwise Emscripten lowers this to invoke_ii (a JS trampoline),
            // not a direct Wasm call_indirect. Guest faults use return reasons.
            const uint32_t state_offset = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&state));
            const uint32_t reason = vita3k_jit_call(found->second.table_index, state_offset);
            if (reason == static_cast<uint32_t>(vita3k::wasmjit::ExitReason::Fault)) {
                const uint32_t address = state.fault_address, write = state.fault_write;
                state = before;
                state.fault_address = address;
                state.fault_write = write;
                state.exit_reason = reason;
                // Memory blocks contain one guest instruction. Roll back all
                // CPU state, including FPU/TLS and writeback registers. Earlier
                // stores WITHIN this instruction may already have committed;
                // memory is deliberately not transactional (e.g. faulting STM).
                char message[96];
                std::snprintf(message, sizeof(message), "guest memory %s fault at %08x",
                    write ? "write" : "read", address);
                return fail(message);
            }
            if (!state.executed || state.executed > found->second.instruction_limit)
                return fail("invalid generated instruction count");
            executed += state.executed;
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
uint32_t WasmJitCPU::get_tpidruro() { return impl->state.tpidruro; }
void WasmJitCPU::set_tpidruro(uint32_t v) { impl->state.tpidruro = v; }
float WasmJitCPU::get_float_reg(uint8_t i) { return std::bit_cast<float>(impl->state.fpu[i & 63]); }
void WasmJitCPU::set_float_reg(uint8_t i, float v) { impl->state.fpu[i & 63] = std::bit_cast<uint32_t>(v); }
CPUContext WasmJitCPU::save_context() {
    CPUContext context{};
    std::copy_n(impl->state.regs, 16, context.cpu_registers.begin());
    std::memcpy(context.fpu_registers.data(), impl->state.fpu, sizeof(impl->state.fpu));
    context.cpsr = impl->state.cpsr;
    context.fpscr = impl->state.fpscr;
    return context;
}
void WasmJitCPU::load_context(const CPUContext &c) {
    std::copy(c.cpu_registers.begin(), c.cpu_registers.end(), impl->state.regs);
    std::memcpy(impl->state.fpu, c.fpu_registers.data(), sizeof(impl->state.fpu));
    // CPUContext has no TPIDRURO slot. Like the existing CPUInterface contract,
    // load_context leaves that per-thread register to set_tpidruro().
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
uint32_t WasmJitCPU::get_fault_address() const { return impl->state.fault_address; }
bool WasmJitCPU::get_fault_write() const { return impl->state.fault_write != 0; }
uint64_t WasmJitCPU::instructions_executed() const { return impl->executed; }
uint64_t WasmJitCPU::compiled_blocks() const { return impl->compiled; }
uint64_t WasmJitCPU::cache_hits() const { return impl->hits; }
uint64_t WasmJitCPU::invalidated_blocks() const { return impl->invalidated; }
