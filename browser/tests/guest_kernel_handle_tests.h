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
DECL_EXPORT(int, sceKernelStopTimer, SceUID timer_handle);
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
DECL_EXPORT(int, _sceKernelCreateMutex, const char *name, SceUInt attr, int init_count, SceKernelMutexOptParam *opt_param);
DECL_EXPORT(int, ksceKernelCreateMutex, const char *name, SceUInt attr, int init_count, SceKernelMutexOptParam *opt_param);
DECL_EXPORT(int, ksceKernelDeleteMutex, SceUID mutexid);
DECL_EXPORT(int, ksceKernelDeleteSema, SceUID semaId);
DECL_EXPORT(int, ksceKernelDeleteCond, SceUID condId);
DECL_EXPORT(int, ksceKernelDeleteMsgPipe, SceUID msgPipeId);
DECL_EXPORT(SceInt32, sceKernelChangeThreadPriority2, SceUID thid, SceInt32 priority);
DECL_EXPORT(int, sceKernelCancelMutex, SceUID mutexId, SceInt32 newCount, SceUInt32 *pNumWaitThreads);

namespace guest_kernel_handles {
using Open = SceUID (*)(EmuEnvState &, SceUID, const char *, const char *);
using Close = int (*)(EmuEnvState &, SceUID, const char *, SceUID);

// Open, Delete and Close (SceKernelThreadMgr 3.74 Open 0x8102d468 and its
// siblings; Delete/Close over 0x8102c228..0x8102c3e4, 0x8102bfe0 and
// 0x8102c104): only an object created OPENABLE (object flags 0xc000, which
// register its name in SceSysmem 0x810037b8) is found. Opening adds a uid for
// the same object, which lives until its last handle is closed. Delete
// expects the creating handle and Close an opened one, unless the title was
// built before SDK 3.10. `destroyed` tells whether an object is gone.
template <typename Objects, typename Create, typename Destroyed>
void check_handles(EmuEnvState &env, Objects &objects, Create create, Open open, Close del, Close close,
    SceInt32 unknown_id, Destroyed destroyed) {
    for (const bool sdk_310 : { false, true }) {
        env.kernel.process_sdk_version = sdk_310 ? 0x03100000 : 0x03000000;
        const SceUID hidden = create("hidden object", 0);
        REQUIRE(hidden > 0);
        REQUIRE(open(env, 0, "fixture", "hidden object") == SCE_KERNEL_ERROR_UID_CANNOT_FIND_BY_NAME);
        REQUIRE(del(env, 0, "fixture", hidden) == 0);
        const SceUID created = create("handle object", SCE_KERNEL_ATTR_OPENABLE);
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
        // The name stays registered while the object lives.
        const SceUID again = open(env, 0, "fixture", "handle object");
        REQUIRE(again > 0 && objects.at(again) == object && close(env, 0, "fixture", again) == 0);
        REQUIRE(close(env, 0, "fixture", closed) == unknown_id);
        REQUIRE(close(env, 0, "fixture", survivor) == 0);
        REQUIRE(destroyed(*object) && !objects.contains(survivor));
        REQUIRE(close(env, 0, "fixture", survivor) == unknown_id);
        REQUIRE(del(env, 0, "fixture", survivor) == unknown_id);
    }
    env.kernel.process_sdk_version = 0;
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
    auto &kernel = env.kernel;
    const auto gone = [](const SyncPrimitive &object) { return object.deleted.load(); };

    check_handles(env, kernel.semaphores,
        [&](const char *name, SceUInt32 attr) { return semaphore_create(kernel, "fixture", name, 0, attr, 0, 1); },
        export_sceKernelOpenSema, export_sceKernelDeleteSema, export_sceKernelCloseSema, SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID, gone);
    check_handles(env, kernel.mutexes,
        [&](const char *name, SceUInt32 attr) {
            SceUID uid = -1;
            REQUIRE(mutex_create(&uid, kernel, env.mem, "fixture", name, 0, attr, 0, Ptr<SceKernelLwMutexWork>(), SyncWeight::Heavy) == 0);
            return uid;
        },
        export_sceKernelOpenMutex, export_sceKernelDeleteMutex, export_sceKernelCloseMutex, SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID, gone);
    check_handles(env, kernel.rwlocks,
        [&](const char *name, SceUInt32 attr) { return rwlock_create(kernel, env.mem, "fixture", name, 0, attr); },
        export_sceKernelOpenRWLock, export_sceKernelDeleteRWLock, export_sceKernelCloseRWLock, SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID, gone);
    check_handles(env, kernel.msgpipes,
        [&](const char *name, SceUInt32 attr) { return msgpipe_create(kernel, "fixture", name, 0, attr, 0x100); },
        export_sceKernelOpenMsgPipe, export_sceKernelDeleteMsgPipe, export_sceKernelCloseMsgPipe, SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID,
        [&](const MsgPipe &pipe) {
            return std::none_of(kernel.msgpipes.begin(), kernel.msgpipes.end(), [&](const auto &entry) { return entry.second.get() == &pipe; });
        });
    check_handles(env, kernel.simple_events,
        [&](const char *name, SceUInt32 attr) { return simple_event_create(kernel, env.mem, "fixture", name, 0, attr, 0); },
        export_sceKernelOpenSimpleEvent, export_sceKernelDeleteSimpleEvent, export_sceKernelCloseSimpleEvent, SCE_KERNEL_ERROR_UNKNOWN_EVENT_ID, gone);
    check_handles(env, kernel.eventflags,
        [&](const char *name, SceUInt32 attr) { return eventflag_create(kernel, "fixture", 0, name, attr, 0); },
        export_sceKernelOpenEventFlag, export_sceKernelDeleteEventFlag, export_sceKernelCloseEventFlag, SCE_KERNEL_ERROR_UNKNOWN_EVF_ID, gone);
    std::puts("Open, Delete and Close handles of sema, mutex, rwlock, msgpipe, simple event and event flag passed");

    // Open looks names up among all registered objects (0xbde00106 and
    // 0x368f1c36 for sema, mutex, cond, rwlock and event flag): another
    // class's object is DIFFERENT_UID_CLASS, a name two objects share is not
    // found (SceSysmem 0x81001fe8 wants a single entry). Simple events,
    // timers and message pipes are looked up in their class (0x201e970b).
    {
        const SceUID sema = semaphore_create(kernel, "fixture", "shared name", 0, SCE_KERNEL_ATTR_OPENABLE, 0, 1);
        REQUIRE(export_sceKernelOpenMutex(env, 0, "fixture", "shared name") == SCE_KERNEL_ERROR_DIFFERENT_UID_CLASS);
        REQUIRE(export_sceKernelOpenEventFlag(env, 0, "fixture", "shared name") == SCE_KERNEL_ERROR_DIFFERENT_UID_CLASS);
        REQUIRE(export_sceKernelOpenSimpleEvent(env, 0, "fixture", "shared name") == SCE_KERNEL_ERROR_UID_CANNOT_FIND_BY_NAME);
        const SceUID event = simple_event_create(kernel, env.mem, "fixture", "shared name", 0, SCE_KERNEL_ATTR_OPENABLE, 0);
        REQUIRE(export_sceKernelOpenSema(env, 0, "fixture", "shared name") == SCE_KERNEL_ERROR_UID_CANNOT_FIND_BY_NAME);
        const SceUID opened_event = export_sceKernelOpenSimpleEvent(env, 0, "fixture", "shared name");
        REQUIRE(opened_event > 0 && kernel.simple_events.at(opened_event) == kernel.simple_events.at(event));
        REQUIRE(export_sceKernelCloseSimpleEvent(env, 0, "fixture", opened_event) == 0);
        REQUIRE(export_sceKernelDeleteSimpleEvent(env, 0, "fixture", event) == 0);
        const SceUID reopened = export_sceKernelOpenSema(env, 0, "fixture", "shared name");
        REQUIRE(reopened > 0 && kernel.semaphores.at(reopened) == kernel.semaphores.at(sema));
        REQUIRE(export_sceKernelCloseSema(env, 0, "fixture", reopened) == 0 && export_sceKernelDeleteSema(env, 0, "fixture", sema) == 0);
        std::puts("Open by name across classes passed");
    }

    // Conditions: DeleteCond and CloseCond close any handle (0x8102d8a8,
    // CloseCond 0x8102d988 calls it), whatever the SDK.
    {
        SceUID mutex_id = -1;
        REQUIRE(mutex_create(&mutex_id, kernel, env.mem, "fixture", "cond handle mutex", 0, 0, 0, Ptr<SceKernelLwMutexWork>(), SyncWeight::Heavy) == 0);
        for (const bool delete_created : { false, true }) {
            SceUID cond_id = -1;
            REQUIRE(condvar_create(&cond_id, kernel, "fixture", "handle cond", 0, SCE_KERNEL_ATTR_OPENABLE, mutex_id, Ptr<SceKernelLwCondWork>(), SyncWeight::Heavy) == 0);
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
        const SceUID timer = timer_create(kernel, env.mem, "fixture", "handle timer", 0, SCE_KERNEL_ATTR_OPENABLE);
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
        const SceUID evf = eventflag_create(kernel, "fixture", 0, "kernel evf", SCE_KERNEL_ATTR_OPENABLE, 0);
        const SceUID opened = eventflag_open(kernel, "fixture", "kernel evf");
        const auto flag = kernel.eventflags.at(evf);
        const SceUID sema = semaphore_create(kernel, "fixture", "not an evf", 0, 0, 0, 1);
        REQUIRE(export_ksceKernelDeleteEventFlag(env, 0, "fixture", sema) == SCE_KERNEL_ERROR_DIFFERENT_UID_CLASS);
        REQUIRE(export_ksceKernelDeleteEventFlag(env, 0, "fixture", 0x7ffffff0) == SCE_KERNEL_ERROR_DIFFERENT_UID_CLASS);
        kernel.process_sdk_version = 0x03600000;
        REQUIRE(export_ksceKernelDeleteEventFlag(env, 0, "fixture", opened) == 0 && flag->handles == 1 && !flag->deleted);
        REQUIRE(export_ksceKernelDeleteEventFlag(env, 0, "fixture", evf) == 0 && flag->deleted && kernel.eventflags.empty());
        kernel.process_sdk_version = 0;
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
        SceUID sema = semaphore_create(kernel, "fixture", "info sema", 0, 0x2080, 1, 3);
        REQUIRE(semaphore_signal(kernel, "fixture", 0, sema, 1) == 0);
        check_info_envelope<SceKernelSemaInfo>(env, info, sema, 0x7ffffff0, SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID, [&](SceUID id, Ptr<SceKernelSemaInfo> p, const SceSize *size) {
            return export__sceKernelGetSemaInfo(env, 0, "fixture", id, p, size);
        });
        auto *sema_info = Ptr<SceKernelSemaInfo>(info).get(env.mem);
        REQUIRE(sema_info->size == 0x3c && sema_info->semaId == sema && std::strcmp(sema_info->name, "info sema") == 0);
        REQUIRE(sema_info->attr == 0x2080 && sema_info->initCount == 1 && sema_info->currentCount == 2);
        REQUIRE(sema_info->maxCount == 3 && sema_info->numWaitThreads == 0);
        const SceUID opened_sema = semaphore_open(kernel, "fixture", "info sema");
        sema_info->size = sizeof(*sema_info);
        REQUIRE(export_sceKernelGetSemaInfo(env, 0, "fixture", opened_sema, Ptr<SceKernelSemaInfo>(info)) == 0);
        REQUIRE(sema_info->semaId == opened_sema && sema_info->attr == (0x2080 | 0x80000));
        // SceLibKernel passes 0 without a record.
        REQUIRE(export_sceKernelGetSemaInfo(env, 0, "fixture", sema, Ptr<SceKernelSemaInfo>()) == SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        REQUIRE(export_sceKernelCloseSema(env, 0, "fixture", opened_sema) == 0);
        REQUIRE(export_sceKernelDeleteSema(env, 0, "fixture", sema) == 0);

        auto owner = kernel.create_thread(env.mem, "info owner", Ptr<const void>(data), SCE_KERNEL_DEFAULT_PRIORITY_USER,
            SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        REQUIRE(owner);
        SceUID mutex = -1;
        REQUIRE(mutex_create(&mutex, kernel, env.mem, "fixture", "info mutex", owner->id, SCE_KERNEL_MUTEX_ATTR_RECURSIVE | SCE_KERNEL_ATTR_OPENABLE, 2, Ptr<SceKernelLwMutexWork>(), SyncWeight::Heavy) == 0);
        check_info_envelope<SceKernelMutexInfo>(env, info, mutex, 0x7ffffff0, SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID, [&](SceUID id, Ptr<SceKernelMutexInfo> p, const SceSize *size) {
            return export__sceKernelGetMutexInfo(env, 0, "fixture", id, p, size);
        });
        auto *mutex_info = Ptr<SceKernelMutexInfo>(info).get(env.mem);
        REQUIRE(mutex_info->size == 0x40 && mutex_info->mutexId == mutex && std::strcmp(mutex_info->name, "info mutex") == 0);
        REQUIRE(mutex_info->attr == (SCE_KERNEL_MUTEX_ATTR_RECURSIVE | SCE_KERNEL_ATTR_OPENABLE) && mutex_info->initCount == 2 && mutex_info->currentCount == 2);
        REQUIRE(mutex_info->currentOwnerId == owner->id && mutex_info->numWaitThreads == 0 && mutex_info->ceilingPriority == 0);
        // Only a recursive mutex starts locked more than once (0x8100e018).
        SceUID rejected = -1;
        REQUIRE(mutex_create(&rejected, kernel, env.mem, "fixture", "count 2", owner->id, 0, 2, Ptr<SceKernelLwMutexWork>(), SyncWeight::Heavy) == SCE_KERNEL_ERROR_ILLEGAL_COUNT);
        REQUIRE(mutex_create(&rejected, kernel, env.mem, "fixture", "count -1", owner->id, SCE_KERNEL_MUTEX_ATTR_RECURSIVE, -1, Ptr<SceKernelLwMutexWork>(), SyncWeight::Heavy) == SCE_KERNEL_ERROR_ILLEGAL_COUNT);
        REQUIRE(rejected == -1);
        const SceUID opened_mutex = mutex_open(kernel, "fixture", "info mutex");
        const SceSize mutex_size = sizeof(SceKernelMutexInfo);
        REQUIRE(export__sceKernelGetMutexInfo(env, 0, "fixture", opened_mutex, Ptr<SceKernelMutexInfo>(info), &mutex_size) == 0);
        REQUIRE(mutex_info->mutexId == opened_mutex && mutex_info->attr == (SCE_KERNEL_MUTEX_ATTR_RECURSIVE | SCE_KERNEL_ATTR_OPENABLE | 0x80000));

        SceUID cond = -1;
        REQUIRE(condvar_create(&cond, kernel, "fixture", "info cond", 0, 0x2080, mutex, Ptr<SceKernelLwCondWork>(), SyncWeight::Heavy) == 0);
        check_info_envelope<SceKernelCondInfo>(env, info, cond, 0x7ffffff0, SCE_KERNEL_ERROR_UNKNOWN_COND_ID, [&](SceUID id, Ptr<SceKernelCondInfo> p, const SceSize *size) {
            return export__sceKernelGetCondInfo(env, 0, "fixture", id, p, size);
        });
        auto *cond_info = Ptr<SceKernelCondInfo>(info).get(env.mem);
        REQUIRE(cond_info->size == 0x34 && cond_info->condId == cond && cond_info->attr == 0x2080);
        REQUIRE(cond_info->mutexId == mutex && cond_info->numWaitThreads == 0 && std::strcmp(cond_info->name, "info cond") == 0);
        const SceUID opened_cond = condvar_open(kernel, "fixture", "info cond");
        const SceSize cond_size = sizeof(SceKernelCondInfo);
        REQUIRE(export__sceKernelGetCondInfo(env, 0, "fixture", opened_cond, Ptr<SceKernelCondInfo>(info), &cond_size) == 0);
        REQUIRE(cond_info->condId == opened_cond && cond_info->attr == 0x2080);
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
        REQUIRE(mutex_close(kernel, "fixture", owner->id, lwmutex, SyncWeight::Light, HandleClose::Delete) == 0);

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
        kernel.process_sdk_version = 0x02100000;
        REQUIRE(create(caller->id, "new cond", 0x80, mutex, &small) == SCE_KERNEL_ERROR_ILLEGAL_ATTR);
        const SceUID plain = create(caller->id, "new cond", 0x2000, mutex, &small);
        REQUIRE(plain > 0 && kernel.condvars.at(plain)->attr == 0x2000);
        kernel.process_sdk_version = 0x02000000;
        const SceUID openable = create(caller->id, "openable cond", 0x80 | 0x2000, mutex, nullptr);
        REQUIRE(openable > 0 && kernel.condvars.at(openable)->attr == (0x80 | 0x2000));
        kernel.process_sdk_version = 0;
        REQUIRE(export_sceKernelDeleteCond(env, 0, "fixture", plain) == 0 && export_sceKernelDeleteCond(env, 0, "fixture", openable) == 0);
        REQUIRE(export_sceKernelDeleteMutex(env, 0, "fixture", mutex) == 0);
        caller->exit_delete(false);
        std::puts("CreateCond checks passed");
    }

    // Event bits (0x8101dcf8): OpenSimpleEvent and OpenTimer raise OPEN, a
    // Close CLOSE and a Delete DELETE on the object before its last handle
    // destroys it, with the calling thread's uid (TCB+0x98, the PUID stored
    // at 0x81006ce4) above the process's.
    {
        auto caller = kernel.create_thread(env.mem, "event caller", Ptr<const void>(data), SCE_KERNEL_DEFAULT_PRIORITY_USER,
            SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        const SceUInt64 by_caller = (SceUInt64(uint32_t(caller->id)) << 32) | uint32_t(KernelState::process_id);
        SceUInt32 result = 0;
        SceUInt64 user_data = 0;
        const auto poll = [&](SceUID id, SceUInt32 bits) {
            result = 0xcccccccc;
            user_data = 0xcccccccc;
            return simple_event_waitorpoll(kernel, "fixture", caller->id, id, bits, &result, &user_data, nullptr, false);
        };
        const SceUID event = simple_event_create(kernel, env.mem, "fixture", "bit event", caller->id, SCE_KERNEL_ATTR_OPENABLE, 0x1);
        REQUIRE(poll(event, SCE_KERNEL_EVENT_OPEN) == SCE_KERNEL_ERROR_EVENT_COND);
        const SceUID opened = export_sceKernelOpenSimpleEvent(env, caller->id, "fixture", "bit event");
        REQUIRE(poll(event, SCE_KERNEL_EVENT_OPEN) == 0 && result == (0x1 | SCE_KERNEL_EVENT_OPEN) && user_data == by_caller);
        REQUIRE(export_sceKernelCloseSimpleEvent(env, caller->id, "fixture", opened) == 0);
        REQUIRE(poll(event, SCE_KERNEL_EVENT_CLOSE) == 0 && result == (0x1 | SCE_KERNEL_EVENT_OPEN | SCE_KERNEL_EVENT_CLOSE));
        const SceUID reopened = export_sceKernelOpenSimpleEvent(env, caller->id, "fixture", "bit event");
        REQUIRE(export_sceKernelDeleteSimpleEvent(env, caller->id, "fixture", event) == 0);
        REQUIRE(poll(reopened, SCE_KERNEL_EVENT_DELETE) == 0 && (result & SCE_KERNEL_EVENT_DELETE) && user_data == by_caller);
        REQUIRE(export_sceKernelCloseSimpleEvent(env, caller->id, "fixture", reopened) == 0);
        // An auto-reset event clears the bits a poll took.
        const SceUID reset_event = simple_event_create(kernel, env.mem, "fixture", "reset event", caller->id,
            SCE_KERNEL_ATTR_OPENABLE | SCE_KERNEL_EVENT_ATTR_AUTO_RESET, 0);
        const SceUID reset_opened = export_sceKernelOpenSimpleEvent(env, caller->id, "fixture", "reset event");
        REQUIRE(poll(reset_event, SCE_KERNEL_EVENT_OPEN) == 0);
        REQUIRE(poll(reset_event, SCE_KERNEL_EVENT_OPEN) == SCE_KERNEL_ERROR_EVENT_COND);
        REQUIRE(export_sceKernelCloseSimpleEvent(env, caller->id, "fixture", reset_opened) == 0);
        REQUIRE(export_sceKernelDeleteSimpleEvent(env, caller->id, "fixture", reset_event) == 0);

        // Timers carry the same bits beside their own (SCE_KERNEL_EVENT_TIMER).
        const SceUID timer = timer_create(kernel, env.mem, "fixture", "bit timer", caller->id, SCE_KERNEL_ATTR_OPENABLE);
        const SceUID timer_opened = export_sceKernelOpenTimer(env, caller->id, "fixture", "bit timer");
        REQUIRE(poll(timer, SCE_KERNEL_EVENT_TIMER) == SCE_KERNEL_ERROR_EVENT_COND);
        REQUIRE(poll(timer, SCE_KERNEL_EVENT_OPEN) == 0 && result == SCE_KERNEL_EVENT_OPEN && user_data == by_caller);
        SceKernelSysClock interval = 1;
        REQUIRE(timer_set(kernel, "fixture", caller->id, timer, 0, &interval, 0) == 0);
        REQUIRE(export_sceKernelStartTimer(env, caller->id, "fixture", timer) == 0);
        const auto limit = vita3k::web::GuestThreadRuntime::now_us() + 1000000;
        while (poll(timer, SCE_KERNEL_EVENT_TIMER) != 0)
            REQUIRE(vita3k::web::GuestThreadRuntime::now_us() < limit);
        REQUIRE(result == (SCE_KERNEL_EVENT_OPEN | SCE_KERNEL_EVENT_TIMER) && user_data == 0);
        // ClearEvent keeps the bits of its pattern, the timer's own included.
        REQUIRE(simple_event_clear(kernel, "fixture", caller->id, timer, SCE_KERNEL_EVENT_TIMER) == 0);
        REQUIRE(poll(timer, SCE_KERNEL_EVENT_OPEN) == SCE_KERNEL_ERROR_EVENT_COND);
        REQUIRE(poll(timer, SCE_KERNEL_EVENT_TIMER) == 0 && result == SCE_KERNEL_EVENT_TIMER);
        REQUIRE(export_sceKernelStopTimer(env, caller->id, "fixture", timer) == 0);
        REQUIRE(simple_event_clear(kernel, "fixture", caller->id, timer, 0) == 0);
        REQUIRE(poll(timer, SCE_KERNEL_EVENT_TIMER) == SCE_KERNEL_ERROR_EVENT_COND);
        REQUIRE(export_sceKernelDeleteTimer(env, caller->id, "fixture", timer) == 0);
        REQUIRE(poll(timer_opened, SCE_KERNEL_EVENT_DELETE) == SCE_KERNEL_ERROR_UNKNOWN_EVENT_ID);
        const auto deleted = kernel.timers.at(timer_opened);
        REQUIRE((deleted->pattern & SCE_KERNEL_EVENT_DELETE) && deleted->last_user_data == by_caller);
        REQUIRE(export_sceKernelCloseTimer(env, caller->id, "fixture", timer_opened) == 0);
        caller->exit_delete(false);
        std::puts("Event bits of simple events and timers passed");
    }

    // The process's SDK version is the process parameter's fw_version, which
    // processmgr keeps whatever the parameter's version (modulemgr 0x81003948
    // copies it to process+0x148); SceLibKernel's own copy wants version 1.
    {
        const Address param = data + 0x800;
        auto *process = Ptr<SceProcessParam>(param).get(env.mem);
        std::memset(process, 0, sizeof(*process));
        process->magic = '2PSP';
        process->version = 0;
        process->fw_version = 0x01500000;
        kernel.load_process_param(env.mem, Ptr<uint32_t>(param));
        REQUIRE(!kernel.process_param && kernel.process_sdk_version == 0x01500000 && kernel.main_module_sdk_version(env.mem) == 0);
        process->version = 1;
        process->fw_version = 0x03600000;
        kernel.load_process_param(env.mem, Ptr<uint32_t>(param));
        REQUIRE(kernel.process_param && kernel.process_sdk_version == 0x03600000 && kernel.main_module_sdk_version(env.mem) == 0x03600000);
        process->magic = 0;
        kernel.load_process_param(env.mem, Ptr<uint32_t>(param));
        REQUIRE(kernel.process_sdk_version == 0);
        kernel.process_param = Ptr<SceProcessParam>();
        std::puts("Process SDK version passed");
    }

    // Kernel deletes (ksceKernelDeleteMutex 0x8100e440, DeleteSema 0x81012444,
    // DeleteCond 0x810203d4, DeleteMsgPipe 0x81016214) close any handle of
    // their class; another uid is DIFFERENT_UID_CLASS, UNKNOWN_MSG_PIPE_ID for
    // a message pipe. Conditions and message pipes need a calling thread.
    {
        auto caller = kernel.create_thread(env.mem, "kernel deleter", Ptr<const void>(data), SCE_KERNEL_DEFAULT_PRIORITY_USER,
            SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        kernel.process_sdk_version = 0x03600000;
        const SceUID sema = semaphore_create(kernel, "fixture", "kernel sema", 0, SCE_KERNEL_ATTR_OPENABLE, 0, 1);
        const SceUID sema_opened = semaphore_open(kernel, "fixture", "kernel sema");
        SceUID mutex = -1;
        REQUIRE(mutex_create(&mutex, kernel, env.mem, "fixture", "kernel mutex", 0, SCE_KERNEL_ATTR_OPENABLE, 0, Ptr<SceKernelLwMutexWork>(), SyncWeight::Heavy) == 0);
        const SceUID mutex_opened = mutex_open(kernel, "fixture", "kernel mutex");
        SceUID cond = -1;
        REQUIRE(condvar_create(&cond, kernel, "fixture", "kernel cond", 0, SCE_KERNEL_ATTR_OPENABLE, mutex, Ptr<SceKernelLwCondWork>(), SyncWeight::Heavy) == 0);
        const SceUID pipe = msgpipe_create(kernel, "fixture", "kernel pipe", 0, SCE_KERNEL_ATTR_OPENABLE, 0x100);
        const SceUID pipe_opened = msgpipe_open(kernel, "fixture", "kernel pipe");
        REQUIRE(export_ksceKernelDeleteSema(env, caller->id, "fixture", mutex) == SCE_KERNEL_ERROR_DIFFERENT_UID_CLASS);
        REQUIRE(export_ksceKernelDeleteMutex(env, caller->id, "fixture", sema) == SCE_KERNEL_ERROR_DIFFERENT_UID_CLASS);
        REQUIRE(export_ksceKernelDeleteMutex(env, caller->id, "fixture", 0x7ffffff0) == SCE_KERNEL_ERROR_DIFFERENT_UID_CLASS);
        REQUIRE(export_ksceKernelDeleteCond(env, 0, "fixture", cond) == SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
        REQUIRE(export_ksceKernelDeleteCond(env, caller->id, "fixture", sema) == SCE_KERNEL_ERROR_DIFFERENT_UID_CLASS);
        REQUIRE(export_ksceKernelDeleteMsgPipe(env, 0, "fixture", pipe) == SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
        REQUIRE(export_ksceKernelDeleteMsgPipe(env, caller->id, "fixture", sema) == SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID);
        // Opened handles close too, whatever the SDK.
        REQUIRE(export_ksceKernelDeleteSema(env, caller->id, "fixture", sema_opened) == 0 && !kernel.semaphores.contains(sema_opened) && kernel.semaphores.contains(sema));
        REQUIRE(export_ksceKernelDeleteMutex(env, caller->id, "fixture", mutex_opened) == 0 && !kernel.mutexes.contains(mutex_opened) && kernel.mutexes.contains(mutex));
        REQUIRE(export_ksceKernelDeleteMsgPipe(env, caller->id, "fixture", pipe_opened) == 0 && !kernel.msgpipes.contains(pipe_opened) && kernel.msgpipes.contains(pipe));
        REQUIRE(export_ksceKernelDeleteCond(env, caller->id, "fixture", cond) == 0 && !kernel.condvars.contains(cond));
        REQUIRE(export_ksceKernelDeleteSema(env, caller->id, "fixture", sema) == 0 && !kernel.semaphores.contains(sema));
        REQUIRE(export_ksceKernelDeleteMutex(env, caller->id, "fixture", mutex) == 0 && !kernel.mutexes.contains(mutex));
        REQUIRE(export_ksceKernelDeleteMsgPipe(env, caller->id, "fixture", pipe) == 0 && !kernel.msgpipes.contains(pipe));
        kernel.process_sdk_version = 0;
        caller->exit_delete(false);
        std::puts("Kernel deletes passed");
    }

    // Priority-ceiling mutexes (ksceKernelCreateMutex 0x8100e018; ownership
    // 0x8100d4a0 raises the owner to the ceiling). A ceiling needs options; 0
    // is the creator's priority, a default-relative one converts; it must be a
    // user priority and not below a creator that starts owning the mutex.
    {
        auto owner = kernel.create_thread(env.mem, "ceiling owner", Ptr<const void>(data), 0x80,
            SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        auto *opt = Ptr<SceKernelMutexOptParam>(data + 0x900).get(env.mem);
        const auto create = [&](SceUID tid, SceUInt32 attr, int count, SceKernelMutexOptParam *option) {
            return export__sceKernelCreateMutex(env, tid, "fixture", "ceiling", attr, count, option);
        };
        opt->size = 9;
        opt->ceilingPriority = 0x70;
        REQUIRE(create(0, SCE_KERNEL_MUTEX_ATTR_CEILING, 0, opt) == SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
        REQUIRE(create(owner->id, 0x8000 | SCE_KERNEL_MUTEX_ATTR_CEILING, 0, opt) == SCE_KERNEL_ERROR_ILLEGAL_ATTR);
        REQUIRE(create(owner->id, SCE_KERNEL_MUTEX_ATTR_CEILING, 0, opt) == SCE_KERNEL_ERROR_ILLEGAL_SIZE);
        REQUIRE(create(owner->id, SCE_KERNEL_MUTEX_ATTR_CEILING, 0, nullptr) == SCE_KERNEL_ERROR_INVALID_ARGUMENT);
        opt->size = 8;
        REQUIRE(create(owner->id, SCE_KERNEL_MUTEX_ATTR_CEILING, 2, opt) == SCE_KERNEL_ERROR_ILLEGAL_COUNT);
        opt->ceilingPriority = 0x20;
        REQUIRE(create(owner->id, SCE_KERNEL_MUTEX_ATTR_CEILING, 0, opt) == SCE_KERNEL_ERROR_ILLEGAL_PRIORITY);
        opt->ceilingPriority = 0x1000;
        REQUIRE(create(owner->id, SCE_KERNEL_MUTEX_ATTR_CEILING, 0, opt) == SCE_KERNEL_ERROR_ILLEGAL_PRIORITY);
        opt->ceilingPriority = 0x90;
        REQUIRE(create(owner->id, SCE_KERNEL_MUTEX_ATTR_CEILING, 1, opt) == SCE_KERNEL_ERROR_ILLEGAL_PRIORITY);
        // The kernel may use any ceiling up to 0xfe and attribute 0x8000.
        opt->ceilingPriority = 0xf0;
        const SceUID kernel_ceiling = export_ksceKernelCreateMutex(env, owner->id, "fixture", "kernel ceiling", 0x8000 | SCE_KERNEL_MUTEX_ATTR_CEILING, 0, opt);
        REQUIRE(kernel_ceiling > 0 && kernel.mutexes.at(kernel_ceiling)->ceiling_priority == 0xf0);
        REQUIRE(export_ksceKernelDeleteMutex(env, owner->id, "fixture", kernel_ceiling) == 0);
        // Relative to the default priority: 0x10000100 - 0x10 is 0xa0 - 0x10.
        opt->ceilingPriority = SCE_KERNEL_DEFAULT_PRIORITY - 0x10;
        const SceUID relative = create(owner->id, SCE_KERNEL_MUTEX_ATTR_CEILING, 0, opt);
        REQUIRE(relative > 0 && kernel.mutexes.at(relative)->ceiling_priority == 0x90);
        opt->ceilingPriority = 0;
        const SceUID own = create(owner->id, SCE_KERNEL_MUTEX_ATTR_CEILING, 0, opt);
        REQUIRE(own > 0 && kernel.mutexes.at(own)->ceiling_priority == 0x80);
        opt->ceilingPriority = 0x70;
        const SceUID ceiling = create(owner->id, SCE_KERNEL_MUTEX_ATTR_CEILING | SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 1, opt);
        REQUIRE(ceiling > 0 && owner->priority == 0x70);
        // Owning raises the priority, releasing restores it; the base priority
        // changes underneath.
        REQUIRE(mutex_lock(kernel, env.mem, "fixture", owner->id, relative, 1, nullptr, SyncWeight::Heavy) == 0 && owner->priority == 0x70);
        REQUIRE(export_sceKernelChangeThreadPriority2(env, owner->id, "fixture", 0, 0x60) == 0x70 && owner->priority == 0x60);
        REQUIRE(export_sceKernelChangeThreadPriority2(env, owner->id, "fixture", 0, 0xa0) == 0x60 && owner->priority == 0x70);
        REQUIRE(mutex_unlock(kernel, "fixture", owner->id, ceiling, 1, SyncWeight::Heavy) == 0 && owner->priority == 0x90);
        REQUIRE(mutex_unlock(kernel, "fixture", owner->id, relative, 1, SyncWeight::Heavy) == 0 && owner->priority == 0xa0);
        REQUIRE(owner->tls.get_ptr<int>().get(env.mem)[TLS_CURRENT_PRIORITY] == 0xa0);
        auto *info = Ptr<SceKernelMutexInfo>(data + 0xa00).get(env.mem);
        info->size = sizeof(*info);
        const SceSize info_size = sizeof(*info);
        REQUIRE(export__sceKernelGetMutexInfo(env, owner->id, "fixture", ceiling, Ptr<SceKernelMutexInfo>(data + 0xa00), &info_size) == 0);
        REQUIRE(info->ceilingPriority == 0x70 && info->attr == (SCE_KERNEL_MUTEX_ATTR_CEILING | SCE_KERNEL_MUTEX_ATTR_RECURSIVE));
        // Cancel hands the mutex to the caller; deletion releases it.
        SceUInt32 woken = 0;
        REQUIRE(export_sceKernelCancelMutex(env, owner->id, "fixture", ceiling, 1, &woken) == 0 && owner->priority == 0x70);
        REQUIRE(export_sceKernelCancelMutex(env, owner->id, "fixture", ceiling, 0, &woken) == 0 && owner->priority == 0xa0);
        REQUIRE(mutex_lock(kernel, env.mem, "fixture", owner->id, ceiling, 1, nullptr, SyncWeight::Heavy) == 0 && owner->priority == 0x70);
        REQUIRE(export_sceKernelDeleteMutex(env, owner->id, "fixture", ceiling) == 0 && owner->priority == 0xa0);
        REQUIRE(export_sceKernelDeleteMutex(env, owner->id, "fixture", relative) == 0);
        REQUIRE(export_sceKernelDeleteMutex(env, owner->id, "fixture", own) == 0);
        // A mutex handed to a waiter whose wait has not returned yet, then
        // deleted: the waiter loses the ceiling once, and its cancelled wait
        // (shutdown) does not release it again.
        opt->ceilingPriority = 0x70;
        const SceUID handed = create(owner->id, SCE_KERNEL_MUTEX_ATTR_CEILING, 1, opt);
        REQUIRE(handed > 0 && owner->priority == 0x70);
        const Address code = alloc(env.mem, 0x1000, "ceiling waiter code");
        guest_sync_delete::build_call(env.mem, code, guest_sync_delete::kLockMutex, { uint32_t(handed), 1, 0, 0, 0 }, data + 0xb00);
        auto waiter = kernel.create_thread(env.mem, "ceiling waiter", Ptr<const void>(code), SCE_KERNEL_DEFAULT_PRIORITY_USER,
            SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        const int waiter_priority = waiter->priority;
        REQUIRE(waiter_priority > 0x70 && waiter->start(0, Ptr<void>{}, false) == 0);
        const auto limit = vita3k::web::GuestThreadRuntime::now_us() + 2000000;
        while (waiter->status != ThreadStatus::wait) {
            REQUIRE(runtime.resume(64).failed == 0);
            REQUIRE(vita3k::web::GuestThreadRuntime::now_us() < limit);
        }
        REQUIRE(mutex_unlock(kernel, "fixture", owner->id, handed, 1, SyncWeight::Heavy) == 0);
        REQUIRE(kernel.mutexes.at(handed)->owner == waiter && waiter->priority == 0x70 && owner->priority == 0xa0);
        REQUIRE(export_sceKernelDeleteMutex(env, owner->id, "fixture", handed) == 0 && waiter->priority == waiter_priority);
        REQUIRE(runtime.shutdown());
        REQUIRE(waiter->priority == waiter_priority);
        REQUIRE(runtime.attach(env));
        free(env.mem, code);
        owner->exit_delete(false);
        std::puts("Priority-ceiling mutexes passed");
    }
    REQUIRE(runtime.shutdown());
    REQUIRE(kernel.threads.empty());
    env.kernel.call_import = {};
    free(env.mem, data);
}
