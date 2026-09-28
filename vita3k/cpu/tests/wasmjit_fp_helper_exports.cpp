// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone Wasm exports for the emitter execution suite. The production
// helper uses the same fp_extended_arithmetic implementation.
#include "../src/wasmjit/fp64.h"
static uint32_t last_flags;
extern "C" uint64_t extended_fp(uint32_t operation, uint64_t a, uint64_t b, uint64_t c, uint32_t fpscr) {
    const auto result = vita3k::wasmjit::fp_extended_arithmetic(operation, a, b, c, fpscr);
    last_flags = result.flags;
    return result.bits;
}
extern "C" uint32_t extended_flags() { return last_flags; }
