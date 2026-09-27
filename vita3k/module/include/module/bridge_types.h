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

#include <mem/ptr.h>

#include <cstdint>
#include <type_traits>

struct MemState;

// The guest's long (and so size_t) is 32-bit; the host's is 64-bit on LP64 hosts
// (wasm64, x86-64). Guest-facing types must name the guest width. Where long is
// also a fixed-width type (int64_t on LP64 Linux) the two cannot be told apart.
template <typename T>
inline constexpr bool is_host_long_v
    = (std::is_same_v<std::remove_cv_t<T>, long> && !std::is_same_v<long, std::int32_t> && !std::is_same_v<long, std::int64_t>)
    || (std::is_same_v<std::remove_cv_t<T>, unsigned long> && !std::is_same_v<unsigned long, std::uint32_t> && !std::is_same_v<unsigned long, std::uint64_t>);

// By default, do no special conversion.
template <typename HostType>
struct BridgeTypes {
    static_assert(!is_host_long_v<HostType>, "name the guest width: long and size_t are 32-bit in the guest");
    typedef HostType ArmType;

    static HostType arm_to_host(const ArmType &t, const MemState &mem) {
        return t;
    }
};

// Convert from address in ARM register/memory to host pointer.
template <typename Pointee>
struct BridgeTypes<Pointee *> {
    static_assert(!is_host_long_v<Pointee>, "name the guest width: long and size_t are 32-bit in the guest");
    // Guest memory holds 32-bit guest addresses, never host pointers: use Ptr<T> *.
    static_assert(!std::is_pointer_v<Pointee>, "a guest pointer to a pointer points at a guest address: use Ptr<T> *");
    typedef Ptr<Pointee> ArmType;

    static Pointee *arm_to_host(const ArmType &t, const MemState &mem) {
        return t.get(mem);
    }
};
