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

#include "SceThreadmgr.h"
#include "../SceKernelModulemgr/SceModulemgr.h"
#include <modules/module_parent.h>

#include <kernel/callback.h>
#include <kernel/state.h>
#include <kernel/sync_primitives.h>
#include <kernel/types.h>
#include <packages/functions.h>

#include <util/lock_and_find.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <thread>

#include <util/tracy.h>
TRACY_MODULE_NAME(SceThreadmgr);

inline static uint64_t get_current_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch())
        .count();
}

EXPORT(int, __sceKernelCreateLwMutex, Ptr<SceKernelLwMutexWork> workarea, const char *name, unsigned int attr, Ptr<SceKernelCreateLwMutex_opt> opt) {
    TRACY_FUNC(__sceKernelCreateLwMutex, workarea, name, attr, opt);
    assert(name != nullptr);
    assert(opt.get(emuenv.mem)->init_count >= 0);

    auto uid_out = &workarea.get(emuenv.mem)->uid;
    return mutex_create(uid_out, emuenv.kernel, emuenv.mem, export_name, name, thread_id, attr, opt.get(emuenv.mem)->init_count, workarea, SyncWeight::Light);
}

EXPORT(int, _sceKernelCancelEvent) {
    TRACY_FUNC(_sceKernelCancelEvent);
    return UNIMPLEMENTED();
}

// The ThreadMgr cancel syscalls copy the woken-thread count out even when the
// cancel fails; the caller's word then reads 0.
template <typename Cancel>
static int cancel_and_report(SceUInt32 *num_wait_threads, Cancel cancel) {
    SceUInt32 count = 0;
    const int result = cancel(&count);
    if (num_wait_threads)
        *num_wait_threads = count;
    return result;
}

EXPORT(SceInt32, _sceKernelCancelEventFlag, SceUID event_id, SceUInt pattern, SceUInt32 *num_wait_thread) {
    TRACY_FUNC(_sceKernelCancelEventFlag, event_id, pattern, num_wait_thread);
    return cancel_and_report(num_wait_thread, [&](SceUInt32 *count) {
        return eventflag_cancel(emuenv.kernel, export_name, thread_id, event_id, pattern, count);
    });
}

EXPORT(int, _sceKernelCancelEventWithSetPattern) {
    TRACY_FUNC(_sceKernelCancelEventWithSetPattern);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelCancelMsgPipe) {
    TRACY_FUNC(_sceKernelCancelMsgPipe);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelCancelMutex, SceUID mutexId, SceInt32 newCount, SceUInt32 *pNumWaitThreads) {
    TRACY_FUNC(_sceKernelCancelMutex, mutexId, newCount, pNumWaitThreads);
    return cancel_and_report(pNumWaitThreads, [&](SceUInt32 *count) {
        return mutex_cancel(emuenv.kernel, export_name, thread_id, mutexId, newCount, count);
    });
}

EXPORT(int, _sceKernelCancelRWLock) {
    TRACY_FUNC(_sceKernelCancelRWLock);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelCancelSema, SceUID semaId, SceInt32 setCount, SceUInt32 *pNumWaitThreads) {
    TRACY_FUNC(_sceKernelCancelSema, semaId, setCount, pNumWaitThreads);
    return cancel_and_report(pNumWaitThreads, [&](SceUInt32 *count) {
        return semaphore_cancel(emuenv.kernel, export_name, thread_id, semaId, setCount, count);
    });
}

EXPORT(int, _sceKernelCancelTimer) {
    TRACY_FUNC(_sceKernelCancelTimer);
    return UNIMPLEMENTED();
}

// SceKernelThreadMgr 3.74: the name and the mutex handle are checked before
// the calling thread and the attributes. A user condition takes TH_PRIO and
// OPENABLE only, and OPENABLE only in titles built before SDK 2.10; the
// options are at most their size word.
EXPORT(SceUID, _sceKernelCreateCond, const char *pName, SceUInt32 attr, SceUID mutexId, const SceKernelCondOptParam *pOptParam) {
    TRACY_FUNC(_sceKernelCreateCond, pName, attr, mutexId, pOptParam);
    constexpr SceUInt32 openable = 0x80;
    if (!pName)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    if (!lock_and_find(mutexId, emuenv.kernel.mutexes, emuenv.kernel.mutex))
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID);
    if (!emuenv.kernel.get_thread(thread_id))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
    if (attr & ~(SCE_KERNEL_ATTR_TH_PRIO | openable))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ATTR);
    if ((attr & openable) && emuenv.kernel.process_sdk_version >= 0x02100000)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ATTR);
    if (pOptParam && pOptParam->size > sizeof(SceKernelCondOptParam))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_SIZE);

    SceUID uid;

    if (auto error = condvar_create(&uid, emuenv.kernel, export_name, pName, thread_id, attr, mutexId, Ptr<SceKernelLwCondWork>{}, SyncWeight::Heavy)) {
        return error;
    }

    return uid;
}

EXPORT(SceUID, _sceKernelCreateEventFlag, const char *pName, SceUInt32 attr, SceUInt32 initPattern, const SceKernelEventFlagOptParam *pOptParam) {
    TRACY_FUNC(_sceKernelCreateEventFlag, pName, attr, initPattern, pOptParam);
    return eventflag_create(emuenv.kernel, export_name, thread_id, pName, attr, initPattern);
}

EXPORT(int, _sceKernelCreateLwCond, Ptr<SceKernelLwCondWork> workarea, const char *name, SceUInt attr, Ptr<SceKernelCreateLwCond_opt> opt) {
    TRACY_FUNC(_sceKernelCreateLwCond, workarea, name, attr, opt);
    // Checked in SceKernelThreadMgr 3.74 order: the copied-in options, the
    // pointers, the attributes, the option size and then the mutex.
    const SceKernelCreateLwCond_opt *options = opt.get(emuenv.mem);
    if (!options)
        return RET_ERROR(SCE_KERNEL_ERROR_INVALID_MEMORY_ACCESS);
    if (!workarea || !name || !options->workarea_mutex)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    if (attr & ~SCE_KERNEL_ATTR_TH_PRIO)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ATTR);
    if (options->opt_param && options->opt_param.get(emuenv.mem)->size > sizeof(SceKernelLwCondOptParam))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_SIZE);

    const SceUID assoc_mutex_uid = options->workarea_mutex.get(emuenv.mem)->uid;
    return condvar_create(&workarea.get(emuenv.mem)->uid, emuenv.kernel, export_name, name, thread_id, attr, assoc_mutex_uid, workarea, SyncWeight::Light);
}

EXPORT(int, _sceKernelCreateMsgPipeWithLR) {
    TRACY_FUNC(_sceKernelCreateMsgPipeWithLR);
    return UNIMPLEMENTED();
}

// SceKernelThreadMgr 3.74: the name, then a calling thread, the attributes,
// options of at most their 8 bytes, which a priority-ceiling mutex needs, the
// count and the ceiling. A ceiling of 0 is the creator's priority and one
// relative to the default priority is converted; it must be a user priority
// and, for a mutex the creator starts owning, not below the creator's. The
// kernel may also use attribute 0x8000 and any ceiling up to 0xfe.
SceInt32 create_mutex(EmuEnvState &emuenv, const char *export_name, SceUID thread_id, const char *name, SceUInt32 attr, int init_count,
    const SceKernelMutexOptParam *opt_param, bool kernel_caller) {
    const SceUInt32 allowed_attr = (kernel_caller ? 0x8000 : 0) | SCE_KERNEL_ATTR_TH_PRIO | SCE_KERNEL_ATTR_OPENABLE | SCE_KERNEL_MUTEX_ATTR_RECURSIVE | SCE_KERNEL_MUTEX_ATTR_CEILING;
    if (!name)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    if ((attr & SCE_KERNEL_ATTR_OPENABLE) && strlen(name) > KERNELOBJECT_MAX_NAME_LENGTH)
        return RET_ERROR(SCE_KERNEL_ERROR_UID_NAME_TOO_LONG);
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
    if (attr & ~allowed_attr)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ATTR);
    if (opt_param && opt_param->size > sizeof(SceKernelMutexOptParam))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_SIZE);
    int ceiling = 0;
    if (attr & SCE_KERNEL_MUTEX_ATTR_CEILING) {
        if (!opt_param)
            return RET_ERROR(SCE_KERNEL_ERROR_INVALID_ARGUMENT);
        if (init_count < 0 || (init_count > 1 && !(attr & SCE_KERNEL_MUTEX_ATTR_RECURSIVE)))
            return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_COUNT);
        ceiling = opt_param->ceilingPriority;
        const int relative = ceiling - SCE_KERNEL_HIGHEST_DEFAULT_PRIORITY;
        if (ceiling == 0)
            ceiling = thread->priority;
        else if (ceiling >= 1 && ceiling <= 0xfe)
            ;
        else if (relative >= 0 && relative <= SCE_KERNEL_LOWEST_DEFAULT_PRIORITY - SCE_KERNEL_HIGHEST_DEFAULT_PRIORITY)
            ceiling = ceiling - SCE_KERNEL_DEFAULT_PRIORITY + SCE_KERNEL_GAME_DEFAULT_PRIORITY_ACTUAL;
        else
            return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_PRIORITY);
        if (!kernel_caller && (ceiling < SCE_KERNEL_HIGHEST_PRIORITY_USER || ceiling > SCE_KERNEL_LOWEST_PRIORITY_USER || (init_count > 0 && ceiling > thread->priority)))
            return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_PRIORITY);
    }

    SceUID uid;
    if (auto error = mutex_create(&uid, emuenv.kernel, emuenv.mem, export_name, name, thread_id, attr, init_count, Ptr<SceKernelLwMutexWork>(0), SyncWeight::Heavy, ceiling)) {
        return error;
    }
    return uid;
}

EXPORT(int, _sceKernelCreateMutex, const char *name, SceUInt attr, int init_count, SceKernelMutexOptParam *opt_param) {
    TRACY_FUNC(_sceKernelCreateMutex, name, attr, init_count, opt_param);
    return create_mutex(emuenv, export_name, thread_id, name, attr, init_count, opt_param, false);
}

EXPORT(SceUID, _sceKernelCreateRWLock, const char *name, SceUInt32 attr, SceKernelMutexOptParam *opt_param) {
    TRACY_FUNC(_sceKernelCreateRWLock, name, attr, opt_param);
    return rwlock_create(emuenv.kernel, emuenv.mem, export_name, name, thread_id, attr);
}

EXPORT(int, _sceKernelCreateSema, const char *name, SceUInt attr, int initVal, Ptr<SceKernelCreateSema_opt> opt) {
    TRACY_FUNC(_sceKernelCreateSema, name, attr, initVal, opt);
    return semaphore_create(emuenv.kernel, export_name, name, thread_id, attr, initVal, opt.get(emuenv.mem)->maxVal);
}

EXPORT(int, _sceKernelCreateSema_16XX, const char *name, SceUInt attr, int initVal, Ptr<SceKernelCreateSema_opt> opt) {
    TRACY_FUNC(_sceKernelCreateSema_16XX, name, attr, initVal, opt);
    return semaphore_create(emuenv.kernel, export_name, name, thread_id, attr, initVal, opt.get(emuenv.mem)->maxVal);
}

EXPORT(SceUID, _sceKernelCreateSimpleEvent, const char *name, SceUInt32 attr, SceUInt32 init_pattern, const SceKernelSimpleEventOptParam *pOptParam) {
    TRACY_FUNC(_sceKernelCreateSimpleEvent, name, attr, init_pattern, pOptParam);
    return simple_event_create(emuenv.kernel, emuenv.mem, export_name, name, thread_id, attr, init_pattern);
}

EXPORT(int, _sceKernelCreateTimer, const char *name, SceUInt32 attr, const uint32_t *opt_params) {
    TRACY_FUNC(_sceKernelCreateTimer, name, attr, opt_params);
    return timer_create(emuenv.kernel, emuenv.mem, export_name, name, thread_id, attr);
}

// SceKernelThreadMgr 3.74 _sceKernelDeleteLwCond (0x8102b0d0): a null
// workarea is ILLEGAL_ADDR; a successful deletion stores -1 in the first two
// workarea words (uid and the associated LwMutex workarea).
EXPORT(int, _sceKernelDeleteLwCond, Ptr<SceKernelLwCondWork> workarea) {
    TRACY_FUNC(_sceKernelDeleteLwCond, workarea);
    if (!workarea)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    SceUID lightweight_condition_id = workarea.get(emuenv.mem)->uid;

    const int res = condvar_delete(emuenv.kernel, export_name, thread_id, lightweight_condition_id, SyncWeight::Light);
    if (res >= 0) {
        uint32_t *const words = workarea.cast<uint32_t>().get(emuenv.mem);
        words[0] = words[1] = UINT32_MAX;
    }
    return res;
}

EXPORT(int, _sceKernelDeleteLwMutex, Ptr<SceKernelLwMutexWork> workarea) {
    TRACY_FUNC(_sceKernelDeleteLwMutex, workarea);
    if (!workarea)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    const auto lightweight_mutex_id = workarea.get(emuenv.mem)->uid;

    return mutex_close(emuenv.kernel, export_name, thread_id, lightweight_mutex_id, SyncWeight::Light, HandleClose::Delete);
}

EXPORT(int, _sceKernelExitCallback) {
    TRACY_FUNC(_sceKernelExitCallback);
    return UNIMPLEMENTED();
}

// The SceKernelThreadMgr 3.74 info syscalls copy the caller's size word in
// (SceLibKernel passes the record's own size word), then that many bytes of
// the record into a zeroed buffer, fill it and copy the same bytes back out,
// also when the fill fails. A size above the record's is NO_MEMORY; without a
// record the fill reports the missing record.
template <typename Info, typename Fill>
static SceInt32 info_syscall(const char *export_name, Info *info, const SceSize *pSize, Fill fill) {
    if (!pSize)
        return RET_ERROR(SCE_KERNEL_ERROR_INVALID_MEMORY_ACCESS);
    if (!info)
        return fill(nullptr);
    const SceSize size = *pSize;
    if (size > sizeof(Info))
        return RET_ERROR(SCE_KERNEL_ERROR_NO_MEMORY);
    Info copy{};
    memcpy(&copy, info, size);
    const SceInt32 result = fill(&copy);
    memcpy(info, &copy, size);
    return result;
}

// SceKernelThreadMgr 3.74 record fill: a zeroed record carrying its own size,
// of which the first info->size bytes reach the caller. The uid the caller
// passed follows in any case; an opened handle also sets `opened_attr` (the
// syscall's buffer holds the whole record).
template <typename Info, typename T, typename Fields>
static SceInt32 fill_info(KernelState &kernel, const char *export_name, std::map<SceUID, std::shared_ptr<T>> &objects, SceUID uid,
    SceInt32 unknown_id, SceUID Info::*uid_field, SceUInt32 opened_attr, Info *info, Fields fields) {
    const std::shared_ptr<T> object = lock_and_find(uid, objects, kernel.mutex);
    if (!object)
        return RET_ERROR(unknown_id);
    if (!info)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    if (info->size > sizeof(Info))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_SIZE);

    Info record{};
    record.size = sizeof(record);
    {
        const std::lock_guard<std::mutex> object_lock(object->mutex);
        if (object->deleted)
            return RET_ERROR(unknown_id);
        record.*uid_field = object->uid;
        strncpy(record.name, object->name, KERNELOBJECT_MAX_NAME_LENGTH);
        record.attr = object->attr;
        fields(record, *object);
    }
    memcpy(info, &record, info->size);
    info->*uid_field = uid;
    if (is_opened_handle(*object, uid))
        info->attr |= opened_attr;
    return SCE_KERNEL_OK;
}

// Attribute of an opened handle's record.
constexpr SceUInt32 OPENED_HANDLE_ATTR = 0x80000;

// SceKernelThreadMgr 3.74: the owner thread must still exist.
EXPORT(SceInt32, _sceKernelGetCallbackInfo, SceUID callbackId, SceKernelCallbackInfo *pInfo, const SceSize *pSize) {
    TRACY_FUNC(_sceKernelGetCallbackInfo, callbackId, pInfo, pSize);
    return info_syscall(export_name, pInfo, pSize, [&](SceKernelCallbackInfo *info) -> SceInt32 {
        const CallbackPtr cb = lock_and_find(callbackId, emuenv.kernel.callbacks, emuenv.kernel.mutex);
        if (!cb)
            return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_CALLBACK_ID);
        if (!info)
            return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        if (info->size > sizeof(*info))
            return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_SIZE);
        if (!emuenv.kernel.get_thread(cb->get_owner_thread_id()))
            return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);

        SceKernelCallbackInfo record{};
        record.size = sizeof(record);
        record.callbackId = callbackId;
        strncpy(record.name, cb->get_name().c_str(), KERNELOBJECT_MAX_NAME_LENGTH);
        record.threadId = cb->get_owner_thread_id();
        record.callbackFunc = cb->get_callback_function();
        record.notifyId = cb->get_notifier_id();
        record.notifyCount = cb->get_num_notifications();
        record.notifyArg = cb->get_notify_arg();
        record.pCommon = cb->get_user_common_ptr();
        memcpy(info, &record, info->size);
        info->callbackId = callbackId;
        return SCE_KERNEL_OK;
    });
}

// SceKernelThreadMgr 3.74: the mutex id is -1 once the mutex is deleted.
EXPORT(SceInt32, _sceKernelGetCondInfo, SceUID condId, Ptr<SceKernelCondInfo> pInfo, const SceSize *pSize) {
    TRACY_FUNC(_sceKernelGetCondInfo, condId, pInfo, pSize);
    return info_syscall(export_name, pInfo.get(emuenv.mem), pSize, [&](SceKernelCondInfo *info) {
        return fill_info(emuenv.kernel, export_name, emuenv.kernel.condvars, condId, SCE_KERNEL_ERROR_UNKNOWN_COND_ID,
            &SceKernelCondInfo::condId, 0, info, [](SceKernelCondInfo &record, const Condvar &condvar) {
                record.mutexId = condvar.associated_mutex ? condvar.associated_mutex->uid : -1;
                record.numWaitThreads = static_cast<SceUInt32>(condvar.waiting_threads->size());
            });
    });
}

EXPORT(SceInt32, _sceKernelGetEventFlagInfo, SceUID evfId, Ptr<SceKernelEventFlagInfo> pInfo, const SceSize *pSize) {
    TRACY_FUNC(_sceKernelGetEventFlagInfo, evfId, pInfo, pSize);
    return info_syscall(export_name, pInfo.get(emuenv.mem), pSize, [&](SceKernelEventFlagInfo *info) {
        return fill_info(emuenv.kernel, export_name, emuenv.kernel.eventflags, evfId, SCE_KERNEL_ERROR_UNKNOWN_EVF_ID,
            &SceKernelEventFlagInfo::evfId, OPENED_HANDLE_ATTR, info, [](SceKernelEventFlagInfo &record, const EventFlag &event) {
                record.initPattern = event.init_pattern;
                record.currentPattern = event.flags;
                record.numWaitThreads = static_cast<SceUInt32>(event.waiting_threads->size());
            });
    });
}

EXPORT(int, _sceKernelGetEventInfo) {
    TRACY_FUNC(_sceKernelGetEventInfo);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, _sceKernelGetEventPattern, SceUID event_id, SceUInt32 *get_pattern) {
    TRACY_FUNC(_sceKernelGetEventPattern, event_id, get_pattern);
    const SimpleEventPtr event = lock_and_find(event_id, emuenv.kernel.simple_events, emuenv.kernel.mutex);
    if (!event)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_EVENT_ID);
    if (!get_pattern)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);

    *get_pattern = event->pattern;
    return SCE_KERNEL_OK;
}

// SceKernelThreadMgr 3.74: a zeroed record carrying its own size, of which the
// first info->size bytes reach the caller. The mutex workarea stays reported
// after the mutex is deleted.
static SceInt32 get_lw_cond_info(const char *export_name, Condvar &condvar, SceKernelLwCondInfo *info) {
    if (!info)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    if (info->size > sizeof(SceKernelLwCondInfo))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_SIZE);

    SceKernelLwCondInfo record{};
    record.size = sizeof(record);
    {
        const std::lock_guard<std::mutex> condvar_lock(condvar.mutex);
        if (condvar.deleted)
            return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_LW_COND_ID);
        record.uid = condvar.uid;
        strncpy(record.name, condvar.name, KERNELOBJECT_MAX_NAME_LENGTH);
        record.attr = condvar.attr;
        record.pWork = condvar.workarea;
        record.pLwMutex = condvar.lwmutex_workarea;
        record.numWaitThreads = static_cast<SceUInt32>(condvar.waiting_threads->size());
    }
    memcpy(info, &record, info->size);
    return SCE_KERNEL_OK;
}

EXPORT(SceInt32, _sceKernelGetLwCondInfo, Ptr<SceKernelLwCondWork> workarea, Ptr<SceKernelLwCondInfo> pInfo) {
    TRACY_FUNC(_sceKernelGetLwCondInfo, workarea, pInfo);
    const SceKernelLwCondWork *work = workarea.get(emuenv.mem);
    if (!work)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    const SceUID uid = work->uid;
    const CondvarPtr condvar = lock_and_find(uid, emuenv.kernel.lwcondvars, emuenv.kernel.mutex);
    if (!condvar)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_LW_COND_ID);

    SceKernelLwCondInfo *info = pInfo.get(emuenv.mem);
    const SceInt32 result = get_lw_cond_info(export_name, *condvar, info);
    // The syscall stores the workarea's uid and copies the whole record back,
    // so the uid arrives even below a size of 8.
    if (result == SCE_KERNEL_OK)
        info->uid = uid;
    return result;
}

EXPORT(SceInt32, _sceKernelGetLwCondInfoById, SceUID lwCondId, Ptr<SceKernelLwCondInfo> pInfo, const SceSize *pSize) {
    TRACY_FUNC(_sceKernelGetLwCondInfoById, lwCondId, pInfo, pSize);
    // The syscall copies the caller's size word in first, then that many
    // bytes of the record in and back out.
    if (!pSize)
        return RET_ERROR(SCE_KERNEL_ERROR_INVALID_MEMORY_ACCESS);
    SceKernelLwCondInfo *info = pInfo.get(emuenv.mem);
    const SceSize size = *pSize;
    if (info && size > sizeof(SceKernelLwCondInfo))
        return RET_ERROR(SCE_KERNEL_ERROR_NO_MEMORY);

    const CondvarPtr condvar = lock_and_find(lwCondId, emuenv.kernel.lwcondvars, emuenv.kernel.mutex);
    if (!condvar)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_LW_COND_ID);
    if (!info)
        return get_lw_cond_info(export_name, *condvar, nullptr);

    SceKernelLwCondInfo copy{};
    memcpy(&copy, info, size);
    const SceInt32 result = get_lw_cond_info(export_name, *condvar, &copy);
    memcpy(info, &copy, size);
    return result;
}

EXPORT(SceInt32, _sceKernelGetLwMutexInfoById, SceUID lightweight_mutex_id, Ptr<SceKernelLwMutexInfo> pInfo, const SceSize *pSize) {
    TRACY_FUNC(_sceKernelGetLwMutexInfoById, lightweight_mutex_id, pInfo, pSize);
    return info_syscall(export_name, pInfo.get(emuenv.mem), pSize, [&](SceKernelLwMutexInfo *info) {
        return fill_info(emuenv.kernel, export_name, emuenv.kernel.lwmutexes, lightweight_mutex_id, SCE_KERNEL_ERROR_UNKNOWN_LW_MUTEX_ID,
            &SceKernelLwMutexInfo::uid, 0, info, [](SceKernelLwMutexInfo &record, const Mutex &mutex) {
                record.pWork = mutex.workarea;
                record.initCount = mutex.init_count;
                record.currentCount = mutex.lock_count;
                record.currentOwnerId = mutex.owner ? mutex.owner->id : 0;
                record.numWaitThreads = static_cast<SceUInt32>(mutex.waiting_threads->size());
            });
    });
}

EXPORT(int, _sceKernelGetMsgPipeInfo) {
    TRACY_FUNC(_sceKernelGetMsgPipeInfo);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, _sceKernelGetMutexInfo, SceUID mutexId, Ptr<SceKernelMutexInfo> pInfo, const SceSize *pSize) {
    TRACY_FUNC(_sceKernelGetMutexInfo, mutexId, pInfo, pSize);
    return info_syscall(export_name, pInfo.get(emuenv.mem), pSize, [&](SceKernelMutexInfo *info) {
        return fill_info(emuenv.kernel, export_name, emuenv.kernel.mutexes, mutexId, SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID,
            &SceKernelMutexInfo::mutexId, OPENED_HANDLE_ATTR, info, [](SceKernelMutexInfo &record, const Mutex &mutex) {
                record.initCount = mutex.init_count;
                record.currentCount = mutex.lock_count;
                record.currentOwnerId = mutex.owner ? mutex.owner->id : 0;
                record.numWaitThreads = static_cast<SceUInt32>(mutex.waiting_threads->size());
                if (mutex.attr & SCE_KERNEL_MUTEX_ATTR_CEILING)
                    record.ceilingPriority = mutex.ceiling_priority;
            });
    });
}

// The lock count of all holders, the write owner, and the waiters by kind.
EXPORT(SceInt32, _sceKernelGetRWLockInfo, SceUID rwlockId, Ptr<SceKernelRWLockInfo> pInfo, const SceSize *pSize) {
    TRACY_FUNC(_sceKernelGetRWLockInfo, rwlockId, pInfo, pSize);
    return info_syscall(export_name, pInfo.get(emuenv.mem), pSize, [&](SceKernelRWLockInfo *info) {
        return fill_info(emuenv.kernel, export_name, emuenv.kernel.rwlocks, rwlockId, SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID,
            &SceKernelRWLockInfo::rwLockId, OPENED_HANDLE_ATTR, info, [](SceKernelRWLockInfo &record, const RWLock &rwlock) {
                for (const auto &[owner, count] : rwlock.owners) {
                    record.lockCount += count;
                    if (rwlock.state == RWLockState::WriteLocked)
                        record.writeOwnerId = owner->id;
                }
                for (const auto &waiter : *rwlock.waiting_threads)
                    ++(waiter.is_write ? record.numWriteWaitThreads : record.numReadWaitThreads);
            });
    });
}

EXPORT(SceInt32, _sceKernelGetSemaInfo, SceUID semaId, Ptr<SceKernelSemaInfo> pInfo, const SceSize *pSize) {
    TRACY_FUNC(_sceKernelGetSemaInfo, semaId, pInfo, pSize);
    return info_syscall(export_name, pInfo.get(emuenv.mem), pSize, [&](SceKernelSemaInfo *info) {
        return fill_info(emuenv.kernel, export_name, emuenv.kernel.semaphores, semaId, SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID,
            &SceKernelSemaInfo::semaId, OPENED_HANDLE_ATTR, info, [](SceKernelSemaInfo &record, const Semaphore &semaphore) {
                record.initCount = semaphore.init_val;
                record.currentCount = semaphore.val;
                record.maxCount = semaphore.max;
                record.numWaitThreads = static_cast<SceUInt32>(semaphore.waiting_threads->size());
            });
    });
}

EXPORT(int, _sceKernelGetSystemInfo) {
    TRACY_FUNC(_sceKernelGetSystemInfo);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelGetSystemTime) {
    TRACY_FUNC(_sceKernelGetSystemTime);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelGetThreadContextForVM, SceUID threadId, Ptr<SceKernelThreadCpuRegisterInfo> pCpuRegisterInfo, Ptr<SceKernelThreadVfpRegisterInfo> pVfpRegisterInfo) {
    TRACY_FUNC(_sceKernelGetThreadContextForVM, threadId, pCpuRegisterInfo, pVfpRegisterInfo);
    STUBBED("Stub");

    const ThreadStatePtr thread = emuenv.kernel.get_thread(threadId);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);

    const auto context = save_context(*thread->cpu);
    SceKernelThreadCpuRegisterInfo *infoCpu = pCpuRegisterInfo.get(emuenv.mem);
    if (infoCpu) {
        if (infoCpu->size != sizeof(*infoCpu))
            return RET_ERROR(SCE_KERNEL_ERROR_INVALID_ARGUMENT_SIZE);

        infoCpu->cpsr = context.cpsr;
        memcpy(infoCpu->reg, context.cpu_registers.data(), 16 * 4);
        infoCpu->sb = 100000; // Todo
        infoCpu->st = 100000; // Todo
        infoCpu->teehbr = 100000; // Todo
        infoCpu->tpidrurw = read_tpidruro(*thread->cpu);
    }

    SceKernelThreadVfpRegisterInfo *infoVfp = pVfpRegisterInfo.get(emuenv.mem);
    if (infoVfp) {
        if (infoVfp->size != sizeof(*infoVfp))
            return RET_ERROR(SCE_KERNEL_ERROR_INVALID_ARGUMENT_SIZE);

        infoVfp->fpscr = context.fpscr;
        memcpy(infoVfp->reg, context.fpu_registers.data(), 64 * 4);
    }

    return SCE_KERNEL_OK;
}

EXPORT(SceInt32, _sceKernelGetThreadCpuAffinityMask, SceUID thid) {
    TRACY_FUNC(_sceKernelGetThreadCpuAffinityMask, thid);
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thid ? thid : thread_id);

    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    if (thread->affinity_mask == 0)
        return SCE_KERNEL_CPU_MASK_USER_ALL;

    return thread->affinity_mask;
}

EXPORT(int, _sceKernelGetThreadEventInfo) {
    TRACY_FUNC(_sceKernelGetThreadEventInfo);
    return UNIMPLEMENTED();
}

// SceKernelThreadMgr 3.74: an unknown thread fails first, then the caller
// itself (or 0) and a null status pointer. A thread that never ran has no
// exit status yet (DORMANT).
EXPORT(int, _sceKernelGetThreadExitStatus, SceUID thid, SceInt32 *pExitStatus) {
    TRACY_FUNC(_sceKernelGetThreadExitStatus, thid, pExitStatus);
    const ThreadStatePtr thread = thid ? emuenv.kernel.get_thread(thid) : ThreadStatePtr();
    if (thid != 0 && !thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    if (thid == 0 || thid == thread_id)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_THREAD_ID);
    if (!pExitStatus)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    const std::lock_guard<std::mutex> thread_lock(thread->mutex);
    if (thread->status != ThreadStatus::dormant)
        return RET_ERROR(SCE_KERNEL_ERROR_NOT_DORMANT);
    if (!thread->started)
        return RET_ERROR(SCE_KERNEL_ERROR_DORMANT);
    *pExitStatus = static_cast<SceInt32>(thread->returned_value);
    return SCE_KERNEL_OK;
}

static SceUInt32 thread_info_status(const KernelState &kernel, const ThreadState &thread, SceUID caller) {
    switch (thread.status) {
    case ThreadStatus::run:
        // The cooperative (browser) runtime executes one guest thread at a
        // time, so any other runnable thread is ready; desktop runs each
        // runnable thread on its own host thread.
        return thread.id == caller || !kernel.execution_host ? SCE_THREAD_RUNNING : SCE_THREAD_READY;
    case ThreadStatus::wait: return SCE_THREAD_WAITING;
    case ThreadStatus::dormant: return SCE_THREAD_DORMANT;
    case ThreadStatus::suspend: return SCE_THREAD_SUSPENDED;
    }
    return 0;
}

// SceKernelThreadMgr 3.74: a zeroed record carrying its own size, of which the
// first info->size bytes reach the caller. Wait state, run clocks, preemption
// counters and CPU ids are not tracked here and stay zero.
SceInt32 get_thread_info(EmuEnvState &emuenv, const char *export_name, SceUID caller, SceUID thid, SceKernelThreadInfo *info) {
    if (!info)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    if (thid == 0 && !emuenv.kernel.get_thread(caller))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
    if (info->size > sizeof(SceKernelThreadInfo))
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_SIZE);
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thid ? thid : caller);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);

    SceKernelThreadInfo record{};
    record.size = sizeof(record);
    record.processId = KernelState::process_id;
    strncpy(record.name, thread->name.c_str(), KERNELOBJECT_MAX_NAME_LENGTH);
    {
        const std::lock_guard<std::mutex> thread_lock(thread->mutex);
        record.attr = thread->attr;
        record.status = thread_info_status(emuenv.kernel, *thread, caller);
        record.entry = SceKernelThreadEntry(thread->entry_point);
        record.stack = Ptr<void>(thread->stack.get());
        record.stackSize = thread->stack_size;
        record.initPriority = thread->init_priority;
        record.currentPriority = thread->priority;
        record.initCpuAffinityMask = thread->init_affinity_mask;
        record.currentCpuAffinityMask = thread->affinity_mask;
        if (thread->status != ThreadStatus::dormant)
            record.exitStatus = SCE_KERNEL_ERROR_NOT_DORMANT;
        else
            record.exitStatus = thread->started ? static_cast<SceInt32>(thread->returned_value) : SCE_KERNEL_ERROR_DORMANT;
    }
    memcpy(info, &record, info->size);
    return SCE_KERNEL_OK;
}

EXPORT(SceInt32, _sceKernelGetThreadInfo, SceUID threadId, Ptr<SceKernelThreadInfo> pInfo, const SceSize *pSize) {
    TRACY_FUNC(_sceKernelGetThreadInfo, threadId, pInfo, pSize);
    // The syscall copies the caller's size word in first, then that many
    // bytes of the record in and back out.
    if (!pSize)
        return RET_ERROR(SCE_KERNEL_ERROR_INVALID_MEMORY_ACCESS);
    SceKernelThreadInfo *info = pInfo.get(emuenv.mem);
    const SceSize size = *pSize;
    if (info && size > sizeof(SceKernelThreadInfo))
        return RET_ERROR(SCE_KERNEL_ERROR_NO_MEMORY);
    if (threadId != 0 && !emuenv.kernel.get_thread(threadId))
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    if (!info)
        return get_thread_info(emuenv, export_name, thread_id, threadId, nullptr);

    SceKernelThreadInfo copy{};
    memcpy(&copy, info, size);
    const SceInt32 result = get_thread_info(emuenv, export_name, thread_id, threadId, &copy);
    memcpy(info, &copy, size);
    return result;
}

EXPORT(int, _sceKernelGetThreadRunStatus) {
    TRACY_FUNC(_sceKernelGetThreadRunStatus);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelGetTimerBase) {
    TRACY_FUNC(_sceKernelGetTimerBase);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelGetTimerEventRemainingTime) {
    TRACY_FUNC(_sceKernelGetTimerEventRemainingTime);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelGetTimerInfo) {
    TRACY_FUNC(_sceKernelGetTimerInfo);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelGetTimerTime) {
    TRACY_FUNC(_sceKernelGetTimerTime);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelLockLwMutex, Ptr<SceKernelLwMutexWork> workarea, int lock_count, unsigned int *ptimeout) {
    TRACY_FUNC(_sceKernelLockLwMutex, workarea, lock_count, ptimeout);
    if (!workarea)
        return RET_ERROR(SCE_KERNEL_ERROR_INVALID_ARGUMENT);

    const auto lwmutexid = workarea.get(emuenv.mem)->uid;
    return mutex_lock(emuenv.kernel, emuenv.mem, export_name, thread_id, lwmutexid, lock_count, ptimeout, SyncWeight::Light);
}

EXPORT(int, _sceKernelLockMutex, SceUID mutexid, int lock_count, unsigned int *timeout) {
    TRACY_FUNC(_sceKernelLockMutex, mutexid, lock_count, timeout);
    return mutex_lock(emuenv.kernel, emuenv.mem, export_name, thread_id, mutexid, lock_count, timeout, SyncWeight::Heavy);
}

EXPORT(SceInt32, _sceKernelLockMutexCB, SceUID mutexId, SceInt32 lockCount, SceUInt32 *pTimeout) {
    TRACY_FUNC(_sceKernelLockMutexCB, mutexId, lockCount, pTimeout);
    process_callbacks(emuenv.kernel, thread_id);
    return mutex_lock(emuenv.kernel, emuenv.mem, export_name, thread_id, mutexId, lockCount, pTimeout, SyncWeight::Heavy);
}

EXPORT(SceInt32, _sceKernelLockReadRWLock, SceUID lock_id, SceUInt32 *timeout) {
    TRACY_FUNC(_sceKernelLockReadRWLock, lock_id, timeout);
    return rwlock_lock(emuenv.kernel, emuenv.mem, export_name, thread_id, lock_id, timeout, false);
}

EXPORT(SceInt32, _sceKernelLockReadRWLockCB, SceUID lock_id, SceUInt32 *timeout) {
    TRACY_FUNC(_sceKernelLockReadRWLockCB, lock_id, timeout);
    process_callbacks(emuenv.kernel, thread_id);
    return rwlock_lock(emuenv.kernel, emuenv.mem, export_name, thread_id, lock_id, timeout, false);
}

EXPORT(SceInt32, _sceKernelLockWriteRWLock, SceUID lock_id, SceUInt32 *timeout) {
    TRACY_FUNC(_sceKernelLockWriteRWLock, lock_id, timeout);
    return rwlock_lock(emuenv.kernel, emuenv.mem, export_name, thread_id, lock_id, timeout, true);
}

EXPORT(SceInt32, _sceKernelLockWriteRWLockCB, SceUID lock_id, SceUInt32 *timeout) {
    TRACY_FUNC(_sceKernelLockWriteRWLockCB, lock_id, timeout);
    process_callbacks(emuenv.kernel, thread_id);
    return rwlock_lock(emuenv.kernel, emuenv.mem, export_name, thread_id, lock_id, timeout, true);
}

EXPORT(int, _sceKernelPMonThreadGetCounter) {
    TRACY_FUNC(_sceKernelPMonThreadGetCounter);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelPollEvent, SceUID event_id, SceUInt32 bit_pattern, SceUInt32 *result_pattern, SceUInt64 *user_data) {
    TRACY_FUNC(_sceKernelPollEvent, event_id, bit_pattern, result_pattern, user_data);
    return simple_event_waitorpoll(emuenv.kernel, export_name, thread_id, event_id, bit_pattern, result_pattern, user_data, nullptr, false);
}

EXPORT(int, _sceKernelPollEventFlag, SceUID event_id, unsigned int flags, unsigned int wait, unsigned int *outBits) {
    TRACY_FUNC(_sceKernelPollEventFlag, event_id, flags, wait, outBits);
    return eventflag_poll(emuenv.kernel, export_name, thread_id, event_id, flags, wait, outBits);
}

EXPORT(int, _sceKernelPulseEventWithNotifyCallback) {
    TRACY_FUNC(_sceKernelPulseEventWithNotifyCallback);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelReceiveMsgPipeVector) {
    TRACY_FUNC(_sceKernelReceiveMsgPipeVector);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelReceiveMsgPipeVectorCB) {
    TRACY_FUNC(_sceKernelReceiveMsgPipeVectorCB);
    return UNIMPLEMENTED();
}

// Firmware 3.74 threadmgr 0x810211ec: a handler for every user thread may
// take START and END events, one for the calling thread only END, one for
// another thread START and END. The result is the handler's UID.
EXPORT(SceUID, _sceKernelRegisterThreadEventHandler, const char *name, SceUID thread_mask, SceUInt32 mask, sceKernelRegisterThreadEventHandlerOpt *opt) {
    TRACY_FUNC(_sceKernelRegisterThreadEventHandler, name, thread_mask, mask, opt);
    if (!name || !opt || !opt->handler)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    constexpr SceUInt32 start_and_end = SCE_KERNEL_THREAD_EVENT_TYPE_START | SCE_KERNEL_THREAD_EVENT_TYPE_END;
    SceUID target = thread_mask;
    if (thread_mask == SCE_KERNEL_THREAD_ID_USER) {
        if (mask & ~start_and_end)
            return RET_ERROR(SCE_KERNEL_ERROR_INVALID_ARGUMENT);
    } else if (thread_mask == 0 || thread_mask == thread_id) {
        if (mask != SCE_KERNEL_THREAD_EVENT_TYPE_END)
            return RET_ERROR(SCE_KERNEL_ERROR_INVALID_ARGUMENT);
        target = thread_id;
    } else {
        if (!emuenv.kernel.get_thread(thread_mask))
            return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
        if (mask & ~start_and_end)
            return RET_ERROR(SCE_KERNEL_ERROR_INVALID_ARGUMENT);
    }
    const SceUID uid = emuenv.kernel.get_next_uid();
    const std::lock_guard<std::mutex> guard(emuenv.kernel.thread_event_mutex);
    emuenv.kernel.thread_event_handlers.push_back({ uid, target, mask, opt->handler, opt->common });
    return uid;
}

EXPORT(int, _sceKernelSendMsgPipeVector) {
    TRACY_FUNC(_sceKernelSendMsgPipeVector);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelSendMsgPipeVectorCB) {
    TRACY_FUNC(_sceKernelSendMsgPipeVectorCB);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelSetEventWithNotifyCallback) {
    TRACY_FUNC(_sceKernelSetEventWithNotifyCallback);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelSetThreadContextForVM, SceUID threadId, Ptr<SceKernelThreadCpuRegisterInfo> pCpuRegisterInfo, Ptr<SceKernelThreadVfpRegisterInfo> pVfpRegisterInfo) {
    TRACY_FUNC(_sceKernelSetThreadContextForVM, threadId, pCpuRegisterInfo, pVfpRegisterInfo);
    const ThreadStatePtr thread = emuenv.kernel.get_thread(threadId);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);

    SceKernelThreadCpuRegisterInfo *infoCpu = pCpuRegisterInfo.get(emuenv.mem);
    if (infoCpu) {
        if (infoCpu->size != sizeof(*infoCpu))
            return RET_ERROR(SCE_KERNEL_ERROR_INVALID_ARGUMENT_SIZE);

        // Todo
    }

    SceKernelThreadVfpRegisterInfo *infoVfp = pVfpRegisterInfo.get(emuenv.mem);
    if (infoVfp) {
        if (infoVfp->size != sizeof(*infoVfp))
            return RET_ERROR(SCE_KERNEL_ERROR_INVALID_ARGUMENT_SIZE);

        // Todo
    }

    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelSetTimerEvent) {
    TRACY_FUNC(_sceKernelSetTimerEvent);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelSetTimerTime) {
    TRACY_FUNC(_sceKernelSetTimerTime);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelSignalLwCond, Ptr<SceKernelLwCondWork> workarea) {
    TRACY_FUNC(_sceKernelSignalLwCond, workarea);
    if (!workarea)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    SceUID condid = workarea.get(emuenv.mem)->uid;
    return condvar_signal(emuenv.kernel, emuenv.mem, export_name, thread_id, condid,
        Condvar::SignalTarget(Condvar::SignalTarget::Type::Any), SyncWeight::Light);
}

EXPORT(int, _sceKernelSignalLwCondAll, Ptr<SceKernelLwCondWork> workarea) {
    TRACY_FUNC(_sceKernelSignalLwCondAll, workarea);
    if (!workarea)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    SceUID condid = workarea.get(emuenv.mem)->uid;
    return condvar_signal(emuenv.kernel, emuenv.mem, export_name, thread_id, condid,
        Condvar::SignalTarget(Condvar::SignalTarget::Type::All), SyncWeight::Light);
}

EXPORT(int, _sceKernelSignalLwCondTo, Ptr<SceKernelLwCondWork> workarea, SceUID thread_target) {
    TRACY_FUNC(_sceKernelSignalLwCondTo, workarea, thread_target);
    if (!workarea)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    SceUID condid = workarea.get(emuenv.mem)->uid;
    return condvar_signal(emuenv.kernel, emuenv.mem, export_name, thread_id, condid,
        Condvar::SignalTarget(Condvar::SignalTarget::Type::Specific, thread_target), SyncWeight::Light);
}

EXPORT(int, _sceKernelStartThread, SceUID thid, SceSize arglen, Ptr<void> argp) {
    TRACY_FUNC(_sceKernelStartThread, thid, arglen, argp);
    auto thread = emuenv.kernel.get_thread(thid);

    if (!thread) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    }

    if (thread->status == ThreadStatus::run) {
        return RET_ERROR(SCE_KERNEL_ERROR_RUNNING);
    }

    const int res = thread->start(arglen, argp, true);
    if (res < 0) {
        return RET_ERROR(res);
    }
    return res;
}

EXPORT(int, _sceKernelTryReceiveMsgPipeVector) {
    TRACY_FUNC(_sceKernelTryReceiveMsgPipeVector);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelTrySendMsgPipeVector) {
    TRACY_FUNC(_sceKernelTrySendMsgPipeVector);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelUnlockLwMutex) {
    TRACY_FUNC(_sceKernelUnlockLwMutex);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, _sceKernelWaitCond, SceUID condId, SceUInt32 *pTimeout) {
    TRACY_FUNC(_sceKernelWaitCond, condId, pTimeout);
    return condvar_wait(emuenv.kernel, emuenv.mem, export_name, thread_id, condId, pTimeout, SyncWeight::Heavy);
}

EXPORT(SceInt32, _sceKernelWaitCondCB, SceUID condId, SceUInt32 *pTimeout) {
    TRACY_FUNC(_sceKernelWaitCondCB, condId, pTimeout);
    process_callbacks(emuenv.kernel, thread_id);
    return condvar_wait(emuenv.kernel, emuenv.mem, export_name, thread_id, condId, pTimeout, SyncWeight::Heavy);
}

EXPORT(SceInt32, _sceKernelWaitEvent, SceUID event_id, SceUInt32 bit_pattern, SceUInt32 *result_pattern, SceUInt64 *user_data, SceUInt32 *timeout) {
    TRACY_FUNC(_sceKernelWaitEvent, event_id, bit_pattern, result_pattern, user_data, timeout);
    return simple_event_waitorpoll(emuenv.kernel, export_name, thread_id, event_id, bit_pattern, result_pattern, user_data, timeout, true);
}

EXPORT(SceInt32, _sceKernelWaitEventCB, SceUID event_id, SceUInt32 bit_pattern, SceUInt32 *result_pattern, SceUInt64 *user_data, SceUInt32 *timeout) {
    TRACY_FUNC(_sceKernelWaitEventCB, event_id, bit_pattern, result_pattern, user_data, timeout);
    process_callbacks(emuenv.kernel, thread_id);
    return simple_event_waitorpoll(emuenv.kernel, export_name, thread_id, event_id, bit_pattern, result_pattern, user_data, timeout, true);
}

EXPORT(SceInt32, _sceKernelWaitEventFlag, SceUID evfId, SceUInt32 bitPattern, SceUInt32 waitMode, SceUInt32 *pResultPat, SceUInt32 *pTimeout) {
    TRACY_FUNC(_sceKernelWaitEventFlag, evfId, bitPattern, waitMode, pResultPat, pTimeout);
    return eventflag_wait(emuenv.kernel, export_name, thread_id, evfId, bitPattern, waitMode, pResultPat, pTimeout);
}

EXPORT(SceInt32, _sceKernelWaitEventFlagCB, SceUID evfId, SceUInt32 bitPattern, SceUInt32 waitMode, SceUInt32 *pResultPat, SceUInt32 *pTimeout) {
    TRACY_FUNC(_sceKernelWaitEventFlagCB, evfId, bitPattern, waitMode, pResultPat, pTimeout);
    process_callbacks(emuenv.kernel, thread_id);
    return eventflag_wait(emuenv.kernel, export_name, thread_id, evfId, bitPattern, waitMode, pResultPat, pTimeout);
}

EXPORT(int, _sceKernelWaitException) {
    TRACY_FUNC(_sceKernelWaitException);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelWaitExceptionCB) {
    TRACY_FUNC(_sceKernelWaitExceptionCB);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelWaitLwCond, Ptr<SceKernelLwCondWork> workarea, SceUInt32 *timeout) {
    TRACY_FUNC(_sceKernelWaitLwCond, workarea, timeout);
    if (!workarea)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    const auto cond_id = workarea.get(emuenv.mem)->uid;
    return condvar_wait(emuenv.kernel, emuenv.mem, export_name, thread_id, cond_id, timeout, SyncWeight::Light);
}

EXPORT(SceInt32, _sceKernelWaitLwCondCB, Ptr<SceKernelLwCondWork> pWork, SceUInt32 *pTimeout) {
    TRACY_FUNC(_sceKernelWaitLwCondCB, pWork, pTimeout);
    if (!pWork)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    process_callbacks(emuenv.kernel, thread_id);
    const auto cond_id = pWork.get(emuenv.mem)->uid;
    return condvar_wait(emuenv.kernel, emuenv.mem, export_name, thread_id, cond_id, pTimeout, SyncWeight::Light);
}

EXPORT(int, _sceKernelWaitMultipleEvents) {
    TRACY_FUNC(_sceKernelWaitMultipleEvents);
    return UNIMPLEMENTED();
}

EXPORT(int, _sceKernelWaitMultipleEventsCB) {
    TRACY_FUNC(_sceKernelWaitMultipleEventsCB);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, _sceKernelWaitSema, SceUID semaId, SceInt32 needCount, SceUInt32 *pTimeout) {
    TRACY_FUNC(_sceKernelWaitSema, semaId, needCount, pTimeout);
    return semaphore_wait(emuenv.kernel, export_name, thread_id, semaId, needCount, pTimeout);
}

EXPORT(SceInt32, _sceKernelWaitSemaCB, SceUID semaId, SceInt32 needCount, SceUInt32 *pTimeout) {
    TRACY_FUNC(_sceKernelWaitSemaCB, semaId, needCount, pTimeout);
    process_callbacks(emuenv.kernel, thread_id);
    return semaphore_wait(emuenv.kernel, export_name, thread_id, semaId, needCount, pTimeout);
}

EXPORT(int, _sceKernelWaitSignal, uint32_t unknown, uint32_t delay, uint32_t timeout) {
    TRACY_FUNC(_sceKernelWaitSignal, unknown, delay, timeout);
    STUBBED("sceKernelWaitSignal");
    const auto thread = emuenv.kernel.get_thread(thread_id);
    thread->update_status(ThreadStatus::wait);
    thread->signal.wait();
    thread->update_status(ThreadStatus::run);
    return SCE_KERNEL_OK;
}

EXPORT(int, _sceKernelWaitSignalCB, uint32_t unknown, uint32_t delay, uint32_t timeout) {
    TRACY_FUNC(_sceKernelWaitSignalCB, unknown, delay, timeout);
    process_callbacks(emuenv.kernel, thread_id);
    return CALL_EXPORT(_sceKernelWaitSignal, unknown, delay, timeout);
}

// Browser fiber runtime: park instead of blocking the host thread. The
// target's dormant transition (raise_waiting_threads) unlinks the waiter and
// wakes it; a waiter still linked afterwards timed out or was cancelled.
static int wait_thread_end_cooperative(KernelState &kernel, const ThreadStatePtr &waiter, const ThreadStatePtr &target, int *stat, SceUInt *timeout) {
    // Same order as raise_waiting_threads: target, then waiter.
    const auto unlink = [&] {
        const std::lock_guard<std::mutex> target_lock(target->mutex);
        const auto it = std::find(target->waiting_threads.begin(), target->waiting_threads.end(), waiter);
        const bool linked = it != target->waiting_threads.end();
        if (linked)
            target->waiting_threads.erase(it);
        const std::lock_guard<std::mutex> waiter_lock(waiter->mutex);
        if (waiter->status != ThreadStatus::run)
            waiter->update_status(ThreadStatus::run);
        return linked;
    };
    {
        const std::lock_guard<std::mutex> target_lock(target->mutex);
        if (target->status == ThreadStatus::dormant) {
            if (stat)
                *stat = target->returned_value;
            return SCE_KERNEL_OK;
        }
        const std::lock_guard<std::mutex> waiter_lock(waiter->mutex);
        waiter->update_status(ThreadStatus::wait);
        target->waiting_threads.push_back(waiter);
    }
    const auto start = std::chrono::steady_clock::now();
    const auto duration = timeout ? std::optional<uint32_t>(*timeout) : std::nullopt;
    KernelExecutionHost::WaitResult result;
    try {
        result = kernel.execution_host->wait_sync(*waiter, duration);
    } catch (...) {
        unlink();
        throw;
    }
    const bool still_linked = unlink();
    if (timeout) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
                                 .count();
        *timeout = elapsed >= *duration ? 0 : *duration - static_cast<uint32_t>(elapsed);
    }
    if (still_linked)
        return result == KernelExecutionHost::WaitResult::timeout
            ? SCE_KERNEL_ERROR_WAIT_TIMEOUT
            : SCE_KERNEL_ERROR_WAIT_CANCEL;
    if (stat)
        *stat = target->returned_value;
    return SCE_KERNEL_OK;
}

static int wait_thread_end(KernelState &kernel, ThreadStatePtr &waiter, ThreadStatePtr &target, int *stat) {
    std::unique_lock<std::mutex> waiter_lock(waiter->mutex);
    {
        const std::unique_lock<std::mutex> thread_lock(target->mutex);
        if (target->status == ThreadStatus::dormant) {
            if (stat != nullptr) {
                *stat = target->returned_value;
            }
            return 0;
        }

        waiter->update_status(ThreadStatus::wait);
        target->waiting_threads.push_back(waiter);
    }
    waiter->status_cond.wait(waiter_lock, [&]() {
        return waiter->status == ThreadStatus::run;
    });
    return 0;
}

EXPORT(int, _sceKernelWaitThreadEnd, SceUID thid, int *stat, SceUInt *timeout) {
    TRACY_FUNC(_sceKernelWaitThreadEnd, thid, stat, timeout);
    auto waiter = emuenv.kernel.get_thread(thread_id);
    auto target = emuenv.kernel.get_thread(thid);
    if (!target) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    }
    if (emuenv.kernel.execution_host)
        return wait_thread_end_cooperative(emuenv.kernel, waiter, target, stat, timeout);
    return wait_thread_end(emuenv.kernel, waiter, target, stat);
}

EXPORT(int, _sceKernelWaitThreadEndCB, SceUID thid, int *stat, SceUInt *timeout) {
    TRACY_FUNC(_sceKernelWaitThreadEndCB, thid, stat, timeout);
    auto waiter = emuenv.kernel.get_thread(thread_id);
    auto target = emuenv.kernel.get_thread(thid);
    if (!target) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    }
    process_callbacks(emuenv.kernel, thread_id);
    return wait_thread_end(emuenv.kernel, waiter, target, stat);
}

EXPORT(SceInt32, sceKernelCancelCallback, SceUID callbackId) {
    TRACY_FUNC(sceKernelCancelCallback, callbackId);
    const CallbackPtr cb = lock_and_find(callbackId, emuenv.kernel.callbacks, emuenv.kernel.mutex);

    if (!cb)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_CALLBACK_ID);
    cb->cancel();

    return SCE_KERNEL_OK;
}

EXPORT(int, sceKernelChangeActiveCpuMask) {
    TRACY_FUNC(sceKernelChangeActiveCpuMask);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, sceKernelChangeThreadCpuAffinityMask, SceUID thid, SceInt32 affinity_mask) {
    TRACY_FUNC(sceKernelChangeThreadCpuAffinityMask, thid, affinity_mask);
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thid ? thid : thread_id);

    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);

    const SceInt32 old_affinity = thread->affinity_mask;

    if (affinity_mask & ~SCE_KERNEL_CPU_MASK_USER_ALL)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_CPU_AFFINITY_MASK);

    thread->affinity_mask = affinity_mask;
    thread->tls.get_ptr<int>().get(emuenv.mem)[TLS_CPU_AFFINITY_MASK] = affinity_mask;
    return old_affinity;
}

EXPORT(SceInt32, sceKernelChangeThreadPriority2, SceUID thid, SceInt32 priority) {
    TRACY_FUNC(sceKernelChangeThreadPriority2, thid, priority);
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thid ? thid : thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);

    const SceInt32 old_priority = thread->priority;

    if (priority == SCE_KERNEL_CURRENT_THREAD_PRIORITY) {
        priority = emuenv.kernel.get_thread(thread_id)->priority;
    }

    if (priority >= SCE_KERNEL_HIGHEST_DEFAULT_PRIORITY
        && priority <= SCE_KERNEL_LOWEST_DEFAULT_PRIORITY) {
        priority = SCE_KERNEL_GAME_DEFAULT_PRIORITY_ACTUAL + (priority - SCE_KERNEL_DEFAULT_PRIORITY);
    }

    if (priority < SCE_KERNEL_HIGHEST_PRIORITY_USER || priority > SCE_KERNEL_LOWEST_PRIORITY_USER)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_PRIORITY);

    thread->set_base_priority(priority);

    return old_priority;
}

EXPORT(SceInt32, sceKernelChangeThreadPriority, SceUID thid, SceInt32 priority) {
    TRACY_FUNC(sceKernelChangeThreadPriority, thid, priority);
    auto err = CALL_EXPORT(sceKernelChangeThreadPriority2, thid, priority);
    if (err < 0)
        return err;

    return SCE_KERNEL_OK;
}

EXPORT(int, sceKernelChangeThreadVfpException, SceInt32 clearMask, SceInt32 setMask) {
    TRACY_FUNC(sceKernelChangeThreadVfpException, clearMask, setMask);
    if (((clearMask | setMask) & 0xf7ffff60) != 0 || (clearMask & setMask) != 0) {
        return RET_ERROR(SCE_KERNEL_ERROR_INVALID_ARGUMENT);
    }
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
    // The mask only selects which cumulative VFP exception flags interrupt
    // the thread; the emulated CPU raises no such interrupt, so the mask is
    // state that libkernel reads back from TLS.
    int &vfp_exception = thread->tls.get_ptr<int>().get(emuenv.mem)[TLS_VFP_EXCEPTION];
    int old_exception = vfp_exception;
    vfp_exception = setMask | (vfp_exception & ~clearMask);
    return old_exception;
}

EXPORT(SceInt32, sceKernelCheckCallback) {
    TRACY_FUNC(sceKernelCheckCallback);
    return process_callbacks(emuenv.kernel, thread_id);
}

EXPORT(int, sceKernelCheckWaitableStatus) {
    TRACY_FUNC(sceKernelCheckWaitableStatus);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, sceKernelClearEvent, SceUID event_id, SceUInt32 clear_pattern) {
    TRACY_FUNC(sceKernelClearEvent, event_id, clear_pattern);
    return simple_event_clear(emuenv.kernel, export_name, thread_id, event_id, clear_pattern);
}

EXPORT(SceInt32, sceKernelClearEventFlag, SceUID evfId, SceUInt32 bitPattern) {
    TRACY_FUNC(sceKernelClearEventFlag, evfId, bitPattern);
    return eventflag_clear(emuenv.kernel, export_name, evfId, bitPattern);
}

EXPORT(int, sceKernelCloseCond, SceUID condId) {
    TRACY_FUNC(sceKernelCloseCond, condId);
    return condvar_delete(emuenv.kernel, export_name, thread_id, condId, SyncWeight::Heavy);
}

EXPORT(int, sceKernelCloseEventFlag, SceUID evfId) {
    TRACY_FUNC(sceKernelCloseEventFlag, evfId);
    return eventflag_close(emuenv.kernel, export_name, thread_id, evfId, HandleClose::Close);
}

EXPORT(int, sceKernelCloseMsgPipe, SceUID msgPipeId) {
    TRACY_FUNC(sceKernelCloseMsgPipe, msgPipeId);
    return msgpipe_close(emuenv.kernel, export_name, thread_id, msgPipeId, HandleClose::Close);
}

EXPORT(int, sceKernelCloseMutex, SceUID mutexId) {
    TRACY_FUNC(sceKernelCloseMutex, mutexId);
    return mutex_close(emuenv.kernel, export_name, thread_id, mutexId, SyncWeight::Heavy, HandleClose::Close);
}

EXPORT(int, sceKernelCloseMutex_089, SceUID mutexId) {
    TRACY_FUNC(sceKernelCloseMutex_089, mutexId);
    return mutex_close(emuenv.kernel, export_name, thread_id, mutexId, SyncWeight::Heavy, HandleClose::Close);
}

EXPORT(int, sceKernelCloseRWLock, SceUID lockId) {
    TRACY_FUNC(sceKernelCloseRWLock, lockId);
    return rwlock_close(emuenv.kernel, export_name, thread_id, lockId, HandleClose::Close);
}

EXPORT(int, sceKernelCloseSema, SceUID semaId) {
    TRACY_FUNC(sceKernelCloseSema, semaId);
    return semaphore_close(emuenv.kernel, export_name, thread_id, semaId, HandleClose::Close);
}

EXPORT(int, sceKernelCloseSimpleEvent, SceUID eventId) {
    TRACY_FUNC(sceKernelCloseSimpleEvent, eventId);
    return simple_event_close(emuenv.kernel, export_name, thread_id, eventId, HandleClose::Close);
}

EXPORT(int, sceKernelCloseTimer, SceUID timerId) {
    TRACY_FUNC(sceKernelCloseTimer, timerId);
    return timer_close(emuenv.kernel, export_name, thread_id, timerId, HandleClose::Close);
}

EXPORT(SceUID, sceKernelCreateCallback, char *name, SceUInt32 attr, Ptr<SceKernelCallbackFunction> callbackFunc, Ptr<void> pCommon) {
    TRACY_FUNC(sceKernelCreateCallback, name, attr, callbackFunc, pCommon);
    if (attr || !callbackFunc.address())
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ATTR);

    ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    std::string cb_name = name;
    auto cb = std::make_shared<Callback>(thread_id, cb_name, callbackFunc, pCommon);
    std::lock_guard lock(emuenv.kernel.mutex);
    SceUID cb_uid = emuenv.kernel.get_next_uid();
    emuenv.kernel.callbacks.emplace(cb_uid, cb);
    thread->callbacks.push_back(cb);
    return cb_uid;
}

// SceKernelThreadMgr 3.74: the options may be at most 0x1c bytes; the syscall
// allows the affinity bits of all four cores. A game's own code may ask for
// attributes 0x05002000 only, a system module for any outside 0x797f5fff.
EXPORT(int, sceKernelCreateThreadForUser, const char *name, SceKernelThreadEntry entry, int init_priority, SceKernelCreateThread_opt *options) {
    TRACY_FUNC(sceKernelCreateThreadForUser, name, entry, init_priority, options);
    const SceKernelThreadOptParam *option = options->option.get(emuenv.mem);
    if (option && option->size > 0x1c)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_SIZE);
    if (options->cpu_affinity_mask & ~0xf0000)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_CPU_AFFINITY_MASK);
    const SceUInt32 attr = options->attr | SCE_KERNEL_THREAD_ATTR_USER;
    const bool system_caller = CALL_EXPORT(sceKernelIsCalledFromSysModule, options->caller) != 0;
    if (system_caller ? (attr & 0x797f5fff) != 0 : (attr & ~0x85002000u) != 0)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_ATTR);

    const ThreadStatePtr thread = emuenv.kernel.create_thread(emuenv.mem, name, entry.cast<void>(), init_priority, options->cpu_affinity_mask, options->stack_size, option);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_ERROR);
    {
        const std::lock_guard<std::mutex> thread_lock(thread->mutex);
        thread->attr = attr;
    }
    return thread->id;
}

static int delay_thread(KernelState &kernel, SceUID thread_id, SceUInt delay_us) {
    if (delay_us == 0)
        return SCE_KERNEL_ERROR_INVALID_ARGUMENT;

    const ThreadStatePtr thread = kernel.get_thread(thread_id);
    if (kernel.execution_host) {
        // Browser fiber runtime: park until the deadline instead of blocking
        // the host thread. No queue is involved (nothing else can resume a
        // delay early except deletion, which wait_sync reports as cancelled).
        {
            const std::lock_guard<std::mutex> lock(thread->mutex);
            thread->update_status(ThreadStatus::wait);
        }
        kernel.execution_host->wait_sync(*thread, delay_us);
        {
            const std::lock_guard<std::mutex> lock(thread->mutex);
            if (thread->status != ThreadStatus::run)
                thread->update_status(ThreadStatus::run);
        }
        return SCE_KERNEL_OK;
    }
    std::unique_lock<std::mutex> lock(thread->mutex);
    thread->update_status(ThreadStatus::wait);
    thread->status_cond.wait_for(lock, std::chrono::microseconds(delay_us),
        [&] { return thread->status == ThreadStatus::run; });
    if (thread->status != ThreadStatus::run)
        thread->update_status(ThreadStatus::run);
    return SCE_KERNEL_OK;
}

static int delay_thread_cb(EmuEnvState &emuenv, SceUID thread_id, SceUInt delay_us) {
    auto start = std::chrono::high_resolution_clock::now(); // Meseaure the time taken to process callbacks
    process_callbacks(emuenv.kernel, thread_id);
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    if (delay_us > elapsed.count()) // If we spent less time than requested processing callbacks, sleep the remaining time
        return delay_thread(emuenv.kernel, thread_id, delay_us - elapsed.count());
    else // Else return directly
        return SCE_KERNEL_OK;
}

EXPORT(int, sceKernelDelayThread, SceUInt delay) {
    TRACY_FUNC(sceKernelDelayThread, delay);
    return delay_thread(emuenv.kernel, thread_id, delay);
}

EXPORT(int, sceKernelDelayThread200, SceUInt delay) {
    TRACY_FUNC(sceKernelDelayThread200, delay);
    if (delay < 201)
        delay = 201;
    return delay_thread(emuenv.kernel, thread_id, delay);
}

EXPORT(int, sceKernelDelayThreadCB, SceUInt delay) {
    TRACY_FUNC(sceKernelDelayThreadCB, delay);
    return delay_thread_cb(emuenv, thread_id, delay);
}

EXPORT(int, sceKernelDelayThreadCB200, SceUInt delay) {
    TRACY_FUNC(sceKernelDelayThreadCB200, delay);
    if (delay < 201)
        delay = 201;
    return delay_thread_cb(emuenv, thread_id, delay);
}

EXPORT(int, sceKernelDeleteCallback, SceUID callbackId) {
    TRACY_FUNC(sceKernelDeleteCallback, callbackId);
    const CallbackPtr cb = lock_and_find(callbackId, emuenv.kernel.callbacks, emuenv.kernel.mutex);
    if (!cb)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_CALLBACK_ID);
    auto cb_owner_thread = emuenv.kernel.get_thread(cb->get_owner_thread_id());
    std::lock_guard lock(emuenv.kernel.mutex);
    emuenv.kernel.callbacks.erase(callbackId);
    if (cb_owner_thread) {
        auto &v = cb_owner_thread->callbacks;
        std::erase(v, cb);
    }
    return 0;
}

EXPORT(int, sceKernelDeleteCond, SceUID condition_variable_id) {
    TRACY_FUNC(sceKernelDeleteCond, condition_variable_id);
    return condvar_delete(emuenv.kernel, export_name, thread_id, condition_variable_id, SyncWeight::Heavy);
}

EXPORT(int, sceKernelDeleteEventFlag, SceUID event_id) {
    TRACY_FUNC(sceKernelDeleteEventFlag, event_id);
    return eventflag_close(emuenv.kernel, export_name, thread_id, event_id, HandleClose::Delete);
}

EXPORT(SceInt32, sceKernelDeleteMsgPipe, SceUID msgPipeId) {
    TRACY_FUNC(sceKernelDeleteMsgPipe, msgPipeId);
    return msgpipe_close(emuenv.kernel, export_name, thread_id, msgPipeId, HandleClose::Delete);
}

EXPORT(int, sceKernelDeleteMutex, SceUID mutexid) {
    TRACY_FUNC(sceKernelDeleteMutex, mutexid);
    return mutex_close(emuenv.kernel, export_name, thread_id, mutexid, SyncWeight::Heavy, HandleClose::Delete);
}

EXPORT(SceInt32, sceKernelDeleteRWLock, SceUID lock_id) {
    TRACY_FUNC(sceKernelDeleteRWLock, lock_id);
    return rwlock_close(emuenv.kernel, export_name, thread_id, lock_id, HandleClose::Delete);
}

EXPORT(int, sceKernelDeleteSema, SceUID semaid) {
    TRACY_FUNC(sceKernelDeleteSema, semaid);
    return semaphore_close(emuenv.kernel, export_name, thread_id, semaid, HandleClose::Delete);
}

EXPORT(int, sceKernelDeleteSimpleEvent, SceUID event_id) {
    TRACY_FUNC(sceKernelDeleteSimpleEvent, event_id);
    return simple_event_close(emuenv.kernel, export_name, thread_id, event_id, HandleClose::Delete);
}

EXPORT(int, sceKernelDeleteThread, SceUID thid) {
    TRACY_FUNC(sceKernelDeleteThread, thid);
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thid);
    if (!thread || thread->status != ThreadStatus::dormant) {
        return SCE_KERNEL_ERROR_NOT_DORMANT;
    }
    thread->exit_delete(false);
    return 0;
}

EXPORT(int, sceKernelDeleteTimer, SceUID timer_handle) {
    TRACY_FUNC(sceKernelDeleteTimer, timer_handle);
    return timer_close(emuenv.kernel, export_name, thread_id, timer_handle, HandleClose::Delete);
}

EXPORT(int, sceKernelExitDeleteThread, int status) {
    TRACY_FUNC(sceKernelExitDeleteThread, status);
    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    {
        // Joiners and GetThreadExitStatus read the exit status from here.
        const std::lock_guard<std::mutex> lock(thread->mutex);
        thread->returned_value = static_cast<uint32_t>(status);
    }
    thread->exit_delete();

    return status;
}

EXPORT(SceInt32, sceKernelGetCallbackCount, SceUID callbackId) {
    TRACY_FUNC(sceKernelGetCallbackCount, callbackId);
    const CallbackPtr cb = lock_and_find(callbackId, emuenv.kernel.callbacks, emuenv.kernel.mutex);

    if (!cb)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_CALLBACK_ID);

    return cb->get_num_notifications();
}

EXPORT(int, sceKernelGetMsgPipeCreatorId) {
    TRACY_FUNC(sceKernelGetMsgPipeCreatorId);
    return UNIMPLEMENTED();
}

EXPORT(SceUID, sceKernelGetProcessId) {
    TRACY_FUNC(sceKernelGetProcessId);
    return KernelState::process_id;
}

EXPORT(uint64_t, sceKernelGetSystemTimeWide) {
    TRACY_FUNC(sceKernelGetSystemTimeWide);
    return get_current_time();
}

EXPORT(SceInt32, sceKernelGetThreadCpuAffinityMask, SceUID thid) {
    TRACY_FUNC(sceKernelGetThreadCpuAffinityMask, thid);
    return CALL_EXPORT(_sceKernelGetThreadCpuAffinityMask, thid);
}

EXPORT(int, sceKernelGetThreadStackFreeSize) {
    TRACY_FUNC(sceKernelGetThreadStackFreeSize);
    return UNIMPLEMENTED();
}

EXPORT(Ptr<void>, sceKernelGetThreadTLSAddr, SceUID thid, int key) {
    TRACY_FUNC(sceKernelGetThreadTLSAddr, thid, key);
    return emuenv.kernel.get_thread_tls_addr(emuenv.mem, thid, key);
}

EXPORT(int, sceKernelGetThreadmgrUIDClass) {
    TRACY_FUNC(sceKernelGetThreadmgrUIDClass);
    return UNIMPLEMENTED();
}

EXPORT(uint64_t, sceKernelGetTimerBaseWide, SceUID timer_handle) {
    TRACY_FUNC(sceKernelGetTimerBaseWide, timer_handle);
    const TimerPtr timer_info = timer_find(emuenv.kernel, timer_handle);

    if (!timer_info)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_TIMER_ID);

    return timer_info->time;
}

EXPORT(uint64_t, sceKernelGetTimerTimeWide, SceUID timer_handle) {
    TRACY_FUNC(sceKernelGetTimerTimeWide, timer_handle);
    const TimerPtr timer_info = timer_find(emuenv.kernel, timer_handle);

    if (!timer_info)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_TIMER_ID);

    return get_current_time() - timer_info->time;
}

EXPORT(SceInt32, sceKernelNotifyCallback, SceUID callbackId, SceInt32 notifyArg) {
    TRACY_FUNC(sceKernelNotifyCallback, callbackId, notifyArg);
    const CallbackPtr cb = lock_and_find(callbackId, emuenv.kernel.callbacks, emuenv.kernel.mutex);
    if (!cb)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_CALLBACK_ID);

    cb->direct_notify(notifyArg);

    return SCE_KERNEL_OK;
}

EXPORT(SceUID, sceKernelOpenCond, const char *pName) {
    TRACY_FUNC(sceKernelOpenCond, pName);
    return condvar_open(emuenv.kernel, export_name, pName);
}

EXPORT(SceUID, sceKernelOpenEventFlag, const char *pName) {
    TRACY_FUNC(sceKernelOpenEventFlag, pName);
    return eventflag_open(emuenv.kernel, export_name, pName);
}

EXPORT(SceUID, sceKernelOpenMsgPipe, const char *pName) {
    TRACY_FUNC(sceKernelOpenMsgPipe, pName);
    return msgpipe_open(emuenv.kernel, export_name, pName);
}

EXPORT(SceUID, sceKernelOpenMutex, const char *pName) {
    TRACY_FUNC(sceKernelOpenMutex, pName);
    return mutex_open(emuenv.kernel, export_name, pName);
}

EXPORT(SceUID, sceKernelOpenMutex_089, const char *pName) {
    TRACY_FUNC(sceKernelOpenMutex_089, pName);
    return mutex_open(emuenv.kernel, export_name, pName);
}

EXPORT(SceUID, sceKernelOpenRWLock, const char *pName) {
    TRACY_FUNC(sceKernelOpenRWLock, pName);
    return rwlock_open(emuenv.kernel, export_name, pName);
}

EXPORT(SceUID, sceKernelOpenSema, const char *pName) {
    TRACY_FUNC(sceKernelOpenSema, pName);
    return semaphore_open(emuenv.kernel, export_name, pName);
}

EXPORT(SceUID, sceKernelOpenSimpleEvent, const char *pName) {
    TRACY_FUNC(sceKernelOpenSimpleEvent, pName);
    return simple_event_open(emuenv.kernel, export_name, thread_id, pName);
}

EXPORT(SceUID, sceKernelOpenTimer, const char *pName) {
    TRACY_FUNC(sceKernelOpenTimer, pName);
    return timer_open(emuenv.kernel, export_name, thread_id, pName);
}

EXPORT(int, sceKernelPollSema, SceUID semaid, int32_t needCount) {
    TRACY_FUNC(sceKernelPollSema, semaid, needCount);
    assert(needCount >= 0);
    const SemaphorePtr semaphore = lock_and_find(semaid, emuenv.kernel.semaphores, emuenv.kernel.mutex);
    if (!semaphore) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID);
    }
    std::unique_lock<std::mutex> semaphore_lock(semaphore->mutex);
    if (semaphore->val < needCount) {
        return SCE_KERNEL_ERROR_SEMA_ZERO;
    }
    semaphore->val -= needCount;
    return SCE_KERNEL_OK;
}

EXPORT(SceInt32, sceKernelPulseEvent, SceUID event_id, SceUInt32 set_pattern, SceUInt64 user_data) {
    TRACY_FUNC(sceKernelPulseEvent, event_id, set_pattern, user_data);
    return simple_event_setorpulse(emuenv.kernel, export_name, thread_id, event_id, set_pattern, user_data, false);
}

EXPORT(int, sceKernelRegisterCallbackToEvent) {
    TRACY_FUNC(sceKernelRegisterCallbackToEvent);
    return UNIMPLEMENTED();
}

EXPORT(int, sceKernelResumeThreadForVM, SceUID threadId) {
    TRACY_FUNC(sceKernelResumeThreadForVM, threadId);
    STUBBED("STUB");

    const ThreadStatePtr thread = emuenv.kernel.get_thread(threadId);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);

    thread->resume();

    return 0;
}

EXPORT(int, sceKernelSendSignal, SceUID target_thread_id) {
    TRACY_FUNC(sceKernelSendSignal, target_thread_id);
    STUBBED("sceKernelSendSignal");
    const auto thread = emuenv.kernel.get_thread(target_thread_id);
    if (!thread->signal.send()) {
        return SCE_KERNEL_ERROR_ALREADY_SENT;
    }
    return SCE_KERNEL_OK;
}

EXPORT(SceInt32, sceKernelSetEvent, SceUID event_id, SceUInt32 set_pattern, SceUInt64 user_data) {
    TRACY_FUNC(sceKernelSetEvent, event_id, set_pattern, user_data);
    return simple_event_setorpulse(emuenv.kernel, export_name, thread_id, event_id, set_pattern, user_data, true);
}

EXPORT(SceInt32, sceKernelSetEventFlag, SceUID evfId, SceUInt32 bitPattern) {
    TRACY_FUNC(sceKernelSetEventFlag, evfId, bitPattern);
    return eventflag_set(emuenv.kernel, export_name, thread_id, evfId, bitPattern);
}

EXPORT(int, sceKernelSetTimerTimeWide, SceUID timer_handle, SceUInt64 time) {
    TRACY_FUNC(sceKernelSetTimerTimeWide, timer_handle, time);
    const TimerPtr timer_info = timer_find(emuenv.kernel, timer_handle);
    if (!timer_info)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_TIMER_ID);

    auto oldTime = timer_info->time;
    timer_info->time = time;

    return oldTime;
}

EXPORT(int, sceKernelSignalCond, SceUID condid) {
    TRACY_FUNC(sceKernelSignalCond, condid);
    return condvar_signal(emuenv.kernel, emuenv.mem, export_name, thread_id, condid,
        Condvar::SignalTarget(Condvar::SignalTarget::Type::Any), SyncWeight::Heavy);
}

EXPORT(int, sceKernelSignalCondAll, SceUID condid) {
    TRACY_FUNC(sceKernelSignalCondAll, condid);
    return condvar_signal(emuenv.kernel, emuenv.mem, export_name, thread_id, condid,
        Condvar::SignalTarget(Condvar::SignalTarget::Type::All), SyncWeight::Heavy);
}

EXPORT(int, sceKernelSignalCondTo, SceUID condid, SceUID thread_target) {
    TRACY_FUNC(sceKernelSignalCondTo, condid, thread_target);
    return condvar_signal(emuenv.kernel, emuenv.mem, export_name, thread_id, condid,
        Condvar::SignalTarget(Condvar::SignalTarget::Type::Specific, thread_target), SyncWeight::Heavy);
}

EXPORT(int, sceKernelSignalSema, SceUID semaid, int signal) {
    TRACY_FUNC(sceKernelSignalSema, semaid, signal);
    return semaphore_signal(emuenv.kernel, export_name, thread_id, semaid, signal);
}

EXPORT(int, sceKernelStartTimer, SceUID timer_handle) {
    TRACY_FUNC(sceKernelStartTimer, timer_handle);
    return timer_start(emuenv.kernel, export_name, thread_id, timer_handle);
}

EXPORT(int, sceKernelStopTimer, SceUID timer_handle) {
    TRACY_FUNC(sceKernelStopTimer, timer_handle);
    return timer_stop(emuenv.kernel, export_name, thread_id, timer_handle);
}

EXPORT(int, sceKernelSuspendThreadForVM, SceUID threadId) {
    TRACY_FUNC(sceKernelSuspendThreadForVM, threadId);
    STUBBED("STUB");

    const ThreadStatePtr thread = emuenv.kernel.get_thread(threadId);
    if (!thread)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);

    thread->suspend();

    return 0;
}

EXPORT(int, sceKernelTryLockMutex, SceUID mutexid, int lock_count) {
    TRACY_FUNC(sceKernelTryLockMutex, mutexid, lock_count);
    return mutex_try_lock(emuenv.kernel, emuenv.mem, export_name, thread_id, mutexid, lock_count, SyncWeight::Heavy);
}

EXPORT(int, sceKernelTryLockReadRWLock) {
    TRACY_FUNC(sceKernelTryLockReadRWLock);
    return UNIMPLEMENTED();
}

EXPORT(int, sceKernelTryLockWriteRWLock) {
    TRACY_FUNC(sceKernelTryLockWriteRWLock);
    return UNIMPLEMENTED();
}

EXPORT(int, sceKernelUnlockMutex, SceUID mutexid, int unlock_count) {
    TRACY_FUNC(sceKernelUnlockMutex, mutexid, unlock_count);
    return mutex_unlock(emuenv.kernel, export_name, thread_id, mutexid, unlock_count, SyncWeight::Heavy);
}

EXPORT(int, sceKernelUnlockReadRWLock, SceUID lock_id) {
    TRACY_FUNC(sceKernelUnlockReadRWLock, lock_id);
    return rwlock_unlock(emuenv.kernel, emuenv.mem, export_name, thread_id, lock_id, false);
}

EXPORT(int, sceKernelUnlockWriteRWLock, SceUID lock_id) {
    TRACY_FUNC(sceKernelUnlockWriteRWLock, lock_id);
    return rwlock_unlock(emuenv.kernel, emuenv.mem, export_name, thread_id, lock_id, true);
}

EXPORT(int, sceKernelUnregisterCallbackFromEvent) {
    TRACY_FUNC(sceKernelUnregisterCallbackFromEvent);
    return UNIMPLEMENTED();
}

EXPORT(int, sceKernelUnregisterCallbackFromEventAll) {
    TRACY_FUNC(sceKernelUnregisterCallbackFromEventAll);
    return UNIMPLEMENTED();
}

EXPORT(int, sceKernelUnregisterThreadEventHandler, SceUID uid) {
    TRACY_FUNC(sceKernelUnregisterThreadEventHandler, uid);
    const std::lock_guard<std::mutex> guard(emuenv.kernel.thread_event_mutex);
    auto &handlers = emuenv.kernel.thread_event_handlers;
    const auto handler = std::find_if(handlers.begin(), handlers.end(), [&](const auto &entry) { return entry.uid == uid; });
    if (handler == handlers.end())
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_THREAD_EVENT_ID);
    handlers.erase(handler);
    return 0;
}

EXPORT(int, sceKernelWaitThreadEndCB_089) {
    TRACY_FUNC(sceKernelWaitThreadEndCB_089);
    return UNIMPLEMENTED();
}

EXPORT(int, sceKernelWaitThreadEnd_089) {
    TRACY_FUNC(sceKernelWaitThreadEnd_089);
    return UNIMPLEMENTED();
}
