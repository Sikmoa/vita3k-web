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

#include <string>
#include <util/fs.h>
#include <vector>

struct Patch {
    uint8_t seg;
    uint32_t offset;
    std::vector<uint8_t> values;
};

struct PatchHeader {
    std::string titleid;
    std::string bin;
};

using Patches = std::vector<Patch>;

struct MemState;

// A loaded module segment (SceKernelSegmentInfo vaddr and memsz).
struct PatchSegment {
    uint32_t vaddr;
    uint32_t memsz;
};

// Guest bytes one patch replaced.
struct PatchedRange {
    uint32_t address;
    uint32_t size;
};

Patches get_patches(fs::path &path, const std::string &titleid, const std::string &bin);
Patch parse_patch(const std::string &patch);
// Writes each patch into its segment; a patch outside its segment is logged
// and skipped. Returns the ranges written, which a JIT must invalidate.
std::vector<PatchedRange> apply_patches(MemState &mem, const Patches &patches, const std::vector<PatchSegment> &segments);
