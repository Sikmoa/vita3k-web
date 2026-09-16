// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

#if defined(__EMSCRIPTEN__) && defined(__wasm64__) && !defined(VITA3K_WEB_MEMORY64)
#error A wasm64 browser ABI must select VITA3K_WEB_MEMORY64 consistently
#endif

namespace vita3k::memory {
// These are linear-memory offsets, never Vita addresses or allocator sizes.
inline constexpr uint64_t guest_address_space_size = uint64_t{1} << 32;
inline constexpr uint64_t guest_window_base = guest_address_space_size;
inline constexpr uint64_t guest_window_end = guest_window_base + guest_address_space_size;
inline constexpr uint64_t wasm_page_size = 65536;

#ifdef VITA3K_WEB_MEMORY64
inline constexpr bool direct_memory64 = true;
static_assert(sizeof(void *) == 8 && sizeof(uintptr_t) == 8,
    "VITA3K_WEB_MEMORY64 requires a true wasm64 C/C++ ABI");
#if !defined(__EMSCRIPTEN__) || !defined(__wasm64__)
#error VITA3K_WEB_MEMORY64 is only supported by the Emscripten wasm64 browser build
#endif
#else
inline constexpr bool direct_memory64 = false;
#endif

inline bool guest_range_fits(uint32_t address, uint64_t size) {
    return size <= guest_address_space_size - uint64_t(address);
}

// Only use after checking the guest mapping's lifetime and range. Keeping the
// integer addition explicit avoids both wasm32 truncation and signed extension.
inline uint8_t *direct_pointer(uint32_t address) {
    if constexpr (direct_memory64)
        return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(guest_window_base + uint64_t(address)));
    return nullptr;
}
} // namespace vita3k::memory
