// Firmware (SceKernelThreadMgr 3.74) object handles and the info syscalls of
// the production sync objects, without guest threads.
#pragma once
#include "guest_sync_delete_tests.h"
#include <kernel/callback.h>

DECL_EXPORT(SceUID, sceKernelOpenSema, const char *pName);
DECL_EXPORT(int, sceKernelDeleteSema, SceUID semaid);
DECL_EXPORT(int, sceKernelCloseSema, SceUID semaId);
DECL_EXPORT(SceUID, sceKernelOpenMutex, const char *pName);
DECL_EXPORT(int, sceKernelDeleteMutex, SceUID mutexid);
DECL_EXPORT(int, sceKernelCloseMutex, SceUID mutexId);
DECL_EXPORT(SceUID, sceKernelOpenRWLock, const char *pName);
DECL_EXPORT(SceInt32, sceKernelDeleteRWLock, SceUID lock_id);
DECL_EXPORT(int, sceKernelCloseRWLock, SceUID lockId);
DECL_EXPORT(SceUID, sceKernelOpenMsgPipe, const char *pName);
DECL_EXPORT(SceInt32, sceKernelDeleteMsgPipe, SceUID msgPipeId);
DECL_EXPORT(int, sceKernelCloseMsgPipe, SceUID msgPipeId);
DECL_EXPORT(SceUID, sceKernelOpenSimpleEvent, const char *pName);
DECL_EXPORT(int, sceKernelDeleteSimpleEvent, SceUID event_id);
DECL_EXPORT(int, sceKernelCloseSimpleEvent, SceUID eventId);
DECL_EXPORT(SceUID, sceKernelOpenEventFlag, const char *pName);
DECL_EXPORT(int, sceKernelDeleteEventFlag, SceUID event_id);
DECL_EXPORT(SceUID, sceKernelOpenCond, const char *pName);
DECL_EXPORT(int, sceKernelDeleteCond, SceUID condition_variable_id);
DECL_EXPORT(int, sceKernelCloseCond, SceUID condId);
DECL_EXPORT(SceUID, sceKernelOpenTimer, const char *pName);
DECL_EXPORT(int, sceKernelDeleteTimer, SceUID timer_handle);
DECL_EXPORT(int, sceKernelCloseTimer, SceUID timerId);
DECL_EXPORT(int, sceKernelStartTimer, SceUID timer_handle);
DECL_EXPORT(uint64_t, sceKernelGetTimerBaseWide, SceUID timer_handle);
DECL_EXPORT(int, ksceKernelDeleteEventFlag, SceUID evfId);
DECL_EXPORT(SceInt32, _sceKernelGetSemaInfo, SceUID semaId, Ptr<SceKernelSemaInfo> pInfo, const SceSize *pSize);
DECL_EXPORT(SceInt32, _sceKernelGetMutexInfo, SceUID mutexId, Ptr<SceKernelMutexInfo> pInfo, const SceSize *pSize);
DECL_EXPORT(SceInt32, _sceKernelGetEventFlagInfo, SceUID evfId, Ptr<SceKernelEventFlagInfo> pInfo, const SceSize *pSize);
DECL_EXPORT(SceInt32, _sceKernelGetRWLockInfo, SceUID rwlockId, Ptr<SceKernelRWLockInfo> pInfo, const SceSize *pSize);
DECL_EXPORT(SceInt32, _sceKernelGetLwMutexInfoById, SceUID lightweight_mutex_id, Ptr<SceKernelLwMutexInfo> pInfo, const SceSize *pSize);
DECL_EXPORT(SceInt32, _sceKernelGetCallbackInfo, SceUID callbackId, SceKernelCallbackInfo *pInfo, const SceSize *pSize);
DECL_EXPORT(SceInt32, sceKernelGetSemaInfo, SceUID semaId, Ptr<SceKernelSemaInfo> pInfo);
DECL_EXPORT(SceUID, _sceKernelCreateCond, const char *pName, SceUInt32 attr, SceUID mutexId, const SceKernelCondOptParam *pOptParam);

namespace guest_kernel_handles {
using Open = SceUID (*)(EmuEnvState &, SceUID, const char *, const char *);
using Close = int (*)(EmuEnvState &, SceUID, const char *, SceUID);

// Open, Delete and Close (SceKernelThreadMgr 3.74 Open 0x8102d468 and its
// siblings; Delete/Close over 0x8102c228..0x8102c3e4, 0x8102bfe0 and
// 0x8102c104): opening adds a uid for the same object, which lives until its
// last handle is closed. Delete expects the creating handle and Close an
// opened one, unless the title was built before SDK 3.10. `destroyed` tells
// whether an object is gone.
template <typename Objects, typename Create, typename Destroyed>
void check_handles(EmuEnvState &env, const Address param, Objects &objects, Create create, Open open, Close del, Close close,
    SceInt32 unknown_id, Destroyed destroyed) {
    auto *process = Ptr<SceProcessParam>(param).get(env.mem);
    std::memset(process, 0, sizeof(*process));
    process->magic = '2PSP';
    process->version = 1;
    process->fw_version = 0x03100000;
    for (const bool sdk_310 : { false, true }) {
        env.kernel.process_param = sdk_310 ? Ptr<SceProcessParam>(param) : Ptr<SceProcessParam>();
        const SceUID created = create("handle object");
        REQUIRE(created > 0);
        const auto object = objects.at(created);
        REQUIRE(open(env, 0, "fixture", nullptr) == SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        REQUIRE(open(env, 0, "fixture", "a name of thirty-two characters!") == SCE_KERNEL_ERROR_UID_NAME_TOO_LONG);
        REQUIRE(open(env, 0, "fixture", "no such object") == SCE_KERNEL_ERROR_UID_CANNOT_FIND_BY_NAME);
        const SceUID opened = open(env, 0, "fixture", "handle object");
        REQUIRE(opened > 0 && opened != created && objects.at(opened) == object && object->handles == 2);
        REQUIRE(is_opened_handle(*object, opened) && !is_opened_handle(*object, created));
        SceUID survivor = opened;
        if (sdk_310) {
            REQUIRE(close(env, 0, "fixture", created) == unknown_id);
            REQUIRE(del(env, 0, "fixture", opened) == unknown_id);
            REQUIRE(object->handles == 2 && objects.contains(created) && objects.contains(opened));
            REQUIRE(del(env, 0, "fixture", created) == 0);
        } else {
            REQUIRE(del(env, 0, "fixture", opened) == 0);
            survivor = created;
        }
        const SceUID closed = survivor == created ? opened : created;
        REQUIRE(object->handles == 1 && !destroyed(*object) && !objects.contains(closed));
        REQUIRE(close(env, 0, "fixture", closed) == unknown_id);
        REQUIRE(close(env, 0, "fixture", survivor) == 0);
        REQUIRE(destroyed(*object) && !objects.contains(survivor));
        REQUIRE(close(env, 0, "fixture", survivor) == unknown_id);
        REQUIRE(del(env, 0, "fixture", survivor) == unknown_id);
    }
    env.kernel.process_param = Ptr<SceProcessParam>();
}

// Envelope of the info syscalls (0x8102a834 and siblings): the caller's size
// word, then that many bytes of the record in and back out; the record fill
// checks the id, the record and its own size word.
template <typename Info, typename Call>
void check_info_envelope(EmuEnvState &env, Address address, SceUID valid, SceUID unknown, SceInt32 unknown_id, Call call) {
    auto *info = Ptr<Info>(address).get(env.mem);
    const Ptr<Info> info_ptr(address);
    const auto reset = [&](SceSize size) {
        std::memset(info, 0xcc, sizeof(Info) + 8);
        info->size = size;
    };
    const auto untouched_from = [&](size_t offset) {
        const auto *bytes = reinterpret_cast<const uint8_t *>(info);
        for (size_t i = offset; i < sizeof(Info) + 8; ++i)
            if (bytes[i] != 0xcc)
                return false;
        return true;
    };
    SceSize size = sizeof(Info);
    REQUIRE(call(valid, info_ptr, nullptr) == SCE_KERNEL_ERROR_INVALID_MEMORY_ACCESS);
    REQUIRE(call(unknown, Ptr<Info>(), &size) == unknown_id);
    REQUIRE(call(valid, Ptr<Info>(), &size) == SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    reset(sizeof(Info));
    size = sizeof(Info) + 1;
    REQUIRE(call(valid, info_ptr, &size) == SCE_KERNEL_ERROR_NO_MEMORY);
    REQUIRE(untouched_from(4));
    size = sizeof(Info);
    REQUIRE(call(unknown, info_ptr, &size) == unknown_id);
    REQUIRE(info->size == sizeof(Info) && untouched_from(4));
    reset(sizeof(Info) + 1);
    REQUIRE(call(valid, info_ptr, &size) == SCE_KERNEL_ERROR_ILLEGAL_SIZE);
    REQUIRE(info->size == sizeof(Info) + 1 && untouched_from(4));
    // A short record gets its prefix; the uid follows at offset 4.
    reset(8);
    REQUIRE(call(valid, info_ptr, &size) == 0);
    REQUIRE(info->size == sizeof(Info) && Ptr<SceUID>(address + 4).get(env.mem)[0] == valid && untouched_from(8));
    reset(sizeof(Info));
    size = 8;
    REQUIRE(call(valid, info_ptr, &size) == 0);
    REQUIRE(info->size == sizeof(Info) && Ptr<SceUID>(address + 4).get(env.mem)[0] == valid && untouched_from(8));
    // Even below 8 the syscall moves the uid when its size word asks for it.
    reset(4);
    size = sizeof(Info);
    REQUIRE(call(valid, info_ptr, &size) == 0);
    REQUIRE(info->size == sizeof(Info) && Ptr<SceUID>(address + 4).get(env.mem)[0] == valid && untouched_from(8));
    reset(sizeof(Info));
    REQUIRE(call(valid, info_ptr, &size) == 0 && info->size == sizeof(Info) && untouched_from(sizeof(Info)));
}
} // namespace guest_kernel_handles

inline void test_guest_kernel_handles(EmuEnvState &env, vita3k::web::GuestThreadRuntime &runtime) {
    using namespace guest_kernel_handles;
    const Address data = alloc(env.mem, 0x1000, "kernel handle data");
    REQUIRE(data);
    env.kernel.call_import = [&](CPUState &cpu, uint32_t nid, SceUID tid) {
        call_import(env, cpu, nid, tid);
        REQUIRE(env.missing_nids.empty());
    };
    REQUIRE(runtime.attach(env));
    const Address param = data + 0x800;
    auto &kernel = env.kernel;
    const auto gone = [](const SyncPrimitive &object) { return object.deleted.load(); };

    check_handles(env, param, kernel.semaphores,
        [&](const char *name) { return semaphore_create(kernel, "fixture", name, 0, 0, 0, 1); },
        export_sceKernelOpenSema, export_sceKernelDeleteSema, export_sceKernelCloseSema, SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID, gone);
    check_handles(env, param, kernel.mutexes,
        [&](const char *name) {
            SceUID uid = -1;
            REQUIRE(mutex_create(&uid, kernel, env.mem, "fixture", name, 0, 0, 0, Ptr<SceKernelLwMutexWork>(), SyncWeight::Heavy) == 0);
            return uid;
        },
        export_sceKernelOpenMutex, export_sceKernelDeleteMutex, export_sceKernelCloseMutex, SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID, gone);
    check_handles(env, param, kernel.rwlocks,
        [&](const char *name) { return rwlock_create(kernel, env.mem, "fixture", name, 0, 0); },
        export_sceKernelOpenRWLock, export_sceKernelDeleteRWLock, export_sceKernelCloseRWLock, SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID, gone);
    check_handles(env, param, kernel.msgpipes,
        [&](const char *name) { return msgpipe_create(kernel, "fixture", name, 0, 0, 0x100); },
        export_sceKernelOpenMsgPipe, export_sceKernelDeleteMsgPipe, export_sceKernelCloseMsgPipe, SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID,
        [&](const MsgPipe &pipe) {
            return std::none_of(kernel.msgpipes.begin(), kernel.msgpipes.end(), [&](const auto &entry) { return entry.second.get() == &pipe; });
        });
    check_handles(env, param, kernel.simple_events,
        [&](const char *name) { return simple_event_create(kernel, env.mem, "fixture", name, 0, 0, 0); },
        export_sceKernelOpenSimpleEvent, export_sceKernelDeleteSimpleEvent, export_sceKernelCloseSimpleEvent, SCE_KERNEL_ERROR_UNKNOWN_EVENT_ID, gone);
    check_handles(env, param, kernel.eventflags,
        [&](const char *name) { return eventflag_create(kernel, "fixture", 0, name, 0, 0); },
        export_sceKernelOpenEventFlag, export_sceKernelDeleteEventFlag, export_sceKernelCloseEventFlag, SCE_KERNEL_ERROR_UNKNOWN_EVF_ID, gone);
    std::puts("Open, Delete and Close handles of sema, mutex, rwlock, msgpipe, simple event and event flag passed");

    // Conditions: DeleteCond and CloseCond close any handle (0x8102d8a8,
    // CloseCond 0x8102d988 calls it), whatever the SDK.
    {
        SceUID mutex_id = -1;
        REQUIRE(mutex_create(&mutex_id, kernel, env.mem, "fixture", "cond handle mutex", 0, 0, 0, Ptr<SceKernelLwMutexWork>(), SyncWeight::Heavy) == 0);
        for (const bool delete_created : { false, true }) {
            SceUID cond_id = -1;
            REQUIRE(condvar_create(&cond_id, kernel, "fixture", "handle cond", 0, 0, mutex_id, Ptr<SceKernelLwCondWork>(), SyncWeight::Heavy) == 0);
            const auto cond = kernel.condvars.at(cond_id);
            REQUIRE(export_sceKernelOpenCond(env, 0, "fixture", nullptr) == SCE_KERNEL_ERROR_ILLEGAL_ADDR);
            REQUIRE(export_sceKernelOpenCond(env, 0, "fixture", "no such cond") == SCE_KERNEL_ERROR_UID_CANNOT_FIND_BY_NAME);
            const SceUID opened = export_sceKernelOpenCond(env, 0, "fixture", "handle cond");
            REQUIRE(opened > 0 && kernel.condvars.at(opened) == cond && cond->handles == 2);
            const SceUID first = delete_created ? cond_id : opened, second = delete_created ? opened : cond_id;
            REQUIRE(export_sceKernelDeleteCond(env, 0, "fixture", first) == 0);
            REQUIRE(cond->handles == 1 && !cond->deleted && cond->associated_mutex);
            REQUIRE(export_sceKernelCloseCond(env, 0, "fixture", first) == SCE_KERNEL_ERROR_UNKNOWN_COND_ID);
            REQUIRE(export_sceKernelCloseCond(env, 0, "fixture", second) == 0);
            REQUIRE(cond->deleted && kernel.condvars.empty());
        }
        REQUIRE(export_sceKernelDeleteMutex(env, 0, "fixture", mutex_id) == 0);
        std::puts("Condition handles passed");
    }

    // Timers: DeleteTimer (0x8101b5ec, 0x8101a478) deletes the timer even
    // with opened handles, which then only close (CloseTimer 0x8102e084).
    {
        const SceUID timer = timer_create(kernel, env.mem, "fixture", "handle timer", 0, 0);
        REQUIRE(timer > 0);
        const auto object = kernel.timers.at(timer);
        REQUIRE(export_sceKernelOpenTimer(env, 0, "fixture", nullptr) == SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        SceUID opened;
        {
            // A desktop waiter holds the timer's mutex while it waits; opening
            // and looking the timer up must not need it.
            const std::lock_guard<std::mutex> waiter_holds(object->mutex);
            opened = export_sceKernelOpenTimer(env, 0, "fixture", "handle timer");
            REQUIRE(timer_find(kernel, opened) == object);
        }
        REQUIRE(opened > 0 && kernel.timers.at(opened) == object && object->handles == 2);
        REQUIRE(export_sceKernelStartTimer(env, 0, "fixture", opened) == 0 && object->is_started);
        REQUIRE(export_sceKernelDeleteTimer(env, 0, "fixture", timer) == 0);
        REQUIRE(object->deleted && !object->is_started && object->handles == 1 && kernel.timers.contains(opened));
        REQUIRE(uint32_t(export_sceKernelGetTimerBaseWide(env, 0, "fixture", opened)) == uint32_t(SCE_KERNEL_ERROR_UNKNOWN_TIMER_ID));
        REQUIRE(export_sceKernelStartTimer(env, 0, "fixture", opened) == SCE_KERNEL_ERROR_UNKNOWN_TIMER_ID);
        REQUIRE(export_sceKernelOpenTimer(env, 0, "fixture", "handle timer") == SCE_KERNEL_ERROR_UID_CANNOT_FIND_BY_NAME);
        REQUIRE(export_sceKernelDeleteTimer(env, 0, "fixture", opened) == SCE_KERNEL_ERROR_UNKNOWN_TIMER_ID);
        REQUIRE(export_sceKernelCloseTimer(env, 0, "fixture", opened) == 0);
        REQUIRE(kernel.timers.empty());
        REQUIRE(export_sceKernelCloseTimer(env, 0, "fixture", opened) == SCE_KERNEL_ERROR_UNKNOWN_TIMER_ID);
        // The last handle's Close deletes a timer that was not deleted
        // (titles before SDK 3.10 may close the creating handle).
        const SceUID only = timer_create(kernel, env.mem, "fixture", "closed timer", 0, 0);
        const auto closed = kernel.timers.at(only);
        REQUIRE(export_sceKernelCloseTimer(env, 0, "fixture", only) == 0);
        REQUIRE(closed->deleted && kernel.timers.empty());
        std::puts("Timer handles passed");
    }

    // ksceKernelDeleteEventFlag (0x8100f950): any handle of an event flag;
    // any other uid, unknown ones included, is DIFFERENT_UID_CLASS.
    {
        const SceUID evf = eventflag_create(kernel, "fixture", 0, "kernel evf", 0, 0);
        const SceUID opened = eventflag_open(kernel, "fixture", "kernel evf");
        const auto flag = kernel.eventflags.at(evf);
        const SceUID sema = semaphore_create(kernel, "fixture", "not an evf", 0, 0, 0, 1);
        REQUIRE(export_ksceKernelDeleteEventFlag(env, 0, "fixture", sema) == SCE_KERNEL_ERROR_DIFFERENT_UID_CLASS);
        REQUIRE(export_ksceKernelDeleteEventFlag(env, 0, "fixture", 0x7ffffff0) == SCE_KERNEL_ERROR_DIFFERENT_UID_CLASS);
        const Address sdk_param = param;
        Ptr<SceProcessParam>(sdk_param).get(env.mem)->fw_version = 0x03600000;
        kernel.process_param = Ptr<SceProcessParam>(sdk_param);
        REQUIRE(export_ksceKernelDeleteEventFlag(env, 0, "fixture", opened) == 0 && flag->handles == 1 && !flag->deleted);
        REQUIRE(export_ksceKernelDeleteEventFlag(env, 0, "fixture", evf) == 0 && flag->deleted && kernel.eventflags.empty());
        kernel.process_param = Ptr<SceProcessParam>();
        REQUIRE(export_sceKernelDeleteSema(env, 0, "fixture", sema) == 0);
        std::puts("Driver event flag deletion passed");
    }

    // Info syscalls: SceLibKernel passes the record's size word; the syscalls
    // (sema 0x8102a834, mutex 0x8102aa3c, event flag 0x8102a5a0, cond
    // 0x8102ab84, rwlock 0x8102b8bc, lwmutex 0x8102afc4, callback 0x81029dc0)
    // move that many bytes. The record carries the uid the caller passed; an
    // opened handle's record has attribute 0x80000 (not for conditions).
    {
        const Address info = data + 0x100;
        SceUID sema = semaphore_create(kernel, "fixture", "info sema", 0, 0x2000, 1, 3);
        REQUIRE(semaphore_signal(kernel, "fixture", 0, sema, 1) == 0);
        check_info_envelope<SceKernelSemaInfo>(env, info, sema, 0x7ffffff0, SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID, [&](SceUID id, Ptr<SceKernelSemaInfo> p, const SceSize *size) {
            return export__sceKernelGetSemaInfo(env, 0, "fixture", id, p, size);
        });
        auto *sema_info = Ptr<SceKernelSemaInfo>(info).get(env.mem);
        REQUIRE(sema_info->size == 0x3c && sema_info->semaId == sema && std::strcmp(sema_info->name, "info sema") == 0);
        REQUIRE(sema_info->attr == 0x2000 && sema_info->initCount == 1 && sema_info->currentCount == 2);
        REQUIRE(sema_info->maxCount == 3 && sema_info->numWaitThreads == 0);
        const SceUID opened_sema = semaphore_open(kernel, "fixture", "info sema");
        sema_info->size = sizeof(*sema_info);
        REQUIRE(export_sceKernelGetSemaInfo(env, 0, "fixture", opened_sema, Ptr<SceKernelSemaInfo>(info)) == 0);
        REQUIRE(sema_info->semaId == opened_sema && sema_info->attr == (0x2000 | 0x80000));
        // SceLibKernel passes 0 without a record.
        REQUIRE(export_sceKernelGetSemaInfo(env, 0, "fixture", sema, Ptr<SceKernelSemaInfo>()) == SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        REQUIRE(export_sceKernelCloseSema(env, 0, "fixture", opened_sema) == 0);
        REQUIRE(export_sceKernelDeleteSema(env, 0, "fixture", sema) == 0);

        auto owner = kernel.create_thread(env.mem, "info owner", Ptr<const void>(data), SCE_KERNEL_DEFAULT_PRIORITY_USER,
            SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        REQUIRE(owner);
        SceUID mutex = -1;
        REQUIRE(mutex_create(&mutex, kernel, env.mem, "fixture", "info mutex", owner->id, SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 2, Ptr<SceKernelLwMutexWork>(), SyncWeight::Heavy) == 0);
        check_info_envelope<SceKernelMutexInfo>(env, info, mutex, 0x7ffffff0, SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID, [&](SceUID id, Ptr<SceKernelMutexInfo> p, const SceSize *size) {
            return export__sceKernelGetMutexInfo(env, 0, "fixture", id, p, size);
        });
        auto *mutex_info = Ptr<SceKernelMutexInfo>(info).get(env.mem);
        REQUIRE(mutex_info->size == 0x40 && mutex_info->mutexId == mutex && std::strcmp(mutex_info->name, "info mutex") == 0);
        REQUIRE(mutex_info->attr == SCE_KERNEL_MUTEX_ATTR_RECURSIVE && mutex_info->initCount == 2 && mutex_info->currentCount == 2);
        REQUIRE(mutex_info->currentOwnerId == owner->id && mutex_info->numWaitThreads == 0 && mutex_info->ceilingPriority == 0);
        // Only a recursive mutex starts locked more than once (0x8100e018).
        SceUID rejected = -1;
        REQUIRE(mutex_create(&rejected, kernel, env.mem, "fixture", "count 2", owner->id, 0, 2, Ptr<SceKernelLwMutexWork>(), SyncWeight::Heavy) == SCE_KERNEL_ERROR_ILLEGAL_COUNT);
        REQUIRE(mutex_create(&rejected, kernel, env.mem, "fixture", "count -1", owner->id, SCE_KERNEL_MUTEX_ATTR_RECURSIVE, -1, Ptr<SceKernelLwMutexWork>(), SyncWeight::Heavy) == SCE_KERNEL_ERROR_ILLEGAL_COUNT);
        REQUIRE(rejected == -1);
        const SceUID opened_mutex = mutex_open(kernel, "fixture", "info mutex");
        const SceSize mutex_size = sizeof(SceKernelMutexInfo);
        REQUIRE(export__sceKernelGetMutexInfo(env, 0, "fixture", opened_mutex, Ptr<SceKernelMutexInfo>(info), &mutex_size) == 0);
        REQUIRE(mutex_info->mutexId == opened_mutex && mutex_info->attr == (SCE_KERNEL_MUTEX_ATTR_RECURSIVE | 0x80000));

        SceUID cond = -1;
        REQUIRE(condvar_create(&cond, kernel, "fixture", "info cond", 0, 0x2000, mutex, Ptr<SceKernelLwCondWork>(), SyncWeight::Heavy) == 0);
        check_info_envelope<SceKernelCondInfo>(env, info, cond, 0x7ffffff0, SCE_KERNEL_ERROR_UNKNOWN_COND_ID, [&](SceUID id, Ptr<SceKernelCondInfo> p, const SceSize *size) {
            return export__sceKernelGetCondInfo(env, 0, "fixture", id, p, size);
        });
        auto *cond_info = Ptr<SceKernelCondInfo>(info).get(env.mem);
        REQUIRE(cond_info->size == 0x34 && cond_info->condId == cond && cond_info->attr == 0x2000);
        REQUIRE(cond_info->mutexId == mutex && cond_info->numWaitThreads == 0 && std::strcmp(cond_info->name, "info cond") == 0);
        const SceUID opened_cond = condvar_open(kernel, "fixture", "info cond");
        const SceSize cond_size = sizeof(SceKernelCondInfo);
        REQUIRE(export__sceKernelGetCondInfo(env, 0, "fixture", opened_cond, Ptr<SceKernelCondInfo>(info), &cond_size) == 0);
        REQUIRE(cond_info->condId == opened_cond && cond_info->attr == 0x2000);
        REQUIRE(export_sceKernelDeleteCond(env, 0, "fixture", opened_cond) == 0 && export_sceKernelDeleteCond(env, 0, "fixture", cond) == 0);
        REQUIRE(export_sceKernelCloseMutex(env, 0, "fixture", opened_mutex) == 0 && export_sceKernelDeleteMutex(env, 0, "fixture", mutex) == 0);

        const SceUID evf = eventflag_create(kernel, "fixture", 0, "info evf", 0x1000, 0x5);
        REQUIRE(eventflag_set(kernel, "fixture", 0, evf, 0x30) == 0);
        check_info_envelope<SceKernelEventFlagInfo>(env, info, evf, 0x7ffffff0, SCE_KERNEL_ERROR_UNKNOWN_EVF_ID, [&](SceUID id, Ptr<SceKernelEventFlagInfo> p, const SceSize *size) {
            return export__sceKernelGetEventFlagInfo(env, 0, "fixture", id, p, size);
        });
        auto *evf_info = Ptr<SceKernelEventFlagInfo>(info).get(env.mem);
        REQUIRE(evf_info->size == 0x38 && evf_info->evfId == evf && evf_info->attr == 0x1000);
        REQUIRE(evf_info->initPattern == 0x5 && evf_info->currentPattern == 0x35 && evf_info->numWaitThreads == 0);
        REQUIRE(export_sceKernelDeleteEventFlag(env, 0, "fixture", evf) == 0);

        const SceUID rwlock = rwlock_create(kernel, env.mem, "fixture", "info rwlock", 0, 0);
        check_info_envelope<SceKernelRWLockInfo>(env, info, rwlock, 0x7ffffff0, SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID, [&](SceUID id, Ptr<SceKernelRWLockInfo> p, const SceSize *size) {
            return export__sceKernelGetRWLockInfo(env, 0, "fixture", id, p, size);
        });
        auto *rwlock_info = Ptr<SceKernelRWLockInfo>(info).get(env.mem);
        REQUIRE(rwlock_info->size == 0x3c && rwlock_info->rwLockId == rwlock && rwlock_info->lockCount == 0 && rwlock_info->writeOwnerId == 0);
        REQUIRE(rwlock_lock(kernel, env.mem, "fixture", owner->id, rwlock, nullptr, true) == 0);
        const SceSize rwlock_size = sizeof(SceKernelRWLockInfo);
        REQUIRE(export__sceKernelGetRWLockInfo(env, 0, "fixture", rwlock, Ptr<SceKernelRWLockInfo>(info), &rwlock_size) == 0);
        REQUIRE(rwlock_info->lockCount == 1 && rwlock_info->writeOwnerId == owner->id);
        REQUIRE(rwlock_info->numReadWaitThreads == 0 && rwlock_info->numWriteWaitThreads == 0);
        REQUIRE(rwlock_unlock(kernel, env.mem, "fixture", owner->id, rwlock, true) == 0);
        REQUIRE(export_sceKernelDeleteRWLock(env, 0, "fixture", rwlock) == 0);

        const Address work = data + 0x400;
        SceUID lwmutex = -1;
        REQUIRE(mutex_create(&lwmutex, kernel, env.mem, "fixture", "info lwmutex", owner->id, 0, 1, Ptr<SceKernelLwMutexWork>(work), SyncWeight::Light) == 0);
        check_info_envelope<SceKernelLwMutexInfo>(env, info, lwmutex, 0x7ffffff0, SCE_KERNEL_ERROR_UNKNOWN_LW_MUTEX_ID, [&](SceUID id, Ptr<SceKernelLwMutexInfo> p, const SceSize *size) {
            return export__sceKernelGetLwMutexInfoById(env, 0, "fixture", id, p, size);
        });
        auto *lw_info = Ptr<SceKernelLwMutexInfo>(info).get(env.mem);
        REQUIRE(lw_info->size == 0x40 && lw_info->uid == lwmutex && lw_info->pWork.address() == work);
        REQUIRE(lw_info->initCount == 1 && lw_info->currentCount == 1 && lw_info->currentOwnerId == owner->id && lw_info->numWaitThreads == 0);
        REQUIRE(mutex_close(kernel, env.mem, "fixture", owner->id, lwmutex, SyncWeight::Light, HandleClose::Delete) == 0);

        // Callbacks: the owner thread must still exist (0x8100baac).
        const SceUID callback = kernel.get_next_uid();
        std::string callback_name = "info callback";
        const auto cb = std::make_shared<Callback>(owner->id, callback_name, Ptr<SceKernelCallbackFunction>(data + 0x10), Ptr<void>(data + 0x20));
        kernel.callbacks.emplace(callback, cb);
        cb->direct_notify(7);
        const auto callback_call = [&](SceUID id, Ptr<SceKernelCallbackInfo> p, const SceSize *size) {
            return export__sceKernelGetCallbackInfo(env, 0, "fixture", id, p.get(env.mem), size);
        };
        check_info_envelope<SceKernelCallbackInfo>(env, info, callback, 0x7ffffff0, SCE_KERNEL_ERROR_UNKNOWN_CALLBACK_ID, callback_call);
        auto *cb_info = Ptr<SceKernelCallbackInfo>(info).get(env.mem);
        REQUIRE(cb_info->size == 0x44 && cb_info->callbackId == callback && std::strcmp(cb_info->name, "info callback") == 0);
        REQUIRE(cb_info->attr == 0 && cb_info->threadId == owner->id && cb_info->callbackFunc.address() == data + 0x10);
        REQUIRE(cb_info->notifyCount == 1 && cb_info->notifyArg == 7 && cb_info->pCommon.address() == data + 0x20);
        owner->exit_delete(false);
        REQUIRE(runtime.resume(8).failed == 0 && !kernel.get_thread(owner->id));
        const SceSize callback_size = sizeof(SceKernelCallbackInfo);
        REQUIRE(callback_call(callback, Ptr<SceKernelCallbackInfo>(info), &callback_size) == SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
        kernel.callbacks.erase(callback);
        std::puts("Info syscalls passed");
    }

    // _sceKernelCreateCond (0x8102aacc, 0x8102d840, ksceKernelCreateCond
    // 0x810200c4): the name and the mutex, then the calling thread, then
    // TH_PRIO | OPENABLE only, OPENABLE only before SDK 2.10, options of at
    // most their size word.
    {
        auto caller = kernel.create_thread(env.mem, "cond creator", Ptr<const void>(data), SCE_KERNEL_DEFAULT_PRIORITY_USER,
            SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        SceUID mutex = -1;
        REQUIRE(mutex_create(&mutex, kernel, env.mem, "fixture", "cond create mutex", 0, 0, 0, Ptr<SceKernelLwMutexWork>(), SyncWeight::Heavy) == 0);
        const auto create = [&](SceUID tid, const char *name, SceUInt32 attr, SceUID mutex_id, const SceKernelCondOptParam *opt) {
            return export__sceKernelCreateCond(env, tid, "fixture", name, attr, mutex_id, opt);
        };
        const SceKernelCondOptParam small{ 4 }, big{ 5 };
        REQUIRE(create(caller->id, nullptr, 0x100, 0x7ffffff0, &big) == SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        REQUIRE(create(0, "new cond", 0x100, 0x7ffffff0, &big) == SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID);
        REQUIRE(create(0, "new cond", 0x100, mutex, &big) == SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
        REQUIRE(create(caller->id, "new cond", 0x100, mutex, &big) == SCE_KERNEL_ERROR_ILLEGAL_ATTR);
        REQUIRE(create(caller->id, "new cond", 0x2000, mutex, &big) == SCE_KERNEL_ERROR_ILLEGAL_SIZE);
        auto *process = Ptr<SceProcessParam>(param).get(env.mem);
        process->fw_version = 0x02100000;
        kernel.process_param = Ptr<SceProcessParam>(param);
        REQUIRE(create(caller->id, "new cond", 0x80, mutex, &small) == SCE_KERNEL_ERROR_ILLEGAL_ATTR);
        const SceUID plain = create(caller->id, "new cond", 0x2000, mutex, &small);
        REQUIRE(plain > 0 && kernel.condvars.at(plain)->attr == 0x2000);
        process->fw_version = 0x02000000;
        const SceUID openable = create(caller->id, "openable cond", 0x80 | 0x2000, mutex, nullptr);
        REQUIRE(openable > 0 && kernel.condvars.at(openable)->attr == (0x80 | 0x2000));
        kernel.process_param = Ptr<SceProcessParam>();
        REQUIRE(export_sceKernelDeleteCond(env, 0, "fixture", plain) == 0 && export_sceKernelDeleteCond(env, 0, "fixture", openable) == 0);
        REQUIRE(export_sceKernelDeleteMutex(env, 0, "fixture", mutex) == 0);
        caller->exit_delete(false);
        std::puts("CreateCond checks passed");
    }
    REQUIRE(runtime.shutdown());
    REQUIRE(kernel.threads.empty());
    env.kernel.call_import = {};
    free(env.mem, data);
}
