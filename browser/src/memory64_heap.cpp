// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
// Browser Memory64 morecore boundary. No malloc interception or guest allocator
// replacement: Emscripten dlmalloc continues to allocate from one bounded break.
#include <mem/memory_model.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <unistd.h>

#ifndef VITA3K_WEB_MEMORY64
#error This object belongs only to the Memory64 browser build
#endif
#ifdef __EMSCRIPTEN_SHARED_MEMORY__
#error The current cooperative Vita3K browser runtime requires unshared memory
#endif

extern "C" {
extern unsigned char __heap_base;

// Strong definitions replace the complete Emscripten libc sbrk.c interface.
// The object is linked directly into every browser executable. The stock
// sbrk.c archive member must not also be linked; a symbol conflict should fail
// the link rather than silently restore an unbounded break (see MEMORY64.md).
// Lazy initialization is deliberate: morecore may be needed before C++ global
// constructors. Do not depend on a pointer-to-integer dynamic initializer.
static uintptr_t runtime_break = 0;

uintptr_t *emscripten_get_sbrk_ptr() {
    if (!runtime_break)
        runtime_break = reinterpret_cast<uintptr_t>(&__heap_base);
    return &runtime_break;
}

void *_sbrk64(int64_t increment) {
    using namespace vita3k::memory;
    constexpr uint64_t alignment = alignof(std::max_align_t);
    const uintptr_t floor = reinterpret_cast<uintptr_t>(&__heap_base);
    const uintptr_t old = *emscripten_get_sbrk_ptr();
    if (floor >= guest_window_base || old < floor || old > guest_window_base) {
        errno = ENOMEM;
        return reinterpret_cast<void *>(UINTPTR_MAX);
    }
    uintptr_t next;
    if (increment >= 0) {
        const uint64_t amount = static_cast<uint64_t>(increment);
        // Reject before rounding; no signed overflow or wrap at INT64_MAX.
        if (amount > guest_window_base - old) {
            errno = ENOMEM;
            return reinterpret_cast<void *>(UINTPTR_MAX);
        }
        const uint64_t rounded = (amount + alignment - 1) & ~(alignment - 1);
        if (rounded > guest_window_base - old) {
            errno = ENOMEM;
            return reinterpret_cast<void *>(UINTPTR_MAX);
        }
        next = old + rounded;
    } else {
        // Unsigned magnitude also handles INT64_MIN. Match libc's rounding
        // toward zero when trimming. Memory.size is never reduced.
        const uint64_t amount = (uint64_t{0} - static_cast<uint64_t>(increment)) & ~(alignment - 1);
        if (amount > old - floor) {
            errno = ENOMEM;
            return reinterpret_cast<void *>(UINTPTR_MAX);
        }
        next = old - amount;
    }
    // Memory is created at its final size; no memory.grow call is needed.
    runtime_break = next;
    return reinterpret_cast<void *>(old);
}

void *sbrk(intptr_t increment) {
    return _sbrk64(increment);
}

int brk(void *pointer) {
    const uintptr_t target = reinterpret_cast<uintptr_t>(pointer);
    if (target < reinterpret_cast<uintptr_t>(&__heap_base)
        || target > vita3k::memory::guest_window_base) {
        errno = ENOMEM;
        return -1;
    }
    *emscripten_get_sbrk_ptr() = target;
    return 0;
}

bool vita3k_web_memory64_ready() {
    using namespace vita3k::memory;
    const uintptr_t current = *emscripten_get_sbrk_ptr();
    // Called before the guest window can be used. Raw Wasm size, not malloc's
    // break: the distinction is the whole purpose of this backend.
    return __builtin_wasm_memory_size(0) == guest_window_end / wasm_page_size
        && reinterpret_cast<uintptr_t>(&__heap_base) < guest_window_base
        && current >= reinterpret_cast<uintptr_t>(&__heap_base)
        && current <= guest_window_base;
}
} // extern "C"
