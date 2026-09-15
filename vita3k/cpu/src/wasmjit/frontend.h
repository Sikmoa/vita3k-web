// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "block_metadata.h"

#include <cstdint>
#include <vector>
#include <dynarmic/ir/basic_block.h>

struct MemState;

namespace vita3k::wasmjit {

// Translate at the actual instruction address (not architectural PC+8/+4 and
// not a Thumb-tagged function pointer). CPSR.T selects ARM/Thumb. PC must be
// word/halfword aligned respectively. No guest instructions are executed.
//
// Block.Location()/EndLocation() retain Dynarmic's full location key: PC,
// CPSR.T/E/IT and FPSCR mode bits (rounding, FZ/DN, vector length/stride).
// NZCV are runtime inputs, NOT translation-key bits. A cache must key on the
// native descriptor, not PC alone, and invalidate on code/permission changes.
//
// Throws std::invalid_argument for zero budget or misaligned PC and
// std::runtime_error on an unmapped/non-executable instruction fetch. Dynarmic
// fetches aligned little-endian 32-bit words even for Thumb16: all four bytes
// must be executable. No partial IR is returned on fetch failure. Serialize
// against memory allocation, writes and protection changes, as for mem_fetch.
// Unsupported guest instructions may translate to ExceptionRaised/Interpret;
// the emitter must reject unsupported IR/terminals rather than silently skip.
// The budget is an upper bound; conditional instructions can split earlier.
// With store_continuations, unconditional region blocks may continue after
// stores, up to max_store_continuations boundaries per block. The output is
// replaced on every call and must accompany the IR to the region emitter.
// The default cap is 2 (three store-delimited segments per block): measured
// 2026-09-15 this keeps roughly half the dispatcher-visit reduction while
// staying near pre-continuation entry cost, and local browser testing showed
// +2 FPS over both the uncapped and the disabled variants on real devices.
// Conditional blocks and calls without this output keep the conservative
// store-ending behavior (including the single-step path).
constexpr size_t kDefaultMaxStoreContinuations = 2;
Dynarmic::IR::Block translate_block(MemState &mem, uint32_t pc, uint32_t cpsr,
    uint32_t max_instructions = 32, uint32_t fpscr = 0,
    std::vector<StoreContinuation> *store_continuations = nullptr,
    size_t max_store_continuations = kDefaultMaxStoreContinuations);

} // namespace vita3k::wasmjit
