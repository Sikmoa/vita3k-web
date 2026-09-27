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

#include "kernel/state.h"

#include <module/module.h>
#include <util/tracy.h>

TRACY_MODULE_NAME(SceProcessmgrForDriver)

EXPORT(int, ksceKernelCreateProcessLocalStorage, const char *name, SceSize size) {
    TRACY_FUNC(ksceKernelCreateProcessLocalStorage, name, size);
    // >> 1 to make result positive (not error).
    // result of memory allocation is always aligned (I think), so lowest bite is always 0
    // kubridge uses this for per-process exception handler context.
    auto pls_data = alloc(emuenv.mem, size, name) >> 1;
    return pls_data;
}

// Firmware 3.74 processmgr 0x8100618d / 0x810060ed: set a process field only
// when DIPSW bit 0xE4 is on, else fail with 0x80029008. The DIPSW table of
// the modelled retail console is clear (ksceKernelCheckDipsw).
EXPORT(int, SceProcessmgrForDriver_61B9B6FA, SceUID pid, SceUInt32 value) {
    constexpr uint32_t dipsw_off = 0x80029008; // name unknown
    return RET_ERROR(dipsw_off);
}

EXPORT(int, SceProcessmgrForDriver_B1C3EFCA, SceUID pid, SceUInt32 value) {
    constexpr uint32_t dipsw_off = 0x80029008; // name unknown
    return RET_ERROR(dipsw_off);
}

// Firmware 3.74 processmgr resolves pid 0 (and its own pid) to the calling
// process; any other is 0x80029001 here, where there is one process.
static bool is_own_process(SceUID pid) {
    return pid == 0 || pid == KernelState::process_id;
}

// Processmgr 0x81000a69: the SDK version from the executable's
// SceProcessParam (sysmodule compares it when loading NP message).
EXPORT(int, SceProcessmgrForDriver_D141C076, SceUID pid, SceUInt32 *sdk_version) {
    TRACY_FUNC(SceProcessmgrForDriver_D141C076, pid, sdk_version);
    if (!is_own_process(pid))
        return RET_ERROR(SCE_KERNEL_ERROR_INVALID_PID);
    const SceProcessParam *param = emuenv.kernel.process_param.get(emuenv.mem);
    *sdk_version = param ? param->fw_version : 0;
    return 0;
}

// Processmgr 0x8100590d: writes the process's PMUSERENR word in SceLibKernel
// (sysmodule sets it around loading libperf).
EXPORT(int, SceProcessmgrForDriver_6599E5D9, SceUID pid, SceUInt32 value) {
    TRACY_FUNC(SceProcessmgrForDriver_6599E5D9, pid, value);
    if (!is_own_process(pid))
        return RET_ERROR(SCE_KERNEL_ERROR_INVALID_PID);
    emuenv.kernel.pmuserenr = value;
    return 0;
}

EXPORT(int, ksceKernelGetProcessInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetProcessLocalStorageAddr) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetProcessLocalStorageAddrForPid, SceUID pid, int key, Ptr<void> *out_addr, int create_if_doesnt_exist) {
    TRACY_FUNC(ksceKernelGetProcessLocalStorageAddrForPid, pid, key, out_addr, create_if_doesnt_exist);
    // See ksceKernelCreateProcessLocalStorage
    *out_addr = key << 1;
    return 0;
}

EXPORT(int, ksceKernelGetProcessStatus) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetProcessTimeCore) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetProcessTimeLowCore) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetProcessTimeWideCore) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetRemoteProcessTime) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelIsCDialogAvailable) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelIsGameBudget) {
    return UNIMPLEMENTED();
}

// kubridge-required stubs
EXPORT(SceUID, ksceKernelAllocRemoteProcessHeap, SceUID pid, uint32_t size, Ptr<void> opt) {
    TRACY_FUNC(ksceKernelAllocRemoteProcessHeap, pid, size, opt);
    STUBBED("ksceKernelAllocRemoteProcessHeap");
    return emuenv.kernel.get_next_uid();
}

EXPORT(int, ksceKernelFreeRemoteProcessHeap, SceUID pid, Ptr<void> ptr) {
    TRACY_FUNC(ksceKernelFreeRemoteProcessHeap, pid, ptr);
    STUBBED("ksceKernelFreeRemoteProcessHeap");
    return 0;
}
