// Asset-free ARM guest lifecycle and production semaphore/mutex integration.
#include "guest_thread_runtime.h"
#include <cpu/functions.h>
#include <emuenv/state.h>
#include <kernel/state.h>
#include <kernel/sync_primitives.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <modules/module_parent.h>
#include <cstdio>
#include <cstdlib>
#include "guest_thread_semaphore_fixture.h"

#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
#include "guest_mspace_tests.h"

int main() {
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
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
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
            // Reject unsupported object deletion without changing the queue.
            REQUIRE(semaphore_delete(env->kernel, "fixture", 0, id) == SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
            REQUIRE(queue->waiting_threads->size() == 1);
            if (scenario == 1) {
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
        REQUIRE(semaphore_delete(env->kernel, "fixture", 0, id) == 0);
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
    REQUIRE(mutex_delete(env->kernel, "fixture", lock_parent->id, lwmutex, SyncWeight::Light)
        == SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
    REQUIRE(env->kernel.lwmutexes.contains(lwmutex));
    REQUIRE(lock->waiting_threads->size() == 1 && lock->owner == lock_parent);
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
        for (unsigned scenario = 0; scenario < 5; ++scenario) {
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
                REQUIRE(mutex_delete(env->kernel, "fixture", owner->id, id, weight) == SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
                REQUIRE((light ? env->kernel.lwmutexes : env->kernel.mutexes).contains(id));
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
            if (scenario != 3) {
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
            REQUIRE(wait_return == uint32_t(scenario <= 1 ? SCE_KERNEL_ERROR_WAIT_TIMEOUT : SCE_KERNEL_ERROR_WAIT_CANCEL));
            REQUIRE(get_current_cpu_state() == nullptr);
            REQUIRE(mutex_delete(env->kernel, "fixture", 0, id, weight) == 0);
            std::printf("%s edge case %u passed\n", light ? "LwMutex" : "Mutex", scenario);
        }
    }
    // Do not retain the last scenario's observer references after their scope.
    env->kernel.call_import = {};
}
