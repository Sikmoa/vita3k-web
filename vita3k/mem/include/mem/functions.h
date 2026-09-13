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

#pragma once

#include <mem/block.h>
#include <mem/util.h>

#include <functional>

struct MemState;

typedef std::function<bool(uint8_t *addr, bool write)> AccessViolationHandler;

constexpr Address user_main_memory_start = 0x80000000U;

// Guest access bits. Native host protection cannot enforce WriteOnly or execute
// permission (guest instructions are interpreted/JIT compiled, not host code).
// Checked accesses enforce all three bits on every touched 4 KiB guest page.
enum struct MemPerm : uint8_t {
    None = 0,
    ReadOnly = 1 << 0,
    WriteOnly = 1 << 1,
    ReadWrite = 3,
    Execute = 1 << 2,
    ReadExecute = 5,
    ReadWriteExecute = 7
};

constexpr MemPerm most_restrictive_perm(MemPerm a, MemPerm b) {
    return static_cast<MemPerm>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

// Interpreter access API. Validates the WHOLE range before copying, including
// allocation, overflow and permissions; failure does not modify the destination.
// Cross-page/cross-allocation access is supported. Zero bytes is a successful
// no-op, even for a null address/buffer. Fetch requires Execute, not ReadOnly.
// These do not dispatch native fault/watch callbacks: denied access returns false.
// Like Ptr, access must be serialized against allocation/protection changes.
bool mem_read(const MemState &state, Address addr, void *destination, size_t size);
bool mem_write(MemState &state, Address addr, const void *source, size_t size);
bool mem_fetch(const MemState &state, Address addr, void *destination, size_t size);
// Page-granular, outward-rounded guest permissions; default allocation is RWX.
// Rejects invalid/unallocated ranges without changing permissions.
bool mem_set_permissions(MemState &state, Address addr, size_t size, MemPerm perm);
// Safe reverse translation. Unrelated, freed, null and shadowed backing pointers
// fail and set addr to zero. Never subtracts unrelated C++ pointers.
bool mem_host_to_guest(const MemState &state, const void *pointer, Address &addr);

bool init(MemState &state, const bool use_page_table);
void deinit_mem(MemState &state);
Address alloc(MemState &state, uint32_t size, const char *name, Address start_addr = user_main_memory_start);
Address alloc_aligned(MemState &state, uint32_t size, const char *name, unsigned int alignment, Address start_addr = user_main_memory_start);
void protect_inner(MemState &state, Address addr, uint32_t size, const MemPerm perm);
void unprotect_inner(MemState &state, Address addr, uint32_t size);
bool add_protect(MemState &state, Address addr, const uint32_t size, const MemPerm perm, const ProtectCallback &callback);
void open_access_parent_protect_segment(MemState &state, Address addr);
void close_access_parent_protect_segment(MemState &state, Address addr);
void add_external_mapping(MemState &mem, Address addr, uint32_t size, uint8_t *addr_ptr);
void remove_external_mapping(MemState &mem, uint8_t *addr_ptr, uint32_t size);
bool is_protecting(MemState &state, Address addr, MemPerm *perm = nullptr);
bool is_valid_addr(const MemState &state, Address addr);
bool is_valid_addr_range(const MemState &state, Address start, Address end);
bool handle_access_violation(MemState &state, uint8_t *addr, bool write) noexcept;
Block alloc_block(MemState &mem, uint32_t size, const char *name, Address start_addr = user_main_memory_start);
Address alloc_at(MemState &state, Address address, uint32_t size, const char *name);
Address try_alloc_at(MemState &state, Address address, uint32_t size, const char *name);
void free(MemState &state, Address address);
uint32_t mem_available(MemState &state);
const char *mem_name(Address address, MemState &state);
