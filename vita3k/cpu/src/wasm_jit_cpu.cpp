// Opt-in M14 proof: Dynarmic frontend/IR -> generated Wasm regions.
// No interpreter fallback. The display application's default backend is unchanged.
#include <cpu/common.h>
#include <cpu/impl/wasm_jit_cpu.h>

#include <cpu/inline_mutex.h>
#include <mem/functions.h>
#include <cpu/state.h>
#include "wasmjit/frontend.h"
#include "wasmjit/emit_wasm.h"
#include "wasmjit/fp64.h"
#include <dynarmic/frontend/A32/a32_location_descriptor.h>
#include <dynarmic/ir/basic_block.h>
#include <dynarmic/ir/opcodes.h>
#include <emscripten.h>
#include <emscripten/emscripten.h>
#include <emscripten/heap.h>

#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <fmt/format.h>
#include <set>
#include <unordered_map>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "mem/functions.h"
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
// Live-region budget for the (single-populated-cache) region cache. Every
// eviction bumps the global dispatch-map epoch, so an undersized limit makes
// a code-streaming title stale the whole map continuously and fall back to
// host round-trips instead of Wasm-side chaining. A stream of ~17k distinct
// regions through a 128-entry LRU showed emit+install at ~18% of wall time
// and a ~57x gap versus the hot-loop bench.
//
// MEASURED, do not raise this on its own. Interleaved A/B (n=3 each, one
// binary, 180s runs, ABBAAB so machine drift cancels): raising the limit to
// 4096 cuts the cost it targets hard - capacity_evictions 7869 -> 1636 (-79%),
// regions formed 8893 -> 5732 (-36%), emit 14770 -> 9181ms (-38%), run_js
// 74787 -> 44704ms (-40%) - but revalidation is O(live regions) per host
// entry, so it grew 26785 -> 69900ms (+161%) and the net accounted total got
// WORSE (115776 -> 123843ms). End to end the guest simply got less done:
// frames 33 -> 20 and reported 6.1 -> 3.7 MIPS, at an unchanged 5.5
// draws/frame, i.e. it was starved rather than finished sooner.
// The 4096 numbers are kept here because they are the prize: execution plus
// compilation is only ~54s instead of ~90s. Realizing it needs revalidation
// to cost O(regions whose code actually changed) instead of O(cache), after
// which this limit should be re-measured. Until then 1024 ships.
// Runtime-tunable so one binary can A/B it (see region_cache_limit()); the
// value is printed in the profile as cache_limit.
constexpr size_t REGION_CACHE_LIMIT = 1024;
constexpr uint32_t REGION_CALL_TICKS = 131072; // Bound latency of host stop checks.

// Region-cache size knob. Read once per process from the Module property
// VITA3K_WASMJIT_REGION_CACHE, else process.env, else REGION_CACHE_LIMIT.
// Values below 64 are clamped: a tiny cache thrashes the dispatch map and the
// value exists to A/B cache SIZE, not to re-test pathological limits.
EM_JS(int, vita3k_jit_region_cache_option, (), {
    const read = (name) => {
        if (typeof Module !== 'undefined' && Module[name] !== undefined)
            return String(Module[name]);
        return (typeof process !== 'undefined' && process.env) ? process.env[name] : undefined;
    };
    const raw = read('VITA3K_WASMJIT_REGION_CACHE');
    if (raw === undefined)
        return 0;
    const value = parseInt(String(raw), 10);
    return Number.isFinite(value) ? value : 0;
});
size_t region_cache_limit() noexcept {
    static const size_t limit = []() {
        const int requested = vita3k_jit_region_cache_option();
        if (requested <= 0)
            return REGION_CACHE_LIMIT;
        return std::max<size_t>(64, static_cast<size_t>(requested));
    }();
    return limit;
}

// Opt-out for matched A/B runs. C getenv does not read browser module
// properties (nor Node process.env), so use the same channel as JIT options.
EM_JS(int, vita3k_jit_inline_mutex_option, (), {
    const v = Module['VITA3K_JIT_INLINE_MUTEX'] ??
        (typeof process !== 'undefined' ? process.env?.VITA3K_JIT_INLINE_MUTEX : undefined);
    return String(v) === '0' ? 0 : 1;
});
bool inline_mutex_enabled() noexcept {
    static const bool enabled = vita3k_jit_inline_mutex_option() != 0;
    return enabled;
}
// Only ARM little-endian stubs can match. Include return/NID in cache
// validation below so patching only the literal cannot retain an intrinsic.
uint32_t hot_stub_nid(const MemState &mem, uint32_t pc, uint32_t cpsr) noexcept {
    if ((cpsr & (0x20u | 0x200u)) || (pc & 3u))
        return 0;
    uint32_t words[3] = {};
    if (!mem_fetch(mem, pc, words, sizeof(words)))
        return 0;
    if (words[0] != 0xEF000000u || words[1] != 0xE1A0F00Eu)
        return 0;
    if (words[2] == vita3k::wasmjit::kGetTlsAddrNid)
        return words[2];
    return inline_mutex_enabled() && vita3k::wasmjit::is_inline_mutex_nid(words[2]) ? words[2] : 0;
}

uint32_t counter_delta(uint32_t before, uint32_t after) noexcept {
    return after - before; // A call cannot execute a full 2^32 ticks.
}

struct RegionBlock {
    Address pc = 0;
    uint32_t psr_mask = 0, psr_value = 0; // dispatch validation bits
    uint32_t ticks = 0;                    // conservative tick cost
    uint32_t hot_nid = 0;                  // inline mutex fast path (0 = none)
    std::vector<uint8_t> original;         // decoded bytes; hot stubs also track return/NID
    std::vector<vita3k::wasmjit::StoreContinuation> store_continuations;
};
struct RegionPage {
    struct Span {
        size_t block_index, block_offset;
        uint32_t page_offset, size;
    };
    uint32_t page = 0;
    uint32_t begin = 4096, end = 0;
    // g_code_page_versions[page] as of the last successful validation of this
    // page. Equal means nothing wrote the page since, so the bytes cannot
    // differ and the mem_fetch + memcmp below is skipped entirely.
    uint32_t version = 0;
    std::vector<Span> spans;
};
struct Region {
    std::vector<RegionBlock> blocks; // sorted by pc
    uint64_t total_ticks = 0;
    Address page_begin = 0, page_end = 0; // Half-open page-index bounds.
    std::vector<uint32_t> code_pages;     // Only pages actually containing code.
    std::vector<RegionPage> validation_pages; // Built once, reused on every entry.
};

// Wide reference counts: multiple CPUs and cached entries can share pages.
std::array<uint32_t, 1 << 20> g_code_pages{};
// Per-page write generation, and a global generation that changes whenever any
// page a cached region could cover is written through a TRACKED path. These
// exist only to make the region-cache sweep affordable:
//
//   - guest stores to a cached code page take the checked path, set smc_dirty,
//     and the host answers with clear_regions_for_page(), which DROPS the
//     affected regions rather than revalidating them;
//   - invalidate_jit_cache() is the sanctioned host-side "this code changed"
//     notification and bumps the covered pages here.
//
// So in steady state no tracked path writes compiled code, the global
// generation does not move, and the sweep is skipped with a single integer
// compare per host entry instead of re-fetching and re-comparing every
// validation page of every cached region (that sweep measured 25-30s of a
// 180s retail run at a 1024-entry cache and ~70s at 4096, while never finding
// a single stale region: entry_evicted was 0 in every measured run).
//
// KNOWN LIMIT, unchanged by this: a host write through a raw pointer into a
// compiled code page bypasses both tracked paths, because HLE resolves guest
// pointers with Ptr<T>::get() -> mem_guest_to_host() and then memcpy's, which
// is indistinguishable from a read at that layer. Audit behind this change:
// across the 39 HLE sources the web build compiles there is no direct
// guest-memory mutation except memcpy through Ptr::get() in 13 places, all
// targeting thread/mutex info, GXM program/uniform data or file buffers - not
// loaded code - and module loading completes before any region exists.
std::array<uint32_t, 1 << 20> g_code_page_versions{};
std::atomic<uint32_t> g_code_write_epoch{1};

// AOT seeding: full location keys (Dynarmic UniqueHash) of every block the
// lazy JIT translated, recorded only when VITA3K_AOT_SEEDS_OUT is set.
bool aot_seed_recording() noexcept {
    static const bool enabled = std::getenv("VITA3K_AOT_SEEDS_OUT") != nullptr;
    return enabled;
}
std::unordered_set<uint64_t> g_aot_seed_keys;

// Record that pages [first_page, last_page] may have changed through a tracked
// path. Only pages that actually carry compiled code bump a generation, so
// data writes (the overwhelming majority of host traffic) cost one counter test
// and leave the sweep skipped.
void note_code_page_write(uint32_t first_page, uint32_t last_page) noexcept {
    // Callers compute the range from Address arithmetic, so a wrapped or
    // inverted range is possible; clamp to the array and reject it rather than
    // walking ~4e9 entries.
    constexpr uint32_t kMaxPage = (1u << 20) - 1;
    if (last_page < first_page || first_page > kMaxPage)
        return;
    if (last_page > kMaxPage)
        last_page = kMaxPage;
    bool any = false;
    for (uint32_t page = first_page; page <= last_page; ++page) {
        if (!g_code_pages[page])
            continue;
        ++g_code_page_versions[page];
        any = true;
    }
    if (any)
        g_code_write_epoch.fetch_add(1, std::memory_order_relaxed);
}

void note_code_range_write(Address address, size_t size) noexcept {
    if (!size)
        return;
    const uint64_t end = (uint64_t(address) + size + 4095) >> 12;
    const uint32_t first = address >> 12;
    if (!first || end > (1u << 20))
        return;
    note_code_page_write(first, static_cast<uint32_t>(end - 1));
}

// Install the mem_write observer at library load, before any CPU exists, so
// every later code write through the memory funnel bumps the generations.
// Without this the conformance/backend suite is wrong in a way that looks like
// a JIT bug: it writes its own probe code with mem_write, and a region left
// over from an earlier test in the same process would be trusted as fresh, so
// the probe would execute stale bytes.
//
// Isolation switch: LIMBO's versioning build showed a reproducible +44% run_js
// and +66% emit_ms regression versus the pre-versioning baseline even though
// the revalidation saving was real. The observer (which fires on EVERY
// mem_write and mem_set_permissions, then walks the page range) is the prime
// suspect, so this toggle lets an A/B isolate it: explicitly '0' keeps the
// observer installed (indirect call remains) but makes its body a no-op,
// removing the page walk and the epoch/page-version bumps.
// DEFAULT IS ON and it is load-bearing for correctness, not just optimization:
// wasmjit_inline_mutex_tests.inc:202 patches a code page via mem_write and
// line 207 requires jit.invalidated_blocks() to increase, which only happens
// if the observer bumps the code write epoch. Turning it off breaks that test
// (and the production code-page-write invalidation guarantee), so OFF is a
// diagnostic state only, never a shipping default.
EM_JS(int, vita3k_jit_write_observer_option, (), {
    const v = Module['VITA3K_WASMJIT_WRITE_OBSERVER'] ??
        (typeof process !== 'undefined' ? process.env?.VITA3K_WASMJIT_WRITE_OBSERVER : undefined);
    return String(v) === '0' ? 0 : 1;
});
bool write_observer_enabled() noexcept {
    static const bool enabled = vita3k_jit_write_observer_option() != 0;
    return enabled;
}
namespace {
struct InstallMemWriteObserver {
    InstallMemWriteObserver() noexcept {
        ::g_mem_write_observer = [](Address addr, size_t size) {
            if (write_observer_enabled())
                note_code_range_write(addr, size);
        };
    }
} install_mem_write_observer;
} // namespace

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
        // Snapshot the current generation so the first validation of this
        // region can skip any page that has not been written since formation.
        page.version = g_code_page_versions[number];
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
                block.hot_nid = hot_stub_nid(mem, pc, location.CPSR().Value());
                block.psr_mask = PSR_DISPATCH_MASK;
                block.psr_value = location.CPSR().Value() & PSR_DISPATCH_MASK;
                block.ticks = static_cast<uint32_t>(ticks);
                // Validate only the member body here. Building a complete
                // one-block module for every candidate duplicates the most
                // expensive part of region formation and is discarded as
                // soon as the next candidate is examined.
                if (!vita3k::wasmjit::validate_region_block(translated, block.store_continuations))
                    return false;
                block.original.resize(block.hot_nid ? 12 : static_cast<size_t>(end - pc));
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
        if (block.hot_nid) {
            // Successful intrinsics fall through to the real MOV PC,LR.
            // Include it now: the fast arm can chain without a cold host miss.
            stack.emplace_back(ir->EndLocation());
        }
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

void mark_code_pages(const Region &region, int delta) {
    for (const uint32_t page : region.code_pages) {
        if (delta > 0)
            ++g_code_pages[page];
        else
            --g_code_pages[page];
    }
}

// Region-entry validation (replaces per-block unchanged()): revalidate the
// guest bytes of EVERY member block once per region entry, but only for pages
// whose write generation moved since the last successful check. Between
// entries, cached code pages are marked in g_code_pages, letting
// checked_memory_write flag smc_dirty for an immediate Smc exit instead of
// waiting for this check; a page whose version is unchanged here provably
// cannot differ, so it is skipped without touching memory.
// On success the observed versions are adopted, so a page that was rewritten
// with identical bytes is not re-read on every later entry.
// `force` ignores the generations and compares every page. It exists for the
// backend suite, whose byte-compare test corrupts the stored reference buffer
// directly (not through a guest write, so no generation moves) and for pages
// whose executability changed without a content write; both are cases the
// version filter would otherwise (correctly, in the real system) skip.
bool region_unchanged(Region &region, MemState &mem, bool force = false) {
    // No per-entry allocations, page grouping or binary searches. Only the
    // fetched range is read, so the scratch page needs no zero-initialization.
    std::array<uint8_t, 4096> bytes;
    for (auto &page : region.validation_pages) {
        const uint32_t version = g_code_page_versions[page.page];
        if (!force && page.version == version)
            continue;
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
        page.version = version;
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
// dispatcher code. Entry layout: {key_lo, key_hi, slot, epoch}; epoch 0 =
// never written.
//
// One slice per core (MAX_CORE_COUNT cores exist; each guest thread owns one,
// recycled through CorenumAllocator). A slice only ever holds that core's own
// table slots, so switching cores no longer has to retire the outgoing core's
// compiled state, and a core can never chain into another core's code. Slices
// are allocated on first dispatch, so a run pays only for the cores it uses.
static uint32_t *dispatch_map_slices[MAX_CORE_COUNT] = {};
static uint32_t dispatch_epoch = 1;
static uint64_t dispatch_global_version = 0;
// Process-global eviction generation. Every dispatch_bump_epoch() (all region-
// eviction paths funnel through it) advances this, so each CPU can tell whether
// any map invalidation happened since its last pump entry without paying a
// per-entry epoch bump (which would stale its own just-inserted entries).
static uint32_t *dispatch_map_slice(std::size_t core) noexcept {
    if (core >= MAX_CORE_COUNT)
        return nullptr;
    uint32_t *&slice = dispatch_map_slices[core];
    if (!slice)
        slice = static_cast<uint32_t *>(
            std::calloc(vita3k::wasmjit::kDispatchMapEntries * 4, sizeof(uint32_t)));
    return slice;
}
static uintptr_t dispatch_map_base(std::size_t core) noexcept {
    return reinterpret_cast<uintptr_t>(dispatch_map_slice(core));
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
        for (uint32_t *slice : dispatch_map_slices)
            if (slice)
                std::memset(slice, 0, vita3k::wasmjit::kDispatchMapEntries * 4 * sizeof(uint32_t));
        dispatch_epoch = 1;
    }
}
// Insert or refresh; bit-identical probing to the dispatcher emitter.
// Returns false only if no reusable slot exists within the probe limit
// (impossible at REGION_CACHE_LIMIT << map size; fails loudly if hit).
static bool dispatch_map_insert(std::size_t core, uint64_t key, uint32_t slot) noexcept {
    using namespace vita3k::wasmjit;
    uint32_t *base = dispatch_map_slice(core);
    if (!base)
        return false;
    const uint32_t lo = static_cast<uint32_t>(key);
    const uint32_t hi = static_cast<uint32_t>(key >> 32);
    uint32_t idx = dispatch_map_index(lo, hi);
    for (uint32_t i = 0; i < kDispatchMaxProbe; ++i) {
        uint32_t *e = &base[(idx & kDispatchMapMask) * 4];
        if (e[3] == dispatch_epoch && e[0] == lo && e[1] == hi) {
            e[2] = slot; // refresh existing mapping
            return true;
        }
        idx++;
    }
    idx = dispatch_map_index(lo, hi);
    for (uint32_t i = 0; i < kDispatchMaxProbe; ++i) {
        uint32_t *e = &base[(idx & kDispatchMapMask) * 4];
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
            // Tracked write: bump the page generation too, so any OTHER
            // cached region covering this page is revalidated on the next
            // host entry even if this thread's region is dropped instead.
            note_code_page_write(static_cast<uint32_t>(page), static_cast<uint32_t>(page));
        }
    }
    return 0;
}
// Native Wasm helper: only the four scratch words are read/written. Flags
// are returned, not stored, so no promoted architectural state is observed.
uint32_t fp64_helper(JitState *state, uint32_t operation, uint32_t fpscr) noexcept {
    const auto a = uint64_t(state->memory_value[0]) | (uint64_t(state->memory_value[1]) << 32);
    const auto b = uint64_t(state->memory_value[2]) | (uint64_t(state->memory_value[3]) << 32);
    const auto result = vita3k::wasmjit::fp64_arithmetic(operation, a, b, fpscr);
    state->memory_value[0] = uint32_t(result.bits);
    state->memory_value[1] = uint32_t(result.bits >> 32);
    return result.flags;
}

uint32_t aot_hash(const uint8_t *data, size_t size) noexcept {
    uint32_t hash = 0x811c9dc5;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x01000193;
    }
    return hash;
}

// Process-wide loaded AOT module. The lookup table is host memory the module
// reads through its env.aot_lut global; the host uses it to choose AOT entry.
using AotEntry = uint32_t (*)(JitState *, uint32_t);
struct AotRuntime {
    bool loaded = false;
    AotEntry entry = nullptr;
    bool disabled = false;
    std::vector<vita3k::wasmjit::AotRange> ranges;
    std::vector<uint64_t> offsets; // lookup-table word offset per range
    std::vector<uint32_t> lut;
    std::vector<std::pair<uint32_t, uint32_t>> spans; // per slot [begin, end)
    uint64_t invalidated_functions = 0;
    uint32_t slot(uint32_t pc) const noexcept {
        for (size_t k = 0; k < ranges.size(); ++k) {
            const uint32_t offset = pc - ranges[k].base;
            if (offset < ranges[k].size)
                return lut[offsets[k] + offset / 2];
        }
        return 0;
    }
    bool covers_page(uint32_t page) const noexcept {
        for (const auto &range : ranges)
            if (page >= (range.base >> 12) && page <= ((range.base + range.size - 1) >> 12))
                return true;
        return false;
    }
} g_aot;

// VITA3K_AOT_DIFF=N (diagnostic): every Nth AOT call is also executed by the
// lazy JIT from the same state and memory, and the results are compared.
struct AotDiffStats {
    uint64_t calls = 0, samples = 0, compared = 0, count_mismatch = 0, mismatches = 0;
    uint64_t fpscr_flags_only = 0, nan_payload_only = 0, snapshot_bytes = 0;
    std::map<std::string, uint64_t> classes;
    std::map<int, uint64_t> per_thread; // compared samples by guest thread id
};
AotDiffStats g_aot_diff;
uint64_t aot_diff_every() {
    static const uint64_t every = [] {
        const char *value = std::getenv("VITA3K_AOT_DIFF");
        return value ? std::strtoull(value, nullptr, 10) : 0ull;
    }();
    return every;
}
bool is_nan32(uint32_t bits) { return (bits & 0x7fffffffu) > 0x7f800000u; }
bool jit_timing() noexcept {
    static const bool enabled = std::getenv("VITA3K_JIT_TIMING") != nullptr;
    return enabled;
}
// VITA3K_AOT_DIFF_AFTER=<seconds>: start sampling that long after startup.
double aot_diff_after_ms() {
    static const double after = [] {
        const char *value = std::getenv("VITA3K_AOT_DIFF_AFTER");
        return (value ? std::strtod(value, nullptr) * 1000.0 : 0.0) + emscripten_get_now();
    }();
    return after;
}
// VITA3K_AOT_DIFF_THREAD=<guest thread id>: sample only that thread's calls.
int aot_diff_thread() {
    static const int thread = [] {
        const char *value = std::getenv("VITA3K_AOT_DIFF_THREAD");
        return value ? std::atoi(value) : 0;
    }();
    return thread;
}

// --- AOT: whole-program ahead-of-time regions (AOT.md) --------------------
// The builder forms one region-shaped function per guest function from a
// static root set, using the same translation and emission rules as the lazy
// path, and assembles all of them into ONE module. At run time the module's
// lookup table maps every member entry PC to its owning function.
// Control-flow summary of one translated block. The IR itself is NOT kept:
// every Dynarmic IR::Block eagerly allocates a 4096-instruction pool, so a
// whole-program cache of blocks exhausts the host heap. Emission re-translates
// members with the recorded parameters, which reproduces the same block.
struct AotTranslated {
    RegionBlock block;
    uint32_t limit = 0;          // translate_block instruction budget used
    bool continue_stores = false; // translated with store continuations
    bool calls = false;          // contains PushRSB
    std::vector<Dynarmic::A32::LocationDescriptor> targets; // terminal links
    std::vector<Dynarmic::A32::LocationDescriptor> returns; // PushRSB targets
    std::optional<Dynarmic::A32::LocationDescriptor> fallthrough; // hot stub return
    std::optional<Dynarmic::A32::LocationDescriptor> condition_failed;
    uint64_t location = 0;
    uint32_t end_pc = 0; // first byte after the block's guest code
};

class AotBuilder {
public:
    AotBuilder(MemState &mem, const WasmJitCPU::AotBuildSpec &spec, vita3k::wasmjit::RegionStateOptions options)
        : mem(mem), spec(spec), options(options), validate_options(options) {
        validate_options.aot = true; // accept exactly what emit_aot_function emits
    }

    bool build(std::vector<uint8_t> &out, std::string &report) {
        using Dynarmic::A32::LocationDescriptor;
        for (const auto &range : spec.code)
            ranges.push_back({range.base, range.size});
        std::sort(ranges.begin(), ranges.end(), [](const auto &a, const auto &b) { return a.base < b.base; });
        for (size_t i = 1; i < ranges.size(); ++i)
            if (ranges[i].base < ranges[i - 1].base + ranges[i - 1].size)
                return fail(report, "overlapping AOT code ranges");
        for (const uint64_t value : spec.function_roots)
            add_root(LocationDescriptor{Dynarmic::IR::LocationDescriptor{value}});
        drain();
        // Seeds (locations the lazy JIT actually executed) must be owned by
        // some function in their own mode; uncovered ones become roots.
        for (const uint64_t value : spec.extra_entries) {
            const LocationDescriptor location{Dynarmic::IR::LocationDescriptor{value}};
            if (!owned(location))
                add_root(location);
        }
        drain();
        return assemble(out, report);
    }

private:
    struct Function {
        uint64_t root = 0;
        std::vector<const AotTranslated *> members; // sorted by PC
        std::vector<uint8_t> body;
    };
    MemState &mem;
    const WasmJitCPU::AotBuildSpec &spec;
    vita3k::wasmjit::RegionStateOptions options, validate_options;
    std::vector<vita3k::wasmjit::AotRange> ranges;
    std::deque<Dynarmic::A32::LocationDescriptor> pending_roots;
    std::unordered_set<uint64_t> roots;
    std::unordered_map<uint64_t, std::unique_ptr<AotTranslated>> translated;
    std::unordered_set<uint64_t> untranslatable;
    std::unordered_map<uint64_t, std::string> root_failures;
    std::string last_rejection;
    std::map<std::string, size_t> rejections; // untranslatable blocks by first rejection
    std::vector<Function> functions;
    // Member locations of successfully emitted functions (mode-exact).
    std::unordered_set<uint64_t> covered;
    size_t split_roots = 0, emit_rejects = 0, member_count = 0;
    const bool trace = std::getenv("VITA3K_AOT_TRACE") != nullptr;
    // Diagnostic: VITA3K_AOT_LIMIT=N stops forming after N functions so a
    // profiled build finishes quickly; the module is then incomplete.
    size_t limit = [] {
        const char *value = std::getenv("VITA3K_AOT_LIMIT");
        return value ? static_cast<size_t>(std::strtoull(value, nullptr, 10)) : SIZE_MAX;
    }();

    static bool fail(std::string &report, const char *why) {
        report = why;
        return false;
    }
    bool in_code(uint32_t pc) const {
        for (const auto &range : ranges)
            if (pc - range.base < range.size)
                return true;
        return false;
    }
    bool owned(const Dynarmic::A32::LocationDescriptor &location) const {
        return covered.contains(location.UniqueHash());
    }
    void add_root(const Dynarmic::A32::LocationDescriptor &location) {
        if (!in_code(location.PC()))
            return;
        if (roots.insert(location.UniqueHash()).second)
            pending_roots.push_back(location);
    }

    // One translation attempt; the same parameters reproduce the same block.
    std::optional<Dynarmic::IR::Block> translate_ir(const Dynarmic::A32::LocationDescriptor &location,
        uint32_t limit, bool continue_stores, RegionBlock &block) {
        using Dynarmic::A32::LocationDescriptor;
        const uint32_t pc = location.PC();
        block = RegionBlock{};
        try {
            // AOT bodies continue across stores without polls, so a block
            // ends only at control flow or the instruction limit.
            auto ir = vita3k::wasmjit::translate_block(mem, pc, location.CPSR().Value(), limit,
                location.FPSCR().Value(), continue_stores ? &block.store_continuations : nullptr,
                REGION_BLOCK_INSTR_LIMIT);
            const uint64_t end = LocationDescriptor(ir.EndLocation()).PC();
            if (end <= pc || end - pc > REGION_MAX_CODE_BYTES)
                return std::nullopt;
            const uint64_t ticks = ir.CycleCount() + ir.ConditionFailedCycleCount();
            if (!ticks || ticks > REGION_MAX_TICKS)
                return std::nullopt;
            if (!vita3k::wasmjit::validate_region_block(ir, block.store_continuations, &last_rejection, validate_options))
                return std::nullopt;
            block.pc = pc;
            block.hot_nid = hot_stub_nid(mem, pc, location.CPSR().Value());
            block.psr_mask = PSR_DISPATCH_MASK;
            block.psr_value = location.CPSR().Value() & PSR_DISPATCH_MASK;
            block.ticks = static_cast<uint32_t>(ticks);
            return ir;
        } catch (const std::exception &) {
            return std::nullopt;
        }
    }

    // Same candidate ladder as form_region: a full store-continued block,
    // then the store-ending block, then a single instruction.
    const AotTranslated *translate(const Dynarmic::A32::LocationDescriptor &location) {
        const uint64_t key = location.UniqueHash();
        if (const auto found = translated.find(key); found != translated.end())
            return found->second.get();
        if (untranslatable.contains(key) || !in_code(location.PC()))
            return nullptr;
        auto result = std::make_unique<AotTranslated>();
        std::optional<Dynarmic::IR::Block> ir;
        last_rejection.clear();
        for (const auto [limit, continue_stores] : {std::pair{REGION_BLOCK_INSTR_LIMIT, true},
                 std::pair{REGION_BLOCK_INSTR_LIMIT, false}, std::pair{1u, false}}) {
            ir = translate_ir(location, limit, continue_stores, result->block);
            if (ir) {
                result->limit = limit;
                result->continue_stores = continue_stores;
                break;
            }
        }
        if (!ir) {
            // The single-instruction attempt names the first unsupported op.
            ++rejections[last_rejection.empty() ? "fetch/translate" : last_rejection];
            if (trace) {
                std::array<uint8_t, 4> bytes{};
                mem_fetch(mem, location.PC(), bytes.data(), bytes.size());
                std::fprintf(stderr, "[aot] untranslatable %08x%s bytes=%02x%02x%02x%02x %s\n", location.PC(),
                    location.TFlag() ? "T" : "A", bytes[0], bytes[1], bytes[2], bytes[3],
                    last_rejection.empty() ? "fetch/translate" : last_rejection.c_str());
            }
            untranslatable.insert(key);
            return nullptr;
        }
        result->location = key;
        result->end_pc = Dynarmic::A32::LocationDescriptor(ir->EndLocation()).PC();
        collect_targets(ir->GetTerminal(), result->targets);
        for (const Dynarmic::IR::Inst &inst : *ir) {
            if (inst.GetOpcode() != Dynarmic::IR::Opcode::PushRSB)
                continue;
            result->calls = true;
            result->returns.emplace_back(Dynarmic::IR::LocationDescriptor{inst.GetArg(0).GetU64()});
        }
        if (result->block.hot_nid)
            result->fallthrough.emplace(ir->EndLocation());
        if (ir->GetCondition() != Dynarmic::IR::Cond::AL && ir->HasConditionFailedLocation())
            result->condition_failed.emplace(ir->ConditionFailedLocation());
        auto *pointer = result.get();
        translated.emplace(key, std::move(result));
        return pointer;
    }

    void drain() {
        using Dynarmic::A32::LocationDescriptor;
        while (!pending_roots.empty() && functions.size() < limit) {
            const LocationDescriptor root = pending_roots.front();
            pending_roots.pop_front();
            if (owned(root))
                continue;
            if (trace)
                std::fprintf(stderr, "[aot] root %08x%s\n", root.PC(), root.TFlag() ? "T" : "A");
            form(root);
        }
    }

    void form(const Dynarmic::A32::LocationDescriptor &root) {
        using Dynarmic::A32::LocationDescriptor;
        Function function;
        function.root = root.UniqueHash();
        std::vector<LocationDescriptor> stack{root};
        std::unordered_set<uint32_t> member_pcs;
        std::vector<LocationDescriptor> overflow;
        uint64_t ticks = 0;
        while (!stack.empty()) {
            const LocationDescriptor location = stack.back();
            stack.pop_back();
            if (member_pcs.contains(location.PC()))
                continue;
            const AotTranslated *block = translate(location);
            if (!block) {
                if (location.UniqueHash() == function.root) {
                    root_failures.emplace(function.root, "untranslatable root");
                    return;
                }
                continue; // runtime Miss -> host/lazy JIT for this edge
            }
            if (function.members.size() >= REGION_MAX_BLOCKS
                || ticks + block->block.ticks > REGION_MAX_TICKS) {
                overflow.push_back(location);
                continue;
            }
            member_pcs.insert(location.PC());
            function.members.push_back(block);
            ticks += block->block.ticks;
            for (const auto &target : block->targets) {
                // A call edge (the block pushed a return address) or a jump to
                // a known function entry leaves this function.
                // Code another function already owns is reached through the
                // lookup table instead of being duplicated here.
                if (block->calls || (roots.contains(target.UniqueHash()) && target.UniqueHash() != function.root))
                    add_root(target);
                else if (covered.contains(target.UniqueHash()))
                    continue;
                else
                    stack.push_back(target);
            }
            for (const auto &ret : block->returns)
                stack.push_back(ret);
            if (block->fallthrough)
                stack.push_back(*block->fallthrough);
            if (block->condition_failed)
                stack.push_back(*block->condition_failed);
        }
        for (const auto &location : overflow) {
            if (!member_pcs.contains(location.PC())) {
                ++split_roots;
                add_root(location);
            }
        }
        std::sort(function.members.begin(), function.members.end(),
            [](const AotTranslated *a, const AotTranslated *b) { return a->block.pc < b->block.pc; });
        // Re-translate the members for emission only; the IR dies here.
        std::vector<Dynarmic::IR::Block> ir_blocks;
        ir_blocks.reserve(function.members.size());
        std::vector<vita3k::wasmjit::RegionBlockMeta> meta;
        for (const auto *member : function.members) {
            const LocationDescriptor location{Dynarmic::IR::LocationDescriptor{member->location}};
            RegionBlock block;
            auto ir = translate_ir(location, member->limit, member->continue_stores, block);
            if (!ir || block.ticks != member->block.ticks) {
                root_failures.emplace(function.root, "nondeterministic retranslation");
                return;
            }
            ir_blocks.push_back(std::move(*ir));
            meta.push_back({block.pc, block.psr_mask, block.psr_value, block.ticks,
                std::move(block.store_continuations), block.hot_nid});
        }
        std::vector<const Dynarmic::IR::Block *> blocks;
        blocks.reserve(ir_blocks.size());
        for (const auto &ir : ir_blocks)
            blocks.push_back(&ir);
        function.body = vita3k::wasmjit::emit_aot_function(blocks, meta, options);
        if (function.body.empty()) {
            ++emit_rejects;
            root_failures.emplace(function.root, "emission rejected");
            return;
        }
        for (const auto *member : function.members)
            covered.insert(member->location);
        member_count += function.members.size();
        functions.push_back(std::move(function));
        if (functions.size() % 2000 == 0)
            std::fprintf(stderr, "[aot] %zu functions, %zu members, %zu roots queued\n",
                functions.size(), member_count, pending_roots.size());
    }

    static void put(std::vector<uint8_t> &out, uint32_t word) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(word >> (8 * i)));
    }

    bool assemble(std::vector<uint8_t> &out, std::string &report) {
        if (functions.empty())
            return fail(report, "no AOT function could be formed");
        // Lookup table: a function's root first, then any member. Only
        // members in the default execution state (IT=0, E=0, FPSCR mode 0)
        // are addressable; the transfer helper checks exactly that.
        // One entry per PC. Several functions (or modes) can contain the same
        // PC; prefer what was observed executing (seeds), then roots, then
        // members, and Thumb on a tie (a data word decoded as ARM must not
        // shadow real Thumb code).
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> ranked_owner; // pc -> (rank, entry)
        const std::unordered_set<uint64_t> observed(spec.extra_entries.begin(), spec.extra_entries.end());
        const auto enterable = [](const AotTranslated *member) {
            const Dynarmic::A32::LocationDescriptor location{Dynarmic::IR::LocationDescriptor{member->location}};
            return location.IT().Value() == 0 && !location.EFlag()
                && (location.FPSCR().Value() & Dynarmic::A32::LocationDescriptor::FPSCR_MODE_MASK) == 0;
        };
        if (functions.size() > vita3k::wasmjit::kAotMaxFunctions)
            return fail(report, "too many AOT functions for the lookup table encoding");
        for (uint32_t slot = 0; slot < functions.size(); ++slot) {
            const auto &members = functions[slot].members;
            for (uint32_t index = 0; index < members.size(); ++index) {
                const auto *member = members[index];
                if (!enterable(member))
                    continue;
                const bool thumb = Dynarmic::A32::LocationDescriptor{
                    Dynarmic::IR::LocationDescriptor{member->location}}.TFlag();
                const uint32_t rank = (observed.contains(member->location) ? 8 : 0)
                    + (member->location == functions[slot].root ? 4 : 0) + (thumb ? 1 : 0);
                const uint32_t entry = vita3k::wasmjit::aot_lut_entry(slot, index, thumb);
                auto [it, inserted] = ranked_owner.emplace(member->block.pc, std::pair{rank, entry});
                if (!inserted && rank > it->second.first)
                    it->second = {rank, entry};
            }
        }
        std::map<uint32_t, uint32_t> owner;
        for (const auto &[pc, ranked] : ranked_owner)
            owner.emplace(pc, ranked.second);
        std::vector<uint8_t> metadata;
        put(metadata, WasmJitCPU::aot_magic);
        put(metadata, WasmJitCPU::aot_version);
        put(metadata, static_cast<uint32_t>(ranges.size()));
        for (const auto &range : ranges) {
            std::vector<uint8_t> bytes(range.size);
            if (!mem_read(mem, range.base, bytes.data(), bytes.size()))
                return fail(report, "AOT code range is not readable");
            put(metadata, range.base);
            put(metadata, range.size);
            put(metadata, aot_hash(bytes.data(), bytes.size()));
        }
        put(metadata, static_cast<uint32_t>(owner.size()));
        for (const auto &[pc, entry] : owner) {
            put(metadata, pc);
            put(metadata, entry);
        }
        // Guest code span of every function (slot order), for invalidation.
        put(metadata, static_cast<uint32_t>(functions.size()));
        for (const auto &function : functions) {
            uint32_t begin = UINT32_MAX, end = 0;
            for (const auto *member : function.members) {
                begin = std::min(begin, member->block.pc);
                end = std::max(end, member->end_pc);
            }
            put(metadata, begin);
            put(metadata, end);
        }
        std::vector<std::vector<uint8_t>> bodies;
        bodies.reserve(functions.size());
        for (auto &function : functions)
            bodies.push_back(std::move(function.body));
        out = vita3k::wasmjit::assemble_aot_module(bodies, ranges, metadata, options);
        if (out.empty())
            return fail(report, "AOT module assembly rejected");
        std::map<std::string, size_t> failures;
        for (const auto &[root, why] : root_failures)
            ++failures[why];
        char line[512];
        std::snprintf(line, sizeof(line),
            "functions=%zu members=%zu translated=%zu untranslatable=%zu entries=%zu split_roots=%zu "
            "emit_rejects=%zu roots=%zu bytes=%zu",
            functions.size(), member_count, translated.size(), untranslatable.size(), owner.size(),
            split_roots, emit_rejects, roots.size(), out.size());
        report = line;
        for (const auto &[why, count] : failures)
            report += std::string(" ") + why + "=" + std::to_string(count);
        std::vector<std::pair<size_t, std::string>> ranked;
        for (const auto &[why, count] : rejections)
            ranked.emplace_back(count, why);
        std::sort(ranked.rbegin(), ranked.rend());
        if (!ranked.empty())
            report += "\n[vita3k-web] AOT untranslatable blocks by reason:";
        for (const auto &[count, why] : ranked)
            report += "\n  " + std::to_string(count) + " " + why;
        return true;
    }
};
} // namespace

// Compilation and host entry into generated functions cross JS. Inside a
// generated region, dispatch and checked memory helpers stay in Wasm.
// Imported helper pointers arrive as table indices.
EM_JS(int, vita3k_jit_install_impl, (const uint8_t *bytes, unsigned length,
    MemoryFunction read_memory, MemoryFunction write_memory, MemoryFunction arithmetic), {
    let slot = -1;
    const freeSlots = Module['vita3kJitFreeSlots'] || (Module['vita3kJitFreeSlots'] = []);
    try {
        const raw = Module['vita3kHostBytes'](bytes, length).slice();
        if (typeof process !== 'undefined' && process.env?.VITA3K_DUMP_JIT) require('fs').writeFileSync('/tmp/jit-module.wasm', raw);
        const module = new WebAssembly.Module(raw);
        const instance = new WebAssembly.Instance(module, {env: {
            memory: wasmMemory,
            mem_read: Module['vita3kNativeFunction'](read_memory),
            mem_write: Module['vita3kNativeFunction'](write_memory),
            fp64: Module['vita3kNativeFunction'](arithmetic)
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
int vita3k_jit_install(const uint8_t *bytes, unsigned length,
    MemoryFunction read_memory, MemoryFunction write_memory) {
    return vita3k_jit_install_impl(bytes, length, read_memory, write_memory, fp64_helper);
}
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
        Module['vita3kJitTable'] = new WebAssembly.Table({initial: 8192, element: 'anyfunc'});
});
EM_JS(int, vita3k_jit_install_region_impl, (const uint8_t *bytes, unsigned length,
    MemoryFunction read_memory, MemoryFunction write_memory, MemoryFunction arithmetic), {
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
            mem_write: Module['vita3kNativeFunction'](write_memory),
            fp64: Module['vita3kNativeFunction'](arithmetic)
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
int vita3k_jit_install_region(const uint8_t *bytes, unsigned length,
    MemoryFunction read_memory, MemoryFunction write_memory) {
    return vita3k_jit_install_region_impl(bytes, length, read_memory, write_memory, fp64_helper);
}
// The multi-region dispatcher module is built once per process; it imports
// the shared region table and the same linear memory as the regions.
EM_JS(int, vita3k_jit_install_dispatch, (const uint8_t *bytes, unsigned length), {
    try {
        const raw = Module['vita3kHostBytes'](bytes, length).slice();
        if (typeof process !== 'undefined' && process.env?.VITA3K_DUMP_JIT) require('fs').writeFileSync('/tmp/jit-dispatch.wasm', raw);
        if (!Module['vita3kJitTable'])
            Module['vita3kJitTable'] = new WebAssembly.Table({initial: 8192, element: 'anyfunc'});
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

// AOT module transport. The Worker supplies a precompiled WebAssembly.Module
// as Module.vita3kAotModule; Node benchmarks may name a file in VITA3K_AOT.
EM_JS(uint32_t, vita3k_aot_metadata_size, (), {
    let module = Module['vita3kAotModule'];
    if (!module) {
        const path = Module['VITA3K_AOT'] ??
            (typeof process !== 'undefined' ? process.env?.VITA3K_AOT : undefined);
        if (!path)
            return 0;
        module = new WebAssembly.Module(require('fs').readFileSync(path));
        Module['vita3kAotModule'] = module;
    }
    const sections = WebAssembly.Module.customSections(module, 'vita3k.aot');
    if (sections.length !== 1) {
        err('[vita3k-web] AOT module has no vita3k.aot metadata section');
        return 0;
    }
    Module['vita3kAotMetadata'] = new Uint8Array(sections[0]);
    return Module['vita3kAotMetadata'].length;
});
EM_JS(void, vita3k_aot_metadata_copy, (uint8_t *dest), {
    const metadata = Module['vita3kAotMetadata'];
    Module['vita3kHostBytes'](dest, metadata.length).set(metadata);
});
// Returns the native function-table slot of the module's entry, so the host
// calls it directly (wasm call_indirect, no JS) as an AotEntry pointer.
EM_JS(int, vita3k_aot_instantiate, (const uint32_t *lut, MemoryFunction read_memory,
    MemoryFunction write_memory, MemoryFunction arithmetic), {
    try {
        const instance = new WebAssembly.Instance(Module['vita3kAotModule'], {env: {
            memory: wasmMemory,
            mem_read: Module['vita3kNativeFunction'](read_memory),
            mem_write: Module['vita3kNativeFunction'](write_memory),
            fp64: Module['vita3kNativeFunction'](arithmetic),
            aot_lut: Module['vita3kHostPointer'](lut)
        }});
        const slot = Number(wasmTable.grow(Module['vita3kMemory64'] ? 1n : 1));
        setWasmTableEntry(slot, instance.exports.entry);
        return slot;
    } catch (error) {
        err('[vita3k-web] AOT instantiation failed: ' + error);
        return -1;
    }
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
    uint64_t mutex_fast_take = 0, mutex_fast_release = 0, mutex_fast_fallback = 0;
    // Dispatch-ownership telemetry (step-2 measurement, no behavior change).
    // host_entries: execute_regions host entries (region path only).
    // post_hle_entries: entries where svc_exits advanced since the previous
    //   entry, i.e. HLE ran between pumps (covers suspended-HLE returns).
    // version_syncs/version_bumps: Memory64 entry version comparisons vs ack
    //   bumps applied (each ack bump advances the global version, so
    //   alternating same-core CPUs bump on every switch even with no
    //   evictions; the miss cost only materializes under eviction churn).
    // entry_scanned/entry_evicted: Memory64 whole-cache revalidation probes
    //   vs regions dropped by the entry scan. select_checks/select_stale:
    //   loop-top selected-region validations vs stale drops.
    //   capacity_evictions: LRU victim removals.
    uint64_t host_entries = 0, post_hle_entries = 0, last_svc_at_entry = 0;
    uint64_t version_syncs = 0, version_bumps = 0;
    uint64_t entry_scanned = 0, entry_evicted = 0;
    // Wall time spent in the whole-cache revalidation sweep above. Diagnostic
    // only: it attributes the Memory64 entry cost that entry_scanned counts.
    double revalidate_ms = 0;
    // g_code_write_epoch as of this core's last whole-cache sweep. Zero forces
    // the first sweep, so regions formed before tracking started are checked.
    uint32_t swept_code_epoch = 0;
    uint64_t select_checks = 0, select_stale = 0, capacity_evictions = 0;
    // Region-mode profiling.
    uint64_t regions = 0, region_misses = 0, smc_exits = 0, dispatches = 0;
    // M16 pump counters: in-Wasm chained transfers (tx_wasm) vs dispatcher
    // Miss returns to the host (host_miss; ~all resolve to compiled regions
    // at the loop top, true compiles are region_misses).
    uint64_t tx_wasm = 0, host_miss = 0;
    // AOT entries (host -> AOT module) and entry misses (PC/mode not owned).
    uint64_t aot_calls = 0, aot_entry_misses = 0;
    uint32_t aot_skip_pc = 0xffffffffu;
    bool aot_enabled = true;    // per thread (VITA3K_AOT_EXCLUDE_THREADS)
    bool diff_reference = false; // running the lazy half of a VITA3K_AOT_DIFF sample
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
    void account_counters() {
        account_fast_counters(state);
        mutex_fast_take += state.mutex_fast_take;
        mutex_fast_release += state.mutex_fast_release;
        mutex_fast_fallback += state.mutex_fast_fallback;
        state.mutex_fast_take = state.mutex_fast_release = state.mutex_fast_fallback = 0;
    }
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
    // ExitReason::Exception: the guest reached an instruction Dynarmic
    // translates to A32ExceptionRaised. Report it exactly like a block the
    // emitter rejected, at the raising instruction instead of the block entry.
    int exception_exit() {
        state.regs[15] = state.fault_pc;
        try {
            return reject(vita3k::wasmjit::translate_block(*parent->mem,
                state.fault_pc, state.cpsr, 1, state.fpscr));
        } catch (const std::exception &error) {
            return fail(error.what());
        }
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
    bool scheduler_slice = false;
    int budget_exhausted() {
        return scheduler_slice ? WasmJitCPU::slice_yield : fail("instruction budget exhausted");
    }
    // Shared region form/emit/install path for the lazy miss path in
    // execute_regions() and for AOT-1 precompile_region(). Forms the closure,
    // emits the Wasm module, installs it in the function table, and publishes
    // the cache entry WITHOUT executing guest code. Failures report through
    // fail()/reject() exactly as the lazy path does; the caller checks for
    // region_cache.end(). Per-build measurements (module bytes, block/tick
    // counts, emit/install time) are reported through `stats` when non-null.
    struct RegionBuildStats {
        size_t blocks = 0;
        uint32_t ticks = 0;
        size_t wasm_bytes = 0;
        double emit_ms = 0;
        double install_ms = 0;
    };
    std::map<uint64_t, RegionEntry>::iterator ensure_region(uint32_t pc, uint64_t key,
        RegionBuildStats *stats) {
        if (region_cache.size() >= region_cache_limit()) {
            ++capacity_evictions;
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
        if (!form_region(*parent->mem, pc, state.cpsr, state.fpscr, *region, ir_blocks)) {
            // Report the actual failing instruction, not just the
            // region failure (which hides the unsupported op).
            try {
                reject(vita3k::wasmjit::translate_block(*parent->mem,
                    pc, state.cpsr, 1, state.fpscr));
            } catch (const std::exception &error) {
                fail(error.what());
            }
            return region_cache.end();
        }
        std::vector<const Dynarmic::IR::Block *> block_ptrs;
        std::vector<vita3k::wasmjit::RegionBlockMeta> meta;
        block_ptrs.reserve(region->blocks.size());
        meta.reserve(region->blocks.size());
        for (size_t i = 0; i < region->blocks.size(); ++i) {
            if (aot_seed_recording())
                g_aot_seed_keys.insert(ir_blocks[i].Location().Value());
            block_ptrs.push_back(&ir_blocks[i]);
            meta.push_back({region->blocks[i].pc, region->blocks[i].psr_mask,
                region->blocks[i].psr_value, region->blocks[i].ticks,
                region->blocks[i].store_continuations, region->blocks[i].hot_nid});
        }
        const auto bytes = vita3k::wasmjit::emit_region(block_ptrs, meta, region_options);
        const double emit_done = emscripten_get_now() - t0;
        emit_ms += emit_done;
        if (stats)
            stats->emit_ms = emit_done;
        if (bytes.empty()) {
            fail("region emission rejected (no fallback)");
            return region_cache.end();
        }
        // Allocate the map node before installing the JS function.
        auto found = region_cache.emplace(key,
            RegionEntry{std::move(region), -1}).first;
        const double t1 = emscripten_get_now();
        const int slot = vita3k_jit_install_region(bytes.data(), bytes.size(),
            checked_memory_read, checked_memory_write);
        const double install_done = emscripten_get_now() - t1;
        install_ms += install_done;
        if (stats)
            stats->install_ms = install_done;
        if (slot < 0) {
            region_cache.erase(found); // No pages were marked yet.
            fail("browser rejected generated region Wasm");
            return region_cache.end();
        }
        found->second.table_index = slot;
        mark_code_pages(*found->second.region, +1);
        ++regions;
        if (stats) {
            stats->blocks = found->second.region->blocks.size();
            stats->ticks = found->second.region->total_ticks;
            stats->wasm_bytes = bytes.size();
        }
        return found;
    }

    // AOT code is immutable. A store into a page it covers means the guest
    // rewrote translated code: stop using the whole module (fail closed to
    // the lazy JIT, which revalidates bytes) and say so loudly.
    void note_aot_smc() {
        if (!g_aot.loaded || g_aot.disabled)
            return;
        const uint32_t page = state.smc_page;
        if (page != std::numeric_limits<uint32_t>::max() && !g_aot.covers_page(page))
            return;
        g_aot.disabled = true;
        std::fprintf(stderr, "[vita3k-web] AOT disabled: guest store into AOT code page %08x (pc %08x)\n",
            page << 12, state.regs[15]);
    }
    // One VITA3K_AOT_DIFF sample: run the AOT call, then the lazy JIT from the
    // same JitState and memory for the same instruction count, compare, and
    // leave the AOT outcome in place so the run continues unchanged. Every
    // allocated page is snapshotted: unchecked AOT stores ignore permissions.
    uint32_t aot_differential(uint32_t granted) {
        auto &mem = *parent->mem;
        auto &stats = g_aot_diff;
        ++stats.samples;
        constexpr uint32_t page_bytes = 4096, pages_total = 1u << 20, chunk = 256;
        std::vector<uint32_t> pages;
        for (uint32_t first = 1; first < pages_total; first += chunk) {
            const uint32_t end = std::min(first + chunk, pages_total);
            if (mem.allocator.free_slot_count(first, end) == int(end - first))
                continue;
            for (uint32_t page = first; page < end; ++page)
                if (mem.allocator.free_slot_count(page, page + 1) == 0)
                    pages.push_back(page);
        }
        const auto host = [&](uint32_t page) { return mem_guest_to_host(mem, page * page_bytes); };
        std::vector<uint8_t> snapshot(size_t(pages.size()) * page_bytes);
        for (size_t i = 0; i < pages.size(); ++i)
            std::memcpy(&snapshot[i * page_bytes], host(pages[i]), page_bytes);
        stats.snapshot_bytes = snapshot.size();
        const auto changed = [&] {
            std::map<uint32_t, std::vector<uint8_t>> result;
            for (size_t i = 0; i < pages.size(); ++i)
                if (std::memcmp(&snapshot[i * page_bytes], host(pages[i]), page_bytes) != 0)
                    result.emplace(pages[i], std::vector<uint8_t>(host(pages[i]), host(pages[i]) + page_bytes));
            return result;
        };
        const auto restore = [&](const std::map<uint32_t, std::vector<uint8_t>> &written) {
            for (const auto &[page, bytes] : written) {
                const size_t i = std::lower_bound(pages.begin(), pages.end(), page) - pages.begin();
                std::memcpy(host(page), &snapshot[i * page_bytes], page_bytes);
            }
        };
        // The inline-mutex table is host state the generated code updates
        // together with the guest workarea: isolate it like guest memory.
        using MutexTable = vita3k::wasmjit::InlineMutexTable;
        auto *mutex_table = reinterpret_cast<MutexTable *>(state.mutex_table);
        std::optional<MutexTable> mutexes_before, mutexes_after_aot;
        if (mutex_table)
            mutexes_before = *mutex_table;
        const vita3k::wasmjit::JitState initial = state;
        const uint32_t svc0 = parent->svc;
        const bool svc_called0 = parent->svc_called;
        const uint64_t executed0 = executed;
        const uint32_t aot_reason = g_aot.entry(&state, granted);
        const vita3k::wasmjit::JitState after_aot = state;
        const uint32_t aot_ticks = counter_delta(initial.executed, after_aot.executed);
        const auto aot_pages = changed();
        restore(aot_pages);
        if (mutex_table) {
            mutexes_after_aot = *mutex_table;
            *mutex_table = *mutexes_before;
        }

        state = initial;
        diff_reference = true;
        const bool slice = scheduler_slice;
        scheduler_slice = true;
        execute_regions(aot_ticks);
        scheduler_slice = slice;
        diff_reference = false;
        const vita3k::wasmjit::JitState after_lazy = state;
        const uint32_t lazy_ticks = counter_delta(initial.executed, after_lazy.executed);
        const auto lazy_pages = changed();

        // Compare. fast_fp AOT images drop cumulative FPSCR flags and may
        // differ in NaN payloads (AOT.md); those differences are counted apart.
        const bool fast_fp = !std::getenv("VITA3K_AOT_EXACT_FP");
        std::vector<std::string> classes;
        if (aot_ticks != lazy_ticks) {
            ++stats.count_mismatch;
        } else {
            ++stats.compared;
            ++stats.per_thread[parent->thread_id];
            for (unsigned r = 0; r < 16; ++r)
                if (after_aot.regs[r] != after_lazy.regs[r])
                    classes.push_back(fmt::format("r{} aot={:08x} jit={:08x}", r, after_aot.regs[r], after_lazy.regs[r]));
            if (after_aot.cpsr != after_lazy.cpsr)
                classes.push_back(fmt::format("cpsr aot={:08x} jit={:08x}", after_aot.cpsr, after_lazy.cpsr));
            const uint32_t fpscr_mask = fast_fp ? ~0x9fu : ~0u;
            if ((after_aot.fpscr & fpscr_mask) != (after_lazy.fpscr & fpscr_mask))
                classes.push_back(fmt::format("fpscr aot={:08x} jit={:08x}", after_aot.fpscr, after_lazy.fpscr));
            else if (after_aot.fpscr != after_lazy.fpscr)
                ++stats.fpscr_flags_only;
            bool nan_only = false;
            for (unsigned w = 0; w < 64; ++w) {
                if (after_aot.fpu[w] == after_lazy.fpu[w])
                    continue;
                if (fast_fp && is_nan32(after_aot.fpu[w]) && is_nan32(after_lazy.fpu[w])) {
                    nan_only = true;
                    continue;
                }
                classes.push_back(fmt::format("fpu[{}] aot={:08x} jit={:08x}", w, after_aot.fpu[w], after_lazy.fpu[w]));
            }
            if (nan_only) ++stats.nan_payload_only;
            if (mutex_table && std::memcmp(mutex_table, &*mutexes_after_aot, sizeof(MutexTable)) != 0)
                classes.push_back("inline-mutex table");
            if (after_aot.exclusive_size != after_lazy.exclusive_size
                || (after_aot.exclusive_size && (after_aot.exclusive_address != after_lazy.exclusive_address
                    || after_aot.exclusive_value != after_lazy.exclusive_value)))
                classes.push_back("exclusive monitor");
            std::set<uint32_t> touched;
            for (const auto &[page, bytes] : aot_pages) touched.insert(page);
            for (const auto &[page, bytes] : lazy_pages) touched.insert(page);
            for (const uint32_t page : touched) {
                const auto a = aot_pages.find(page), l = lazy_pages.find(page);
                const size_t i = std::lower_bound(pages.begin(), pages.end(), page) - pages.begin();
                const uint8_t *aot_bytes = a != aot_pages.end() ? a->second.data() : &snapshot[i * page_bytes];
                const uint8_t *lazy_bytes = l != lazy_pages.end() ? l->second.data() : &snapshot[i * page_bytes];
                for (uint32_t b = 0; b < page_bytes; ++b) {
                    if (aot_bytes[b] != lazy_bytes[b]) {
                        classes.push_back(fmt::format("mem {:08x} aot={:02x} jit={:02x}", page * page_bytes + b, aot_bytes[b], lazy_bytes[b]));
                        break;
                    }
                }
            }
        }
        if (!classes.empty()) {
            ++stats.mismatches;
            for (const auto &c : classes) ++stats.classes[c.substr(0, c.find(' '))];
            if (stats.mismatches <= 20) {
                std::fprintf(stderr, "[aot-diff] MISMATCH thread=%d pc=%08x cpsr=%08x ticks=%u reason=%u:", parent->thread_id,
                    initial.regs[15], initial.cpsr, aot_ticks, aot_reason);
                for (const auto &c : classes) std::fprintf(stderr, " | %s", c.c_str());
                std::fprintf(stderr, "\n");
            }
        }
        if (stats.samples % 25 == 0)
            std::fprintf(stderr, "[aot-diff] samples=%llu compared=%llu count_mismatch=%llu mismatches=%llu fpscr_flags_only=%llu nan_payload_only=%llu snapshot_mb=%.0f\n",
                (unsigned long long)stats.samples, (unsigned long long)stats.compared, (unsigned long long)stats.count_mismatch,
                (unsigned long long)stats.mismatches, (unsigned long long)stats.fpscr_flags_only,
                (unsigned long long)stats.nan_payload_only, stats.snapshot_bytes / 1048576.0);
        if (stats.samples % 25 == 0) {
            std::fprintf(stderr, "[aot-diff] compared per thread:");
            for (const auto &[tid, count] : stats.per_thread) std::fprintf(stderr, " %d:%llu", tid, (unsigned long long)count);
            std::fprintf(stderr, "\n");
        }

        // Keep the AOT outcome: undo the lazy writes, reapply the AOT ones.
        restore(lazy_pages);
        for (const auto &[page, bytes] : aot_pages)
            std::memcpy(host(page), bytes.data(), page_bytes);
        state = after_aot;
        if (mutex_table)
            *mutex_table = *mutexes_after_aot;
        parent->svc = svc0;
        parent->svc_called = svc_called0;
        executed = executed0;
        return aot_reason;
    }

    int execute_regions(uint64_t remaining_budget) {
        ++host_entries;
        // HLE ran between this entry and the previous one (an SVC exit was
        // serviced, possibly with fiber suspension). Single-block SVC exits
        // share the same svc_exits tally, so this is a boundary marker, not
        // a per-import attribution.
        const bool hle_ran = svc_exits != last_svc_at_entry;
        if (hle_ran) {
            ++post_hle_entries;
            last_svc_at_entry = svc_exits;
        }
        // Both checks below hold in every memory mode: regions chain through
        // the shared hint map in direct and page-table memory alike.
        {
            // Hint ownership, independent of the sweep gate below. The hint
            // map/table are process-global: discard hints from any other
            // cooperatively scheduled CPU before this CPU chains through the
            // map, because another CPU's regions are neither in this CPU's
            // cache nor covered by its sweep. After a bump every reachable hint
            // is re-inserted by this CPU's loop top, which validates the region
            // bytes first; that is also what covers tracked code writes made by
            // other CPUs, since any such CPU entered (and bumped) in between.
            // Bump ONLY when an eviction happened since this CPU last synced:
            // an unconditional per-entry bump would stale this CPU's own live
            // entries, forcing one host miss per pump re-entry (measured 717
            // vs 71 on the display fixture). All eviction paths funnel through
            // dispatch_bump_epoch(), which advances the global version.
            // NOTE: the ack bump below advances the global version itself, so
            // two same-core CPUs alternating entries re-bump every switch
            // (version_bumps tracks this ping-pong; host_miss stays flat
            // until real eviction churn stales chained targets).
            ++version_syncs;
            if (dispatch_global_version != last_dispatch_version) {
                ++version_bumps;
                dispatch_bump_epoch();
                last_dispatch_version = dispatch_global_version;
            }
        }
        // Revalidate every cached region whose code a TRACKED path wrote since
        // this CPU's last sweep, on every entry: host code can change any
        // cached region while this CPU's hints stay live, and the writer need
        // not be this CPU's own HLE. Another guest thread's HLE (module
        // loading, a patch through mem_write) can run between two entries of
        // this CPU without an SVC of ours and without a pump entry of its own
        // to bump the dispatch version above, e.g. while it is suspended in
        // the call (backend test foreign_code_write). Guest writes by this CPU
        // take the checked path, set smc_dirty and drop the affected regions;
        // no host mutator runs inside the cooperative pump.
        {
            // Two-level filter, because the unconditional form was 14-16% of
            // wall clock and had never once found anything (entry_evicted was
            // 0 in every measured run):
            //   1. g_code_write_epoch only moves when a TRACKED path writes a
            //      page that currently carries compiled code (see
            //      note_code_page_write). Guest code writes take the smc path
            //      and drop the region outright; data writes are not code
            //      writes. Unchanged epoch => the whole sweep is provably
            //      redundant, so it costs one integer compare.
            //   2. When it does move, region_unchanged() re-reads only the
            //      pages whose own generation changed.
            if (g_code_write_epoch.load(std::memory_order_relaxed) != swept_code_epoch) {
                const auto revalidate_started = std::chrono::steady_clock::now();
                for (auto it = region_cache.begin(); it != region_cache.end();) {
                    ++entry_scanned;
                    if (region_unchanged(*it->second.region, *parent->mem)) {
                        ++it;
                        continue;
                    }
                    ++entry_evicted;
                    vita3k_jit_release_region(it->second.table_index);
                    mark_code_pages(*it->second.region, -1);
                    it = region_cache.erase(it);
                    ++invalidated;
                dispatch_bump_epoch();
                }
                swept_code_epoch = g_code_write_epoch.load(std::memory_order_relaxed);
                revalidate_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - revalidate_started)
                                     .count();
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
                return budget_exhausted();
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
            // AOT first: the module owns this PC unless its entry already
            // reported a PC/mode mismatch here (then the lazy path runs once).
            const bool via_aot = g_aot.loaded && !g_aot.disabled && aot_enabled && !diff_reference
                && pc != aot_skip_pc && g_aot.slot(pc) != 0;
            aot_skip_pc = 0xffffffffu;
            if (g_aot.loaded && !via_aot && std::getenv("VITA3K_AOT_TRACE_MISSES")) {
                static std::unordered_map<uint32_t, uint64_t> uncovered;
                const uint64_t hits = ++uncovered[pc];
                if (hits == 1 || hits == 1000 || hits == 100000)
                    std::fprintf(stderr, "[aot] uncovered pc=%08x cpsr=%08x hits=%llu lut=%08x\n", pc, state.cpsr,
                        static_cast<unsigned long long>(hits), g_aot.slot(pc));
            }
            const auto loc = Dynarmic::A32::LocationDescriptor{pc,
                Dynarmic::A32::PSR{state.cpsr}, Dynarmic::A32::FPSCR{state.fpscr}};
            const uint64_t key = loc.UniqueHash();
            auto found = via_aot ? region_cache.end() : region_cache.find(key);
            if (found != region_cache.end()) {
                ++select_checks;
                if (!region_unchanged(*found->second.region, *parent->mem)) {
                // Guest code changed under us (Ptr/HLE write without tracking,
                // or a store the smc bitmap missed). Drop and recompile.
                ++select_stale;
                vita3k_jit_release_region(found->second.table_index);
                mark_code_pages(*found->second.region, -1);
                region_cache.erase(found);
                ++invalidated;
                    dispatch_bump_epoch();
                found = region_cache.end();
                }
            }
            if (!via_aot && found == region_cache.end()) {
                // The frontend reports untranslatable guest code (fetch
                // faults, bad encodings) by throwing: only this cold path is
                // inside a try, so the hot path keeps direct calls.
                try {
                    RegionBuildStats build{};
                    found = ensure_region(pc, key, &build);
                } catch (const std::exception &e) {
                    return fail(e.what());
                }
                if (found == region_cache.end())
                    return -1; // ensure_region already reported via fail()/reject().
            }
            ++hits;
            if (!via_aot)
                found->second.last_used = ++region_clock;
            if (log_code && !via_aot)
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
            state.write_epochs_base = reinterpret_cast<uintptr_t>(mem_state->write_epochs.get());
            state.write_epoch = mem_state->write_epoch;
            state.smc_dirty = 0;
            state.smc_page = 0;
            // stop() owns an atomic request. Mirror it at entry and return
            // to the atomic host check at least every REGION_CALL_TICKS.
            // Event-loop yielding remains the worker scheduler's job.
            state.stop_flag = stopped.load() ? 1u : 0u;
            state.exit_reason = 0;
            state.svc = 0;
            state.guest_thread_id = static_cast<uint32_t>(parent->thread_id);
            // M16 pump: publish the entry mapping, then let the Wasm-side
            // dispatcher chain compiled-region transfers without host exits.
            // regs[15] is the transfer target after every Miss/Budget/Smc
            // return (all exits publish it), so the loop-top resolve below
            // serves both fresh entries and post-dispatcher Miss targets.
            if (!via_aot && (found->second.table_index < 0
                || found->second.table_index >= static_cast<int>(vita3k::wasmjit::kDispatchTableLimit)))
                return fail("region slot outside dispatch table");
            if (!via_aot && !dispatch_map_insert(core, key, static_cast<uint32_t>(found->second.table_index)))
                return fail("region map full");
            const uintptr_t map_base = dispatch_map_base(core);
            if (map_base == 0)
                return fail("dispatch map allocation failed");
            const uintptr_t state_offset = reinterpret_cast<uintptr_t>(&state);
            // AOT transfers compare the unspent budget as a signed i32 (a
            // function may overrun by its forward path), so keep it positive.
            const uint32_t granted = static_cast<uint32_t>(std::min<uint64_t>(remaining_budget,
                via_aot ? (1u << 30) : UINT32_MAX));
            const uint32_t executed_before = state.executed;
            const uint32_t dispatches_before = state.dispatches;
            const uint32_t tx_before = state.tx_wasm;
            // Wall time in generated code costs two JS calls per entry: only
            // when requested (VITA3K_JIT_TIMING, progress guest_ms).
            const double t2 = jit_timing() ? emscripten_get_now() : 0.0;
            const bool diff_sample = via_aot && aot_diff_every()
                && (!aot_diff_thread() || aot_diff_thread() == parent->thread_id)
                && emscripten_get_now() >= aot_diff_after_ms()
                && ++g_aot_diff.calls % aot_diff_every() == 0;
            const uint32_t reason = diff_sample ? aot_differential(granted)
                : via_aot ? g_aot.entry(&state, granted)
                : vita3k_jit_run_dispatch(state_offset, granted, map_base, dispatch_epoch_addr());
            aot_calls += via_aot;
            if (jit_timing())
                run_js_ms += emscripten_get_now() - t2;
            ++js_calls;
            account_counters();
            dispatches += counter_delta(dispatches_before, state.dispatches);
            tx_wasm += counter_delta(tx_before, state.tx_wasm);
            const uint32_t delta = counter_delta(executed_before, state.executed);
            // AOT checks the budget only at transfers and backward edges, so a
            // call may overrun by at most one function's forward path.
            if (delta > granted && !via_aot)
                return fail("generated region overran its budget");
            executed += delta;
            remaining_budget -= std::min<uint64_t>(delta, remaining_budget);
            // Light-path chained edges can leave the loop (Budget/Stop/Miss/
            // Svc/Fault) without a loop-top smc poll after a code-page store
            // (REGION_ABI.md v1.2): the edge's budget check may exit first.
            // Normalize any pending smc_dirty on EVERY non-Smc exit so
            // "SMC wins over the dispatch budget check" holds everywhere.
            if (state.smc_dirty)
                note_aot_smc();
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
                if (std::getenv("VITA3K_WASMJIT_FAULT_TRACE")) {
                    std::fprintf(stderr, "WasmJitCPU fault trace: %s\n", message);
                    for (unsigned r = 0; r < 15; ++r)
                        std::fprintf(stderr, "  r%-2u=%08x%s", r, state.regs[r], (r % 4 == 3) ? "\n" : "  ");
                    std::fprintf(stderr, "r15=%08x cpsr=%08x\n", state.regs[15], state.cpsr);
                    std::fprintf(stderr, "fpu:");
                    for (unsigned w = 0; w < 8; ++w)
                        std::fprintf(stderr, " %08x", state.fpu[w]);
                    std::fprintf(stderr, "\n");
                    if (state.fault_pc >= 0x81000000 && state.fault_pc < 0x812D96CCu) {
                        const uint32_t word = *reinterpret_cast<const uint32_t *>(
                            mem_guest_to_host(*parent->mem, state.fault_pc & ~1u));
                        std::fprintf(stderr, "faulting instruction word @%08x = %08x\n", state.fault_pc, word);
                    }
                    std::fflush(stderr);
                }
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
                    return budget_exhausted();
                // No forward progress: the next block cannot fit the remaining
                // budget slice. Report a clean slice boundary so the caller
                // can re-grant; not an error, and not a livelock.
                if (state.executed == budget_progress_mark)
                    return scheduler_slice ? WasmJitCPU::slice_yield : 0;
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
            case ExitReason::Exception:
                return exception_exit();
            case ExitReason::EntryMiss:
                // An AOT function does not own next_pc in the current mode.
                // regs[15] still equals next_pc; resolve it lazily once.
                ++aot_entry_misses;
                if (std::getenv("VITA3K_AOT_TRACE_MISSES") && aot_entry_misses <= 40)
                    std::fprintf(stderr, "[aot] entry miss pc=%08x cpsr=%08x fpscr=%08x lut=%08x\n",
                        state.next_pc, state.cpsr, state.fpscr, g_aot.slot(state.next_pc));
                aot_skip_pc = state.next_pc;
                state.regs[15] = state.next_pc;
                continue;
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
                // Inline mutex fast path: flag hot-stub blocks (regs[15] is
                // the block entry PC here, like block.pc in form_region).
                const uint32_t single_hot_nid = hot_stub_nid(*parent->mem, state.regs[15], state.cpsr);
                auto bytes = vita3k::wasmjit::emit_block(ir, single_hot_nid);
                if (bytes.empty() && translation_limit > 1) {
                    // Memory IR is initially safe only in a single guest
                    // instruction: precise CPU rollback and SMC revalidation
                    // must happen before entering the next guest instruction.
                    translation_limit = 1;
                    ir = vita3k::wasmjit::translate_block(*parent->mem,
                        state.regs[15], state.cpsr, translation_limit, state.fpscr);
                    bytes = vita3k::wasmjit::emit_block(ir, single_hot_nid);
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
                Block block{state.regs[15], std::vector<uint8_t>(single_hot_nid ? 12 : end - start), -1, translation_limit};
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
            state.guest_thread_id = static_cast<uint32_t>(parent->thread_id);
            // M15 fast-path bases, exactly as in region mode above.
            const auto *mem_state = parent->mem;
            state.page_table_base = mem_state->sparse_host_memory
                ? reinterpret_cast<uintptr_t>(mem_state->page_table.get()) : 0;
            state.page_perms_base = reinterpret_cast<uintptr_t>(mem_state->page_permissions.get());
            state.code_pages_base = reinterpret_cast<uintptr_t>(g_code_pages.data());
            state.write_epochs_base = reinterpret_cast<uintptr_t>(mem_state->write_epochs.get());
            state.write_epoch = mem_state->write_epoch;
            // Fast-path tallies are per-call scratch (REGION_ABI.md): zero
            // them before the snapshot so the fault rollback below cannot
            // resurrect stale counts.
            state.mem_fast_reads = 0;
            state.mem_fast_writes = 0;
            const State before = state;
            // Host entry currently uses the EM_JS trampoline below.
            // Guest faults use return reasons; checked helpers are Wasm imports.
            const uintptr_t state_offset = reinterpret_cast<uintptr_t>(&state);
            // Wall time in generated code costs two JS calls per entry: only
            // when requested (VITA3K_JIT_TIMING, progress guest_ms).
            const double t2 = jit_timing() ? emscripten_get_now() : 0.0;
            const uint32_t reason = vita3k_jit_call(found->second.table_index, state_offset);
            if (jit_timing())
                run_js_ms += emscripten_get_now() - t2;
            ++js_calls;
            account_counters();
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
                if (std::getenv("VITA3K_WASMJIT_FAULT_TRACE")) {
                    // Rollback overwrote registers; print the pre-rollback snapshot.
                    std::fprintf(stderr, "WasmJitCPU fault trace: %s (pc %08x)\n", message, before.regs[15]);
                    for (unsigned r = 0; r < 15; ++r)
                        std::fprintf(stderr, "  r%-2u=%08x%s", r, before.regs[r], (r % 4 == 3) ? "\n" : "  ");
                    std::fprintf(stderr, "r15=%08x cpsr=%08x\n", before.regs[15], before.cpsr);
                    std::fflush(stderr);
                }
                return fail(message);
            }
            if (reason == static_cast<uint32_t>(vita3k::wasmjit::ExitReason::Exception))
                return exception_exit();
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
uint64_t WasmJitCPU::aot_location(uint32_t address) {
    const bool thumb = address & 1;
    return Dynarmic::A32::LocationDescriptor{address & ~1u, Dynarmic::A32::PSR{thumb ? 0x20u : 0u},
        Dynarmic::A32::FPSCR{0}}.UniqueHash();
}

// The linker ends .ARM.exidx with an EXIDX_CANTUNWIND entry at the first
// byte past the code it covers, so the last function has an end (entries are
// sorted by function start; each function extends to the next entry). Past
// it, the text segment holds read-only data: module info, export tables,
// strings. Scanning that as code turns data into roots and junk blocks.
// Tables without the sentinel (they end with an import stub or a function
// with unwind data) give no end, but the module info the linker places after
// the code still does, as long as every function in the table starts below
// it; in every Limbo module with a sentinel, the sentinel is the module info.
uint32_t WasmJitCPU::aot_code_size(MemState &mem, uint32_t text, uint32_t size, uint32_t exidx_begin, uint32_t exidx_end,
    uint32_t module_info) {
    constexpr uint32_t exidx_cantunwind = 1;
    if (exidx_end < exidx_begin || exidx_end - exidx_begin < 16 || (exidx_end - exidx_begin) % 8)
        return size;
    std::array<uint32_t, 4> last{}; // the last two entries
    if (!mem_read(mem, exidx_end - 16, last.data(), sizeof(last)) || (last[0] | last[2]) & 0x80000000u)
        return size;
    const auto target = [](uint32_t entry, uint32_t word) {
        return entry + static_cast<uint32_t>(static_cast<int32_t>(word << 1) >> 1);
    };
    const uint32_t previous = target(exidx_end - 16, last[0]) & ~1u;
    const uint32_t end = target(exidx_end - 8, last[2]);
    uint32_t code = size;
    if (last[3] == exidx_cantunwind && !(end & 1) && previous < end && previous - text < size && end - text <= size)
        code = end - text;
    // The highest function start: the sentinel's predecessor, or the last entry.
    const uint32_t highest = code != size ? previous : end & ~1u;
    if (highest - text < size && module_info - text < code && highest < module_info)
        code = module_info - text;
    return code;
}

// Each 8-byte entry starts with a prel31 offset to a function. Its bit 0 is
// the Thumb bit of the symbol the linker resolved, and SCE modules have Thumb
// functions whose symbol lacks it (SceLibc 8035b520, SceLibHttp 803bf198:
// hand-written routines that relocations and executed code enter as Thumb).
// Taken as ARM they decode Thumb halfword pairs into junk blocks. No
// function starts with a conditional instruction (flags are undefined on
// entry), so an ARM start whose condition field is neither AL nor the
// unconditional space is Thumb code, as is a start that is not word-aligned.
std::vector<uint32_t> WasmJitCPU::aot_exidx_functions(MemState &mem, uint32_t exidx_begin, uint32_t exidx_end) {
    std::vector<uint32_t> functions;
    if (exidx_end < exidx_begin || exidx_end - exidx_begin >= (64u << 20))
        return functions;
    for (uint32_t entry = exidx_begin; exidx_end - entry >= 8; entry += 8) {
        uint32_t word = 0;
        if (!mem_read(mem, entry, &word, sizeof(word)) || (word & 0x80000000u))
            continue;
        uint32_t function = entry + static_cast<uint32_t>(static_cast<int32_t>(word << 1) >> 1);
        uint32_t first = 0;
        if (!(function & 1)
            && ((function & 3) || (mem_read(mem, function, &first, sizeof(first)) && (first >> 28) < 0xe)))
            function |= 1;
        functions.push_back(function);
    }
    return functions;
}

bool WasmJitCPU::build_aot(MemState &mem, const AotBuildSpec &spec, std::vector<uint8_t> &out, std::string &report) {
    if (vita3k::wasmjit::ablation_removes_budget_checks()) {
        report = "VITA3K_ABLATE with C (or F/G) removes the budget checks that bound AOT loops; "
                 "an image built under it can hang, so build it without the ablation";
        return false;
    }
    auto options = vita3k::wasmjit::region_state_options();
    // Same predicate as each CPU's lazy regions: the fast-path metadata arrays
    // live for the whole MemState lifetime.
    options.assume_fast_bases = mem.page_permissions != nullptr
        && (mem.direct_host_memory || (mem.sparse_host_memory && mem.page_table != nullptr));
    // Plain guest-window accesses: AOT text is read-execute, so its own
    // stores cannot rewrite translated code. VITA3K_AOT_CHECKED_MEMORY=1
    // keeps the fault-reporting probes (diagnosis of guest faults).
    options.unchecked_memory = mem.direct_host_memory && !std::getenv("VITA3K_AOT_CHECKED_MEMORY");
    // Native Wasm FP arithmetic without cumulative FPSCR flags
    // (RegionStateOptions::fast_fp); VITA3K_AOT_EXACT_FP=1 keeps exact flags.
    options.fast_fp = !std::getenv("VITA3K_AOT_EXACT_FP");
    AotBuilder builder(mem, spec, options);
    return builder.build(out, report);
}

int WasmJitCPU::load_aot(MemState &mem, std::string &report) {
    if (g_aot.loaded) {
        report = "already loaded";
        return 1;
    }
    const uint32_t size = vita3k_aot_metadata_size();
    if (!size) {
        report = "no AOT module supplied";
        return 0;
    }
    std::vector<uint8_t> metadata(size);
    vita3k_aot_metadata_copy(metadata.data());
    size_t cursor = 0;
    const auto word = [&](uint32_t &value) {
        if (cursor + 4 > metadata.size())
            return false;
        value = metadata[cursor] | (metadata[cursor + 1] << 8) | (metadata[cursor + 2] << 16)
            | (uint32_t(metadata[cursor + 3]) << 24);
        cursor += 4;
        return true;
    };
    uint32_t magic = 0, version = 0, range_count = 0;
    if (!word(magic) || !word(version) || !word(range_count) || magic != aot_magic || version != aot_version) {
        report = "AOT metadata has the wrong magic/version";
        return -1;
    }
    AotRuntime runtime;
    uint64_t lut_words = 0;
    for (uint32_t i = 0; i < range_count; ++i) {
        uint32_t base = 0, bytes = 0, hash = 0;
        if (!word(base) || !word(bytes) || !word(hash)) {
            report = "truncated AOT range table";
            return -1;
        }
        std::vector<uint8_t> code(bytes);
        if (!mem_read(mem, base, code.data(), code.size()) || aot_hash(code.data(), code.size()) != hash) {
            char line[160];
            std::snprintf(line, sizeof(line), "guest code [%08x,+%x) differs from the AOT build; AOT not used",
                base, bytes);
            report = line;
            return -1;
        }
        runtime.ranges.push_back({base, bytes});
        runtime.offsets.push_back(lut_words);
        lut_words += bytes / 2;
    }
    runtime.lut.assign(lut_words, 0);
    uint32_t pairs = 0;
    if (!word(pairs)) {
        report = "truncated AOT entry table";
        return -1;
    }
    for (uint32_t i = 0; i < pairs; ++i) {
        uint32_t pc = 0, slot = 0;
        if (!word(pc) || !word(slot)) {
            report = "truncated AOT entry table";
            return -1;
        }
        bool placed = false;
        for (size_t k = 0; k < runtime.ranges.size(); ++k) {
            const uint32_t offset = pc - runtime.ranges[k].base;
            if (offset < runtime.ranges[k].size && !(offset & 1)) {
                runtime.lut[runtime.offsets[k] + offset / 2] = slot;
                placed = true;
                break;
            }
        }
        if (!placed || !slot) {
            report = "AOT entry outside its code ranges";
            return -1;
        }
    }
    uint32_t function_count = 0;
    if (!word(function_count) || function_count > vita3k::wasmjit::kAotMaxFunctions) {
        report = "truncated AOT function table";
        return -1;
    }
    runtime.spans.resize(function_count);
    for (auto &[begin, end] : runtime.spans) {
        if (!word(begin) || !word(end) || end < begin) {
            report = "truncated AOT function table";
            return -1;
        }
    }
    g_aot = std::move(runtime);
    const int entry_slot = vita3k_aot_instantiate(g_aot.lut.data(), checked_memory_read, checked_memory_write, fp64_helper);
    if (entry_slot < 0) {
        g_aot = AotRuntime{};
        report = "AOT module instantiation failed";
        return -1;
    }
    g_aot.entry = reinterpret_cast<AotEntry>(static_cast<uintptr_t>(entry_slot));
    // Stores into translated code must take the checked path so smc_dirty
    // reports them (the loaded module never revalidates its bytes).
    for (const auto &range : g_aot.ranges)
        for (uint32_t page = range.base >> 12; page <= (range.base + range.size - 1) >> 12; ++page)
            ++g_code_pages[page];
    g_aot.loaded = true;
    char line[160];
    std::snprintf(line, sizeof(line), "AOT loaded: %u ranges, %u entries, %llu table words",
        range_count, pairs, static_cast<unsigned long long>(lut_words));
    report = line;
    return 1;
}

bool WasmJitCPU::dump_aot_seeds(const char *path) {
    // Accumulate: seeds recorded by earlier runs (possibly with an AOT module
    // loaded, when only uncovered code reaches the lazy JIT) are kept.
    std::unordered_set<uint64_t> all(g_aot_seed_keys.begin(), g_aot_seed_keys.end());
    if (FILE *previous = std::fopen(path, "r")) {
        unsigned long long value = 0;
        while (std::fscanf(previous, "%llx", &value) == 1)
            all.insert(value);
        std::fclose(previous);
    }
    std::vector<uint64_t> keys(all.begin(), all.end());
    std::sort(keys.begin(), keys.end());
    FILE *out = std::fopen(path, "w");
    if (!out)
        return false;
    for (const uint64_t key : keys)
        std::fprintf(out, "%016llx\n", static_cast<unsigned long long>(key));
    return std::fclose(out) == 0;
}

int WasmJitCPU::run() {
    impl->stopped = false;
    impl->parent->svc_called = false;
    impl->error.clear();
    const auto start = impl->executed;
    if (impl->region_mode)
        return impl->execute_regions(impl->budget);
    while (impl->executed - start < impl->budget) {
        if (impl->stopped || impl->breakpoint) return 1;
        const uint32_t limit = std::min<uint64_t>(32, impl->budget - (impl->executed - start));
        const int result = impl->execute(limit);
        if (result || impl->parent->svc_called) return result;
    }
    return impl->budget_exhausted();
}
int WasmJitCPU::run_slice(uint64_t instructions) {
    const auto previous = impl->budget;
    const auto previous_slice = impl->scheduler_slice;
    impl->budget = instructions;
    impl->scheduler_slice = true;
    const int result = run();
    impl->budget = previous;
    impl->scheduler_slice = previous_slice;
    return result;
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
uint32_t WasmJitCPU::get_pc() noexcept { return impl->state.regs[15]; }
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
            // Sanctioned host-side "this code changed" notification: bump the
            // covered pages' generations so the version filter cannot let a
            // stale region survive, even though these entries are dropped.
            note_code_page_write(std::max<uint32_t>(r.page_begin, first_page),
                std::min<uint32_t>(r.page_end, end_page) - 1);
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
    retire_aot(start, length);
}
// AOT functions overlapping the changed code become unreachable: every
// transfer into a function goes through the lookup table, so clearing the
// function's entries retires it (the lazy JIT then retranslates). Process-wide,
// so it also applies before any CPU exists (title patches at launch).
void WasmJitCPU::retire_aot(Address start, size_t length) {
    if (!length) return;
    const uint64_t end = uint64_t(start) + std::min<uint64_t>(length, 0x100000000ULL - start);
    if (g_aot.loaded) {
        for (uint32_t slot = 0; slot < g_aot.spans.size(); ++slot) {
            const auto [begin, span_end] = g_aot.spans[slot];
            if (begin >= end || span_end <= start)
                continue;
            bool cleared = false;
            for (uint32_t pc = begin; pc < span_end; pc += 2) {
                for (size_t k = 0; k < g_aot.ranges.size(); ++k) {
                    const uint32_t offset = pc - g_aot.ranges[k].base;
                    if (offset >= g_aot.ranges[k].size)
                        continue;
                    uint32_t &entry = g_aot.lut[g_aot.offsets[k] + offset / 2];
                    if (entry && (entry & vita3k::wasmjit::kAotMaxFunctions) == slot + 1) {
                        entry = 0;
                        cleared = true;
                    }
                    break;
                }
            }
            if (cleared)
                ++g_aot.invalidated_functions;
        }
    }
}
void WasmJitCPU::release_code_caches() {
    for (const auto &[key, block] : impl->cache)
        vita3k_jit_release(block.table_index);
    impl->invalidated += impl->cache.size();
    impl->cache.clear();
    impl->clear_regions();
}
bool WasmJitCPU::is_thumb_mode() { return impl->state.cpsr & 0x20; }
bool WasmJitCPU::hit_breakpoint() noexcept { return impl->breakpoint; }
void WasmJitCPU::trigger_breakpoint() { impl->breakpoint = true; stop(); }
void WasmJitCPU::set_log_code(bool v) { impl->log_code = v; }
void WasmJitCPU::set_log_mem(bool v) { impl->log_mem = v; }
bool WasmJitCPU::get_log_code() { return impl->log_code; }
bool WasmJitCPU::get_log_mem() { return impl->log_mem; }
void WasmJitCPU::clear_exclusive() noexcept {
    // A reservation is only valid within the thread that took it: the kernel
    // calls this on a guest context switch. Ordinary stores deliberately do
    // not clear it (validity is decided by re-reading memory at STREX).
    impl->state.exclusive_size = 0;
}
std::size_t WasmJitCPU::processor_id() const { return impl->core; }
void WasmJitCPU::set_instruction_budget(uint64_t v) { impl->budget = v; }
void WasmJitCPU::set_region_mode(bool v) { impl->region_mode = v; }
bool WasmJitCPU::inline_mutex_fast_paths_enabled() { return inline_mutex_enabled(); }
void WasmJitCPU::set_inline_mutex_table(vita3k::wasmjit::InlineMutexTable *table) noexcept {
    impl->state.mutex_table = inline_mutex_enabled() ? reinterpret_cast<uintptr_t>(table) : 0;
}
const std::string &WasmJitCPU::get_last_error() const { return impl->error; }
uint32_t WasmJitCPU::get_fault_address() const { return impl->state.fault_address; }
bool WasmJitCPU::get_fault_write() const { return impl->state.fault_write != 0; }
uint64_t WasmJitCPU::instructions_executed() const { return impl->executed; }
double WasmJitCPU::run_ms() const { return impl->run_js_ms; }
void WasmJitCPU::set_aot_enabled(bool enabled) { impl->aot_enabled = enabled; }
void WasmJitCPU::disable_aot() { g_aot.disabled = true; }
uint64_t WasmJitCPU::compiled_blocks() const { return impl->compiled; }
uint64_t WasmJitCPU::regions_formed() const { return impl->regions; }
// AOT-1: install the entry closure before first execution. Mirrors the
// execute_regions() entry setup (dispatch install + map publish) and shares
// its ensure_region() form/emit/install path, but executes no guest code.
WasmJitCPU::PrecompileResult WasmJitCPU::precompile_region() {
    PrecompileResult out{};
    out.entry_pc = impl->state.regs[15];
    const auto loc = Dynarmic::A32::LocationDescriptor{impl->state.regs[15],
        Dynarmic::A32::PSR{impl->state.cpsr}, Dynarmic::A32::FPSCR{impl->state.fpscr}};
    const uint64_t key = loc.UniqueHash();
    if (!impl->dispatch_installed) {
        vita3k_jit_table_ensure();
        const auto dbytes = vita3k::wasmjit::emit_dispatch();
        if (dbytes.empty()) {
            out.error = "dispatch emission rejected";
            return out;
        }
        if (vita3k_jit_install_dispatch(dbytes.data(), dbytes.size()) < 0) {
            out.error = "browser rejected dispatch Wasm";
            return out;
        }
        impl->dispatch_installed = true;
    }
    const auto cached = impl->region_cache.find(key);
    if (cached != impl->region_cache.end()) {
        if (!dispatch_map_insert(impl->core, key,
                static_cast<uint32_t>(cached->second.table_index))) {
            out.error = "region map full";
            return out;
        }
        out.ok = true;
        out.cache_hit = true;
        out.blocks = cached->second.region->blocks.size();
        out.ticks = cached->second.region->total_ticks;
        return out;
    }
    Impl::RegionBuildStats build{};
    const auto found = impl->ensure_region(impl->state.regs[15], key, &build);
    if (found == impl->region_cache.end()) {
        out.error = impl->error; // ensure_region already reported loudly.
        return out;
    }
    if (!dispatch_map_insert(impl->core, key,
            static_cast<uint32_t>(found->second.table_index))) {
        out.error = "region map full";
        return out;
    }
    out.ok = true;
    out.blocks = build.blocks;
    out.ticks = build.ticks;
    out.wasm_bytes = build.wasm_bytes;
    out.emit_ms = build.emit_ms;
    out.install_ms = build.install_ms;
    return out;
}
uint64_t WasmJitCPU::cache_hits() const { return impl->hits; }
uint64_t WasmJitCPU::invalidated_blocks() const { return impl->invalidated; }
std::string WasmJitCPU::get_profile() const {
    char buffer[1344];
    std::snprintf(buffer, sizeof(buffer),
        "emit_ms=%.1f install_ms=%.1f run_js_ms=%.1f js_calls=%llu misses=%llu "
        "svc_exits=%llu blocks=%llu mem_reads=%llu mem_writes=%llu "
        "regions=%llu region_misses=%llu smc_exits=%llu budget_exits=%llu dispatches=%llu "
        "fast_reads=%llu fast_writes=%llu slow_unmapped=%llu slow_perms=%llu "
        "slow_cross=%llu slow_code=%llu slow_other=%llu "
        "tx_wasm=%llu host_miss=%llu promote_flags=%u promote_accounting=%u "
        "mutex_take=%llu mutex_release=%llu mutex_fallback=%llu "
        "host_entries=%llu post_hle_entries=%llu version_syncs=%llu version_bumps=%llu "
        "entry_scanned=%llu entry_evicted=%llu select_checks=%llu select_stale=%llu "
        "capacity_evictions=%llu revalidate_ms=%.1f cache_limit=%zu "
        "aot=%d aot_calls=%llu aot_entry_misses=%llu aot_invalidated=%llu",
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
        unsigned(impl->region_options.promote_flags), unsigned(impl->region_options.promote_accounting),
        (unsigned long long)impl->mutex_fast_take, (unsigned long long)impl->mutex_fast_release,
        (unsigned long long)impl->mutex_fast_fallback,
        (unsigned long long)impl->host_entries, (unsigned long long)impl->post_hle_entries,
        (unsigned long long)impl->version_syncs, (unsigned long long)impl->version_bumps,
        (unsigned long long)impl->entry_scanned, (unsigned long long)impl->entry_evicted,
        (unsigned long long)impl->select_checks, (unsigned long long)impl->select_stale,
        (unsigned long long)impl->capacity_evictions, impl->revalidate_ms,
        region_cache_limit(),
        g_aot.loaded ? (g_aot.disabled ? -1 : 1) : 0,
        (unsigned long long)impl->aot_calls, (unsigned long long)impl->aot_entry_misses,
        (unsigned long long)g_aot.invalidated_functions);
    return buffer;
}

WasmJitCPU::PumpCounters WasmJitCPU::pump_counters() const {
    PumpCounters out{};
    out.host_entries = impl->host_entries;
    out.post_hle_entries = impl->post_hle_entries;
    out.version_syncs = impl->version_syncs;
    out.version_bumps = impl->version_bumps;
    out.entry_scanned = impl->entry_scanned;
    out.entry_evicted = impl->entry_evicted;
    out.select_checks = impl->select_checks;
    out.select_stale = impl->select_stale;
    out.capacity_evictions = impl->capacity_evictions;
    out.js_calls = impl->js_calls;
    out.host_miss = impl->host_miss;
    out.tx_wasm = impl->tx_wasm;
    return out;
}
