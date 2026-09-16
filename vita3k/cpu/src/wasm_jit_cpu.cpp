// Opt-in M14 proof: Dynarmic frontend/IR -> generated Wasm regions.
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
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace {
using JitState = vita3k::wasmjit::JitState;
using MemoryFunction = uint32_t (*)(JitState *, uint32_t, uint32_t) noexcept;

// --- M14c region formation -----------------------------------------------
// A region batches many guest blocks into ONE WebAssembly module with an
// in-module dispatch loop (REGION_ABI.md). Formation is a DFS over
// translate_block following terminal LinkBlock/LinkBlockFast targets; each
// successor is translated with the descriptor carried by the terminal edge,
// so chained in-region blocks assume the architecturally correct PSR/FPSCR
// by construction. The host cache is keyed by full location descriptor.
//
// Formation guarantees (relied on by the emitted dispatch loop):
//  - at most ONE block per guest PC per region (same PC with different PSR is
//    not a member; dispatch Misses and the host forms another region);
//  - blocks sorted by entry PC in the Region;
//  - per-block tick cost = CycleCount + ConditionFailedCycleCount (one tick
//    per guest instruction including condition-failed slots; conservative for
//    the budget check since conditions may pass).
constexpr uint32_t PSR_DISPATCH_MASK = Dynarmic::A32::LocationDescriptor::CPSR_MODE_MASK;
constexpr size_t REGION_MAX_BLOCKS = 512;
constexpr uint64_t REGION_MAX_TICKS = 32768;
constexpr uint32_t REGION_BLOCK_INSTR_LIMIT = 64;
constexpr size_t REGION_MAX_CODE_BYTES = REGION_BLOCK_INSTR_LIMIT * 4;
constexpr size_t REGION_CACHE_LIMIT = 128;
constexpr uint32_t REGION_CALL_TICKS = 131072; // Bound latency of host stop checks.

uint32_t counter_delta(uint32_t before, uint32_t after) noexcept {
    return after - before; // A call cannot execute a full 2^32 ticks.
}

struct RegionBlock {
    Address pc = 0;
    uint32_t psr_mask = 0, psr_value = 0; // dispatch validation bits
    uint32_t ticks = 0;                    // conservative tick cost
    std::vector<uint8_t> original;         // guest bytes [pc, EndLocation.PC)
    std::vector<vita3k::wasmjit::StoreContinuation> store_continuations;
};
struct RegionPage {
    struct Span {
        size_t block_index, block_offset;
        uint32_t page_offset, size;
    };
    uint32_t page = 0;
    uint32_t begin = 4096, end = 0;
    std::vector<Span> spans;
};
struct Region {
    std::vector<RegionBlock> blocks; // sorted by pc
    uint64_t total_ticks = 0;
    Address page_begin = 0, page_end = 0; // Half-open page-index bounds.
    std::vector<uint32_t> code_pages;     // Only pages actually containing code.
    std::vector<RegionPage> validation_pages; // Built once, reused on every entry.
};

void collect_code_pages(Region &region) {
    region.code_pages.clear();
    region.validation_pages.clear();
    // Overlapping blocks can cross a page and then start on the preceding
    // page again. Group by page explicitly; sorted block PCs alone do not
    // guarantee that their page fragments arrive in sorted order.
    std::map<uint32_t, RegionPage> pages;
    for (size_t i = 0; i < region.blocks.size(); ++i) {
        const auto &block = region.blocks[i];
        size_t offset = 0;
        while (offset < block.original.size()) {
            const uint64_t address = uint64_t(block.pc) + offset;
            const uint32_t page_number = static_cast<uint32_t>(address >> 12);
            const uint32_t page_offset = static_cast<uint32_t>(address & 0xfff);
            const uint32_t count = static_cast<uint32_t>(std::min(
                block.original.size() - offset, size_t(4096 - page_offset)));
            auto &page = pages[page_number];
            page.page = page_number;
            page.begin = std::min(page.begin, page_offset);
            page.end = std::max(page.end, page_offset + count);
            page.spans.push_back({i, offset, page_offset, count});
            offset += count;
        }
    }
    region.code_pages.reserve(pages.size());
    region.validation_pages.reserve(pages.size());
    for (auto &[number, page] : pages) {
        region.code_pages.push_back(number);
        region.validation_pages.push_back(std::move(page));
    }
    region.page_begin = region.code_pages.empty() ? 0 : region.code_pages.front();
    region.page_end = region.code_pages.empty() ? 0 : region.code_pages.back() + 1;
}

// Static successor targets of a terminal tree. ReturnToDispatch, PopRSBHint
// and FastDispatchHint have no static target and are reached via dispatch at
// runtime; Interpret/Invalid mean the emitter rejects the block entirely.
void collect_targets(const Dynarmic::IR::Term::Terminal &terminal,
    std::vector<Dynarmic::A32::LocationDescriptor> &out) {
    using namespace Dynarmic::IR::Term;
    struct Visitor {
        std::vector<Dynarmic::A32::LocationDescriptor> *out;
        void operator()(const Invalid &) const {}
        void operator()(const Interpret &) const {}
        void operator()(const ReturnToDispatch &) const {}
        void operator()(const PopRSBHint &) const {}
        void operator()(const FastDispatchHint &) const {}
        void operator()(const LinkBlock &t) const { out->push_back(Dynarmic::A32::LocationDescriptor{t.next}); }
        void operator()(const LinkBlockFast &t) const { out->push_back(Dynarmic::A32::LocationDescriptor{t.next}); }
        void operator()(const boost::recursive_wrapper<If> &t) const {
            collect_targets(t.get().then_, *out);
            collect_targets(t.get().else_, *out);
        }
        void operator()(const boost::recursive_wrapper<CheckBit> &t) const {
            collect_targets(t.get().then_, *out);
            collect_targets(t.get().else_, *out);
        }
        void operator()(const boost::recursive_wrapper<CheckHalt> &t) const {
            collect_targets(t.get().else_, *out);
        }
    } visitor{&out};
    boost::apply_visitor(visitor, terminal);
}

// DFS region formation from an entry location. Returns false when the ENTRY
// block cannot be translated/fetched (caller rejects); interior successors
// that fail fetch are skipped and handled by a runtime Miss, so a single
// unmapped branch target cannot poison the region. `ir_out` receives the
// translated blocks PERMUTED INTO THE SAME SORTED ORDER as region.blocks
// (emit_region validates meta against each block's own location).
bool form_region(MemState &mem, uint32_t entry_pc, uint32_t entry_cpsr,
    uint32_t entry_fpscr, Region &region, std::vector<Dynarmic::IR::Block> &ir_out,
    size_t max_store_continuations = vita3k::wasmjit::kDefaultMaxStoreContinuations) {
    using Dynarmic::A32::LocationDescriptor;
    region = Region{};
    ir_out.clear();
    std::vector<LocationDescriptor> stack;
    std::vector<Dynarmic::IR::Block> ir_blocks;
    std::unordered_set<uint32_t> member_pcs;
    member_pcs.reserve(REGION_MAX_BLOCKS * 2);
    stack.emplace_back(entry_pc, Dynarmic::A32::PSR{entry_cpsr},
        Dynarmic::A32::FPSCR{entry_fpscr});
    while (!stack.empty() && region.blocks.size() < REGION_MAX_BLOCKS
        && region.total_ticks < REGION_MAX_TICKS) {
        const auto location = stack.back();
        stack.pop_back();
        const uint32_t pc = location.PC();
        if (!member_pcs.insert(pc).second)
            continue;
        std::optional<Dynarmic::IR::Block> ir;
        RegionBlock block;
        const auto candidate = [&](uint32_t limit, bool continue_stores) {
            block.store_continuations.clear();
            try {
                auto translated = vita3k::wasmjit::translate_block(mem, pc,
                    location.CPSR().Value(), limit, location.FPSCR().Value(),
                    continue_stores ? &block.store_continuations : nullptr,
                    max_store_continuations);
                const uint64_t end = LocationDescriptor(translated.EndLocation()).PC();
                if (end <= pc || end - pc > REGION_MAX_CODE_BYTES)
                    return false;
                const uint64_t ticks = translated.CycleCount()
                    + translated.ConditionFailedCycleCount();
                if (!ticks || ticks > REGION_MAX_TICKS)
                    return false;
                block.pc = pc;
                block.psr_mask = PSR_DISPATCH_MASK;
                block.psr_value = location.CPSR().Value() & PSR_DISPATCH_MASK;
                block.ticks = static_cast<uint32_t>(ticks);
                // Validate only the member body here. Building a complete
                // one-block module for every candidate duplicates the most
                // expensive part of region formation and is discarded as
                // soon as the next candidate is examined.
                if (!vita3k::wasmjit::validate_region_block(translated, block.store_continuations))
                    return false;
                block.original.resize(static_cast<size_t>(end - pc));
                if (!mem_fetch(mem, pc, block.original.data(), block.original.size()))
                    return false;
                ir.emplace(std::move(translated));
                return true;
            } catch (const std::exception &) {
                return false;
            }
        };
        // A supported first instruction must not be poisoned by later
        // unsupported IR or speculative fetches across an unmapped boundary.
        bool accepted = candidate(REGION_BLOCK_INSTR_LIMIT, true);
        // If continuing past a store exposed an unsupported suffix or fetch
        // fault, recover the original supported store-ending block before
        // falling back to a single instruction.
        if (!accepted && !block.store_continuations.empty())
            accepted = candidate(REGION_BLOCK_INSTR_LIMIT, false);
        if (!accepted && !candidate(1, false)) {
            if (region.blocks.empty())
                return false;
            continue; // This edge becomes a runtime Miss.
        }
        if (region.total_ticks + block.ticks > REGION_MAX_TICKS)
            continue;
        region.total_ticks += block.ticks;
        collect_targets(ir->GetTerminal(), stack);
        if (ir->GetCondition() != Dynarmic::IR::Cond::AL
            && ir->HasConditionFailedLocation())
            stack.emplace_back(ir->ConditionFailedLocation());
        region.blocks.push_back(std::move(block));
        ir_blocks.push_back(std::move(*ir));
    }
    std::vector<size_t> order(region.blocks.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return region.blocks[a].pc < region.blocks[b].pc;
    });
    Region sorted;
    sorted.total_ticks = region.total_ticks;
    std::vector<Dynarmic::IR::Block> sorted_ir;
    sorted_ir.reserve(order.size());
    for (const auto i : order) {
        sorted.blocks.push_back(std::move(region.blocks[i]));
        sorted_ir.push_back(std::move(ir_blocks[i]));
    }
    collect_code_pages(sorted);
    region = std::move(sorted);
    ir_out = std::move(sorted_ir);
    return !region.blocks.empty();
}
// --- end region formation -------------------------------------------------

// Wide reference counts: multiple CPUs and cached entries can share pages.
std::array<uint32_t, 1 << 20> g_code_pages{};
void mark_code_pages(const Region &region, int delta) {
    for (const uint32_t page : region.code_pages) {
        if (delta > 0)
            ++g_code_pages[page];
        else
            --g_code_pages[page];
    }
}

// Region-entry validation (replaces per-block unchanged()): revalidate the
// guest bytes of EVERY member block once per region entry. Between entries,
// cached code pages are marked in g_code_pages, letting checked_memory_write flag
// smc_dirty for an immediate Smc exit instead of waiting for this check.
bool region_unchanged(const Region &region, MemState &mem) {
    // No per-entry allocations, page grouping or binary searches. Only the
    // fetched range is read, so the scratch page needs no zero-initialization.
    std::array<uint8_t, 4096> bytes;
    for (const auto &page : region.validation_pages) {
        if (!mem_fetch(mem, page.page * 4096 + page.begin,
                bytes.data(), page.end - page.begin))
            return false;
        for (const auto &span : page.spans) {
            const auto &original = region.blocks[span.block_index].original;
            if (!std::equal(original.begin() + span.block_offset,
                    original.begin() + span.block_offset + span.size,
                    bytes.begin() + (span.page_offset - page.begin)))
                return false;
        }
    }
    return true;
}

uint32_t memory_fault(JitState &state, uint32_t address, bool write) noexcept {
    state.fault_address = address;
    state.fault_write = write;
    return static_cast<uint32_t>(vita3k::wasmjit::ExitReason::Fault);
}

// Profiling counters (worker is single-threaded; no atomics needed).
// g_mem_* are process-lifetime totals; the fast-path per-call scratch lives
// in JitState (REGION_ABI.md) and is accumulated here between calls.
uint64_t g_mem_reads = 0, g_mem_writes = 0;
uint64_t g_mem_fast_reads = 0, g_mem_fast_writes = 0;
// Slow-fallback reasons (high byte of the helper `bytes` argument).
uint64_t g_mem_slow_unmapped = 0, g_mem_slow_perms = 0, g_mem_slow_cross_page = 0,
    g_mem_slow_code_page = 0, g_mem_slow_other = 0;

void account_slow_reason(uint32_t bytes_arg) noexcept {
    switch (bytes_arg >> 8) {
    case 1: ++g_mem_slow_unmapped; break;
    case 2: ++g_mem_slow_perms; break;
    case 3: ++g_mem_slow_cross_page; break;
    case 4: ++g_mem_slow_code_page; break;
    case 5: ++g_mem_slow_other; break;
    default: break;
    }
}

void account_fast_counters(JitState &state) noexcept {
    g_mem_fast_reads += state.mem_fast_reads;
    g_mem_fast_writes += state.mem_fast_writes;
    state.mem_fast_reads = 0;
    state.mem_fast_writes = 0;
}

// M16 Wasm-side dispatch map (see emit_wasm.h): open-addressed
// location-hash -> table-slot cache in linear memory, read by generated
// dispatcher code. Plain statics: on wasm32 their addresses ARE linear
// offsets, exactly like the page-table bases the JIT already exposes.
// Entry layout: {key_lo, key_hi, slot, epoch}; epoch 0 = never written.
static uint32_t dispatch_map[vita3k::wasmjit::kDispatchMapEntries * 4] = {};
static uint32_t dispatch_epoch = 1;
static uint64_t dispatch_global_version = 0;
// Process-global eviction generation. Every dispatch_bump_epoch() (all region-
// eviction paths funnel through it) advances this, so each CPU can tell whether
// any map invalidation happened since its last pump entry without paying a
// per-entry epoch bump (which would stale its own just-inserted entries).
static uintptr_t dispatch_map_base() {
    return reinterpret_cast<uintptr_t>(&dispatch_map[0]);
}
static uintptr_t dispatch_epoch_addr() {
    return reinterpret_cast<uintptr_t>(&dispatch_epoch);
}
// Every region-cache removal path must bump the epoch (stale entries can
// never match afterwards); table slots are nulled on release as backup.
static void dispatch_bump_epoch() noexcept {
    ++dispatch_epoch;
    ++dispatch_global_version;
    ++dispatch_epoch;
    if (dispatch_epoch == 0) { // Never use the never-written marker.
        std::memset(dispatch_map, 0, sizeof(dispatch_map));
        dispatch_epoch = 1;
    }
}
// Insert or refresh; bit-identical probing to the dispatcher emitter.
// Returns false only if no reusable slot exists within the probe limit
// (impossible at REGION_CACHE_LIMIT << map size; fails loudly if hit).
static bool dispatch_map_insert(uint64_t key, uint32_t slot) noexcept {
    using namespace vita3k::wasmjit;
    const uint32_t lo = static_cast<uint32_t>(key);
    const uint32_t hi = static_cast<uint32_t>(key >> 32);
    uint32_t idx = dispatch_map_index(lo, hi);
    for (uint32_t i = 0; i < kDispatchMaxProbe; ++i) {
        uint32_t *e = &dispatch_map[(idx & kDispatchMapMask) * 4];
        if (e[3] == dispatch_epoch && e[0] == lo && e[1] == hi) {
            e[2] = slot; // refresh existing mapping
            return true;
        }
        idx++;
    }
    idx = dispatch_map_index(lo, hi);
    for (uint32_t i = 0; i < kDispatchMaxProbe; ++i) {
        uint32_t *e = &dispatch_map[(idx & kDispatchMapMask) * 4];
        if (e[3] != dispatch_epoch) { // empty or stale: overwrite
            e[0] = lo;
            e[1] = hi;
            e[2] = slot;
            e[3] = dispatch_epoch;
            return true;
        }
        idx++;
    }
    return false;
}

bool valid_memory_size(uint32_t bytes) noexcept {
    return bytes == 1 || bytes == 2 || bytes == 4 || bytes == 8 || bytes == 16;
}

EMSCRIPTEN_KEEPALIVE uint32_t checked_memory_read(JitState *state, uint32_t address, uint32_t bytes) noexcept {
    // RegionState non-observer ABI: do not inspect/mutate CPSR, GPRs, PC or
    // accounting here, or call HLE/debug/context callbacks. Those fields may
    // be local until the region epilogue. Same rule applies to write below.
    ++g_mem_reads;
    account_slow_reason(bytes);
    bytes &= 0xffu; // Fast-path fallback reason rides in the high byte.
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
    ++g_mem_writes;
    account_slow_reason(bytes);
    bytes &= 0xffu; // Fast-path fallback reason rides in the high byte.
    if (!valid_memory_size(bytes) || !state->memory_cookie)
        return memory_fault(*state, address, true);
    auto *mem = reinterpret_cast<MemState *>(static_cast<uintptr_t>(state->memory_cookie));
    std::array<uint8_t, 16> value{};
    for (uint32_t i = 0; i < bytes; ++i)
        value[i] = static_cast<uint8_t>(state->memory_value[i / 4] >> ((i % 4) * 8));
    if (!mem_write(*mem, address, value.data(), bytes))
        return memory_fault(*state, address, true);
    // Only successful writes dirty code. Widen before computing the range.
    const uint64_t end = uint64_t(address) + bytes;
    for (uint64_t page = address >> 12; page < (end + 4095) / 4096; ++page) {
        if (g_code_pages[page]) {
            if (!state->smc_dirty)
                state->smc_page = static_cast<uint32_t>(page);
            else if (state->smc_page != page)
                state->smc_page = std::numeric_limits<uint32_t>::max();
            state->smc_dirty = 1;
        }
    }
    return 0;
}
} // namespace

// Compilation and host entry into generated functions cross JS. Inside a
// generated region, dispatch and checked memory helpers stay in Wasm.
// Imported helper pointers arrive as table indices.
EM_JS(int, vita3k_jit_install, (const uint8_t *bytes, unsigned length,
    MemoryFunction read_memory, MemoryFunction write_memory), {
    let slot = -1;
    const freeSlots = Module['vita3kJitFreeSlots'] || (Module['vita3kJitFreeSlots'] = []);
    try {
        const raw = Module['vita3kHostBytes'](bytes, length).slice();
        if (typeof process !== 'undefined' && process.env?.VITA3K_DUMP_JIT) require('fs').writeFileSync('/tmp/jit-module.wasm', raw);
        const module = new WebAssembly.Module(raw);
        const instance = new WebAssembly.Instance(module, {env: {
            memory: wasmMemory,
            mem_read: Module['vita3kNativeFunction'](read_memory),
            mem_write: Module['vita3kNativeFunction'](write_memory)
        }});
        // This is Emscripten's native function table, whose indexing ABI is
        // toolchain-owned. The independent M16 region table remains i32.
        slot = freeSlots.length ? freeSlots.pop()
            : Number(wasmTable.grow(Module['vita3kMemory64'] ? 1n : 1));
        setWasmTableEntry(slot, instance.exports.block);
        return slot;
    } catch (error) {
        if (slot >= 0) { setWasmTableEntry(slot, null); freeSlots.push(slot); }
        console.error('Vita3K JIT compilation failed:', error);
        return -1;
    }
});
EM_JS(uint32_t, vita3k_jit_call, (int slot, uintptr_t state), {
    const fn = Module['vita3kNativeFunction'](slot);
    return fn(Module['vita3kHostPointer'](state));
});
EM_JS(void, vita3k_jit_release, (int slot), {
    setWasmTableEntry(slot, null);
    (Module['vita3kJitFreeSlots'] || (Module['vita3kJitFreeSlots'] = [])).push(slot);
});
// Region modules export run(state, budget) -> reason instead of block(state).
// The HOST calls run via EM_JS (never call_indirect from Wasm), so region
// functions live in a JS map keyed by slot — no shared-table slot aliasing.
// M16: region run functions are ALSO published into a host-shared funcref
// table so the Wasm-side dispatcher can call_indirect them without host
// round-trips. Table slots reuse the JS-map slot namespace (bounded by
// REGION_CACHE_LIMIT, far below the initial table size).
EM_JS(void, vita3k_jit_table_ensure, (), {
    if (!Module['vita3kJitTable'])
        Module['vita3kJitTable'] = new WebAssembly.Table({initial: 512, element: 'anyfunc'});
});
EM_JS(int, vita3k_jit_install_region, (const uint8_t *bytes, unsigned length,
    MemoryFunction read_memory, MemoryFunction write_memory), {
    const regions = Module['vita3kJitRegions'] || (Module['vita3kJitRegions'] = new Map());
    let slot = -1;
    const freeSlots = Module['vita3kJitFreeRegionSlots'] || (Module['vita3kJitFreeRegionSlots'] = []);
    try {
        const raw = Module['vita3kHostBytes'](bytes, length).slice();
        if (typeof process !== 'undefined' && process.env?.VITA3K_DUMP_JIT) require('fs').writeFileSync('/tmp/jit-region-' + arguments[2] + '-' + Date.now() + '.wasm', raw);
        const module = new WebAssembly.Module(raw);
        const instance = new WebAssembly.Instance(module, {env: {
            memory: wasmMemory,
            mem_read: Module['vita3kNativeFunction'](read_memory),
            mem_write: Module['vita3kNativeFunction'](write_memory)
        }});
        const run = instance.exports.run;
        if (typeof run !== 'function') throw new Error('region module does not export run');
        slot = freeSlots.length ? freeSlots.pop() : regions.size;
        regions.set(slot, run);
        const table = Module['vita3kJitTable'];
        if (table) {
            while (slot >= table.length) table.grow(256);
            table.set(slot, run);
        }
        return slot;
    } catch (error) {
        if (slot >= 0) {
            regions.delete(slot);
            freeSlots.push(slot);
        }
        console.error('Vita3K JIT region compilation failed:', error);
        return -1;
    }
});
// The multi-region dispatcher module is built once per process; it imports
// the shared region table and the same linear memory as the regions.
EM_JS(int, vita3k_jit_install_dispatch, (const uint8_t *bytes, unsigned length), {
    try {
        const raw = Module['vita3kHostBytes'](bytes, length).slice();
        if (typeof process !== 'undefined' && process.env?.VITA3K_DUMP_JIT) require('fs').writeFileSync('/tmp/jit-dispatch.wasm', raw);
        if (!Module['vita3kJitTable'])
            Module['vita3kJitTable'] = new WebAssembly.Table({initial: 512, element: 'anyfunc'});
        const module = new WebAssembly.Module(raw);
        const instance = new WebAssembly.Instance(module, {env: {
            memory: wasmMemory,
            region_table: Module['vita3kJitTable']
        }});
        const dispatch = instance.exports.dispatch;
        if (typeof dispatch !== 'function') throw new Error('dispatch module does not export dispatch');
        Module['vita3kJitDispatch'] = dispatch;
        return 0;
    } catch (error) {
        console.error('Vita3K JIT dispatch compilation failed:', error);
        return -1;
    }
});
EM_JS(uint32_t, vita3k_jit_run_dispatch, (uintptr_t state, uint32_t remaining, uintptr_t map_base, uintptr_t epoch_addr), {
    const fn = Module['vita3kJitDispatch'];
    return fn(Module['vita3kHostPointer'](state), remaining,
        Module['vita3kHostPointer'](map_base), Module['vita3kHostPointer'](epoch_addr));
});
EM_JS(uint32_t, vita3k_jit_run, (int slot, uintptr_t state, uint32_t budget), {
    const fn = Module['vita3kJitRegions'].get(slot);
    return fn(Module['vita3kHostPointer'](state), budget);
});
EM_JS(void, vita3k_jit_release_region, (int slot), {
    Module['vita3kJitRegions'].delete(slot);
    (Module['vita3kJitFreeRegionSlots'] || (Module['vita3kJitFreeRegionSlots'] = [])).push(slot);
    // Null the shared-table entry too; the epoch guard makes it unreachable,
    // this only converts a hypothetical bug into a loud trap instead of
    // silent wrong-region execution.
    const table = Module['vita3kJitTable'];
    if (table && slot < table.length) table.set(slot, null);
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
    // M14c region: one WebAssembly.Module per REGION with an in-Wasm
    // dispatch loop. Keyed by entry location hash (the dispatch loop
    // validates PSR per block, so one region serves many locations).
    struct RegionEntry {
        std::shared_ptr<Region> region;
        int table_index = -1;
        uint64_t last_used = 0;
    };
    uint64_t region_clock = 0;
    uint64_t last_dispatch_version = 0;
    CPUState *parent;
    std::size_t core;
    State state{};
    std::atomic<bool> stopped{false};
    bool breakpoint = false, log_code = false, log_mem = false;
    uint64_t budget = 1'000'000'000'000, executed = 0, compiled = 0, hits = 0, invalidated = 0;
    // Phase profiling: milliseconds and counts for the JIT cost centers.
    double emit_ms = 0, install_ms = 0, run_js_ms = 0;
    uint64_t js_calls = 0, misses = 0, svc_exits = 0, budget_exits = 0;
    // Region-mode profiling.
    uint64_t regions = 0, region_misses = 0, smc_exits = 0, dispatches = 0;
    // M16 pump counters: in-Wasm chained transfers (tx_wasm) vs dispatcher
    // Miss returns to the host (host_miss; ~all resolve to compiled regions
    // at the loop top, true compiles are region_misses).
    uint64_t tx_wasm = 0, host_miss = 0;
    bool dispatch_installed = false;
    // Region mode is the production path (M14c); single-block execution
    // remains for step() and the single-block module suite.
    bool region_mode = true;
    vita3k::wasmjit::RegionStateOptions region_options = vita3k::wasmjit::region_state_options();
    std::string error;
    std::map<Key, Block> cache;
    std::map<uint64_t, RegionEntry> region_cache;

    Impl(CPUState *parent, std::size_t core) : parent(parent), core(core) {
        static_assert(sizeof(vita3k::wasmjit::HostAddress) == sizeof(uintptr_t), "JIT host ABI must match the runtime");
        state.memory_cookie = reinterpret_cast<uintptr_t>(parent->mem);
        // The fast-path base arrays are allocated once in MemState init and
        // freed only at deinit; region modules compile and run strictly
        // inside that window, so nonzero bases observed here stay nonzero
        // for every call into those modules. g_code_pages is process-static
        // (std::array::data() never null). The per-call publish sites below
        // refresh the same bases, so this predicate stays exact.
        region_options.assume_fast_bases = parent->mem->page_permissions != nullptr
            && (parent->mem->direct_host_memory
                || (parent->mem->sparse_host_memory && parent->mem->page_table != nullptr));
    }
    ~Impl() { clear(); }
    void clear_regions() {
        for (auto &[key, entry] : region_cache) {
            if (entry.table_index >= 0) {
                vita3k_jit_release_region(entry.table_index);
                mark_code_pages(*entry.region, -1);
            }
        }
        invalidated += region_cache.size();
        region_cache.clear();
        dispatch_bump_epoch();
    }
    void clear_regions_for_page(uint32_t page) {
        bool erased = false;
        for (auto it = region_cache.begin(); it != region_cache.end();) {
            const Region &region = *it->second.region;
            if (!std::binary_search(region.code_pages.begin(), region.code_pages.end(), page)) {
                ++it;
                continue;
            }
            if (it->second.table_index >= 0)
                vita3k_jit_release_region(it->second.table_index);
            mark_code_pages(region, -1);
            it = region_cache.erase(it);
            ++invalidated;
            erased = true;
        }
        if (erased)
            dispatch_bump_epoch();
    }
    void clear() {
        for (const auto &[key, block] : cache) vita3k_jit_release(block.table_index);
        invalidated += cache.size();
        cache.clear();
        clear_regions();
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
    // Region-mode execution: form/compile a region on miss, then let the
    // generated module's in-Wasm dispatch loop run many guest blocks per
    // host entry. Handles all ExitReason values from REGION_ABI.md.
    int execute_regions(uint64_t remaining_budget) {
        if (parent->mem->direct_host_memory) {
            // The hint map/table are process-global. Discard hints from any
            // other cooperatively scheduled CPU before publishing this CPU's
            // revalidated region set; table slots themselves remain reusable.
            // Bump ONLY when an eviction happened since this CPU last synced:
            // an unconditional per-entry bump would stale this CPU's own live
            // entries, forcing one host miss per pump re-entry (measured 717
            // vs 71 on the display fixture). All eviction paths funnel through
            // dispatch_bump_epoch(), which advances the global version.
            if (dispatch_global_version != last_dispatch_version) {
                dispatch_bump_epoch();
                last_dispatch_version = dispatch_global_version;
            }
            // Host/HLE/loader Ptr writes are intentionally unchecked and can
            // change any cached region, not only the first region entered.
            // Revalidate ALL potential dispatch targets after each host entry
            // (including a suspended HLE return). No host mutator runs inside
            // this cooperative pump; generated code-page writes use smc_dirty.
            // Thus M16 cannot chain to stale bytes or a freed/non-executable
            // region after a direct host write. No per-access host logging/hook.
            for (auto it = region_cache.begin(); it != region_cache.end();) {
                if (region_unchanged(*it->second.region, *parent->mem)) {
                    ++it;
                    continue;
                }
                vita3k_jit_release_region(it->second.table_index);
                mark_code_pages(*it->second.region, -1);
                it = region_cache.erase(it);
                ++invalidated;
                dispatch_bump_epoch();
            }
        }
        uint64_t budget_progress_mark = state.executed;
        using vita3k::wasmjit::ExitReason;
        while (true) {
            if (stopped || breakpoint)
                return 1;
            // ThreadState uses a per-kernel NOP+WFI return sentinel as LR.
            // Dynarmic translates WFI as a hint only when hooked; the JIT
            // frontend deliberately leaves hints unhooked, so recognize this
            // sentinel before region formation and report a clean guest return.
            if ((state.cpsr & 0x20) != 0) {
                std::array<uint8_t, 4> halt{};
                if (mem_fetch(*parent->mem, state.regs[15], halt.data(), halt.size())
                    && halt[0] == 0x00 && halt[1] == 0xBF
                    && halt[2] == 0x30 && halt[3] == 0xBF)
                    return 1;
            }
            if (remaining_budget == 0)
                return fail("instruction budget exhausted");
            // M16: the shared region table must exist before any region
            // install in this iteration publishes into it (installs that
            // run before the first dispatcher setup would otherwise leave
            // null slots behind and trap the pump's call_indirect). The
            // dispatcher module itself is installed once, lazily, here.
            if (!dispatch_installed) {
                vita3k_jit_table_ensure();
                const auto dbytes = vita3k::wasmjit::emit_dispatch();
                if (dbytes.empty())
                    return fail("dispatch emission rejected");
                if (vita3k_jit_install_dispatch(dbytes.data(), dbytes.size()) < 0)
                    return fail("browser rejected dispatch Wasm");
                dispatch_installed = true;
            }
            const uint32_t pc = state.regs[15];
            const auto loc = Dynarmic::A32::LocationDescriptor{pc,
                Dynarmic::A32::PSR{state.cpsr}, Dynarmic::A32::FPSCR{state.fpscr}};
            const uint64_t key = loc.UniqueHash();
            auto found = region_cache.find(key);
            if (found != region_cache.end() && !region_unchanged(*found->second.region, *parent->mem)) {
                // Guest code changed under us (Ptr/HLE write without tracking,
                // or a store the smc bitmap missed). Drop and recompile.
                vita3k_jit_release_region(found->second.table_index);
                mark_code_pages(*found->second.region, -1);
                region_cache.erase(found);
                ++invalidated;
                dispatch_bump_epoch();
                found = region_cache.end();
            }
            if (found == region_cache.end()) {
                if (region_cache.size() >= REGION_CACHE_LIMIT) {
                    const auto victim = std::min_element(region_cache.begin(), region_cache.end(),
                        [](const auto &a, const auto &b) {
                            return a.second.last_used < b.second.last_used;
                        });
                    vita3k_jit_release_region(victim->second.table_index);
                    mark_code_pages(*victim->second.region, -1);
                    region_cache.erase(victim);
                    ++invalidated;
                    dispatch_bump_epoch();
                }
                const double t0 = emscripten_get_now();
                ++region_misses;
                auto region = std::make_shared<Region>();
                std::vector<Dynarmic::IR::Block> ir_blocks;
                if (!form_region(*parent->mem, pc, state.cpsr, state.fpscr, *region, ir_blocks))
                    return fail("region formation failed at entry");
                std::vector<const Dynarmic::IR::Block *> block_ptrs;
                std::vector<vita3k::wasmjit::RegionBlockMeta> meta;
                block_ptrs.reserve(region->blocks.size());
                meta.reserve(region->blocks.size());
                for (size_t i = 0; i < region->blocks.size(); ++i) {
                    block_ptrs.push_back(&ir_blocks[i]);
                    meta.push_back({region->blocks[i].pc, region->blocks[i].psr_mask,
                        region->blocks[i].psr_value, region->blocks[i].ticks,
                        region->blocks[i].store_continuations});
                }
                const auto bytes = vita3k::wasmjit::emit_region(block_ptrs, meta, region_options);
                emit_ms += emscripten_get_now() - t0;
                if (bytes.empty())
                    return fail("region emission rejected (no fallback)");
                // Allocate the map node before installing the JS function.
                found = region_cache.emplace(key,
                    RegionEntry{std::move(region), -1}).first;
                const double t1 = emscripten_get_now();
                const int slot = vita3k_jit_install_region(bytes.data(), bytes.size(),
                    checked_memory_read, checked_memory_write);
                install_ms += emscripten_get_now() - t1;
                if (slot < 0) {
                    region_cache.erase(found); // No pages were marked yet.
                    return fail("browser rejected generated region Wasm");
                }
                found->second.table_index = slot;
                mark_code_pages(*found->second.region, +1);
                ++regions;
            }
            ++hits;
            found->second.last_used = ++region_clock;
            if (log_code)
                std::fprintf(stderr, "JIT region PC=%08x blocks=%zu\n",
                    pc, found->second.region->blocks.size());
            // Refresh per-run state fields (bases are stable but cheap).
            state.memory_cookie = reinterpret_cast<uintptr_t>(parent->mem);
            // M15 inline memory fast path (REGION_ABI.md): expose MemState's
            // page table, permission bytes and the code-page refcounts as
            // linear-memory offsets. The two arrays never reallocate after
            // init and g_code_pages is process-static, so the bases outlive
            // every compiled region. For wasm32 a zero page_table_base disables
            // the fast path. Memory64 ignores that field entirely and uses
            // permission/code metadata plus the fixed guest window. Sparse entries
            // must be SPARSE page pointers (host offset of the page start);
            // native non-sparse tables hold address biases instead, so they
            // must not enable the fast path.
            const auto *mem_state = parent->mem;
            state.page_table_base = mem_state->sparse_host_memory
                ? reinterpret_cast<uintptr_t>(mem_state->page_table.get()) : 0;
            state.page_perms_base = reinterpret_cast<uintptr_t>(mem_state->page_permissions.get());
            state.code_pages_base = reinterpret_cast<uintptr_t>(g_code_pages.data());
            state.smc_dirty = 0;
            state.smc_page = 0;
            // stop() owns an atomic request. Mirror it at entry and return
            // to the atomic host check at least every REGION_CALL_TICKS.
            // Event-loop yielding remains the worker scheduler's job.
            state.stop_flag = stopped.load() ? 1u : 0u;
            state.exit_reason = 0;
            state.svc = 0;
            // M16 pump: publish the entry mapping, then let the Wasm-side
            // dispatcher chain compiled-region transfers without host exits.
            // regs[15] is the transfer target after every Miss/Budget/Smc
            // return (all exits publish it), so the loop-top resolve below
            // serves both fresh entries and post-dispatcher Miss targets.
            if (found->second.table_index < 0
                || found->second.table_index >= static_cast<int>(vita3k::wasmjit::kDispatchTableLimit))
                return fail("region slot outside dispatch table");
            if (!dispatch_map_insert(key, static_cast<uint32_t>(found->second.table_index)))
                return fail("region map full");
            const uintptr_t state_offset = reinterpret_cast<uintptr_t>(&state);
            const uint32_t granted = static_cast<uint32_t>(std::min<uint64_t>(remaining_budget, UINT32_MAX));
            const uint32_t executed_before = state.executed;
            const uint32_t dispatches_before = state.dispatches;
            const uint32_t tx_before = state.tx_wasm;
            const double t2 = emscripten_get_now();
            const uint32_t reason = vita3k_jit_run_dispatch(state_offset, granted,
                dispatch_map_base(), dispatch_epoch_addr());
            run_js_ms += emscripten_get_now() - t2;
            ++js_calls;
            account_fast_counters(state);
            dispatches += counter_delta(dispatches_before, state.dispatches);
            tx_wasm += counter_delta(tx_before, state.tx_wasm);
            const uint32_t delta = counter_delta(executed_before, state.executed);
            if (delta > granted)
                return fail("generated region overran its budget");
            executed += delta;
            remaining_budget -= delta;
            // Light-path chained edges can leave the loop (Budget/Stop/Miss/
            // Svc/Fault) without a loop-top smc poll after a code-page store
            // (REGION_ABI.md v1.2): the edge's budget check may exit first.
            // Normalize any pending smc_dirty on EVERY non-Smc exit so
            // "SMC wins over the dispatch budget check" holds everywhere.
            if (reason != static_cast<uint32_t>(ExitReason::Smc) && state.smc_dirty) {
                ++smc_exits;
                if (state.smc_page == std::numeric_limits<uint32_t>::max())
                    clear_regions();
                else
                    clear_regions_for_page(state.smc_page);
                state.smc_dirty = 0;
            }
            switch (static_cast<ExitReason>(reason)) {
            case ExitReason::Svc:
                parent->svc = state.svc;
                parent->svc_called = true;
                ++svc_exits;
                return 0;
            case ExitReason::Fault: {
                // No rollback: completed blocks and the instructions before the
                // faulting memory op already executed architecturally
                // (REGION_ABI), and the checked helpers validate the whole
                // access before writing, so the faulting instruction left no
                // partial effects. What IS stale is the PC: region bodies
                // write regs[15] only at terminals, so it still points at the
                // block entry. Point it at the faulting instruction
                // (fault_pc), matching single-block faults.
                state.regs[15] = state.fault_pc;
                char message[96];
                std::snprintf(message, sizeof(message), "guest memory %s fault at %08x (pc %08x)",
                    state.fault_write ? "write" : "read", state.fault_address, state.fault_pc);
                return fail(message);
            }
            case ExitReason::Miss:
                // next_pc set by the module AND regs[15] already equals it
                // (every Miss path publishes both); the loop top resolves
                // the target through the host cache, refreshing the Wasm
                // map on the way into the pump. Unmapped targets compile
                // through the normal formation path there.
                ++host_miss;
                continue;
            case static_cast<ExitReason>(vita3k::wasmjit::DispatchReason::RegionOverrun):
                return fail("generated region overran its budget");
            case ExitReason::Budget:
                ++budget_exits;
                // True exhaustion (budget fully consumed) is an error, exactly
                // like single-block mode and the interpreter oracle: the
                // budget is a runaway guard, not a completion.
                if (remaining_budget == 0)
                    return fail("instruction budget exhausted");
                // No forward progress: the next block cannot fit the remaining
                // budget slice. Report a clean slice boundary so the caller
                // can re-grant; not an error, and not a livelock.
                if (state.executed == budget_progress_mark)
                    return 0;
                budget_progress_mark = state.executed;
                continue; // re-enter with the remaining budget
            case ExitReason::Smc:
                ++smc_exits;
                // next_pc is a continuation, not the modified address.
                // Invalidate only regions that actually cover the dirty code
                // page; unrelated hot regions remain reusable.
                if (state.smc_page == std::numeric_limits<uint32_t>::max())
                    clear_regions();
                else
                    clear_regions_for_page(state.smc_page);
                state.smc_dirty = 0;
                continue;
            case ExitReason::Stop:
                return 1;
            default:
                return fail("generated region returned unsupported exit");
            }
        }
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
                const double t0 = emscripten_get_now();
                auto ir = [&] {
                    try {
                        return vita3k::wasmjit::translate_block(*parent->mem,
                            state.regs[15], state.cpsr, translation_limit, state.fpscr);
                    } catch (const std::runtime_error &) {
                        // Speculative fetches beyond a valid first instruction
                        // may fault before the emitter can request a retry.
                        if (translation_limit == 1) throw;
                        translation_limit = 1;
                        return vita3k::wasmjit::translate_block(*parent->mem,
                            state.regs[15], state.cpsr, translation_limit, state.fpscr);
                    }
                }();
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
                emit_ms += emscripten_get_now() - t0;
                ++misses;
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
                const double t1 = emscripten_get_now();
                found->second.table_index = vita3k_jit_install(bytes.data(), bytes.size(),
                    checked_memory_read, checked_memory_write);
                install_ms += emscripten_get_now() - t1;
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
            state.memory_cookie = reinterpret_cast<uintptr_t>(parent->mem);
            // M15 fast-path bases, exactly as in region mode above.
            const auto *mem_state = parent->mem;
            state.page_table_base = mem_state->sparse_host_memory
                ? reinterpret_cast<uintptr_t>(mem_state->page_table.get()) : 0;
            state.page_perms_base = reinterpret_cast<uintptr_t>(mem_state->page_permissions.get());
            state.code_pages_base = reinterpret_cast<uintptr_t>(g_code_pages.data());
            // Fast-path tallies are per-call scratch (REGION_ABI.md): zero
            // them before the snapshot so the fault rollback below cannot
            // resurrect stale counts.
            state.mem_fast_reads = 0;
            state.mem_fast_writes = 0;
            const State before = state;
            // Host entry currently uses the EM_JS trampoline below.
            // Guest faults use return reasons; checked helpers are Wasm imports.
            const uintptr_t state_offset = reinterpret_cast<uintptr_t>(&state);
            const double t2 = emscripten_get_now();
            const uint32_t reason = vita3k_jit_call(found->second.table_index, state_offset);
            run_js_ms += emscripten_get_now() - t2;
            ++js_calls;
            account_fast_counters(state);
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
                ++svc_exits;
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
    if (impl->region_mode) {
        try {
            return impl->execute_regions(impl->budget);
        } catch (const std::exception &e) {
            return impl->fail(e.what());
        }
    }
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
    bool erased_region = false;
    for (auto it = impl->region_cache.begin(); it != impl->region_cache.end();) {
        const Region &r = *it->second.region;
        if (first_page < r.page_end && r.page_begin < end_page) {
            if (it->second.table_index >= 0) vita3k_jit_release_region(it->second.table_index);
            mark_code_pages(r, -1);
            it = impl->region_cache.erase(it);
            ++impl->invalidated;
            erased_region = true;
        } else ++it;
    }
    // Nulling a slot alone does not retire its map entries; slot reuse can
    // otherwise make an old key dispatch unrelated code after host invalidation.
    if (erased_region)
        dispatch_bump_epoch();
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
void WasmJitCPU::set_region_mode(bool v) { impl->region_mode = v; }
const std::string &WasmJitCPU::get_last_error() const { return impl->error; }
uint32_t WasmJitCPU::get_fault_address() const { return impl->state.fault_address; }
bool WasmJitCPU::get_fault_write() const { return impl->state.fault_write != 0; }
uint64_t WasmJitCPU::instructions_executed() const { return impl->executed; }
uint64_t WasmJitCPU::compiled_blocks() const { return impl->compiled; }
uint64_t WasmJitCPU::regions_formed() const { return impl->regions; }
uint64_t WasmJitCPU::cache_hits() const { return impl->hits; }
uint64_t WasmJitCPU::invalidated_blocks() const { return impl->invalidated; }
std::string WasmJitCPU::get_profile() const {
    char buffer[640];
    std::snprintf(buffer, sizeof(buffer),
        "emit_ms=%.1f install_ms=%.1f run_js_ms=%.1f js_calls=%llu misses=%llu "
        "svc_exits=%llu blocks=%llu mem_reads=%llu mem_writes=%llu "
        "regions=%llu region_misses=%llu smc_exits=%llu budget_exits=%llu dispatches=%llu "
        "fast_reads=%llu fast_writes=%llu slow_unmapped=%llu slow_perms=%llu "
        "slow_cross=%llu slow_code=%llu slow_other=%llu "
        "tx_wasm=%llu host_miss=%llu promote_flags=%u promote_accounting=%u",
        impl->emit_ms, impl->install_ms, impl->run_js_ms,
        (unsigned long long)impl->js_calls, (unsigned long long)impl->misses,
        (unsigned long long)impl->svc_exits, (unsigned long long)impl->compiled,
        (unsigned long long)g_mem_reads, (unsigned long long)g_mem_writes,
        (unsigned long long)impl->regions, (unsigned long long)impl->region_misses,
        (unsigned long long)impl->smc_exits, (unsigned long long)impl->budget_exits,
        (unsigned long long)impl->dispatches,
        (unsigned long long)g_mem_fast_reads, (unsigned long long)g_mem_fast_writes,
        (unsigned long long)g_mem_slow_unmapped, (unsigned long long)g_mem_slow_perms,
        (unsigned long long)g_mem_slow_cross_page, (unsigned long long)g_mem_slow_code_page,
        (unsigned long long)g_mem_slow_other,
        (unsigned long long)impl->tx_wasm, (unsigned long long)impl->host_miss,
        unsigned(impl->region_options.promote_flags), unsigned(impl->region_options.promote_accounting));
    return buffer;
}
