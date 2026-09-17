// Asset-free ARM guest lifecycle and production semaphore integration.
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
}
