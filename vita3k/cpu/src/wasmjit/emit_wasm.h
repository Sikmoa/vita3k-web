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
};
static_assert(std::is_standard_layout_v<JitState>);
static_assert(sizeof(JitState) == 84);

// Emits an MVP Wasm module importing ONLY env.memory (unshared, min 1 page)
// and exporting block: (i32 stateOffset) -> i32 reason. The export can be put
// directly in an Emscripten function table (signature "ii"). stateOffset must
// address a live JitState in that memory; out-of-bounds accesses trap.
//
// EMPTY vector means unsupported IR/terminal/location or exceeded limits; no
// partial module is returned. No guest instructions execute during emission.
// Supported modules return after ONE block (no chaining). Each invocation
// overwrites executed, exit_reason and svc; regs[15] is the next guest PC.
// Svc returns AFTER the frontend's post-SVC PC write, leaving handling to host.
// Fault/Unsupported are reserved for host failures, not emitted no-op fallbacks.
//
// Frontend MUST use one tick per guest instruction: CycleCount and
// ConditionFailedCycleCount become executed. Caller must translate at most its
// remaining instruction budget and dispatch/check budget between invocations.
// There is no mid-block budget check. Cache keys must include the A32 location
// descriptor's mode bits; entry state must match it. IT blocks are not supported.
// Limits: 4096 IR instructions, 4096 guest ticks, terminal depth 16 / 256 nodes.
std::vector<uint8_t> emit_block(const Dynarmic::IR::Block &block);

} // namespace vita3k::wasmjit
