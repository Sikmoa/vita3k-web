// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace vita3k::wasmjit {

struct FP64Result {
    uint64_t bits;
    uint32_t flags;
};

// ARM scalar binary64: operation 0 = add, 1 = sub, 2 = mul, 3 = div.
// Uses FPSCR.RMode[23:22] (RN-even, +inf, -inf, zero), FZ[24], DN[25].
// flags contains ONLY newly raised IOC/DZC/OFC/UFC/IXC/IDC in FPSCR bit
// positions (mask 0x9f); the caller must OR it into the cumulative FPSCR.
// Precondition: the emitter has rejected live exception enables [15,12:8].
// Trap delivery is not implemented; other FPSCR bits are ignored.
// Invalid operation selectors return the default NaN with IOC.
// Integer-only, allocation-free, independent of host FP state and Dynarmic.
FP64Result fp64_arithmetic(uint32_t operation, uint64_t a, uint64_t b, uint32_t fpscr) noexcept;

} // namespace vita3k::wasmjit
