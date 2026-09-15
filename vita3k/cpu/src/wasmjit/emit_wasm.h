// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "block_metadata.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace Dynarmic::IR {
class Block;
}

namespace vita3k::wasmjit {

enum class ExitReason : uint32_t {
    Continue = 0,
    Svc = 1,
    Fault = 2,
    Unsupported = 3,
    // Region-mode exits (REGION_ABI.md): the module returns to the host so
    // it can dispatch/compile, account budget, or handle self-modifying code.
    Miss = 4,   // next_pc set; host looks up/forms another region
    Budget = 5, // next_pc set; per-call tick budget exhausted
    Smc = 6,    // store into a cached code page observed (smc_dirty)
    Stop = 7,   // host requested stop via stop_flag
};

// Shared with generated Wasm, not Dynarmic's native backend JitState.
struct JitState {
    uint32_t regs[16];
    uint32_t cpsr;
    uint32_t fpscr;
    uint32_t svc;
    uint32_t exit_reason;
    uint32_t executed;
    uint32_t memory_cookie;
    uint32_t fault_address;
    uint32_t fault_write;
    uint32_t memory_value[4];
    uint32_t fpu[64]; // S0..S31 alias D0..D15; D16..D31 follow (little-endian words)
    uint32_t tpidruro;
    // M14c region ABI appends (REGION_ABI.md; offsets are contractual).
    uint32_t next_pc;         // +372 resume PC for Miss/Budget/Smc/Stop
    uint32_t fault_pc;        // +376 guest PC of the faulting instruction
    uint32_t page_table_base; // +380 host offset of page_table entries, 0=off
    uint32_t page_perms_base; // +384 host offset of page permission bytes, 0=off
    uint32_t smc_dirty;       // +388 set by checked writes into code pages
    uint32_t stop_flag;       // +392 host sets 1 to request a return
    uint32_t dispatches;      // +396 region dispatch-loop iterations (profiling)
    // M15 memory fast-path appends (REGION_ABI.md; offsets are contractual).
    // Host offsets into linear memory; 0 disables the fast path entirely.
    uint32_t code_pages_base; // +400 host offset of code-page refcounts, 0=off
    // Per-call fast-path tallies (generated Wasm increments; host accumulates
    // into process counters and zeroes before each call). Fallback reasons are
    // NOT state fields: the emitter encodes the reason in the high byte of the
    // helper's `bytes` argument and the helper accounts it.
    uint32_t mem_fast_reads;  // +404 inline fast-path reads this call
    uint32_t mem_fast_writes; // +408 inline fast-path writes this call
    uint32_t smc_page;        // +412 code page that triggered smc_dirty
    // M16 Wasm-side dispatch pump (REGION_ABI.md): per-dispatcher-call tally
    // of region->region transfers completed inside Wasm. The host accumulates
    // it into process counters the same way it does `dispatches`.
    uint32_t tx_wasm;         // +416 in-Wasm cached-region transfers
};
static_assert(std::is_standard_layout_v<JitState>);
static_assert(offsetof(JitState, memory_cookie) == 84);
static_assert(offsetof(JitState, memory_value) == 96);
static_assert(offsetof(JitState, fpu) == 112);
static_assert(offsetof(JitState, tpidruro) == 368);
static_assert(offsetof(JitState, next_pc) == 372);
static_assert(offsetof(JitState, fault_pc) == 376);
static_assert(offsetof(JitState, page_table_base) == 380);
static_assert(offsetof(JitState, page_perms_base) == 384);
static_assert(offsetof(JitState, smc_dirty) == 388);
static_assert(offsetof(JitState, stop_flag) == 392);
static_assert(offsetof(JitState, dispatches) == 396);
static_assert(offsetof(JitState, code_pages_base) == 400);
static_assert(offsetof(JitState, mem_fast_reads) == 404);
static_assert(offsetof(JitState, mem_fast_writes) == 408);
static_assert(offsetof(JitState, smc_page) == 412);
static_assert(offsetof(JitState, tx_wasm) == 416);
static_assert(sizeof(JitState) == 420);

// Emits an MVP Wasm module importing env.memory (unshared, min 1 page)
// and env.mem_read/env.mem_write: (stateOffset i32, address i32, bytes i32)->i32.
// Helpers are noexcept native Wasm functions: 0=success, 2=fault. Read fills
// memory_value in little-endian order; write consumes it. Only the low `bytes`
// bytes are significant. On failure helpers set fault_address/fault_write.
// The module exports block: (i32 stateOffset) -> i32 reason; it can be put
// directly in an Emscripten function table (signature "ii"). stateOffset must
// address a live JitState in that memory; out-of-bounds accesses trap.
//
// EMPTY vector means unsupported IR/terminal/location or exceeded limits; no
// partial module is returned. No guest instructions execute during emission.
// Supported modules return after ONE block (no chaining). Each invocation
// overwrites executed, exit_reason and svc; regs[15] is the next guest PC.
// Svc returns AFTER the frontend's post-SVC PC write, leaving handling to host.
// Memory IR is accepted ONLY for CycleCount()==1, so the dispatcher must retry
// unsupported multi-instruction blocks with a single-instruction translation.
// A failing memory helper returns Fault with executed=0 and the instruction PC.
// Parent MUST snapshot/restore architectural regs/CPSR/FPU on Fault (not the
// fault fields). Earlier stores of a multi-access instruction may have completed.
//
// M15 inline memory fast path: when page_table_base, page_perms_base and
// code_pages_base are all populated (any 0=off), 1/2/4-byte A32 memory IR lowers
// INLINE instead of calling the checked helpers: page-table lookup +
// permission/refcount probe + direct Wasm load/store against the sparse page
// backing. The fast path is taken only
// when it is provably equivalent to the checked path: fast-path disabled,
// guest page 0 (the checked path rejects addr < host_page_size even when a
// sparse backing was force-allocated there), page-crossing access, unmapped
// page, missing Read/Write permission and stores into code-tracked pages all
// fall back to the imported checked helper with identical fault semantics.
// Fallback reasons are encoded in the high byte of the helper's
// `bytes` argument (1=unmapped, 2=perms, 3=cross-page, 4=code page, 5=other);
// the helper masks them off before use. Fast successes increment
// mem_fast_reads/mem_fast_writes in JitState. Alignment is never checked:
// Wasm unaligned access is a little-endian byte-wise access, exactly the
// semantics of mem_read/mem_write copies; narrow loads are zero-extending,
// matching every A32/ReadMemoryN producer (sign extension is separate IR).
// No guest data address is ever used as a host offset OUTSIDE this probe: the
// generated code never dereferences a page-table entry without validating the
// mapped page first.
//
// Frontend MUST use one tick per guest instruction: CycleCount and
// ConditionFailedCycleCount become executed. Caller must translate at most its
// remaining instruction budget and dispatch/check budget between invocations.
// There is no mid-block budget check. Cache keys must include the A32 location
// descriptor's mode bits; entry state must match it. IT is advanced from the
// frontend descriptors on both successful and condition-failed paths.
// Unknown operations fail closed, including FP arithmetic and exclusive memory.
// Limits: 4096 IR instructions, 4096 guest ticks, terminal depth 16 / 256 nodes.
std::vector<uint8_t> emit_block(const Dynarmic::IR::Block &block);

// M14c region emission (REGION_ABI.md). One WebAssembly.Module per REGION:
// many guest basic blocks with an in-module dispatch loop, so hot loops never
// return to the host. The module exports run(state:i32, budget:i32)->i32 and
// exits only for Svc/Fault/Miss/Budget/Smc/Stop.
// GPRs R0..R14 used by any member are loaded once at run entry and retained
// across block boundaries. A shared exit epilogue publishes all registers
// any member can write and the completed-block tick/dispatch counters,
// including on faults and condition-failed paths. Checked helpers access
// memory/fault/SMC fields only; they must not inspect or modify cached GPRs.
// `blocks` and `meta` must be parallel, non-empty, sorted strictly ascending
// by entry_pc (formation guarantees at most one block per guest PC).
// Dispatch validates each entry's
// CPSR mode bits (psr_mask/psr_value) and FPSCR mode bits against the block's
// own location. LinkBlock terminals chaining to a member with the SAME full
// location descriptor take the LIGHT dispatch path (REGION_ABI.md v1.2): the
// edge preloads the successor's constant block index and skips the regs[15]
// reload and the static PC search; per-iteration dispatches++/stop/smc
// polling still run, and the edge itself performs the successor's budget
// check. Non-member links set next_pc and return Miss. Memory IR is accepted
// at any CycleCount: a faulting helper sets fault_pc and CPSR mode bits
// (including IT) from the faulting memory op's own location immediate (arg0),
// preserving arithmetic flags, and returns Fault without rollback. Counts
// include completed blocks and earlier completed store-delimited segments;
// the faulting segment is uncounted. Per-call budget is the `budget` argument
// (max additional ticks). Without continuations, dispatch checks meta.ticks.
// With frontend-provided store_continuations (unconditional blocks only),
// dispatch checks the first segment and each continuation checks the next.
// Store boundaries fall through unless stop/SMC/budget requires an exit;
// those exits publish the boundary's location and completed ticks. Metadata
// must describe the exact, unmodified IR returned by translate_block.
// Limits: 512 blocks, 32768 total ticks,
// 4 MiB module. EMPTY vector = unsupported.
struct RegionBlockMeta {
    uint32_t entry_pc;
    uint32_t psr_mask;
    uint32_t psr_value;
    uint32_t ticks; // conservative: CycleCount + ConditionFailedCycleCount
    std::vector<StoreContinuation> store_continuations{};
};
std::vector<uint8_t> emit_region(
    const std::vector<const Dynarmic::IR::Block *> &blocks,
    const std::vector<RegionBlockMeta> &meta);

// Checks whether a block can be lowered into a region body without assembling
// a complete module. Region formation uses this to avoid repeatedly building
// and discarding one-block Wasm modules for every candidate successor.
bool validate_region_block(const Dynarmic::IR::Block &block,
    const std::vector<StoreContinuation> &store_continuations = {});

// M16 Wasm-side multi-region dispatcher (REGION_ABI.md).
// The dispatcher pumps already-compiled region run() functions through a
// host-shared funcref table, resolving transfer targets through an
// open-addressed guest-location -> table-slot map in linear memory.
//
// Map entry (16 bytes, host-written, dispatcher-read):
//   +0 key_lo  low 32 bits of the region-cache location hash
//   +4 key_hi  high 32 bits of the region-cache location hash
//   +8 slot    shared-table index of the region's run function
//   +12 epoch  map epoch at insert; must equal *epoch_addr to match
// An entry with epoch == 0 was never written and terminates probing.
// Stale entries (epoch mismatch) are skipped on lookup and overwritten on
// insert. The host bumps *epoch_addr on EVERY region eviction, so a stale
// slot can never match; table slots are additionally nulled on release.
// Map capacity is fixed (kDispatchMapEntries, power of two); the host keeps
// live regions far below it (REGION_CACHE_LIMIT) and fails loudly if an
// insert finds no reusable slot within kDispatchMaxProbe.
//
// mrun(state, remaining, map_base, epoch_addr) -> ExitReason runs the pump:
// entry and every transfer resolve (pc, cpsr, fpscr) to the location hash
// exactly like the host region cache, probe the map, and call_indirect the
// slot with a REGION_CALL_TICKS-clamped slice of the remaining budget.
// Stop is checked per transfer from state.stop_flag. Regions returning Miss
// with a mapped target chain inside Wasm (tx_wasm++); unmapped targets,
// Svc, Fault, Stop, Smc and slice-exhausted Budget return to the host with
// next_pc published exactly as a single-region run would. A region reporting
// more ticks than its slice returns DispatchOverrun (host fails, as it does
// for the equivalent single-region overrun today).
constexpr uint32_t kDispatchMapEntries = 2048;
constexpr uint32_t kDispatchMapMask = kDispatchMapEntries - 1;
constexpr uint32_t kDispatchMaxProbe = 64;
constexpr uint32_t kDispatchEntryBytes = 16;
constexpr uint32_t kDispatchTableLimit = 512;
constexpr uint32_t kDispatchSliceTicks = 131072; // == REGION_CALL_TICKS
// Hash must be bit-identical to dispatch_map_index() in wasm_jit_cpu.cpp.
constexpr uint32_t kDispatchHashK = 0x9e3779b9u;
inline uint32_t dispatch_map_index(uint32_t key_lo, uint32_t key_hi) {
    return (key_lo ^ (key_hi * kDispatchHashK)) & kDispatchMapMask;
}
enum class DispatchReason : uint32_t {
    RegionOverrun = 8, // region reported more ticks than its slice
};
std::vector<uint8_t> emit_dispatch();

} // namespace vita3k::wasmjit
