// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace vita3k::wasmjit {

// A complete guest store instruction followed by more instructions in the
// same translated block. The frontend records this before reading the next
// instruction, after every store element, register writeback and IT advance.
// No IR rewriting is permitted between recording and emission.
struct StoreContinuation {
    uint32_t ir_offset;       // Number of IR instructions before the boundary.
    uint64_t next_location;   // Full Dynarmic location of the next instruction.
    uint32_t completed_ticks; // Cumulative guest ticks from this block's entry.
};

} // namespace vita3k::wasmjit
