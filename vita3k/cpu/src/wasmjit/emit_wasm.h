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
};
static_assert(std::is_standard_layout_v<JitState>);
static_assert(offsetof(JitState, memory_cookie) == 84);
static_assert(offsetof(JitState, memory_value) == 96);
static_assert(offsetof(JitState, fpu) == 112);
static_assert(offsetof(JitState, tpidruro) == 368);
static_assert(sizeof(JitState) == 372);

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
// No guest data address is ever used directly as a host linear-memory offset.
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

} // namespace vita3k::wasmjit
