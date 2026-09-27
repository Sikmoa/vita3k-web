// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <mem/functions.h>
#include <mem/state.h>

#include <util/align.h>
#include <util/log.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <csignal>
#include <sys/mman.h>
#include <unistd.h>
#endif

constexpr uint32_t STANDARD_PAGE_SIZE = KiB(4);
// The guest address space does not fit in Wasm32 size_t.
constexpr uint64_t TOTAL_MEM_SIZE = vita3k::memory::guest_address_space_size;
constexpr uint32_t PAGE_COUNT = TOTAL_MEM_SIZE / STANDARD_PAGE_SIZE;
constexpr bool LOG_PROTECT = false;
#ifdef NDEBUG
constexpr bool PAGE_NAME_TRACKING = false;
#else
constexpr bool PAGE_NAME_TRACKING = true;
#endif

#ifndef __EMSCRIPTEN__
// TODO: support multiple handlers
static AccessViolationHandler access_violation_handler;
static void register_access_violation_handler(const AccessViolationHandler &handler);
static void delete_memory(uint8_t *memory);
#endif

static Address alloc_inner(MemState &state, uint32_t start_page, uint32_t page_count, const char *name, const bool force);

#ifdef VITA3K_WEB_MEMORY64
// A fixed window belongs to one live guest machine in this Wasm instance.
// A second MemState must fail rather than alias another machine's bytes.
static std::atomic<MemState *> direct_window_owner{nullptr};
extern "C" bool vita3k_web_memory64_ready();
#endif

#ifndef __EMSCRIPTEN__
#ifdef _WIN32
static std::string get_error_msg() {
    return std::system_category().message(GetLastError());
}
#else
static std::string get_error_msg() {
    return strerror(errno);
}
#endif
#endif

bool init(MemState &state, const bool use_page_table) {
    deinit_mem(state);
#ifdef __EMSCRIPTEN__
    state.host_page_size = STANDARD_PAGE_SIZE;
    state.direct_host_memory = vita3k::memory::direct_memory64;
    state.sparse_host_memory = !state.direct_host_memory;
#ifdef VITA3K_WEB_MEMORY64
    MemState *expected = nullptr;
    if (!vita3k_web_memory64_ready()
        || !direct_window_owner.compare_exchange_strong(expected, &state)) {
        deinit_mem(state);
        return false;
    }
#endif
#elif defined(_WIN32)
    SYSTEM_INFO system_info = {};
    GetSystemInfo(&system_info);
    state.host_page_size = system_info.dwPageSize;
#else
    state.host_page_size = static_cast<int>(sysconf(_SC_PAGESIZE));
#endif

    assert(state.host_page_size >= 4096); // Limit imposed by Unicorn.

#ifndef __EMSCRIPTEN__
    void *preferred_address = reinterpret_cast<void *>(1ULL << 34);

#ifdef _WIN32
    state.memory = Memory(static_cast<uint8_t *>(VirtualAlloc(preferred_address, TOTAL_MEM_SIZE, MEM_RESERVE, PAGE_NOACCESS)), delete_memory);
    if (!state.memory) {
        // fallback
        state.memory = Memory(static_cast<uint8_t *>(VirtualAlloc(nullptr, TOTAL_MEM_SIZE, MEM_RESERVE, PAGE_NOACCESS)), delete_memory);

        if (!state.memory) {
            LOG_CRITICAL("VirtualAlloc failed: {}", get_error_msg());
            return false;
        }
    }
#else
    // http://man7.org/linux/man-pages/man2/mmap.2.html
    const int prot = PROT_NONE;
    const int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    const int fd = 0;
    const off_t offset = 0;
    // preferred_address is only a hint for mmap, if it can't use it, the kernel will choose itself the address
    state.memory = Memory(static_cast<uint8_t *>(mmap(preferred_address, TOTAL_MEM_SIZE, prot, flags, fd, offset)), delete_memory);
    if (state.memory.get() == MAP_FAILED) {
        LOG_CRITICAL("mmap failed {}", get_error_msg());
        state.memory.release(); // MAP_FAILED must not be passed to the deleter.
        return false;
    }
#endif
#endif

    try {
        state.alloc_table = AllocPageTable(new AllocMemPage[PAGE_COUNT]{});
        state.page_permissions = std::make_unique<MemPerm[]>(PAGE_COUNT);
        state.write_epochs = std::make_unique<uint32_t[]>(PAGE_COUNT);
        state.allocator.set_maximum(PAGE_COUNT);
        state.use_page_table = !state.direct_host_memory && (state.sparse_host_memory || use_page_table);
        if (state.use_page_table) {
            state.page_table = PageTable(new PagePtr[PAGE_COUNT]);
            std::fill_n(state.page_table.get(), PAGE_COUNT, state.memory.get());
        }
    } catch (const std::bad_alloc &) {
        deinit_mem(state);
        return false;
    }

#ifndef __EMSCRIPTEN__
    const auto handler = [&state](uint8_t *addr, bool write) noexcept {
        return handle_access_violation(state, addr, write);
    };
    register_access_violation_handler(handler);
#endif

    // Reserve the null host page in the same allocator; Wasm needs no backing.
    const uint32_t null_pages = state.host_page_size / STANDARD_PAGE_SIZE;
    if (state.allocator.allocate_at(0, null_pages) < 0) {
        deinit_mem(state);
        return false;
    }
    state.alloc_table[0].allocated = 1;
    state.alloc_table[0].size = null_pages;
    return true;
}

#ifndef __EMSCRIPTEN__
static void delete_memory(uint8_t *memory) {
    if (memory != nullptr) {
#ifdef _WIN32
        const BOOL ret = VirtualFree(memory, 0, MEM_RELEASE);
        assert(ret);
#else
        munmap(memory, TOTAL_MEM_SIZE);
#endif
    }
}
#endif

bool is_valid_addr(const MemState &state, Address addr) {
    const uint32_t page_num = addr / STANDARD_PAGE_SIZE;
    return addr >= state.host_page_size && state.alloc_table && state.allocator.free_slot_count(page_num, page_num + 1) == 0;
}

bool is_valid_addr_range(const MemState &state, Address start, uint64_t end) {
    if (start >= end || end > TOTAL_MEM_SIZE || start < state.host_page_size || !state.alloc_table)
        return false;
    const uint32_t start_page = start / STANDARD_PAGE_SIZE;
    const uint32_t end_page = (uint64_t(end) + STANDARD_PAGE_SIZE - 1) / STANDARD_PAGE_SIZE;
    return state.allocator.free_slot_count(start_page, end_page) == 0;
}

static Address alloc_inner(MemState &state, uint32_t start_page, uint32_t page_count, const char *name, const bool force) {
    if (!state.alloc_table || !page_count || start_page >= PAGE_COUNT || page_count > PAGE_COUNT - start_page)
        return 0;
    int page_num;
    if (force) {
        if (state.allocator.allocate_at(start_page, page_count) < 0)
            return 0;
        page_num = start_page;
    } else {
        page_num = state.allocator.allocate_from(start_page, page_count, false);
        if (page_num < 0)
            return 0;
    }

    const uint64_t size = uint64_t(page_count) * STANDARD_PAGE_SIZE;
    const Address addr = uint32_t(page_num) * STANDARD_PAGE_SIZE;
    if (state.direct_host_memory) {
        // No physical backing allocation and no page translation table. Reuse
        // always starts zeroed, even though free does not shrink Wasm memory.
        std::memset(vita3k::memory::direct_pointer(addr), 0, static_cast<size_t>(size));
    } else if (state.sparse_host_memory) {
        // Every page in an allocation points into ONE buffer. ELF segment copies,
        // stacks and HLE Ptr+length users rely on this contiguity.
        if (size > std::numeric_limits<size_t>::max() - (STANDARD_PAGE_SIZE - 1)) {
            state.allocator.free(page_num, page_count);
            return 0;
        }
        // Guest pages are page-aligned. Preserve the same low address bits in
        // sparse host backing so HLE allocators' sub-page alignment survives
        // Ptr(host, mem) translation. Keep the original owner for delete[].
        auto backing = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[static_cast<size_t>(size) + STANDARD_PAGE_SIZE - 1]{});
        if (!backing) {
            state.allocator.free(page_num, page_count);
            return 0;
        }
        const size_t offset = (STANDARD_PAGE_SIZE - (reinterpret_cast<uintptr_t>(backing.get()) % STANDARD_PAGE_SIZE)) % STANDARD_PAGE_SIZE;
        auto *base = backing.get() + offset;
        try {
            state.sparse_allocations.emplace(page_num, SparseAllocation { std::move(backing), offset });
        } catch (const std::bad_alloc &) {
            state.allocator.free(page_num, page_count);
            return 0;
        }
        for (uint32_t page = 0; page < page_count; ++page)
            state.page_table[page_num + page] = base + size_t(page) * STANDARD_PAGE_SIZE;
    }
#ifndef __EMSCRIPTEN__
    else {
        const uint64_t commit_start = align_down(uint64_t(addr), state.host_page_size);
        const uint64_t commit_end = align(uint64_t(addr) + size, state.host_page_size);
        const size_t commit_size = commit_end - commit_start;
        uint8_t *const commit_ptr = &state.memory[commit_start];
#ifdef _WIN32
        const bool committed = VirtualAlloc(commit_ptr, commit_size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
        const bool committed = mprotect(commit_ptr, commit_size, PROT_READ | PROT_WRITE) == 0;
#endif
        if (!committed) {
            LOG_CRITICAL("Memory commit failed: {}", get_error_msg());
            state.allocator.free(page_num, page_count);
            return 0;
        }
        std::memset(&state.memory[addr], 0, static_cast<size_t>(size));
    }
#endif

    AllocMemPage &page = state.alloc_table[page_num];
    assert(!page.allocated);
    page.allocated = 1;
    page.size = page_count;
    std::fill_n(state.page_permissions.get() + page_num, page_count, MemPerm::ReadWriteExecute);
    if (PAGE_NAME_TRACKING) {
        try {
            state.page_name_map.emplace(page_num, name ? name : "");
        } catch (const std::bad_alloc &) {
            // Debug labels must not turn a successful allocation into a leak.
        }
    }
    return addr;
}

Address alloc_aligned(MemState &state, uint32_t size, const char *name, unsigned int alignment, Address start_addr) {
    if (alignment == 0)
        return alloc(state, size, name, start_addr);
    if (!size || (alignment & (alignment - 1)))
        return 0;
    const uint64_t padded_size = uint64_t(size) + alignment - 1;
    const uint64_t count = align(padded_size, STANDARD_PAGE_SIZE) / STANDARD_PAGE_SIZE;
    if (count >= PAGE_COUNT)
        return 0;
    const std::lock_guard<std::mutex> lock(state.generation_mutex);
    const Address addr = alloc_inner(state, start_addr / STANDARD_PAGE_SIZE, static_cast<uint32_t>(count), name, false);
    if (!addr)
        return 0;
    const Address align_addr = static_cast<Address>(align(uint64_t(addr), alignment));
    const uint32_t page_num = addr / STANDARD_PAGE_SIZE;
    const uint32_t align_page_num = align_addr / STANDARD_PAGE_SIZE;
    if (page_num != align_page_num) {
        AllocMemPage &page = state.alloc_table[page_num];
        AllocMemPage &align_page = state.alloc_table[align_page_num];
        const uint32_t remnant_front = align_page_num - page_num;
        state.allocator.free(page_num, remnant_front);
        align_page.allocated = 1;
        align_page.size = page.size - remnant_front;
        page = {};
        std::fill_n(state.page_permissions.get() + page_num, remnant_front, MemPerm::None);
        if (state.sparse_host_memory) {
            auto owner = state.sparse_allocations.extract(page_num);
            owner.key() = align_page_num;
            owner.mapped().offset += size_t(remnant_front) * STANDARD_PAGE_SIZE;
            state.sparse_allocations.insert(std::move(owner));
            std::fill_n(state.page_table.get() + page_num, remnant_front, nullptr);
        }
        if (PAGE_NAME_TRACKING) {
            auto entry = state.page_name_map.extract(page_num);
            if (!entry.empty()) {
                entry.key() = align_page_num;
                state.page_name_map.insert(std::move(entry));
            }
        }
    }
    return align_addr;
}

// Unchecked translation for trusted HLE access. A sparse entry is a page base;
// a native entry remains the original direct-path absolute-address bias.
uint8_t *mem_guest_to_host(const MemState &state, Address addr) {
    if (state.direct_host_memory)
        return is_valid_addr(state, addr) ? vita3k::memory::direct_pointer(addr) : nullptr;
    if (state.use_page_table) {
        auto *base = state.page_table[addr / STANDARD_PAGE_SIZE];
        if (!base)
            return nullptr;
        return base + (state.sparse_host_memory ? addr % STANDARD_PAGE_SIZE : addr);
    }
    return state.memory ? state.memory.get() + addr : nullptr;
}

static uint8_t *page_pointer(const MemState &state, Address addr) {
    return mem_guest_to_host(state, addr);
}

static bool check_range(const MemState &state, Address addr, size_t size, MemPerm required) {
    if (!size)
        return true;
    if (!state.alloc_table || addr < state.host_page_size || uint64_t(size) > TOTAL_MEM_SIZE - addr)
        return false;
    const uint32_t first = addr / STANDARD_PAGE_SIZE;
    const uint32_t end = (uint64_t(addr) + size + STANDARD_PAGE_SIZE - 1) / STANDARD_PAGE_SIZE;
    if (state.allocator.free_slot_count(first, end) != 0)
        return false;
    for (uint32_t page = first; page < end; ++page) {
        if ((static_cast<uint8_t>(state.page_permissions[page]) & static_cast<uint8_t>(required)) != static_cast<uint8_t>(required))
            return false;
        if (!state.direct_host_memory && !page_pointer(state, page * STANDARD_PAGE_SIZE))
            return false;
    }
    return true;
}

static bool copy_from_guest(const MemState &state, Address addr, void *destination, size_t size, MemPerm required) {
    if ((!destination && size) || !check_range(state, addr, size, required))
        return false;
    if (state.direct_host_memory) {
        if (size)
            std::memcpy(destination, vita3k::memory::direct_pointer(addr), size);
        return true;
    }
    auto *output = static_cast<uint8_t *>(destination);
    while (size) {
        const size_t count = std::min(size, size_t(STANDARD_PAGE_SIZE - addr % STANDARD_PAGE_SIZE));
        std::memcpy(output, page_pointer(state, addr), count);
        output += count;
        addr += static_cast<Address>(count);
        size -= count;
    }
    return true;
}

bool mem_read(const MemState &state, Address addr, void *destination, size_t size) {
    return copy_from_guest(state, addr, destination, size, MemPerm::ReadOnly);
}

bool mem_fetch(const MemState &state, Address addr, void *destination, size_t size) {
    return copy_from_guest(state, addr, destination, size, MemPerm::Execute);
}

void (*g_mem_write_observer)(Address addr, size_t size) = nullptr;

bool mem_write(MemState &state, Address addr, const void *source, size_t size) {
    if ((!source && size) || !check_range(state, addr, size, MemPerm::WriteOnly))
        return false;
    if (g_mem_write_observer)
        g_mem_write_observer(addr, size);
    mem_mark_written(state, addr, size);
    if (state.direct_host_memory) {
        if (size)
            std::memcpy(vita3k::memory::direct_pointer(addr), source, size);
        return true;
    }
    auto *input = static_cast<const uint8_t *>(source);
    while (size) {
        const size_t count = std::min(size, size_t(STANDARD_PAGE_SIZE - addr % STANDARD_PAGE_SIZE));
        std::memcpy(page_pointer(state, addr), input, count);
        input += count;
        addr += static_cast<Address>(count);
        size -= count;
    }
    return true;
}

void mem_mark_written(MemState &state, Address addr, size_t size) {
    if (!size || !state.write_epochs)
        return;
    const uint64_t end = std::min<uint64_t>(uint64_t(addr) + size, TOTAL_MEM_SIZE);
    for (uint64_t page = addr / STANDARD_PAGE_SIZE; page * STANDARD_PAGE_SIZE < end; ++page)
        state.write_epochs[page] = state.write_epoch;
}

void mem_mark_written_host(MemState &state, const void *pointer, size_t size) {
    Address addr = 0;
    if (size && mem_host_to_guest(state, pointer, addr))
        mem_mark_written(state, addr, size);
}

uint32_t mem_next_write_epoch(MemState &state) {
    return ++state.write_epoch;
}

uint32_t mem_written_epoch(const MemState &state, Address addr, size_t size) {
    if (!size || !state.write_epochs)
        return 0;
    uint32_t latest = 0;
    const uint64_t end = std::min<uint64_t>(uint64_t(addr) + size, TOTAL_MEM_SIZE);
    for (uint64_t page = addr / STANDARD_PAGE_SIZE; page * STANDARD_PAGE_SIZE < end; ++page)
        latest = std::max(latest, state.write_epochs[page]);
    return latest;
}

bool mem_set_permissions(MemState &state, Address addr, size_t size, MemPerm perm) {
    if (static_cast<uint8_t>(perm) > static_cast<uint8_t>(MemPerm::ReadWriteExecute)
        || !check_range(state, addr, size, MemPerm::None))
        return false;
    if (size) {
        protect_inner(state, addr, static_cast<uint32_t>(size), perm);
        // A permission change can invalidate a cached region without changing
        // a single byte (losing Execute makes the fetch fail), so it counts as
        // a tracked write for the JIT's per-page generations.
        if (g_mem_write_observer)
            g_mem_write_observer(addr, size);
    }
    return true;
}

bool mem_host_to_guest(const MemState &state, const void *pointer, Address &addr) {
    addr = 0;
    if (!pointer || !state.alloc_table)
        return false;
    const uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
    if (state.direct_host_memory) {
        if (value < vita3k::memory::guest_window_base || value >= vita3k::memory::guest_window_end)
            return false;
        const uint64_t candidate = uint64_t(value) - vita3k::memory::guest_window_base;
        if (candidate > UINT32_MAX || !is_valid_addr(state, static_cast<Address>(candidate)))
            return false;
        addr = static_cast<Address>(candidate);
        return true;
    }
    const auto accept = [&](uintptr_t base, uint64_t size, Address guest) {
        if (value < base || uint64_t(value - base) >= size)
            return false;
        const uint64_t candidate64 = uint64_t(guest) + uint64_t(value - base);
        if (candidate64 >= TOTAL_MEM_SIZE)
            return false;
        const Address candidate = static_cast<Address>(candidate64);
        if (!is_valid_addr(state, candidate) || reinterpret_cast<uintptr_t>(page_pointer(state, candidate)) != value)
            return false;
        addr = candidate;
        return true;
    };
    auto mapping = state.external_mapping.lower_bound(value);
    if (mapping != state.external_mapping.end()
        && accept(mapping->first, mapping->second.size, mapping->second.address))
        return true;
    if (!state.sparse_host_memory)
        return accept(reinterpret_cast<uintptr_t>(state.memory.get()), TOTAL_MEM_SIZE, 0);
    for (const auto &[page, allocation] : state.sparse_allocations) {
        if (accept(reinterpret_cast<uintptr_t>(allocation.memory.get()) + allocation.offset,
                uint64_t(state.alloc_table[page].size) * STANDARD_PAGE_SIZE, page * STANDARD_PAGE_SIZE))
            return true;
    }
    return false;
}

void unprotect_inner(MemState &state, Address addr, uint32_t size) {
    protect_inner(state, addr, size, MemPerm::ReadWriteExecute);
}

void protect_inner(MemState &state, Address addr, uint32_t size, const MemPerm perm) {
    if (!size || !check_range(state, addr, size, MemPerm::None))
        return;
    const uint32_t first = addr / STANDARD_PAGE_SIZE;
    const uint32_t end = (uint64_t(addr) + size + STANDARD_PAGE_SIZE - 1) / STANDARD_PAGE_SIZE;
    std::fill(state.page_permissions.get() + first, state.page_permissions.get() + end, perm);
#ifndef __EMSCRIPTEN__
    // Native mappings retain OS protection. Guest Execute only needs readable
    // host storage; never make guest bytes executable host code.
    const auto bits = static_cast<uint8_t>(perm);
    for (uint32_t page = first; page < end;) {
        auto *target = page_pointer(state, page * STANDARD_PAGE_SIZE);
        uint32_t next = page + 1;
        while (next < end && reinterpret_cast<uintptr_t>(page_pointer(state, next * STANDARD_PAGE_SIZE))
                == reinterpret_cast<uintptr_t>(target) + size_t(next - page) * STANDARD_PAGE_SIZE)
            ++next;
        const uintptr_t aligned = align_down(reinterpret_cast<uintptr_t>(target), state.host_page_size);
        const uintptr_t aligned_end = align(reinterpret_cast<uintptr_t>(target) + size_t(next - page) * STANDARD_PAGE_SIZE, state.host_page_size);
        auto *aligned_start = reinterpret_cast<uint8_t *>(aligned);
        const size_t host_size = aligned_end - aligned;
#ifdef _WIN32
        const DWORD protection = (bits & 2) ? PAGE_READWRITE : (bits ? PAGE_READONLY : PAGE_NOACCESS);
        DWORD old_protect = 0;
        const BOOL ret = VirtualProtect(aligned_start, host_size, protection, &old_protect);
        LOG_CRITICAL_IF(!ret, "VirtualProtect failed: {}", get_error_msg());
#else
        const int protection = (bits & 2) ? (PROT_READ | PROT_WRITE) : (bits ? PROT_READ : PROT_NONE);
        const int ret = mprotect(aligned_start, host_size, protection);
        LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
#endif
        page = next;
    }
#endif
}

bool handle_access_violation(MemState &state, uint8_t *addr, bool write) noexcept {
    Address vaddr = 0;
    const std::unique_lock<std::mutex> lock(state.protect_mutex);
    if (!mem_host_to_guest(state, addr, vaddr))
        return false;
    if (LOG_PROTECT) {
        fmt::print("Access: {}\n", log_hex(vaddr));
    }

    auto it = state.protect_tree.lower_bound(vaddr);
    if (it == state.protect_tree.end()) {
        // HACK: keep going
        unprotect_inner(state, align_down(vaddr, state.host_page_size), state.host_page_size);
        LOG_CRITICAL("Unhandled write protected region was valid. Address=0x{:X}", vaddr);
        return true;
    }

    ProtectSegmentInfo &info = it->second;
    if (vaddr < it->first || uint64_t(vaddr) >= uint64_t(it->first) + info.size) {
        // HACK: keep going
        unprotect_inner(state, align_down(vaddr, state.host_page_size), state.host_page_size);
        LOG_CRITICAL("Unhandled write protected region was valid. Address=0x{:X}", vaddr);
        return true;
    }

    for (auto &[block_addr, block] : info.blocks) {
        block.callback(vaddr, write);
    }

    unprotect_inner(state, it->first, info.size);
    state.protect_tree.erase(it);

    return true;
}

bool add_protect(MemState &state, Address addr, const uint32_t size, const MemPerm perm, const ProtectCallback &callback) {
    if (!size || !callback || !check_range(state, addr, size, MemPerm::None))
        return false;
    const std::lock_guard<std::mutex> lock(state.protect_mutex);
    const Address original_addr = addr;
    uint64_t end = align(uint64_t(addr) + size, STANDARD_PAGE_SIZE);
    addr = align_down(addr, STANDARD_PAGE_SIZE);
    ProtectSegmentInfo protect(static_cast<uint32_t>(end - addr), perm);
    protect.blocks.emplace(original_addr, ProtectBlockInfo { size, callback });
    for (auto it = state.protect_tree.begin(); it != state.protect_tree.end();) {
        const uint64_t other_end = uint64_t(it->first) + it->second.size;
        if (it->first < end && addr < other_end) {
            addr = std::min(addr, it->first);
            end = std::max(end, other_end);
            protect.blocks.merge(it->second.blocks);
            protect.perm = most_restrictive_perm(protect.perm, it->second.perm);
            it = state.protect_tree.erase(it);
        } else {
            ++it;
        }
    }
    protect.size = static_cast<uint32_t>(end - addr);
    protect_inner(state, addr, protect.size, protect.perm);
    state.protect_tree.emplace(addr, std::move(protect));
    return true;
}

bool is_protecting(MemState &state, Address addr, MemPerm *perm) {
    const std::lock_guard<std::mutex> lock(state.protect_mutex);
    auto ite = state.protect_tree.lower_bound(addr);

    if (ite != state.protect_tree.end() && uint64_t(addr) < uint64_t(ite->first) + ite->second.size) {
        if (perm)
            *perm = ite->second.perm;

        return true;
    }

    return false;
}

static uint8_t *original_page_pointer(const MemState &mem, uint32_t page) {
    if (!mem.sparse_host_memory)
        return mem.memory.get() + uint64_t(page) * STANDARD_PAGE_SIZE;
    auto it = mem.sparse_allocations.upper_bound(page);
    if (it == mem.sparse_allocations.begin())
        return nullptr;
    --it;
    return it->second.memory.get() + it->second.offset + size_t(page - it->first) * STANDARD_PAGE_SIZE;
}

void add_external_mapping(MemState &mem, Address addr, uint32_t size, uint8_t *addr_ptr) {
    // Direct mode cannot alias a separately allocated host buffer. The browser
    // runtime does not use renderer external mappings; fail closed if introduced.
    if (mem.direct_host_memory) {
        LOG_ERROR("External host aliases are unavailable with the fixed Memory64 guest window");
        return;
    }
    if (!mem.use_page_table || !addr_ptr || !size || (addr % STANDARD_PAGE_SIZE) || (size % STANDARD_PAGE_SIZE)
        || !check_range(mem, addr, size, MemPerm::None))
        return;
    const uintptr_t value = reinterpret_cast<uintptr_t>(addr_ptr);
    if (size > std::numeric_limits<uintptr_t>::max() - value)
        return;
#ifndef __EMSCRIPTEN__
    // OS protection must not affect memory outside the caller's mapping.
    if ((value % mem.host_page_size) || (addr % mem.host_page_size) || (size % mem.host_page_size))
        return;
#endif
    for (const auto &[host, mapping] : mem.external_mapping) {
        if ((uint64_t(addr) < uint64_t(mapping.address) + mapping.size && uint64_t(mapping.address) < uint64_t(addr) + size)
            || (value < host + mapping.size && host < uint64_t(value) + size))
            return;
    }
    // A remap must use independent host storage, not existing guest backing.
    Address existing;
    if (mem_host_to_guest(mem, addr_ptr, existing) || mem_host_to_guest(mem, addr_ptr + size - 1, existing))
        return;
    const uint32_t first = addr / STANDARD_PAGE_SIZE;
    const uint32_t count = size / STANDARD_PAGE_SIZE;
    std::vector<MemPerm> permissions(mem.page_permissions.get() + first, mem.page_permissions.get() + first + count);
    unprotect_inner(mem, addr, size);
    for (uint32_t page = 0; page < count; ++page)
        std::memcpy(addr_ptr + size_t(page) * STANDARD_PAGE_SIZE, original_page_pointer(mem, first + page), STANDARD_PAGE_SIZE);
#ifndef __EMSCRIPTEN__
    // Preserve the native direct-alias fault behavior without marking the live
    // external mapping inaccessible in the guest permission metadata.
    protect_inner(mem, addr, size, MemPerm::None);
#endif
    for (uint32_t page = 0; page < count; ++page) {
        mem.page_table[first + page] = mem.sparse_host_memory ? addr_ptr + size_t(page) * STANDARD_PAGE_SIZE
                                                            : reinterpret_cast<uint8_t *>(value - addr);
    }
    for (uint32_t page = 0; page < count; ++page)
        protect_inner(mem, addr + page * STANDARD_PAGE_SIZE, STANDARD_PAGE_SIZE, permissions[page]);
    const std::unique_lock<std::mutex> lock(mem.protect_mutex);
    mem.external_mapping[value] = { addr, size };
}

static void erase_protections(MemState &mem, Address addr, uint32_t size) {
    const std::unique_lock<std::mutex> lock(mem.protect_mutex);
    for (auto it = mem.protect_tree.begin(); it != mem.protect_tree.end();) {
        if (uint64_t(it->first) < uint64_t(addr) + size && uint64_t(addr) < uint64_t(it->first) + it->second.size) {
            unprotect_inner(mem, it->first, it->second.size);
            it = mem.protect_tree.erase(it);
        } else {
            ++it;
        }
    }
}

void remove_external_mapping(MemState &mem, uint8_t *addr_ptr, uint32_t size) {
    if (!mem.use_page_table) {
        Address addr;
        if (mem_host_to_guest(mem, addr_ptr, addr)) {
            erase_protections(mem, addr, size);
            unprotect_inner(mem, addr, size);
        }
        return;
    }
    const uintptr_t value = reinterpret_cast<uintptr_t>(addr_ptr);
    auto it = mem.external_mapping.find(value);
    if (it == mem.external_mapping.end())
        return;
    const MemExternalMapping mapping = it->second;
    mem.external_mapping.erase(it);
    erase_protections(mem, mapping.address, mapping.size);
    unprotect_inner(mem, mapping.address, mapping.size);
    const uint32_t first = mapping.address / STANDARD_PAGE_SIZE;
    const uint32_t count = mapping.size / STANDARD_PAGE_SIZE;
    for (uint32_t page = 0; page < count; ++page)
        mem.page_table[first + page] = mem.sparse_host_memory ? original_page_pointer(mem, first + page) : mem.memory.get();
    unprotect_inner(mem, mapping.address, mapping.size);
    for (uint32_t page = 0; page < count; ++page)
        std::memcpy(original_page_pointer(mem, first + page), addr_ptr + size_t(page) * STANDARD_PAGE_SIZE, STANDARD_PAGE_SIZE);
}

Address alloc(MemState &state, uint32_t size, const char *name, Address start_addr) {
    const std::lock_guard<std::mutex> lock(state.generation_mutex);
    const uint32_t page_count = align(uint64_t(size), STANDARD_PAGE_SIZE) / STANDARD_PAGE_SIZE;
    const Address addr = alloc_inner(state, start_addr / STANDARD_PAGE_SIZE, page_count, name, false);
    return addr;
}

Address alloc_at(MemState &state, Address address, uint32_t size, const char *name) {
    auto addr = try_alloc_at(state, address, size, name);
    LOG_CRITICAL_IF(addr == 0, "Failed to allocate at specific page. Memory address:{}, size:{}, name:{}", log_hex(address), log_hex(size), name);
    return addr;
}

Address try_alloc_at(MemState &state, Address address, uint32_t size, const char *name) {
    const std::lock_guard<std::mutex> lock(state.generation_mutex);
    if (!size || address < state.host_page_size || uint64_t(address) + size > TOTAL_MEM_SIZE)
        return 0;
    const uint32_t wanted_page = address / STANDARD_PAGE_SIZE;
    const uint64_t rounded_size = uint64_t(size) + address % STANDARD_PAGE_SIZE;
    const uint32_t page_count = align(rounded_size, STANDARD_PAGE_SIZE) / STANDARD_PAGE_SIZE;
    const Address addr = alloc_inner(state, wanted_page, page_count, name, true);
    return addr ? address : 0;
}

Block alloc_block(MemState &mem, uint32_t size, const char *name, Address start_addr) {
    const Address address = alloc(mem, size, name, start_addr);
    return Block(address, [&mem](Address stack) {
        free(mem, stack);
    });
}

void free(MemState &state, Address address) {
    const std::lock_guard<std::mutex> lock(state.generation_mutex);
    const uint32_t page_num = address / STANDARD_PAGE_SIZE;
    if (!state.alloc_table || address < state.host_page_size)
        return;
    AllocMemPage &page = state.alloc_table[page_num];
    if (!page.allocated) {
        LOG_CRITICAL("Freeing unallocated page");
        return;
    }
    const uint32_t page_count = page.size;
    const Address region_start = page_num * STANDARD_PAGE_SIZE;
    const uint64_t region_end = uint64_t(region_start) + uint64_t(page_count) * STANDARD_PAGE_SIZE;
    // Detach any external mappings before deleting their original owner.
    for (auto it = state.external_mapping.begin(); it != state.external_mapping.end();) {
        auto current = it++;
        if (uint64_t(current->second.address) < region_end && uint64_t(region_start) < uint64_t(current->second.address) + current->second.size)
            remove_external_mapping(state, reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(current->first)), current->second.size);
    }
    erase_protections(state, region_start, static_cast<uint32_t>(region_end - region_start));
    page = {};
    state.allocator.free(page_num, page_count);
    std::fill_n(state.page_permissions.get() + page_num, page_count, MemPerm::None);
    // Unmapping removes Execute as mem_set_permissions does: cached
    // translations of these pages must not run again.
    if (g_mem_write_observer)
        g_mem_write_observer(region_start, static_cast<size_t>(region_end - region_start));
    if (PAGE_NAME_TRACKING)
        state.page_name_map.erase(page_num);
    if (state.sparse_host_memory) {
        std::fill_n(state.page_table.get() + page_num, page_count, nullptr);
        state.sparse_allocations.erase(page_num);
        return;
    }

#ifndef __EMSCRIPTEN__
    uint64_t host_page = align_down(uint64_t(region_start), state.host_page_size);
    uint64_t batch_start = 0;
    size_t batch_size = 0;

    while (host_page < region_end) {
        uint64_t host_page_end = host_page + state.host_page_size;
        uint32_t first_guest = host_page / STANDARD_PAGE_SIZE;
        uint32_t last_guest = host_page_end / STANDARD_PAGE_SIZE;

        if (state.allocator.free_slot_count(first_guest, last_guest) == static_cast<int>(last_guest - first_guest)) {
            if (batch_size == 0)
                batch_start = host_page;
            batch_size += state.host_page_size;
        } else if (batch_size > 0) {
            uint8_t *memory = &state.memory[batch_start];
#ifdef _WIN32
            const BOOL ret = VirtualFree(memory, batch_size, MEM_DECOMMIT);
            LOG_CRITICAL_IF(!ret, "VirtualFree failed: {}", get_error_msg());
#else
            int ret = mprotect(memory, batch_size, PROT_NONE);
            LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
            ret = madvise(memory, batch_size, MADV_DONTNEED);
            LOG_CRITICAL_IF(ret == -1, "madvise failed: {}", get_error_msg());
#endif
            batch_size = 0;
        }
        host_page = host_page_end;
    }

    if (batch_size > 0) {
        uint8_t *memory = &state.memory[batch_start];
#ifdef _WIN32
        const BOOL ret = VirtualFree(memory, batch_size, MEM_DECOMMIT);
        LOG_CRITICAL_IF(!ret, "VirtualFree failed: {}", get_error_msg());
#else
        int ret = mprotect(memory, batch_size, PROT_NONE);
        LOG_CRITICAL_IF(ret == -1, "mprotect failed: {}", get_error_msg());
        ret = madvise(memory, batch_size, MADV_DONTNEED);
        LOG_CRITICAL_IF(ret == -1, "madvise failed: {}", get_error_msg());
#endif
    }
#endif
}

uint32_t mem_available(MemState &state) {
    if (!state.alloc_table)
        return 0;
    return state.allocator.free_slot_count(0, state.allocator.max_offset) * STANDARD_PAGE_SIZE;
}

const char *mem_name(Address address, MemState &state) {
    if (PAGE_NAME_TRACKING) {
        auto it = state.page_name_map.find(address / STANDARD_PAGE_SIZE);
        if (it != state.page_name_map.end())
            return it->second.c_str();
    }
    return "";
}

void deinit_mem(MemState &state) {
    const std::lock_guard<std::mutex> gen_lock(state.generation_mutex);

    // Release protections on borrowed buffers while their owners still exist.
    while (!state.external_mapping.empty()) {
        const auto it = state.external_mapping.begin();
        remove_external_mapping(state, reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(it->first)), it->second.size);
    }
    {
        const std::lock_guard<std::mutex> prot_lock(state.protect_mutex);
        state.protect_tree.clear();
    }

    state.sparse_allocations.clear();
    state.page_permissions.reset();
    state.write_epochs.reset();
    state.sparse_host_memory = false;
    state.direct_host_memory = false;
#ifdef VITA3K_WEB_MEMORY64
    MemState *expected = &state;
    direct_window_owner.compare_exchange_strong(expected, nullptr);
#endif
    state.memory.reset();
    state.alloc_table.reset();
    state.allocator.reset();
    state.page_name_map.clear();
    state.page_table.reset();
    state.external_mapping.clear();
    state.use_page_table = false;
    state.host_page_size = 0;
}

#ifdef _WIN32

static LONG WINAPI exception_handler(PEXCEPTION_POINTERS pExp) noexcept {
    if (pExp->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT && IsDebuggerPresent()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto ptr = reinterpret_cast<uint8_t *>(pExp->ExceptionRecord->ExceptionInformation[1]);
    const bool is_writing = pExp->ExceptionRecord->ExceptionInformation[0] == 1;
    const bool is_executing = pExp->ExceptionRecord->ExceptionInformation[0] == 8;

    if (pExp->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && !is_executing) {
        if (access_violation_handler(ptr, is_writing)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

static void register_access_violation_handler(const AccessViolationHandler &handler) {
    access_violation_handler = handler;
    if (!AddVectoredExceptionHandler(1, exception_handler)) {
        LOG_CRITICAL("Failed to register an exception handler");
    }
}

#elif !defined(__EMSCRIPTEN__)

static void signal_handler(int, siginfo_t *info, void *uct) noexcept {
    auto context = static_cast<ucontext_t *>(uct);

#ifdef __aarch64__
#ifdef __APPLE__
    const uint32_t esr = context->uc_mcontext->__es.__esr;
#else
    _aarch64_ctx *ctx = reinterpret_cast<_aarch64_ctx *>(context->uc_mcontext.__reserved);
    // get the ESR register
    while (ctx->magic != ESR_MAGIC) {
        if (ctx->magic == 0)
            [[unlikely]]
            raise(SIGTRAP);
        else
            [[likely]]
            ctx = reinterpret_cast<_aarch64_ctx *>(reinterpret_cast<uint8_t *>(ctx) + ctx->size);
    }

    const uint64_t esr = reinterpret_cast<esr_context *>(ctx)->esr;
#endif
    // https://developer.arm.com/documentation/ddi0595/2021-03/AArch64-Registers/ESR-EL1--Exception-Syndrome-Register--EL1-
    const uint32_t exception_class = static_cast<uint32_t>(esr) >> 26;
    const bool is_executing = (exception_class == 0b100000) || (exception_class == 0b100001);
    const bool is_data_abort = (exception_class == 0b100100) || (exception_class == 0b100101);
    const bool is_writing = is_data_abort && (esr & (1 << 6));
#else
#ifdef __APPLE__
    const uint64_t err = context->uc_mcontext->__es.__err;
#else
    const uint64_t err = context->uc_mcontext.gregs[REG_ERR];
#endif
    const bool is_executing = err & 0x10;
    const bool is_writing = err & 0x2;
#endif

    if (!is_executing) {
        if (access_violation_handler(reinterpret_cast<uint8_t *>(info->si_addr), is_writing)) {
            return;
        }
    }

    LOG_CRITICAL("Unhandled access to 0x{:X}", reinterpret_cast<uintptr_t>(info->si_addr));
    raise(SIGTRAP);
    return;
}

static void register_access_violation_handler(const AccessViolationHandler &handler) {
    access_violation_handler = handler;
    struct sigaction sa;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = signal_handler;
    if (sigaction(SIGSEGV, &sa, NULL) == -1) {
        LOG_CRITICAL("Failed to register an exception handler");
    }
#ifdef __APPLE__
    // When accessing memory region which is PROT_NONE on macOS, it is raising SIGBUS not SIGSEGV.
    // So apply same signal handler to SIGBUS
    if (sigaction(SIGBUS, &sa, NULL) == -1) {
        LOG_CRITICAL("Failed to register an exception handler to SIGBUS");
    }
#endif
}

#endif
