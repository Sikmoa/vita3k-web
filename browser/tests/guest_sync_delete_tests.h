// Firmware (SceKernelThreadMgr 3.74) deletion, condition and count semantics
// of the production sync objects under the fiber runtime, plus the desktop
// (no execution host) paths that complete without blocking.
#pragma once
#include "guest_thread_runtime.h"
#include "guest_thread_semaphore_fixture.h"
#include <kernel/state.h>
#include <kernel/sync_primitives.h>
#include <kernel/thread/thread_state.h>
#include <limits>
#include <module/module.h>

DECL_EXPORT(SceInt32, _sceKernelGetCondInfo, SceUID condId, Ptr<SceKernelCondInfo> pInfo);
DECL_EXPORT(int, _sceKernelWaitEventCB, SceUID event_id, SceUInt32 bit_pattern, SceUInt32 *result_pattern, SceUInt64 *user_data, SceUInt32 *timeout);

namespace guest_sync_delete {
// Calls `stub` with r0..r3 and a fifth stack argument, storing r0 at result.
inline void build_call(MemState &mem, Address code, uint32_t nid, const uint32_t (&args)[5], Address result) {
    const Address stub_address = code + 0x100;
    const uint32_t stub[] = {0xef000000, 0xe1a0f00e, nid};
    std::memcpy(Ptr<void>(stub_address).get(mem), stub, sizeof(stub));
    guest_thread_fixture::Arm p(code);
    p.emit(0xe92d4010); // push {r4,lr}
    p.emit(0xe24dd008); // sub sp, sp, #8
    p.constant(0, args[4]);
    p.emit(0xe58d0000); // str r0, [sp]
    p.constant(4, result);
    for (unsigned reg = 0; reg < 4; ++reg)
        p.constant(reg, args[reg]);
    p.call(stub_address);
    p.store(0, 0);
    p.emit(0xe28dd008); // add sp, sp, #8
    p.emit(0xe8bd8010); // pop {r4,pc}
    p.finish(mem);
}

// Test-only import: the fiber calls condvar_wait on a heavy condition.
constexpr uint32_t kHeavyCondWait = 0xc0de0001;
constexpr uint32_t kWaitEventFlag = 0x83c0e2af;
} // namespace guest_sync_delete

inline void test_guest_sync_deletion(EmuEnvState &env, vita3k::web::GuestThreadRuntime &runtime) {
    using namespace guest_sync_delete;
    const Address code = alloc(env.mem, 0x1000, "sync delete code");
    const Address data = alloc(env.mem, 0x1000, "sync delete data");
    REQUIRE(code && data);
    const auto word = [&](Address offset) -> uint32_t & { return *Ptr<uint32_t>(data + offset).get(env.mem); };
    env.kernel.call_import = [&](CPUState &cpu, uint32_t nid, SceUID tid) {
        if (nid == kHeavyCondWait) {
            auto *timeout = read_reg(cpu, 1) ? Ptr<SceUInt32>(read_reg(cpu, 1)).get(env.mem) : nullptr;
            write_reg(cpu, 0, condvar_wait(env.kernel, env.mem, "fixture", tid, read_reg(cpu, 0), timeout, SyncWeight::Heavy));
            return;
        }
        call_import(env, cpu, nid, tid);
        REQUIRE(env.missing_nids.empty());
    };
    const auto run_until = [&](auto done) {
        const auto limit = vita3k::web::GuestThreadRuntime::now_us() + 2000000;
        while (!done()) {
            REQUIRE(runtime.resume(64).failed == 0);
            REQUIRE(vita3k::web::GuestThreadRuntime::now_us() < limit);
        }
    };
    const auto spawn = [&](const char *name, unsigned slot, uint32_t nid, const uint32_t (&args)[5], bool start = true) {
        build_call(env.mem, code + 0x200 * slot, nid, args, data + 4 * slot);
        word(4 * slot) = 0xcccccccc;
        auto t = env.kernel.create_thread(env.mem, name, Ptr<const void>(code + 0x200 * slot), SCE_KERNEL_DEFAULT_PRIORITY_USER,
            SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        REQUIRE(t);
        if (start)
            REQUIRE(t->start(0, Ptr<void>{}, false) == 0);
        return t;
    };
    const auto finish = [&] {
        REQUIRE(runtime.shutdown());
        REQUIRE(env.kernel.threads.empty());
        REQUIRE(get_current_cpu_state() == nullptr);
    };

    // Event flags: deletion wakes with WAIT_DELETE; set and cancel ignore a
    // waiter that was itself deleted (it would otherwise consume the flags).
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        REQUIRE(runtime.attach(env));
        const SceUID evf = eventflag_create(env.kernel, "fixture", 0, "delete evf", 0x1000, 0);
        REQUIRE(evf >= 0);
        const auto flag = env.kernel.eventflags.at(evf);
        word(0x80) = word(0x84) = 0xcccccccc;
        auto first = spawn("evf waiter", 0, kWaitEventFlag, { uint32_t(evf), 1, SCE_EVENT_WAITAND | SCE_EVENT_WAITCLEAR, data + 0x80, 0 });
        run_until([&] { return first->status == ThreadStatus::wait; });
        if (scenario == 0) {
            REQUIRE(eventflag_delete(env.kernel, "fixture", 0, evf) == 0);
            REQUIRE(!env.kernel.eventflags.contains(evf) && flag->waiting_threads->empty());
            run_until([&] { return first->status == ThreadStatus::dormant; });
            REQUIRE(word(0) == uint32_t(SCE_KERNEL_ERROR_WAIT_DELETE));
        } else if (scenario == 1) {
            auto second = spawn("evf survivor", 1, kWaitEventFlag, { uint32_t(evf), 1, SCE_EVENT_WAITAND, data + 0x84, 0 });
            run_until([&] { return second->status == ThreadStatus::wait; });
            first->exit_delete(false);
            REQUIRE(eventflag_set(env.kernel, "fixture", 0, evf, 1) == 0);
            REQUIRE(flag->flags == 1 && flag->waiting_threads->empty());
            run_until([&] { return second->status == ThreadStatus::dormant && !env.kernel.threads.contains(first->id); });
            REQUIRE(word(4) == 0 && word(0x84) == 1 && word(0x80) == 0);
        } else {
            first->exit_delete(false);
            SceUInt32 count = 0xcccccccc;
            REQUIRE(eventflag_cancel(env.kernel, "fixture", 0, evf, 5, &count) == 0);
            REQUIRE(count == 0 && flag->flags == 5 && word(0x80) == 0);
            run_until([&] { return !env.kernel.threads.contains(first->id); });
        }
        finish();
        if (scenario != 0)
            REQUIRE(eventflag_delete(env.kernel, "fixture", 0, evf) == 0);
        std::printf("EventFlag deletion case %u passed\n", scenario);
    }

    // Heavy conditions: the same wait core with the Cond/Mutex error codes.
    // 0 cond deleted, 1 mutex deleted, 2 timeout re-acquires, 3/4 cond or
    // mutex deleted while the signalled waiter re-acquires the mutex.
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        REQUIRE(runtime.attach(env));
        SceUID mutex_id = -1, cond_id = -1;
        REQUIRE(mutex_create(&mutex_id, env.kernel, env.mem, "fixture", "heavy mutex", 0, 0, 0, Ptr<SceKernelLwMutexWork>{}, SyncWeight::Heavy) == 0);
        REQUIRE(condvar_create(&cond_id, env.kernel, "fixture", "heavy cond", 0, 0, mutex_id, SyncWeight::Heavy) == 0);
        const auto mutex = env.kernel.mutexes.at(mutex_id);
        const auto cond = env.kernel.condvars.at(cond_id);
        REQUIRE(cond->uid == cond_id);
        word(0x90) = scenario == 2 ? 0 : 0xcccccccc;
        auto waiter = spawn("cond waiter", 0, kHeavyCondWait, { uint32_t(cond_id), scenario == 2 ? data + 0x90 : 0, 0, 0, 0 }, false);
        REQUIRE(mutex_lock(env.kernel, env.mem, "fixture", waiter->id, mutex_id, 1, nullptr, SyncWeight::Heavy) == 0);
        REQUIRE(waiter->start(0, Ptr<void>{}, false) == 0);
        run_until([&] { return waiter->status != ThreadStatus::run; });
        SceInt32 expected = 0;
        if (scenario == 2) {
            run_until([&] { return waiter->status == ThreadStatus::dormant; });
            REQUIRE(word(0) == uint32_t(SCE_KERNEL_ERROR_WAIT_TIMEOUT) && word(0x90) == 0);
            REQUIRE(mutex->owner == waiter && mutex->lock_count == 1);
            REQUIRE(mutex_unlock(env.kernel, "fixture", waiter->id, mutex_id, 1, SyncWeight::Heavy) == 0);
        } else {
            REQUIRE(waiter->status == ThreadStatus::wait && cond->waiting_threads->size() == 1 && !mutex->owner);
            const bool relock = scenario >= 3;
            auto holder = env.kernel.create_thread(env.mem, "holder", Ptr<const void>(code), SCE_KERNEL_DEFAULT_PRIORITY_USER,
                SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
            REQUIRE(holder);
            if (relock) {
                REQUIRE(mutex_lock(env.kernel, env.mem, "fixture", holder->id, mutex_id, 1, nullptr, SyncWeight::Heavy) == 0);
                REQUIRE(condvar_signal(env.kernel, env.mem, "fixture", 0, cond_id, Condvar::SignalTarget(Condvar::SignalTarget::Type::Any), SyncWeight::Heavy) == 0);
                run_until([&] { return mutex->waiting_threads->size() == 1 && waiter->status == ThreadStatus::wait; });
            }
            if (scenario == 0 || scenario == 3) {
                REQUIRE(condvar_delete(env.kernel, "fixture", 0, cond_id, SyncWeight::Heavy) == 0);
                expected = SCE_KERNEL_ERROR_WAIT_DELETE_COND;
            } else {
                REQUIRE(mutex_delete(env.kernel, "fixture", 0, mutex_id, SyncWeight::Heavy) == 0);
                expected = SCE_KERNEL_ERROR_WAIT_DELETE_MUTEX;
                REQUIRE(!cond->associated_mutex);
            }
            REQUIRE(cond->waiting_threads->empty() && mutex->waiting_threads->empty());
            if (scenario == 1) {
                // The dissociated condition reports mutex -1 (firmware 0x81020908).
                auto *info = Ptr<SceKernelCondInfo>(data + 0xa0).get(env.mem);
                std::memset(info, 0xcc, sizeof(*info));
                info->size = sizeof(*info);
                REQUIRE(export__sceKernelGetCondInfo(env, 0, "fixture", cond_id, Ptr<SceKernelCondInfo>(data + 0xa0)) == 0);
                REQUIRE(info->condId == cond_id && info->mutexId == -1 && info->numWaitThreads == 0);
            }
            run_until([&] { return waiter->status == ThreadStatus::dormant; });
            REQUIRE(word(0) == uint32_t(expected));
            REQUIRE(mutex->owner == (relock ? holder : ThreadStatePtr{}));
            if (scenario == 3)
                REQUIRE(mutex_unlock(env.kernel, "fixture", holder->id, mutex_id, 1, SyncWeight::Heavy) == 0);
        }
        finish();
        REQUIRE(condvar_delete(env.kernel, "fixture", 0, cond_id, SyncWeight::Heavy)
            == (scenario == 0 || scenario == 3 ? SCE_KERNEL_ERROR_UNKNOWN_COND_ID : 0));
        REQUIRE(export__sceKernelGetCondInfo(env, 0, "fixture", cond_id, Ptr<SceKernelCondInfo>(data + 0xa0)) == SCE_KERNEL_ERROR_UNKNOWN_COND_ID);
        REQUIRE(mutex_delete(env.kernel, "fixture", 0, mutex_id, SyncWeight::Heavy)
            == (scenario == 1 || scenario == 4 ? SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID : 0));
        std::printf("Cond deletion case %u passed\n", scenario);
    }

    // Lock/unlock counts (SceKernelThreadMgr 0x810232c8/0x8100ec00 and
    // SceLibKernel sceKernelTryLockLwMutex 0x810003c0), uncontended only.
    REQUIRE(runtime.attach(env));
    {
        const auto make_thread = [&](const char *name) {
            auto t = env.kernel.create_thread(env.mem, name, Ptr<const void>(code), SCE_KERNEL_DEFAULT_PRIORITY_USER,
                SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
            REQUIRE(t);
            return t;
        };
        auto a = make_thread("count a"), b = make_thread("count b");
        for (const auto weight : {SyncWeight::Heavy, SyncWeight::Light}) {
            const bool light = weight == SyncWeight::Light;
            const auto work = [&](Address offset) { return light ? Ptr<SceKernelLwMutexWork>(data + offset) : Ptr<SceKernelLwMutexWork>{}; };
            SceUID plain = -1, recursive = -1;
            REQUIRE(mutex_create(&plain, env.kernel, env.mem, "fixture", "plain", 0, 0, 0, work(0x100), weight) == 0);
            REQUIRE(mutex_create(&recursive, env.kernel, env.mem, "fixture", "recursive", 0, SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, work(0x140), weight) == 0);
            if (light) {
                Ptr<SceKernelLwMutexWork>(data + 0x100).get(env.mem)->uid = plain;
                Ptr<SceKernelLwMutexWork>(data + 0x140).get(env.mem)->uid = recursive;
            }
            REQUIRE(mutex_lock(env.kernel, env.mem, "fixture", a->id, plain, 0, nullptr, weight) == SCE_KERNEL_ERROR_ILLEGAL_COUNT);
            REQUIRE(mutex_lock(env.kernel, env.mem, "fixture", a->id, plain, 2, nullptr, weight) == SCE_KERNEL_ERROR_ILLEGAL_COUNT);
            REQUIRE(mutex_try_lock(env.kernel, env.mem, "fixture", a->id, plain, 2, weight) == SCE_KERNEL_ERROR_ILLEGAL_COUNT);
            REQUIRE(mutex_try_lock(env.kernel, env.mem, "fixture", a->id, plain, 1, weight) == 0);
            // Owned: the user-side LwMutex try-lock reports ownership first.
            REQUIRE(mutex_try_lock(env.kernel, env.mem, "fixture", b->id, plain, 2, weight)
                == (light ? SCE_KERNEL_ERROR_LW_MUTEX_FAILED_TO_OWN : SCE_KERNEL_ERROR_ILLEGAL_COUNT));
            REQUIRE(mutex_try_lock(env.kernel, env.mem, "fixture", a->id, plain, 2, weight)
                == (light ? SCE_KERNEL_ERROR_LW_MUTEX_RECURSIVE : SCE_KERNEL_ERROR_ILLEGAL_COUNT));
            REQUIRE(mutex_try_lock(env.kernel, env.mem, "fixture", b->id, plain, 0, weight) == SCE_KERNEL_ERROR_ILLEGAL_COUNT);
            REQUIRE(mutex_unlock(env.kernel, "fixture", b->id, plain, 1, weight)
                == (light ? SCE_KERNEL_ERROR_LW_MUTEX_NOT_OWNED : SCE_KERNEL_ERROR_MUTEX_NOT_OWNED));
            REQUIRE(mutex_unlock(env.kernel, "fixture", a->id, plain, 1, weight) == 0);
            REQUIRE(mutex_lock(env.kernel, env.mem, "fixture", a->id, recursive, std::numeric_limits<int>::max(), nullptr, weight) == 0);
            REQUIRE(mutex_lock(env.kernel, env.mem, "fixture", a->id, recursive, 1, nullptr, weight)
                == (light ? SCE_KERNEL_ERROR_LW_MUTEX_LOCK_OVF : SCE_KERNEL_ERROR_MUTEX_LOCK_OVF));
            REQUIRE(env.kernel.mutex.try_lock()); env.kernel.mutex.unlock();
            REQUIRE(mutex_unlock(env.kernel, "fixture", a->id, recursive, std::numeric_limits<int>::max(), weight) == 0);
            REQUIRE(mutex_delete(env.kernel, "fixture", 0, plain, weight) == 0);
            REQUIRE(mutex_delete(env.kernel, "fixture", 0, recursive, weight) == 0);
        }

        // Desktop paths (no execution host) that complete without blocking,
        // run on a thread object standing in for the calling host thread.
        auto *const host = env.kernel.execution_host;
        env.kernel.execution_host = nullptr;
        a->status = ThreadStatus::run;
        for (const auto weight : {SyncWeight::Heavy, SyncWeight::Light}) {
            const bool light = weight == SyncWeight::Light;
            SceUID mutex_id = -1, cond_id = -1;
            REQUIRE(mutex_create(&mutex_id, env.kernel, env.mem, "fixture", "desktop mutex", 0, 0, 0,
                light ? Ptr<SceKernelLwMutexWork>(data + 0x100) : Ptr<SceKernelLwMutexWork>{}, weight) == 0);
            REQUIRE(condvar_create(&cond_id, env.kernel, "fixture", "desktop cond", 0, 0, mutex_id, weight) == 0);
            REQUIRE(mutex_lock(env.kernel, env.mem, "fixture", a->id, mutex_id, 1, nullptr, weight) == 0);
            // An expired timeout re-acquires the mutex before WAIT_TIMEOUT.
            SceUInt32 zero = 0;
            REQUIRE(condvar_wait(env.kernel, env.mem, "fixture", a->id, cond_id, &zero, weight) == SCE_KERNEL_ERROR_WAIT_TIMEOUT);
            const auto mutex = (light ? env.kernel.lwmutexes : env.kernel.mutexes).at(mutex_id);
            REQUIRE(mutex->owner == a && mutex->lock_count == 1 && a->status == ThreadStatus::run);
            // Waiting requires owning the mutex.
            REQUIRE(condvar_wait(env.kernel, env.mem, "fixture", b->id, cond_id, &zero, weight)
                == (light ? SCE_KERNEL_ERROR_LW_MUTEX_NOT_OWNED : SCE_KERNEL_ERROR_MUTEX_NOT_OWNED));
            REQUIRE(mutex_unlock(env.kernel, "fixture", a->id, mutex_id, 1, weight) == 0);
            REQUIRE(condvar_delete(env.kernel, "fixture", 0, cond_id, weight) == 0);
            REQUIRE(mutex_delete(env.kernel, "fixture", 0, mutex_id, weight) == 0);
        }
        // _sceKernelWaitEventCB waits (it used to poll), and deletion removes
        // the simple event itself.
        const SceUID event = simple_event_create(env.kernel, env.mem, "fixture", "desktop event", 0, 0, 0);
        REQUIRE(event >= 0);
        SceUInt32 zero = 0;
        REQUIRE(export__sceKernelWaitEventCB(env, a->id, "fixture", event, 1, nullptr, nullptr, &zero) == SCE_KERNEL_ERROR_WAIT_TIMEOUT);
        REQUIRE(simple_event_delete(env.kernel, "fixture", 0, event) == 0);
        REQUIRE(!env.kernel.simple_events.contains(event));
        REQUIRE(simple_event_delete(env.kernel, "fixture", 0, event) == SCE_KERNEL_ERROR_UNKNOWN_EVENT_ID);
        // A waiting desktop thread that is deleted is only set running; its
        // wait must unlink itself instead of reporting an acquired semaphore.
        const SceUID sema = semaphore_create(env.kernel, "fixture", "desktop sema", 0, 0, 0, 1);
        REQUIRE(sema >= 0);
        a->status = ThreadStatus::wait;
        a->exit_delete(false);
        REQUIRE(a->status == ThreadStatus::run);
        REQUIRE(semaphore_wait(env.kernel, "fixture", a->id, sema, 1, nullptr) == SCE_KERNEL_ERROR_WAIT_CANCEL);
        REQUIRE(env.kernel.semaphores.at(sema)->waiting_threads->empty());
        REQUIRE(semaphore_delete(env.kernel, "fixture", 0, sema) == 0);
        a->status = ThreadStatus::dormant;
        env.kernel.execution_host = host;
    }
    finish();
    env.kernel.call_import = {};
    std::puts("Sync counts, desktop condition timeout and simple event deletion passed");
}
