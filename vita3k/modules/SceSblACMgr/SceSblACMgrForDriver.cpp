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

// Firmware 3.74 acmgr.skprx predicates of the calling process (pid 0) that
// depend only on its program authority id or the DIPSW table. The ones over
// the SELF capability bits (IsSystemProgram, IsNonGameProgram) are not
// implemented: those bits are in the title's encrypted signed metadata.

EXPORT(int, ksceSblACMgrIsDevelopmentMode) {
    return 0; // ksceKernelCheckDipsw(0x9F); the retail DIPSW table is clear
}

EXPORT(int, ksceSblACMgrIsGameProgram, SceUID pid) {
    return emuenv.kernel.process_is_game_program();
}

// PSM Developer Assistant: PAID 0x210000101CD20007..0x210000101CD2000A or
// 0x2800C0101CD2000B; otherwise only a fake-signed SELF on a development
// console. A retail title is neither.
EXPORT(int, ksceSblACMgrIsPSMDevAssistant, SceUID pid) {
    const uint64_t paid = emuenv.kernel.process_program_authority_id;
    return paid - 0x210000101CD20007ULL < 4 || paid == 0x2800C0101CD2000BULL;
}
