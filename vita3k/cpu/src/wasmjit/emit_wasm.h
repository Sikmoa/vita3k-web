// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

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
static_assert(sizeof(JitState) == 412);

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
// M15 inline memory fast path: when page_table_base/page_perms_base are
// populated (0=off), 1/2/4-byte A32 memory IR lowers INLINE instead of calling
// the checked helpers: page-table lookup + permission/refcount probe + direct
// Wasm load/store against the sparse page backing. The fast path is taken only
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
// exits only for Svc/Fault/Miss/Budget/Smc/Stop. `blocks` and `meta` must be
// parallel, non-empty, sorted strictly ascending by entry_pc (formation
// guarantees at most one block per guest PC). Dispatch validates each entry's
// CPSR mode bits (psr_mask/psr_value) and FPSCR mode bits against the block's
// own location. LinkBlock terminals chaining to a member with the SAME full
// location descriptor become direct branches; others set next_pc and return
// Miss. Memory IR is accepted at any CycleCount: a faulting helper sets
// fault_pc from the faulting memory op's own location immediate (arg0) and
// returns Fault WITHOUT rollback or executed adjustment. Per-call budget is
// the `budget` argument (max additional ticks); dispatch refuses a block whose
// meta.ticks would exceed it and returns Budget with next_pc set. Limits: 512
// blocks, 32768 total ticks, 4 MiB module. EMPTY vector = unsupported.
struct RegionBlockMeta {
    uint32_t entry_pc;
    uint32_t psr_mask;
    uint32_t psr_value;
    uint32_t ticks; // conservative: CycleCount + ConditionFailedCycleCount
};
std::vector<uint8_t> emit_region(
    const std::vector<const Dynarmic::IR::Block *> &blocks,
    const std::vector<RegionBlockMeta> &meta);

} // namespace vita3k::wasmjit
