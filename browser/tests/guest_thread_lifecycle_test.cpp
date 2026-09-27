// Asset-free ARM guest lifecycle and production semaphore/mutex integration.
#include "guest_thread_runtime.h"
#include <cpu/functions.h>
#include <emuenv/state.h>
#include <kernel/state.h>
#include <kernel/sync_primitives.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <modules/module_parent.h>
#include <module/module.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include "guest_thread_semaphore_fixture.h"

#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
#include "guest_mspace_tests.h"
#include "guest_msg_dialog_tests.h"
#include "guest_fios_overlay_tests.h"
#include "guest_clib_tests.h"
#include "guest_regmgr_tests.h"
#include "guest_np_offline_tests.h"
#include "guest_near_tests.h"
#include "guest_apputil_tests.h"
#include "guest_appmgr_rtc_tests.h"
#include "inline_mutex_fixture.h"
#include "guest_sync_delete_tests.h"
#include "guest_kernel_info_tests.h"

DECL_EXPORT(int, sceKernelDeleteLwCond, Ptr<SceKernelLwCondWork> workarea);
DECL_EXPORT(int, sceKernelSignalLwCondTo, Ptr<SceKernelLwCondWork> workarea, SceUID thread_target);
DECL_EXPORT(int, sceKernelSignalLwCond, Ptr<SceKernelLwCondWork> workarea);
DECL_EXPORT(int, sceKernelWaitLwCond, Ptr<SceKernelLwCondWork> workarea, SceUInt32 *timeout);

int main() {
    test_inline_mutex_runtime();
    auto env = std::make_unique<EmuEnvState>();
    REQUIRE(init(env->mem, true));
    const Address code = alloc(env->mem, 4096, "thread fixture code");
    const Address data = alloc(env->mem, 4096, "thread fixture data");
    REQUIRE(code && data);
    const auto program = guest_thread_fixture::build(env->mem, code, data);
    auto *shared = Ptr<guest_thread_fixture::Shared>(data).get(env->mem);
    unsigned waits = 0, signals = 0;
    REQUIRE(env->kernel.init(env->mem, [&](CPUState &cpu, uint32_t nid, SceUID tid) {
        const auto sp = read_sp(cpu);
        if (nid == 0x0c7b834b) {
            ++waits;
            if (waits == 2) shared->allow_signal = 1;
        }
        if (nid == 0xe6b761d1) {
            ++signals;
            const auto sema = env->kernel.semaphores.at(shared->semaphore);
            if (signals == 1) {
                REQUIRE(sema->val == 0);
                REQUIRE(sema->waiting_threads->size() == 1);
                const auto waiter = (*sema->waiting_threads->begin()).thread;
                REQUIRE(waiter->status == ThreadStatus::wait);
                REQUIRE(shared->parent_done == 0);
            }
        }
        call_import(*env, cpu, nid, tid);
        REQUIRE(env->missing_nids.empty());
        REQUIRE(read_sp(cpu) == sp);
        if (nid == 0xc5c11ee7) {
            const auto child = env->kernel.get_thread(static_cast<SceUID>(read_reg(cpu, 0)));
            REQUIRE(child && child->status == ThreadStatus::dormant);
            REQUIRE(shared->child_done == 0);
        }
    }, false));
    vita3k::web::GuestThreadRuntime runtime(128);
    REQUIRE(runtime.attach(*env));
    auto parent = env->kernel.create_thread(env->mem, "parent fixture", Ptr<const void>(program.parent),
        SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
        SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
    REQUIRE(parent && parent->status == ThreadStatus::dormant);
    test_guest_mspace(*env, *parent);
    test_guest_msg_dialog(*env, *parent);
    test_guest_fios_overlay(*env, *parent);
    test_guest_clib(*env, *parent);
    test_guest_regmgr(*env, *parent);
    test_guest_np_offline(*env, *parent);
    test_guest_near(*env, *parent);
    test_guest_apputil(*env, *parent);
    test_guest_appmgr_rtc(*env, *parent);
    REQUIRE(parent->start(0, Ptr<void>{}, false) == 0);
    const auto progress = runtime.resume(256);
    REQUIRE(progress.failed == 0);
    REQUIRE(progress.idle);
    REQUIRE(progress.dormant == 2);
    REQUIRE(waits == 2 && signals == 2);
    REQUIRE(shared->first_wait == 0 && shared->second_wait == 0);
    REQUIRE(shared->start_result == 0);
    REQUIRE(shared->signal_result == 0 && shared->second_signal_result == 0);
    REQUIRE(shared->parent_done == 1 && shared->child_done == 1);
    auto child = env->kernel.get_thread(shared->child);
    REQUIRE(child && child->status == ThreadStatus::dormant);
    REQUIRE(parent->returned_value == 42 && child->returned_value == 43);
    const auto sema = env->kernel.semaphores.at(shared->semaphore);
    REQUIRE(sema->val == 1 && sema->waiting_threads->empty());
    REQUIRE(get_current_cpu_state() == nullptr);
    // Delete via production bridge, then drain before releasing memory.
    write_reg(*parent->cpu, 0, child->id);
    call_import(*env, *parent->cpu, 0x1bbde3d9, parent->id);
    REQUIRE(read_reg(*parent->cpu, 0) == 0);
    runtime.resume(32);
    REQUIRE(!env->kernel.threads.contains(child->id));
    REQUIRE(runtime.shutdown());
    REQUIRE(env->kernel.threads.empty());
    REQUIRE(!env->kernel.execution_host);
    child.reset(); parent.reset();
    std::puts("Guest thread lifecycle: creation, start, polling, semaphore wait/signal, return and deletion passed");

    // Each case uses a fresh runtime attachment and the same production queues.
    // No synthetic scheduler wakeup substitutes for a semaphore operation.
    // Scenario 4 deletes the semaphore under its parked waiter; scenario 5
    // cancels after the waiter itself was deleted.
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        env->kernel.call_import = [&](CPUState &cpu, uint32_t nid, SceUID tid) {
            call_import(*env, cpu, nid, tid);
            REQUIRE(env->missing_nids.empty());
        };
        REQUIRE(runtime.attach(*env));
        const auto id = semaphore_create(env->kernel, "fixture", "edge semaphore", 0, 0, 0, 1);
        REQUIRE(id >= 0);
        const Address timeout = data + 0x200, result = data + 0x204;
        *Ptr<uint32_t>(timeout).get(env->mem) = 0;
        *Ptr<uint32_t>(result).get(env->mem) = 0xcccccccc;
        guest_thread_fixture::build_waiter(env->mem, code, id, scenario == 0 ? timeout : 0, result);
        auto waiter = env->kernel.create_thread(env->mem, "edge waiter", Ptr<const void>(code),
            SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
            SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        REQUIRE(waiter && waiter->start(0, Ptr<void>{}, false) == 0);
        auto progress = runtime.resume(64);
        REQUIRE(progress.failed == 0 && progress.idle);
        const auto queue = env->kernel.semaphores.at(id);
        if (scenario == 0) {
            REQUIRE(waiter->status == ThreadStatus::dormant);
            REQUIRE(*Ptr<uint32_t>(result).get(env->mem) == uint32_t(SCE_KERNEL_ERROR_WAIT_TIMEOUT));
            REQUIRE(*Ptr<uint32_t>(timeout).get(env->mem) == 0);
        } else {
            REQUIRE(waiter->status == ThreadStatus::wait);
            REQUIRE(queue->waiting_threads->size() == 1);
            REQUIRE(*Ptr<uint32_t>(result).get(env->mem) == 0xcccccccc);
            if (scenario == 4) {
                // Firmware deletion succeeds and wakes the waiter with WAIT_DELETE.
                REQUIRE(semaphore_delete(env->kernel, "fixture", 0, id) == 0);
                REQUIRE(!env->kernel.semaphores.contains(id) && queue->waiting_threads->empty());
                REQUIRE(waiter->status == ThreadStatus::run);
                runtime.resume(64);
                REQUIRE(waiter->status == ThreadStatus::dormant);
                REQUIRE(*Ptr<uint32_t>(result).get(env->mem) == uint32_t(SCE_KERNEL_ERROR_WAIT_DELETE));
            } else if (scenario == 5) {
                // A deleted waiter is no longer waiting: cancel neither counts
                // nor wakes it.
                waiter->exit_delete(false);
                SceUInt32 count = 0xcccccccc;
                REQUIRE(semaphore_cancel(env->kernel, "fixture", 0, id, 0, &count) == 0);
                REQUIRE(count == 0 && queue->waiting_threads->empty());
                runtime.resume(64);
                REQUIRE(!env->kernel.threads.contains(waiter->id));
            } else if (scenario == 1) {
                SceUInt32 count = 0;
                REQUIRE(semaphore_cancel(env->kernel, "fixture", 0, id, 0, &count) == 0);
                REQUIRE(count == 1);
                runtime.resume(64);
                REQUIRE(waiter->status == ThreadStatus::dormant);
                REQUIRE(*Ptr<uint32_t>(result).get(env->mem) == uint32_t(SCE_KERNEL_ERROR_WAIT_CANCEL));
            } else if (scenario == 2) {
                waiter->exit_delete(false);
                runtime.resume(64);
                REQUIRE(!env->kernel.threads.contains(waiter->id));
            }
        }
        // Scenario 3 deliberately shuts down with a live parked HLE frame.
        REQUIRE(runtime.shutdown());
        REQUIRE(env->kernel.threads.empty());
        REQUIRE(queue->waiting_threads->empty());
        REQUIRE(queue->val == 0);
        REQUIRE(get_current_cpu_state() == nullptr);
        REQUIRE(semaphore_delete(env->kernel, "fixture", 0, id) == (scenario == 4 ? SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID : 0));
        std::printf("Semaphore edge case %u passed\n", scenario);
    }

    // Contended lightweight mutex: the child's lock must park on the runtime
    // until the parent's unlock transfers ownership. Guest code uses the
    // production lock/unlock exports; the host creates the object directly.
    env->kernel.call_import = [&](CPUState &cpu, uint32_t nid, SceUID tid) {
        call_import(*env, cpu, nid, tid);
        REQUIRE(env->missing_nids.empty());
    };
    REQUIRE(runtime.attach(*env));
    const Address work = data + 0x300;
    SceUID lwmutex = -1;
    REQUIRE(mutex_create(&lwmutex, env->kernel, env->mem, "fixture", "edge lwmutex",
        0, 0, 0, Ptr<SceKernelLwMutexWork>(work), SyncWeight::Light) == 0);
    // The production CreateLwMutex wrapper passes this field as uid_out.
    Ptr<SceKernelLwMutexWork>(work).get(env->mem)->uid = lwmutex;
    REQUIRE(env->kernel.lwmutexes.contains(lwmutex));
    *Ptr<uint32_t>(data + 0x40).get(env->mem) = 0xcccccccc;
    *Ptr<uint32_t>(data + 0x44).get(env->mem) = 0;
    *Ptr<uint32_t>(data + 0x48).get(env->mem) = 0;
    *Ptr<uint32_t>(data + 0x4c).get(env->mem) = 0xcccccccc;
    *Ptr<uint32_t>(data + 0x50).get(env->mem) = 0xcccccccc;
    *Ptr<uint32_t>(data + 0x54).get(env->mem) = 0;
    guest_thread_fixture::build_lwmutex_pair(env->mem, code, data);
    auto lock_parent = env->kernel.create_thread(env->mem, "lock parent", Ptr<const void>(code),
        SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
        SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
    auto lock_child = env->kernel.create_thread(env->mem, "lock child", Ptr<const void>(code + 0x400),
        SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
        SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
    REQUIRE(lock_parent && lock_child);
    // Host-side uncontended round-trip first: proves create/lock/unlock are
    // linked and correct before any guest branch/park logic runs.
    REQUIRE(mutex_lock(env->kernel, env->mem, "fixture", lock_parent->id,
        lwmutex, 1, nullptr, SyncWeight::Light) == 0);
    REQUIRE(mutex_unlock(env->kernel, "fixture", lock_parent->id,
        lwmutex, 1, SyncWeight::Light) == 0);
    REQUIRE(lock_parent->start(0, Ptr<void>{}, false) == 0);
    REQUIRE(lock_child->start(0, Ptr<void>{}, false) == 0);
    const auto parked = runtime.resume(64);
    REQUIRE(parked.failed == 0 && parked.waiting == 1 && parked.runnable == 1);
    const auto lock = env->kernel.lwmutexes.at(lwmutex);
    const auto *lock_work = Ptr<SceKernelLwMutexWork>(work).get(env->mem);
    REQUIRE(lock_child->status == ThreadStatus::wait);
    REQUIRE(lock_parent->status == ThreadStatus::run);
    REQUIRE(lock->waiting_threads->size() == 1);
    REQUIRE((*lock->waiting_threads->begin()).thread == lock_child);
    REQUIRE((*lock->waiting_threads->begin()).lock_count == 1);
    REQUIRE(lock->owner == lock_parent && lock->lock_count == 1);
    REQUIRE(lock_work->owner == uint32_t(lock_parent->id) && lock_work->lockCount == 1);
    REQUIRE(*Ptr<uint32_t>(data + 0x40).get(env->mem) == 0u);
    REQUIRE(*Ptr<uint32_t>(data + 0x4c).get(env->mem) == 0xccccccccu);
    REQUIRE(*Ptr<uint32_t>(data + 0x50).get(env->mem) == 0xccccccccu);
    // Firmware unlock errors (SceLibKernel sceKernelUnlockLwMutex): a thread
    // that does not own it, and a count above one on a non-recursive mutex,
    // fail without touching the owner or the queue.
    REQUIRE(mutex_unlock(env->kernel, "fixture", lock_child->id, lwmutex, 1, SyncWeight::Light)
        == SCE_KERNEL_ERROR_LW_MUTEX_NOT_OWNED);
    REQUIRE(mutex_unlock(env->kernel, "fixture", 0, lwmutex, 1, SyncWeight::Light)
        == SCE_KERNEL_ERROR_LW_MUTEX_NOT_OWNED);
    REQUIRE(mutex_unlock(env->kernel, "fixture", lock_parent->id, lwmutex, 2, SyncWeight::Light)
        == SCE_KERNEL_ERROR_ILLEGAL_COUNT);
    REQUIRE(mutex_unlock(env->kernel, "fixture", lock_parent->id, lwmutex, 0, SyncWeight::Light)
        == SCE_KERNEL_ERROR_ILLEGAL_COUNT);
    REQUIRE(env->kernel.lwmutexes.contains(lwmutex));
    REQUIRE(lock->waiting_threads->size() == 1 && lock->owner == lock_parent);
    REQUIRE(lock_work->owner == uint32_t(lock_parent->id) && lock_work->lockCount == 1);
    *Ptr<uint32_t>(data + 0x54).get(env->mem) = 1; // open the host gate
    const auto drained = runtime.resume(256);
    REQUIRE(drained.failed == 0 && drained.idle && drained.dormant == 2);
    REQUIRE(lock->waiting_threads->empty());
    REQUIRE(lock->owner == lock_child && lock->lock_count == 1);
    REQUIRE(lock_work->owner == uint32_t(lock_child->id) && lock_work->lockCount == 1);
    REQUIRE(*Ptr<uint32_t>(data + 0x40).get(env->mem) == 0u); // parent lock
    REQUIRE(*Ptr<uint32_t>(data + 0x4c).get(env->mem) == 0u); // parent unlock
    REQUIRE(*Ptr<uint32_t>(data + 0x50).get(env->mem) == 0u); // child lock
    REQUIRE(*Ptr<uint32_t>(data + 0x48).get(env->mem) == 2u); // child marker
    REQUIRE(lock_parent->returned_value == 42);
    REQUIRE(lock_child->returned_value == 43);
    REQUIRE(mutex_unlock(env->kernel, "fixture", lock_child->id, lwmutex, 1, SyncWeight::Light) == 0);
    REQUIRE(!lock->owner && lock->lock_count == 0);
    REQUIRE(lock_work->owner == uint32_t(-1) && lock_work->lockCount == 0);
    REQUIRE(mutex_delete(env->kernel, "fixture", lock_child->id, lwmutex, SyncWeight::Light) == 0);
    REQUIRE(runtime.shutdown());
    REQUIRE(env->kernel.threads.empty());
    REQUIRE(get_current_cpu_state() == nullptr);
    std::puts("LwMutex contention: parent lock, child parked, unlock wake, both passed");

    // Run both queue families through zero/parked timeout, deletion before
    // cleanup, and unlock BEFORE cleanup with/without a surviving waiter.
    for (const auto weight : {SyncWeight::Heavy, SyncWeight::Light}) {
        const bool light = weight == SyncWeight::Light;
        // Scenario 5 deletes the mutex under its parked waiter.
        for (unsigned scenario = 0; scenario < 6; ++scenario) {
            SceUID observed_waiter = -1;
            uint32_t wait_return = 0xcccccccc;
            env->kernel.call_import = [&](CPUState &cpu, uint32_t nid, SceUID tid) {
                call_import(*env, cpu, nid, tid);
                REQUIRE(env->missing_nids.empty());
                if (tid == observed_waiter && (nid == 0x46e7be7b || nid == 0x1d8d7945))
                    wait_return = read_reg(cpu, 0);
            };
            REQUIRE(runtime.attach(*env));
            auto owner = env->kernel.create_thread(env->mem, "mutex owner", Ptr<const void>(code),
                SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
                SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
            REQUIRE(owner);
            SceUID id = -1;
            REQUIRE(mutex_create(&id, env->kernel, env->mem, "fixture", "edge mutex", owner->id,
                SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, light ? Ptr<SceKernelLwMutexWork>(work) : Ptr<SceKernelLwMutexWork>{}, weight) == 0);
            auto *wa = Ptr<SceKernelLwMutexWork>(work).get(env->mem);
            if (light) {
                wa->uid = id;
                REQUIRE(wa->owner == uint32_t(-1) && wa->lockCount == 0);
            }
            const auto mutex = (light ? env->kernel.lwmutexes : env->kernel.mutexes).at(id);
            const auto check_owner = [&](const ThreadStatePtr &expected, int count) {
                REQUIRE(mutex->owner == expected && mutex->lock_count == count);
                if (light) {
                    REQUIRE(wa->owner == (expected ? uint32_t(expected->id) : uint32_t(-1)));
                    REQUIRE(wa->lockCount == uint32_t(count));
                    REQUIRE(wa->uid == id);
                }
            };
            REQUIRE(mutex_lock(env->kernel, env->mem, "fixture", owner->id, id, 1, nullptr, weight) == 0);
            check_owner(owner, 1);
            const Address timeout = data + 0x200, result = data + 0x204, survivor_result = data + 0x208;
            *Ptr<uint32_t>(timeout).get(env->mem) = scenario == 1 ? 50000 : 0;
            *Ptr<uint32_t>(result).get(env->mem) = 0xcccccccc;
            *Ptr<uint32_t>(survivor_result).get(env->mem) = 0xcccccccc;
            const uint32_t argument = light ? work : uint32_t(id);
            guest_thread_fixture::build_mutex_waiter(env->mem, code, argument, light, 2,
                scenario <= 1 ? timeout : 0, result);
            auto waiter = env->kernel.create_thread(env->mem, "mutex waiter", Ptr<const void>(code),
                SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
                SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
            REQUIRE(waiter);
            observed_waiter = waiter->id;
            REQUIRE(waiter->start(0, Ptr<void>{}, false) == 0);
            // Stop as soon as the actual HLE frame parks (or zero timeout returns).
            auto edge = runtime.resume(1);
            for (unsigned i = 0; i < 64 && waiter->status == ThreadStatus::run; ++i)
                edge = runtime.resume(1);
            REQUIRE(edge.failed == 0);
            if (scenario != 0) {
                REQUIRE(waiter->status == ThreadStatus::wait);
                REQUIRE(edge.waiting == 1);
                // The parked HLE frame must not retain any production lock.
                REQUIRE(mutex->mutex.try_lock()); mutex->mutex.unlock();
                REQUIRE(waiter->mutex.try_lock()); waiter->mutex.unlock();
                REQUIRE(env->kernel.mutex.try_lock()); env->kernel.mutex.unlock();
                REQUIRE(mutex->waiting_threads->size() == 1);
                REQUIRE((*mutex->waiting_threads->begin()).thread == waiter);
                REQUIRE((*mutex->waiting_threads->begin()).lock_count == 2);
                REQUIRE(*Ptr<uint32_t>(result).get(env->mem) == 0xcccccccc);
                check_owner(owner, 1);
                // Only the owner may unlock; the count cannot underflow.
                REQUIRE(mutex_unlock(env->kernel, "fixture", waiter->id, id, 1, weight)
                    == (light ? SCE_KERNEL_ERROR_LW_MUTEX_NOT_OWNED : SCE_KERNEL_ERROR_MUTEX_NOT_OWNED));
                REQUIRE(mutex_unlock(env->kernel, "fixture", owner->id, id, 2, weight)
                    == (light ? SCE_KERNEL_ERROR_LW_MUTEX_UNLOCK_UDF : SCE_KERNEL_ERROR_MUTEX_UNLOCK_UDF));
                check_owner(owner, 1);
                REQUIRE(mutex->waiting_threads->size() == 1);
            }
            if (scenario <= 1) {
                if (scenario == 1) {
                    REQUIRE(edge.next_deadline_us);
                    // Root-side passage of time only; no host wait inside HLE.
                    while (vita3k::web::GuestThreadRuntime::now_us() < *edge.next_deadline_us) {}
                }
                REQUIRE(runtime.resume(64).failed == 0);
                REQUIRE(waiter->status == ThreadStatus::dormant);
                REQUIRE(*Ptr<uint32_t>(result).get(env->mem) == uint32_t(SCE_KERNEL_ERROR_WAIT_TIMEOUT));
                REQUIRE(*Ptr<uint32_t>(timeout).get(env->mem) == 0);
                REQUIRE(mutex->waiting_threads->empty());
                check_owner(owner, 1);
            } else if (scenario == 2) {
                waiter->exit_delete(false);
                REQUIRE(runtime.resume(64).failed == 0);
                REQUIRE(!env->kernel.threads.contains(waiter->id));
                REQUIRE(mutex->waiting_threads->empty());
                check_owner(owner, 1);
            } else if (scenario == 5) {
                // Firmware deletion succeeds and wakes the waiter with
                // WAIT_DELETE; the woken waiter leaves the dead workarea alone.
                if (light)
                    wa->lockCount = 7; // sentinel: no write-back after deletion
                REQUIRE(mutex_delete(env->kernel, "fixture", owner->id, id, weight) == 0);
                REQUIRE(!(light ? env->kernel.lwmutexes : env->kernel.mutexes).contains(id));
                REQUIRE(mutex->waiting_threads->empty() && mutex->deleted);
                REQUIRE(runtime.resume(64).failed == 0);
                REQUIRE(waiter->status == ThreadStatus::dormant);
                REQUIRE(*Ptr<uint32_t>(result).get(env->mem) == uint32_t(SCE_KERNEL_ERROR_WAIT_DELETE));
                if (light)
                    REQUIRE(wa->lockCount == 7);
                // Its id no longer resolves.
                REQUIRE(mutex_unlock(env->kernel, "fixture", owner->id, id, 1, weight)
                    == (light ? SCE_KERNEL_ERROR_UNKNOWN_LW_MUTEX_ID : SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID));
            } else if (scenario == 3) {
                // A second live waiter proves unlock skips the deleted front
                // entry and hands off the requested recursive count exactly once.
                guest_thread_fixture::build_mutex_waiter(env->mem, code + 0x400, argument, light, 2, 0, survivor_result);
                auto survivor = env->kernel.create_thread(env->mem, "mutex survivor", Ptr<const void>(code + 0x400),
                    SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
                    SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
                REQUIRE(survivor && survivor->start(0, Ptr<void>{}, false) == 0);
                REQUIRE(runtime.resume(64).failed == 0);
                REQUIRE(survivor->status == ThreadStatus::wait && mutex->waiting_threads->size() == 2);
                waiter->exit_delete(false);
                REQUIRE(waiter->status == ThreadStatus::run);
                REQUIRE(mutex->waiting_threads->size() == 2); // no cleanup dispatch yet
                REQUIRE(mutex_unlock(env->kernel, "fixture", owner->id, id, 1, weight) == 0);
                REQUIRE(mutex->waiting_threads->empty());
                check_owner(survivor, 2);
                REQUIRE(*Ptr<uint32_t>(survivor_result).get(env->mem) == 0xcccccccc);
                REQUIRE(runtime.resume(64).failed == 0);
                REQUIRE(!env->kernel.threads.contains(waiter->id));
                REQUIRE(survivor->status == ThreadStatus::dormant);
                REQUIRE(*Ptr<uint32_t>(survivor_result).get(env->mem) == 0);
                check_owner(survivor, 2);
                REQUIRE(mutex_unlock(env->kernel, "fixture", survivor->id, id, 1, weight) == 0);
                check_owner(survivor, 1);
                REQUIRE(mutex_unlock(env->kernel, "fixture", survivor->id, id, 1, weight) == 0);
                check_owner({}, 0);
            }
            if (scenario != 3 && scenario != 5) {
                if (scenario == 4) {
                    // Delete the only waiter, then unlock before its cleanup.
                    waiter->exit_delete(false);
                    REQUIRE(mutex->waiting_threads->size() == 1);
                }
                REQUIRE(mutex_unlock(env->kernel, "fixture", owner->id, id, 1, weight) == 0);
                REQUIRE(mutex->waiting_threads->empty());
                check_owner({}, 0);
            }
            REQUIRE(runtime.shutdown());
            REQUIRE(env->kernel.threads.empty() && mutex->waiting_threads->empty());
            REQUIRE(wait_return == uint32_t(scenario <= 1 ? SCE_KERNEL_ERROR_WAIT_TIMEOUT
                    : scenario == 5 ? SCE_KERNEL_ERROR_WAIT_DELETE : SCE_KERNEL_ERROR_WAIT_CANCEL));
            REQUIRE(get_current_cpu_state() == nullptr);
            if (scenario != 5)
                REQUIRE(mutex_delete(env->kernel, "fixture", 0, id, weight) == 0);
            std::printf("%s edge case %u passed\n", light ? "LwMutex" : "Mutex", scenario);
        }
    }
    // sceKernelWaitThreadEnd through the production import and the runtime's
    // import filter: join on return, timeout, an already-dormant target, a
    // waiter deleted while parked, a target ending in ExitDeleteThread, and a
    // deleted waiter whose target goes dormant before the waiter is reaped.
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        env->kernel.call_import = [&](CPUState &cpu, uint32_t nid, SceUID tid) {
            call_import(*env, cpu, nid, tid);
            REQUIRE(env->missing_nids.empty());
        };
        REQUIRE(runtime.attach(*env));
        guest_thread_fixture::build_thread_end_pair(env->mem, code, data, scenario == 4);
        const auto word = [&](unsigned offset) -> uint32_t & { return *Ptr<uint32_t>(data + offset).get(env->mem); };
        word(0x60) = scenario == 2; // gate
        word(0x64) = 0xcccccccc; // stat
        word(0x68) = 0xcccccccc; // result
        word(0x70) = scenario == 1 ? data + 0x74 : 0; // timeout pointer
        word(0x74) = 50000;
        // Scenario 5 needs the target dispatched first once both are woken.
        auto target = env->kernel.create_thread(env->mem, "join target", Ptr<const void>(code + 0x400),
            SCE_KERNEL_DEFAULT_PRIORITY_USER - (scenario == 5 ? 8 : 0), SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
            SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        auto waiter = env->kernel.create_thread(env->mem, "join waiter", Ptr<const void>(code),
            SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
            SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        REQUIRE(target && waiter);
        word(0x6c) = static_cast<uint32_t>(target->id);
        REQUIRE(target->start(0, Ptr<void>{}, false) == 0);
        if (scenario == 2) {
            REQUIRE(runtime.resume(64).failed == 0);
            REQUIRE(target->status == ThreadStatus::dormant);
        }
        // The target parks in DelayThread between gate polls, so drive the
        // runtime (deadlines included) until the expected state is reached.
        const auto run_until = [&](auto done) {
            const auto limit = vita3k::web::GuestThreadRuntime::now_us() + 2000000;
            while (!done()) {
                REQUIRE(runtime.resume(64).failed == 0);
                REQUIRE(vita3k::web::GuestThreadRuntime::now_us() < limit);
            }
        };
        REQUIRE(waiter->start(0, Ptr<void>{}, false) == 0);
        if (scenario == 2) {
            run_until([&] { return waiter->status == ThreadStatus::dormant; });
            REQUIRE(word(0x68) == 0 && word(0x64) == 43);
        } else {
            run_until([&] { return !target->waiting_threads.empty(); });
            REQUIRE(waiter->status == ThreadStatus::wait);
            REQUIRE(target->waiting_threads.size() == 1 && target->waiting_threads.front() == waiter);
            // The parked HLE frame must not retain either thread's lock.
            REQUIRE(target->mutex.try_lock()); target->mutex.unlock();
            REQUIRE(waiter->mutex.try_lock()); waiter->mutex.unlock();
            REQUIRE(word(0x68) == 0xcccccccc);
        }
        if (scenario == 0 || scenario == 4) {
            word(0x60) = 1;
            run_until([&] { return waiter->status == ThreadStatus::dormant; });
            REQUIRE(word(0x68) == 0 && word(0x64) == 43);
            if (scenario == 4)
                REQUIRE(!env->kernel.threads.contains(target->id));
            else
                REQUIRE(target->status == ThreadStatus::dormant);
        } else if (scenario == 5) {
            // Shutdown-style deletion of both while the target sleeps: the
            // waiter is woken while still linked, and the more urgent target
            // reaches its dormant transition first.
            target->exit_delete(false);
            waiter->exit_delete(false);
            run_until([&] { return env->kernel.threads.empty(); });
            REQUIRE(target->waiting_threads.empty());
        } else if (scenario == 1) {
            run_until([&] { return waiter->status == ThreadStatus::dormant; });
            REQUIRE(word(0x68) == uint32_t(SCE_KERNEL_ERROR_WAIT_TIMEOUT));
            REQUIRE(word(0x74) == 0 && word(0x64) == 0xcccccccc);
            REQUIRE(target->waiting_threads.empty() && target->status != ThreadStatus::dormant);
        } else if (scenario == 3) {
            waiter->exit_delete(false);
            run_until([&] { return !env->kernel.threads.contains(waiter->id); });
            REQUIRE(target->waiting_threads.empty() && target->status != ThreadStatus::dormant);
        }
        if (scenario == 1 || scenario == 3) {
            word(0x60) = 1;
            run_until([&] { return target->status == ThreadStatus::dormant; });
            REQUIRE(target->waiting_threads.empty());
        }
        if (scenario != 5)
            REQUIRE(target->returned_value == 43);
        REQUIRE(runtime.shutdown());
        REQUIRE(env->kernel.threads.empty());
        REQUIRE(get_current_cpu_state() == nullptr);
        std::printf("WaitThreadEnd case %u passed\n", scenario);
    }

    // sceKernelWaitLwCond through the production imports and the runtime's
    // import filter. Guest waiters lock the LwMutex, wait, record the
    // workarea owner and unlock; a guest signaler holds the mutex while it
    // signals, so woken waiters must park again on the mutex re-acquire.
    {
        namespace lw = guest_thread_fixture::lwcond;
        const Address lw_code = alloc(env->mem, 4096, "lwcond fixture code");
        const Address lw_data = alloc(env->mem, 4096, "lwcond fixture data");
        REQUIRE(lw_code && lw_data);
        lw::build(env->mem, lw_code, lw_data);
        const auto word = [&](Address offset) -> uint32_t & { return *Ptr<uint32_t>(lw_data + offset).get(env->mem); };
        const auto waiter_word = [&](unsigned slot, Address offset) -> uint32_t & { return word(lw::waiter_area(slot) + offset); };
        const auto lw_export = [&](auto fn, auto... args) {
            return fn(*env, 0, "fixture", Ptr<SceKernelLwCondWork>(lw_data + lw::kCond), args...);
        };
        for (unsigned scenario = 0; scenario < 14; ++scenario) {
            env->kernel.call_import = [&](CPUState &cpu, uint32_t nid, SceUID tid) {
                call_import(*env, cpu, nid, tid);
                REQUIRE(env->missing_nids.empty());
            };
            REQUIRE(runtime.attach(*env));
            std::memset(Ptr<void>(lw_data).get(env->mem), 0xcc, 0x200);
            for (unsigned slot = 0; slot < 4; ++slot)
                word(lw::kTimeout + 4 * slot) = 0;
            word(lw::kTimeout) = scenario == 2 || scenario == 12 ? lw_data + 0x70 : 0; // waiter 0 timeout pointer
            word(0x70) = 50000;
            word(lw::kSignaler) = 0; word(lw::kSignaler + 0x10) = 0;
            std::memset(Ptr<void>(lw_data + lw::kMutex).get(env->mem), 0, 0x60);
            SceUID mutex_id = -1, cond_id = -1;
            REQUIRE(mutex_create(&mutex_id, env->kernel, env->mem, "fixture", "lwcond mutex", 0, 0, 0,
                Ptr<SceKernelLwMutexWork>(lw_data + lw::kMutex), SyncWeight::Light) == 0);
            Ptr<SceKernelLwMutexWork>(lw_data + lw::kMutex).get(env->mem)->uid = mutex_id;
            REQUIRE(condvar_create(&cond_id, env->kernel, "fixture", "lwcond", 0, 0, mutex_id,
                Ptr<SceKernelLwCondWork>(lw_data + lw::kCond), SyncWeight::Light) == 0);
            Ptr<SceKernelLwCondWork>(lw_data + lw::kCond).get(env->mem)->uid = cond_id;
            const auto mutex = env->kernel.lwmutexes.at(mutex_id);
            const auto cond = env->kernel.lwcondvars.at(cond_id);
            const auto *work = Ptr<SceKernelLwMutexWork>(lw_data + lw::kMutex).get(env->mem);
            const auto thread = [&](const char *name, Address entry, int priority = SCE_KERNEL_DEFAULT_PRIORITY_USER) {
                auto t = env->kernel.create_thread(env->mem, name, Ptr<const void>(entry), priority,
                    SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
                REQUIRE(t && t->start(0, Ptr<void>{}, false) == 0);
                return t;
            };
            const auto run_until = [&](auto done) {
                const auto limit = vita3k::web::GuestThreadRuntime::now_us() + 2000000;
                while (!done()) {
                    REQUIRE(runtime.resume(64).failed == 0);
                    REQUIRE(vita3k::web::GuestThreadRuntime::now_us() < limit);
                }
            };
            const auto mutex_free = [&] {
                return !mutex->owner && mutex->lock_count == 0 && mutex->waiting_threads->empty()
                    && work->owner == uint32_t(-1) && work->lockCount == 0;
            };
            const auto completed = [&](unsigned slot, const ThreadStatePtr &t) {
                REQUIRE(t->status == ThreadStatus::dormant);
                REQUIRE(waiter_word(slot, 0) == 0 && waiter_word(slot, 4) == 0);
                REQUIRE(waiter_word(slot, 8) == uint32_t(t->id)); // re-acquired on return
                REQUIRE(waiter_word(slot, 0xc) == 0 && waiter_word(slot, 0x10) == 1);
            };
            const unsigned waiters = scenario == 1 ? 3 : (scenario == 0 || scenario == 5 || scenario == 9) ? 2 : 1;
            std::vector<ThreadStatePtr> w;
            for (unsigned slot = 0; slot < waiters; ++slot) {
                w.push_back(thread("lwcond waiter", lw_code + 0x100 * slot));
                run_until([&] { return cond->waiting_threads->size() == slot + 1 || waiter_word(slot, 4) != 0xcccccccc; });
            }
            if (scenario != 2) {
                // Each waiter released the mutex (kernel object and guest
                // workarea) and parked in FIFO order on the condition.
                REQUIRE(cond->waiting_threads->size() == waiters);
                REQUIRE((*cond->waiting_threads->begin()).thread == w[0]);
                for (unsigned slot = 0; slot < waiters; ++slot) {
                    REQUIRE(w[slot]->status == ThreadStatus::wait);
                    REQUIRE(waiter_word(slot, 0) == 0 && waiter_word(slot, 4) == 0xcccccccc);
                }
                REQUIRE(mutex_free());
                // The parked HLE frame must not retain either production lock.
                REQUIRE(cond->mutex.try_lock()); cond->mutex.unlock();
                REQUIRE(mutex->mutex.try_lock()); mutex->mutex.unlock();
                // GetLwCondInfo counts the parked waiters.
                auto *info = Ptr<SceKernelLwCondInfo>(lw_data + 0x900).get(env->mem);
                info->size = sizeof(*info);
                REQUIRE(lw_export(export__sceKernelGetLwCondInfo, Ptr<SceKernelLwCondInfo>(lw_data + 0x900)) == 0);
                REQUIRE(info->uid == cond_id && info->numWaitThreads == waiters);
                REQUIRE(info->pWork.address() == lw_data + lw::kCond && info->pLwMutex.address() == lw_data + lw::kMutex);
            }
            if (scenario == 0 || scenario == 1) {
                auto signaler = thread("lwcond signaler", lw_code + (scenario == 1 ? 0x600 : 0x400));
                word(lw::kSignaler) = 1;
                run_until([&] { return word(lw::kSignaler + 0xc) == 1; });
                REQUIRE(word(lw::kSignaler + 4) == 0 && word(lw::kSignaler + 8) == 0);
                // Woken waiters left the condition and now wait for the mutex
                // the signaler still holds.
                const unsigned woken = scenario == 1 ? 3 : 1;
                REQUIRE(cond->waiting_threads->size() == waiters - woken);
                REQUIRE(mutex->owner == signaler && mutex->waiting_threads->size() == woken);
                REQUIRE((*mutex->waiting_threads->begin()).thread == w[0]);
                for (unsigned slot = 0; slot < waiters; ++slot)
                    REQUIRE(waiter_word(slot, 4) == 0xcccccccc);
                word(lw::kSignaler + 0x10) = 1;
                run_until([&] {
                    for (unsigned slot = 0; slot < woken; ++slot)
                        if (w[slot]->status != ThreadStatus::dormant) return false;
                    return signaler->status == ThreadStatus::dormant;
                });
                REQUIRE(word(lw::kSignaler + 0x14) == 0);
                for (unsigned slot = 0; slot < woken; ++slot)
                    completed(slot, w[slot]);
                if (scenario == 0) {
                    // Signal-one woke exactly the first waiter; wake the second.
                    REQUIRE(w[1]->status == ThreadStatus::wait && cond->waiting_threads->size() == 1);
                    word(lw::kSignaler + 0xc) = 0;
                    REQUIRE(signaler->start(0, Ptr<void>{}, false) == 0);
                    run_until([&] { return w[1]->status == ThreadStatus::dormant && signaler->status == ThreadStatus::dormant; });
                    completed(1, w[1]);
                }
            } else if (scenario == 2) {
                // Timeout: firmware re-acquires the mutex before returning
                // WAIT_TIMEOUT, and the remaining time is zero.
                run_until([&] { return w[0]->status == ThreadStatus::dormant; });
                REQUIRE(waiter_word(0, 0) == 0);
                REQUIRE(waiter_word(0, 4) == uint32_t(SCE_KERNEL_ERROR_WAIT_TIMEOUT));
                REQUIRE(waiter_word(0, 8) == uint32_t(w[0]->id));
                REQUIRE(waiter_word(0, 0xc) == 0 && waiter_word(0, 0x10) == 1);
                REQUIRE(word(0x70) == 0);
                REQUIRE(cond->waiting_threads->empty());
            } else if (scenario == 3) {
                // Deleting a condition with a parked waiter succeeds; the waiter
                // gets WAIT_DELETE_LW_COND without the mutex, so its unlock fails.
                REQUIRE(lw_export(export_sceKernelDeleteLwCond) == 0);
                REQUIRE(!env->kernel.lwcondvars.contains(cond_id) && cond->waiting_threads->empty());
                REQUIRE(word(lw::kCond) == uint32_t(-1) && word(lw::kCond + 4) == uint32_t(-1));
                run_until([&] { return w[0]->status == ThreadStatus::dormant; });
                REQUIRE(waiter_word(0, 4) == uint32_t(SCE_KERNEL_ERROR_WAIT_DELETE_LW_COND));
                REQUIRE(waiter_word(0, 8) == uint32_t(-1));
                REQUIRE(waiter_word(0, 0xc) == uint32_t(SCE_KERNEL_ERROR_LW_MUTEX_NOT_OWNED));
            } else if (scenario == 8) {
                // Deleted while the signalled waiter re-acquires the mutex the
                // signaler holds: it wakes with WAIT_DELETE_LW_COND, unowned.
                auto signaler = thread("lwcond signaler", lw_code + 0x400);
                word(lw::kSignaler) = 1;
                run_until([&] { return word(lw::kSignaler + 0xc) == 1; });
                REQUIRE(mutex->owner == signaler && mutex->waiting_threads->size() == 1);
                REQUIRE(lw_export(export_sceKernelDeleteLwCond) == 0);
                REQUIRE(mutex->waiting_threads->empty() && mutex->owner == signaler);
                run_until([&] { return w[0]->status == ThreadStatus::dormant; });
                REQUIRE(waiter_word(0, 4) == uint32_t(SCE_KERNEL_ERROR_WAIT_DELETE_LW_COND));
                REQUIRE(waiter_word(0, 8) == uint32_t(signaler->id));
                REQUIRE(waiter_word(0, 0xc) == uint32_t(SCE_KERNEL_ERROR_LW_MUTEX_NOT_OWNED));
                word(lw::kSignaler + 0x10) = 1;
                run_until([&] { return signaler->status == ThreadStatus::dormant; });
                REQUIRE(word(lw::kSignaler + 0x14) == 0);
            } else if (scenario == 9) {
                // SignalLwCondTo (SceKernelThreadMgr 0x8102dee4/0x810244b8).
                REQUIRE(lw_export(export_sceKernelSignalLwCondTo, 0) == SCE_KERNEL_ERROR_ILLEGAL_THREAD_ID);
                REQUIRE(lw_export(export_sceKernelSignalLwCondTo, 0x7ffffff0) == SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID);
                REQUIRE(export_sceKernelSignalLwCondTo(*env, w[0]->id, "fixture", Ptr<SceKernelLwCondWork>(lw_data + lw::kCond), w[0]->id)
                    == SCE_KERNEL_ERROR_ILLEGAL_THREAD_ID);
                REQUIRE(cond->waiting_threads->size() == 2);
                // Wakes exactly the target, not the queue head.
                REQUIRE(lw_export(export_sceKernelSignalLwCondTo, w[1]->id) == 0);
                REQUIRE(cond->waiting_threads->size() == 1 && (*cond->waiting_threads->begin()).thread == w[0]);
                run_until([&] { return w[1]->status == ThreadStatus::dormant; });
                completed(1, w[1]);
                REQUIRE(w[0]->status == ThreadStatus::wait);
                // A thread that is not waiting on the condition.
                REQUIRE(lw_export(export_sceKernelSignalLwCondTo, w[1]->id) == SCE_KERNEL_ERROR_ILLEGAL_THREAD_ID);
                // Processes built with an SDK before 2.00 get -1 instead.
                const Address param = lw_data + 0x800;
                auto *process = Ptr<SceProcessParam>(param).get(env->mem);
                std::memset(process, 0, sizeof(*process));
                process->magic = '2PSP'; process->version = 1; process->fw_version = 0x01500000;
                env->kernel.process_param = Ptr<SceProcessParam>(param);
                REQUIRE(lw_export(export_sceKernelSignalLwCondTo, w[1]->id) == -1);
                process->fw_version = 0x03600000;
                REQUIRE(lw_export(export_sceKernelSignalLwCondTo, w[1]->id) == SCE_KERNEL_ERROR_ILLEGAL_THREAD_ID);
                env->kernel.process_param = Ptr<SceProcessParam>();
                REQUIRE(w[0]->status == ThreadStatus::wait && cond->waiting_threads->size() == 1);
                REQUIRE(lw_export(export_sceKernelSignalLwCond) == 0);
                run_until([&] { return w[0]->status == ThreadStatus::dormant; });
                completed(0, w[0]);
                // Signalling an empty condition succeeds.
                REQUIRE(lw_export(export_sceKernelSignalLwCond) == 0);
            } else if (scenario == 10) {
                // Deleting the mutex wakes the condition's waiters with
                // WAIT_DELETE_LW_MUTEX and dissociates the condition.
                REQUIRE(mutex_delete(env->kernel, "fixture", 0, mutex_id, SyncWeight::Light) == 0);
                REQUIRE(cond->waiting_threads->empty() && !cond->associated_mutex);
                run_until([&] { return w[0]->status == ThreadStatus::dormant; });
                REQUIRE(waiter_word(0, 4) == uint32_t(SCE_KERNEL_ERROR_WAIT_DELETE_LW_MUTEX));
                REQUIRE(waiter_word(0, 0xc) == uint32_t(SCE_KERNEL_ERROR_UNKNOWN_LW_MUTEX_ID));
            } else if (scenario == 11) {
                // Signalled, then the mutex is deleted before the waiter runs:
                // its re-acquire finds no mutex.
                REQUIRE(lw_export(export_sceKernelSignalLwCond) == 0);
                REQUIRE(mutex_delete(env->kernel, "fixture", 0, mutex_id, SyncWeight::Light) == 0);
                run_until([&] { return w[0]->status == ThreadStatus::dormant; });
                REQUIRE(waiter_word(0, 4) == uint32_t(SCE_KERNEL_ERROR_UNKNOWN_LW_MUTEX_ID));
                REQUIRE(waiter_word(0, 0xc) == uint32_t(SCE_KERNEL_ERROR_UNKNOWN_LW_MUTEX_ID));
                REQUIRE(!mutex->owner && mutex->waiting_threads->empty());
            } else if (scenario == 12) {
                // The re-acquire after a signal has no timeout, even when the
                // wait's own deadline passes while the signaler holds the mutex.
                auto signaler = thread("lwcond signaler", lw_code + 0x400);
                word(lw::kSignaler) = 1;
                run_until([&] { return word(lw::kSignaler + 0xc) == 1; });
                REQUIRE(mutex->owner == signaler && mutex->waiting_threads->size() == 1);
                const auto deadline = vita3k::web::GuestThreadRuntime::now_us() + 60000;
                while (vita3k::web::GuestThreadRuntime::now_us() < deadline) {}
                REQUIRE(runtime.resume(64).failed == 0);
                REQUIRE(w[0]->status == ThreadStatus::wait && mutex->waiting_threads->size() == 1);
                word(lw::kSignaler + 0x10) = 1;
                run_until([&] { return w[0]->status == ThreadStatus::dormant && signaler->status == ThreadStatus::dormant; });
                completed(0, w[0]);
            } else if (scenario == 13) {
                // Signalled, then the condition is deleted before the waiter
                // runs: firmware returns the signal's success without
                // re-acquiring (the woken wait no longer finds the object).
                REQUIRE(lw_export(export_sceKernelSignalLwCond) == 0);
                REQUIRE(lw_export(export_sceKernelDeleteLwCond) == 0);
                run_until([&] { return w[0]->status == ThreadStatus::dormant; });
                REQUIRE(waiter_word(0, 4) == 0);
                REQUIRE(waiter_word(0, 8) == uint32_t(-1));
                REQUIRE(waiter_word(0, 0xc) == uint32_t(SCE_KERNEL_ERROR_LW_MUTEX_NOT_OWNED));
            } else if (scenario == 4) {
                // A waiter deleted while parked leaves the queue and never
                // re-acquires the mutex.
                w[0]->exit_delete(false);
                run_until([&] { return !env->kernel.threads.contains(w[0]->id); });
                REQUIRE(cond->waiting_threads->empty());
                REQUIRE(waiter_word(0, 4) == 0xcccccccc);
            } else if (scenario == 5) {
                // A deleted waiter still queued ahead must not consume a
                // signal-one: the more urgent signaler runs before it unlinks.
                w[0]->exit_delete(false);
                auto signaler = thread("lwcond signaler", lw_code + 0x400, SCE_KERNEL_DEFAULT_PRIORITY_USER - 8);
                word(lw::kSignaler) = 1; word(lw::kSignaler + 0x10) = 1;
                run_until([&] { return w[1]->status == ThreadStatus::dormant && !env->kernel.threads.contains(w[0]->id); });
                REQUIRE(word(lw::kSignaler + 8) == 0);
                completed(1, w[1]);
                REQUIRE(cond->waiting_threads->empty());
            } else if (scenario == 6) {
                // Signalled, then deleted before it resumes: the dying waiter
                // must not re-acquire (and leak) the mutex.
                REQUIRE(condvar_signal(env->kernel, env->mem, "fixture", 0, cond_id,
                    Condvar::SignalTarget(Condvar::SignalTarget::Type::Any), SyncWeight::Light) == 0);
                REQUIRE(cond->waiting_threads->empty() && w[0]->status == ThreadStatus::run);
                w[0]->exit_delete(false);
                run_until([&] { return !env->kernel.threads.contains(w[0]->id); });
                REQUIRE(waiter_word(0, 4) == 0xcccccccc);
            } else if (scenario == 7) {
                // Ownership handed to a waiter parked on the re-acquire, which
                // is then deleted before it resumes: the mutex must not stay
                // owned by the dead thread.
                auto signaler = thread("lwcond signaler", lw_code + 0x400);
                word(lw::kSignaler) = 1;
                run_until([&] { return word(lw::kSignaler + 0xc) == 1; });
                REQUIRE(mutex->owner == signaler && mutex->waiting_threads->size() == 1);
                REQUIRE(mutex_unlock(env->kernel, "fixture", signaler->id, mutex_id, 1, SyncWeight::Light) == 0);
                REQUIRE(mutex->owner == w[0] && w[0]->status == ThreadStatus::run);
                w[0]->exit_delete(false);
                word(lw::kSignaler + 0x10) = 1;
                run_until([&] { return !env->kernel.threads.contains(w[0]->id) && signaler->status == ThreadStatus::dormant; });
                REQUIRE(waiter_word(0, 4) == 0xcccccccc);
            }
            REQUIRE(mutex_free());
            REQUIRE(runtime.shutdown());
            REQUIRE(env->kernel.threads.empty());
            REQUIRE(get_current_cpu_state() == nullptr);
            const bool cond_deleted = scenario == 3 || scenario == 8 || scenario == 13;
            const bool mutex_deleted = scenario == 10 || scenario == 11;
            REQUIRE(condvar_delete(env->kernel, "fixture", 0, cond_id, SyncWeight::Light)
                == (cond_deleted ? SCE_KERNEL_ERROR_UNKNOWN_LW_COND_ID : 0));
            REQUIRE(mutex_delete(env->kernel, "fixture", 0, mutex_id, SyncWeight::Light)
                == (mutex_deleted ? SCE_KERNEL_ERROR_UNKNOWN_LW_MUTEX_ID : 0));
            std::printf("LwCond case %u passed\n", scenario);
        }
        // A null workarea is ILLEGAL_ADDR (SceKernelThreadMgr syscall entries).
        const Ptr<SceKernelLwCondWork> null_work;
        REQUIRE(export_sceKernelDeleteLwCond(*env, 0, "fixture", null_work) == SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        REQUIRE(export_sceKernelSignalLwCond(*env, 0, "fixture", null_work) == SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        REQUIRE(export_sceKernelSignalLwCondTo(*env, 0, "fixture", null_work, 1) == SCE_KERNEL_ERROR_ILLEGAL_ADDR);
        REQUIRE(export_sceKernelWaitLwCond(*env, 0, "fixture", null_work, nullptr) == SCE_KERNEL_ERROR_ILLEGAL_ADDR);
    }
    test_guest_sync_deletion(*env, runtime);
    test_guest_kernel_info(*env, runtime);
    // A throwing HLE import must be diagnosed at the fiber boundary, counted
    // once, and reaped without executing guest writeback or acknowledging it
    // as success. Exercise standard and non-standard C++ exceptions alike.
    for (bool unknown : {false, true}) {
        env->kernel.call_import = [unknown](CPUState &, uint32_t nid, SceUID) {
            REQUIRE(nid == 0x0c7b834b);
            if (unknown) throw 7;
            throw std::runtime_error("intentional HLE exception fixture");
        };
        REQUIRE(runtime.attach(*env));
        const Address result = data + 0x204;
        *Ptr<uint32_t>(result).get(env->mem) = 0xcccccccc;
        guest_thread_fixture::build_waiter(env->mem, code, 0, 0, result);
        auto faulting = env->kernel.create_thread(env->mem, "exception fixture", Ptr<const void>(code),
            SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
            SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        REQUIRE(faulting && faulting->start(0, Ptr<void>{}, false) == 0);
        const auto failed = runtime.resume(64);
        REQUIRE(failed.failed == 1 && failed.idle && failed.runnable == 0);
        REQUIRE(!env->kernel.threads.contains(faulting->id));
        REQUIRE(faulting->returned_value == 0xDEADDEAD);
        REQUIRE(*Ptr<uint32_t>(result).get(env->mem) == 0xcccccccc);
        REQUIRE(get_current_cpu_state() == nullptr);
        REQUIRE(runtime.resume(64).failed == 1); // not double-counted after reap
        REQUIRE(runtime.shutdown());
        REQUIRE(env->kernel.threads.empty() && !env->kernel.execution_host);
    }
    std::puts("Guest thread exceptions: diagnostics, failure accounting and clean teardown passed");
    {
        // VitaSDK SCE_DBG_ASSERT: sceDbgAssertionHandler(file, line, 0, component,
        // msg), then SCE_DBG_BREAK_ACTION (bkpt). The handler returns and requests
        // no process exit; the break fails the thread, so the run is not reported
        // as a clean exit.
        unsigned handler_calls = 0;
        bool exit_requested = false;
        env->kernel.call_import = [&](CPUState &cpu, uint32_t nid, SceUID tid) {
            REQUIRE(nid == 0x1AF3678B);
            ++handler_calls;
            call_import(*env, cpu, nid, tid);
            REQUIRE(env->missing_nids.empty());
        };
        env->kernel.process_exit_callback = [&](int, std::optional<AppLaunchRequest>) { exit_requested = true; };
        REQUIRE(runtime.attach(*env));
        const Address stub = code + 0x100, result = data + 0x204, text = data + 0x300;
        const uint32_t stub_words[] = { 0xef000000, 0xe1a0f00e, 0x1AF3678B };
        std::memcpy(Ptr<void>(stub).get(env->mem), stub_words, sizeof(stub_words));
        std::strcpy(Ptr<char>(text).get(env->mem), "Assertion (x) failed.\n");
        *Ptr<uint32_t>(result).get(env->mem) = 0xcccccccc;
        guest_thread_fixture::Arm p(code);
        p.emit(0xe92d4010); // push {r4,lr}
        p.emit(0xe24dd008); // sub sp,sp,#8
        p.constant(4, result);
        p.constant(0, text);
        p.emit(0xe58d0000); // str r0,[sp]: msg, the fifth argument
        p.constant(0, text); p.constant(1, 7); p.constant(2, 0); p.constant(3, text);
        p.call(stub);
        p.store(0, 0);
        p.emit(0xe1200070); // bkpt #0
        p.emit(0xe28dd008); // add sp,sp,#8
        p.emit(0xe8bd8010); // pop {r4,pc}
        p.finish(env->mem);
        auto asserting = env->kernel.create_thread(env->mem, "assertion fixture", Ptr<const void>(code),
            SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
            SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        REQUIRE(asserting && asserting->start(0, Ptr<void>{}, false) == 0);
        const auto progress = runtime.resume(64);
        REQUIRE(handler_calls == 1 && !exit_requested);
        REQUIRE(*Ptr<uint32_t>(result).get(env->mem) == 0);
        REQUIRE(progress.failed == 1 && progress.idle && progress.runnable == 0);
        REQUIRE(asserting->returned_value == 0xDEADDEAD);
        REQUIRE(runtime.shutdown());
        REQUIRE(env->kernel.threads.empty() && !env->kernel.execution_host);
        env->kernel.process_exit_callback = {};
        std::puts("Guest assertion: handler returns, the guest break fails the thread, no clean exit");
    }
    // Do not retain the last scenario's observer references after their scope.
    env->kernel.call_import = {};
}
