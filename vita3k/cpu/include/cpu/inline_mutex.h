// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace vita3k::wasmjit {

// ABI between the cooperative kernel host and generated Wasm. This is HOST
// linear memory, not a guest allocation. Only one guest fiber executes at a
// time, and generated code never yields inside a successful probe/commit.
// These plain words are NOT safe for a parallel/shared-memory CPU backend.
constexpr uint32_t kInlineMutexEntries = 1024;
constexpr uint32_t kInlineMutexNoOwner = UINT32_MAX;
constexpr uint32_t kInlineMutexRecursive = 2;

struct InlineMutexEntry {
    uint32_t workarea = 0; // exact guest address; collisions decline, never evict
    uint32_t uid = 0;      // lifetime tag, also checked against workarea.uid
    uint32_t owner = kInlineMutexNoOwner;
    uint32_t count = 0;
    uint32_t attr = 0;
    uint32_t enabled = 0; // no waiters and no suspended HLE operation on this mutex
    uint32_t dirty = 0;
    uint32_t next_dirty = 0; // intrusive list: slot index + 1, zero terminates
};

struct InlineMutexTable {
    uint32_t dirty_head = 0;
    std::array<InlineMutexEntry, kInlineMutexEntries> entries{};
};

constexpr uint32_t inline_mutex_index(uint32_t workarea) {
    return ((workarea >> 5) ^ (workarea >> 15)) & (kInlineMutexEntries - 1);
}

// Generated success updates the entry AND guest owner/count, and links the
// entry once per phase into dirty_head. The execution host MUST drain that
// list into the kernel's Mutex objects before HLE, scheduling, exceptions, or
// thread deletion can observe them. It costs O(distinct changed mutexes), not
// O(table size) or O(inlined calls). HLE disables an entry for its entire
// operation (including fiber suspension), then republishes authoritative
// owner/count/attributes when the final such operation returns. Deletion
// clears the lifetime tag before an address/slot can be registered again.
static_assert(std::is_standard_layout_v<InlineMutexEntry>);
static_assert(std::is_standard_layout_v<InlineMutexTable>);
static_assert(sizeof(InlineMutexEntry) == 32);
static_assert(offsetof(InlineMutexTable, entries) == 4);

} // namespace vita3k::wasmjit
