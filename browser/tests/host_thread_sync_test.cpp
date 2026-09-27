// Desktop (no execution host) timer waits against real concurrent host
// threads: a guest thread waits on a timer in sceKernelWaitEvent while
// another deletes or closes it. Built only with -pthread.
#include <emuenv/state.h>
#include <kernel/state.h>
#include <kernel/sync_primitives.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <limits>
#include <thread>

#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace {
using namespace std::chrono_literals;

// A running guest thread without a CPU: the timer wait only needs its state.
ThreadStatePtr make_thread(EmuEnvState &env) {
    auto thread = std::make_shared<ThreadState>(env.kernel.get_next_uid(), env.kernel, env.mem);
    thread->priority = 0x60;
    thread->status = ThreadStatus::run;
    const std::lock_guard<std::mutex> lock(env.kernel.mutex);
    env.kernel.threads.emplace(thread->id, thread);
    return thread;
}

struct Waiter {
    std::future<SceInt32> result;
    SceUInt32 pattern = 0;
    SceUInt64 user_data = 0;
};

// Starts `thread` waiting on `timer` for `bits` and returns once it waits.
void wait_on(EmuEnvState &env, const ThreadStatePtr &thread, SceUID timer, SceUInt32 bits, Waiter &waiter) {
    waiter.result = std::async(std::launch::async, [&env, thread, timer, bits, &waiter] {
        return simple_event_waitorpoll(env.kernel, "fixture", thread->id, timer, bits, &waiter.pattern, &waiter.user_data, nullptr, true);
    });
    const auto limit = std::chrono::steady_clock::now() + 2s;
    while (true) {
        const TimerPtr object = env.kernel.timers.at(timer);
        {
            const std::lock_guard<std::mutex> lock(object->mutex);
            if (object->waiting_threads->find(thread) != object->waiting_threads->end())
                return;
        }
        REQUIRE(std::chrono::steady_clock::now() < limit);
        std::this_thread::sleep_for(1ms);
    }
}

// Runs `call` on the test thread and requires it to finish promptly.
template <typename Call>
SceInt32 bounded(Call call) {
    auto done = std::async(std::launch::async, call);
    REQUIRE(done.wait_for(2s) == std::future_status::ready);
    return done.get();
}
} // namespace

int main() {
    auto env = std::make_unique<EmuEnvState>();
    REQUIRE(init(env->mem, true));
    REQUIRE(env->kernel.init(env->mem, [](CPUState &, uint32_t, SceUID) {}, false));
    REQUIRE(!env->kernel.execution_host);
    auto &kernel = env->kernel;
    const auto waiter_thread = make_thread(*env);
    const auto deleter = make_thread(*env);

    // Deleting a stopped timer's only handle, and a running timer's with a
    // long interval, completes at once; the waiter wakes with WAIT_DELETE.
    for (const bool running : { false, true }) {
        const SceUID timer = timer_create(kernel, env->mem, "fixture", "waited timer", deleter->id, 0);
        if (running) {
            SceKernelSysClock interval = 60'000'000;
            REQUIRE(timer_set(kernel, "fixture", deleter->id, timer, 0, &interval, 0) == 0);
            REQUIRE(timer_start(kernel, "fixture", deleter->id, timer) == 0);
        }
        Waiter waiter;
        wait_on(*env, waiter_thread, timer, SCE_KERNEL_EVENT_TIMER, waiter);
        REQUIRE(bounded([&] { return timer_close(kernel, "fixture", deleter->id, timer, HandleClose::Delete); }) == 0);
        REQUIRE(waiter.result.wait_for(2s) == std::future_status::ready);
        REQUIRE(waiter.result.get() == SCE_KERNEL_ERROR_WAIT_DELETE);
        REQUIRE(waiter_thread->status == ThreadStatus::run && kernel.timers.empty());
    }
    std::puts("DeleteTimer with a waiter passed");

    // With an opened handle left, DeleteTimer stops the timer and wakes only
    // a waiter for DELETE; the last CloseTimer wakes the rest with WAIT_DELETE.
    {
        const SceUID timer = timer_create(kernel, env->mem, "fixture", "opened timer", deleter->id, SCE_KERNEL_ATTR_OPENABLE);
        const SceUID opened = timer_open(kernel, "fixture", deleter->id, "opened timer");
        const auto other_thread = make_thread(*env);
        Waiter timed, deleted;
        wait_on(*env, waiter_thread, timer, SCE_KERNEL_EVENT_TIMER, timed);
        wait_on(*env, other_thread, timer, SCE_KERNEL_EVENT_DELETE, deleted);
        REQUIRE(bounded([&] { return timer_close(kernel, "fixture", deleter->id, timer, HandleClose::Delete); }) == 0);
        REQUIRE(deleted.result.wait_for(2s) == std::future_status::ready);
        REQUIRE(deleted.result.get() == 0 && (deleted.pattern & SCE_KERNEL_EVENT_DELETE));
        REQUIRE(deleted.user_data == ((SceUInt64(uint32_t(deleter->id)) << 32) | uint32_t(KernelState::process_id)));
        REQUIRE(timed.result.wait_for(50ms) == std::future_status::timeout);
        REQUIRE(bounded([&] { return timer_close(kernel, "fixture", deleter->id, opened, HandleClose::Close); }) == 0);
        REQUIRE(timed.result.wait_for(2s) == std::future_status::ready);
        REQUIRE(timed.result.get() == SCE_KERNEL_ERROR_WAIT_DELETE && kernel.timers.empty());
    }
    std::puts("DeleteTimer and CloseTimer with an opened handle passed");

    // A timer that fires wakes its waiter; the next waiter then times the
    // next expiry.
    {
        const SceUID timer = timer_create(kernel, env->mem, "fixture", "firing timer", deleter->id, SCE_KERNEL_EVENT_ATTR_AUTO_RESET);
        SceKernelSysClock interval = 20'000;
        REQUIRE(timer_set(kernel, "fixture", deleter->id, timer, 0, &interval, 1) == 0);
        const auto other_thread = make_thread(*env);
        Waiter first, second;
        wait_on(*env, waiter_thread, timer, SCE_KERNEL_EVENT_TIMER, first);
        wait_on(*env, other_thread, timer, SCE_KERNEL_EVENT_TIMER, second);
        REQUIRE(timer_start(kernel, "fixture", deleter->id, timer) == 0);
        REQUIRE(first.result.wait_for(2s) == std::future_status::ready && first.result.get() == 0);
        REQUIRE(first.pattern == SCE_KERNEL_EVENT_TIMER);
        REQUIRE(second.result.wait_for(2s) == std::future_status::ready && second.result.get() == 0);
        REQUIRE(bounded([&] { return timer_close(kernel, "fixture", deleter->id, timer, HandleClose::Delete); }) == 0);
    }
    std::puts("Timer expiry with two waiters passed");
    return 0;
}
