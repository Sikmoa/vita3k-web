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

#include <kernel/state.h>

// Firmware 3.74 acmgr: writes ksceSblACMgrIsGameProgram of the caller.
EXPORT(int, _sceSblACMgrIsGameProgram, SceInt32 *result) {
    constexpr uint32_t SCE_SBL_ERROR_INVALID_ARGUMENT = 0x800F0916; // name unknown
    if (!result)
        return RET_ERROR(SCE_SBL_ERROR_INVALID_ARGUMENT);
    *result = emuenv.kernel.process_is_game_program();
    return 0;
}
