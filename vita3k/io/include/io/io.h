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

constexpr int SCE_ERROR_ERRNO_ENOENT = 0x80010002; // Associated file or directory does not exist
constexpr int SCE_ERROR_ERRNO_EEXIST = 0x80010011; // File exists
constexpr int SCE_ERROR_ERRNO_EMFILE = 0x80010018; // Too many files are open
constexpr int SCE_ERROR_ERRNO_EBADFD = 0x80010051; // File descriptor is invalid for this operation
constexpr int SCE_ERROR_ERRNO_EOPNOTSUPP = 0x8001005F; // Operation not supported
// Firmware 3.74 iofilemgr, exfatfs and PfsMgr answers.
constexpr int SCE_ERROR_ERRNO_EPERM = 0x80010001; // PfsMgr refuses the operation on the mount
constexpr int SCE_ERROR_ERRNO_EBADF = 0x80010009;
constexpr int SCE_ERROR_ERRNO_EACCES = 0x8001000D;
constexpr int SCE_ERROR_ERRNO_EFAULT = 0x8001000E;
constexpr int SCE_ERROR_ERRNO_ENODEV = 0x80010013;
constexpr int SCE_ERROR_ERRNO_EISDIR = 0x80010015;
#define SCE_ERROR_ERRNO_EINVAL 0x80010016 // as kernel/types.h
constexpr int SCE_ERROR_ERRNO_ENOSPC = 0x8001001C;
constexpr int SCE_ERROR_ERRNO_EROFS = 0x8001001E;
constexpr int SCE_ERROR_ERRNO_ENAMETOOLONG = 0x8001005B;
constexpr int SCE_ERROR_ERRNO_ENOBUFS = 0x80010069; // the file has no buffer cache
constexpr int SCE_ERROR_ERRNO_ENOTSUP = 0x80010030; // no such command for the device
constexpr int SCE_KERNEL_ERROR_UNTERMINATED_STRING = 0x8002710B; // a user path longer than the kernel copies
