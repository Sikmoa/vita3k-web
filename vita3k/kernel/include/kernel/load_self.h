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

#include <util/fs.h>
#include <util/types.h>

#include <cstddef>
#include <string>

struct KernelState;
struct MemState;
struct KernelModule;

SceUID load_self(KernelState &kernel, MemState &mem, const void *self, const std::string &self_path, const fs::path &dump_path);
// Size-aware entry point for byte-oriented/browser transports; null/empty input
// is rejected. Checks image and module/link-table bounds, not all relocation
// semantics or subsequent guest execution. This is not a hostile-code sandbox.
// Only the legacy load_self wrapper retains the trusted, unknown-size contract.
SceUID load_self_sized(KernelState &kernel, MemState &mem, const void *self, std::size_t self_size, const std::string &self_path, const fs::path &dump_path);
int unload_self(KernelState &kernel, MemState &mem, KernelModule &module);
