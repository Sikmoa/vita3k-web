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

#include "../SceLibKernel/SceLibKernel.h"
#include "SceThreadmgr.h"
#include <module/module.h>
#include <util/tracy.h>

TRACY_MODULE_NAME(SceThreadmgrForDriver);

#include <kernel/state.h>
#include <kernel/sync_primitives.h>
#include <kernel/thread/thread_state.h>

#include <utility>

EXPORT(int, ksceKernelCancelCallback) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelCancelMsgPipe) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelCancelMutex, SceUID mutexId, SceInt32 newCount, SceUInt32 *pNumWaitThreads) {
    TRACY_FUNC(ksceKernelCancelMutex, mutexId, newCount, pNumWaitThreads);
    return mutex_cancel(emuenv.kernel, export_name, thread_id, mutexId, newCount, pNumWaitThreads);
}

EXPORT(int, ksceKernelChangeCurrentThreadAttr) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelChangeThreadCpuAffinityMask) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelChangeThreadPriority) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelChangeThreadSuspendStatus) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelClearEvent) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelClearEventFlag) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelCreateCallback) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelCreateCond) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelCreateEventFlag) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelCreateMsgPipe) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelCreateMutex, const char *name, SceUInt attr, int init_count, SceKernelMutexOptParam *opt_param) {
    TRACY_FUNC(ksceKernelCreateMutex, name, attr, init_count, opt_param);
    return CALL_EXPORT(_sceKernelCreateMutex, name, attr, init_count, opt_param);
}

EXPORT(int, ksceKernelCreateSema) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelCreateSimpleEvent) {
    return UNIMPLEMENTED();
}

EXPORT(SceUID, ksceKernelCreateThread, const char *name, SceKernelThreadEntry entry, int initPriority, SceSize stackSize, SceUInt attr, int cpuAffinityMask, void *pOptParam) {
    const ThreadStatePtr thread = emuenv.kernel.create_thread(emuenv.mem, name, entry.cast<void>(), initPriority, cpuAffinityMask, stackSize, nullptr);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    return thread->id;
}

EXPORT(int, ksceKernelDeleteCallback) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelDeleteCond) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelDeleteEventFlag) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelDeleteFastMutex) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelDeleteMsgPipe) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelDeleteMutex, SceUID mutexid) {
    TRACY_FUNC(ksceKernelDeleteMutex, mutexid);
    return CALL_EXPORT(sceKernelDeleteMutex, mutexid);
}

EXPORT(int, ksceKernelDeleteSema) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelDeleteThread) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelEnqueueWorkQueue) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetCallbackCount) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetMutexInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetProcessIdFromTLS) {
    return UNIMPLEMENTED();
}

// Firmware 3.74 exports this NID to user code too (SceThreadmgr): the low
// word of the counter sceKernelGetSystemTimeWide reads.
EXPORT(SceUInt32, ksceKernelGetSystemTimeLow) {
    TRACY_FUNC(ksceKernelGetSystemTimeLow);
    return static_cast<SceUInt32>(CALL_EXPORT(sceKernelGetSystemTimeWide));
}

EXPORT(int, ksceKernelGetTLSAddr) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetThreadCpuAffinityMask) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetThreadCpuRegisters) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetThreadCurrentPriority) {
    TRACY_FUNC(ksceKernelGetThreadCurrentPriority);
    return CALL_EXPORT(sceKernelGetThreadCurrentPriority);
}

EXPORT(int, ksceKernelGetThreadId) {
    TRACY_FUNC(ksceKernelGetThreadId);
    return thread_id;
}

EXPORT(int, ksceKernelGetThreadIdList) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetThreadInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetThreadStackFreeSize) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetThreadTLSAddr) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetThreadmgrUIDClass) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetTimerBaseWide) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelGetTimerTimeWide) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelInitializeFastMutex) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelLockFastMutex) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelLockMutex, SceUID mutexid, int lock_count, unsigned int *timeout) {
    TRACY_FUNC(ksceKernelLockMutex, mutexid, lock_count, timeout);
    return CALL_EXPORT(_sceKernelLockMutex, mutexid, lock_count, timeout);
}

EXPORT(int, ksceKernelLockMutexCB_089) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelNotifyCallback) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelPollEventFlag) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelPollSema) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelPulseEvent) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelPulseEventWithNotifyCallback) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelReceiveMsgPipeVector) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelReceiveMsgPipeVectorCB) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelRegisterCallbackToEvent) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelRegisterTimer) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelRunWithStack) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelSendMsgPipeVector) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelSetEvent) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelSetEventFlag) {
    return UNIMPLEMENTED();
}

// Swaps the calling thread's permission and returns the previous one.
EXPORT(SceInt32, ksceKernelSetPermission, SceInt32 permission) {
    TRACY_FUNC(ksceKernelSetPermission, permission);
    if (permission < 0)
        return RET_ERROR(SCE_KERNEL_ERROR_INVALID_ARGUMENT);
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
    const std::lock_guard<std::mutex> thread_lock(thread->mutex);
    return std::exchange(thread->permission, permission);
}

EXPORT(int, ksceKernelSetProcessId) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelSetTimerTimeWide) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelSignalCond) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelSignalCondAll) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelSignalCondTo) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelSignalSema) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelStartThread, SceUID thid, SceSize arglen, Ptr<void> argp) {
    return CALL_EXPORT(_sceKernelStartThread, thid, arglen, argp);
}

EXPORT(int, ksceKernelStartTimer) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelStopTimer) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelTryLockMutex) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelTryLockReadRWLock) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelTryLockWriteRWLock) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelTryReceiveMsgPipeVector) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelTrySendMsgPipeVector) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelUnlockFastMutex) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelUnlockMutex, SceUID mutexid, int unlock_count) {
    TRACY_FUNC(ksceKernelUnlockMutex, mutexid, unlock_count);
    return CALL_EXPORT(sceKernelUnlockMutex, mutexid, unlock_count);
}

EXPORT(int, ksceKernelUnlockReadRWLock) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelUnlockWriteRWLock) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelUnregisterCallbackFromEvent) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelUnregisterCallbackFromEventAll) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelWaitCond) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelWaitEvent) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelWaitEventCB) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelWaitEventFlag) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelWaitEventFlagCB) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelWaitSema) {
    return UNIMPLEMENTED();
}

EXPORT(int, ksceKernelWaitThreadEnd, SceUID thid, int *stat, SceUInt *timeout) {
    return CALL_EXPORT(_sceKernelWaitThreadEnd, thid, stat, timeout);
}

EXPORT(int, ksceKernelWaitThreadEndCB) {
    return UNIMPLEMENTED();
}

// Whether the calling thread is running its callbacks (SceKernelThreadMgr 3.74
// sets that thread state bit when it diverts a thread into its callbacks and
// clears it in _sceKernelExitCallback); 0 without a calling thread.
EXPORT(int, SceThreadmgrForDriver_20C228E4) {
    TRACY_FUNC(SceThreadmgrForDriver_20C228E4);
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    return thread && thread->is_processing_callbacks;
}

// Firmware 3.74 threadmgr 0x81013405 / 0x81013329: store a value in each
// thread of the process and in the CP15 performance-monitor register
// (PMUSERENR / PMCR). The emulated CPU has no performance monitor.
EXPORT(int, SceThreadmgrForDriver_1AAFA818, SceUID pid, SceUInt32 value) {
    return 0;
}

EXPORT(int, SceThreadmgrForDriver_5053B005, SceUID pid, SceUInt32 value) {
    return 0;
}
