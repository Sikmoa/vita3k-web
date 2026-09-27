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

#include <module/vargs.h>

#include <cstdint>
#include <optional>
#include <string>

namespace module {

// Formats a guest printf format string. Every argument is read from `args` with the
// guest's ARM EABI type (long, size_t, ptrdiff_t and pointers are 32-bit; long long,
// intmax_t and double are 64-bit), never with the host's C type. Returns nullopt, after
// logging, for a conversion it cannot perform faithfully (e.g. wide characters).
std::optional<std::string> format_guest(const char *format, CPUState &cpu, MemState &mem, vargs &args);

// C snprintf contract over format_guest: stores at most count - 1 characters and a
// terminating NUL when count > 0, and returns the untruncated length, or -1 when
// format_guest fails.
int snprintf_guest(char *buffer, std::uint32_t count, const char *format, CPUState &cpu, MemState &mem, vargs &args);

} // namespace module
