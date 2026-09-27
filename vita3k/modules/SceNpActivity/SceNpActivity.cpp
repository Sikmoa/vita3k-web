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

#include <module/module.h>

EXPORT(int, sceNpActivityInit) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpActivityPostAppStartupStatus) {
    return UNIMPLEMENTED();
}

// Firmware 3.74 np_activity_sdk checks its initialized flag first; only
// sceNpActivityInit (not implemented here) sets it.
EXPORT(int, sceNpActivityPostStatus, const char *message, const char *url, Ptr<const void> option) {
    constexpr uint32_t SCE_NP_ACTIVITY_ERROR_NOT_INITIALIZED = 0x80552302;
    return RET_ERROR(SCE_NP_ACTIVITY_ERROR_NOT_INITIALIZED);
}

EXPORT(int, sceNpActivityTerm) {
    return UNIMPLEMENTED();
}
