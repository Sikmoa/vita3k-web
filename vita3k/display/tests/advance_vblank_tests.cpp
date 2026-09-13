// Native tests for the factored per-vblank work (advance_vblank) and the
// non-blocking paths of wait_vblank. These use the production EmuEnvState,
// DisplayState and ThreadState objects without initializing memory, the CPU or
// the kernel: advance_vblank only touches display state and thread statuses.
//
// The native blocking wait (status_cond) is exercised too, by driving
// advance_vblank from a second host thread exactly like vblank_sync_thread
// would. The browser cooperative branch (__EMSCRIPTEN__) is compiled only for
// the Emscripten runtime and needs the Worker event loop; it is validated by
// the browser fixture runs instead.

#include <display/functions.h>
#include <display/state.h>
#include <emuenv/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace {

ThreadStatePtr make_thread(EmuEnvState &emuenv) {
    return std::make_shared<ThreadState>(emuenv.kernel.get_next_uid(), emuenv.kernel, emuenv.mem);
}

ThreadStatus status_of(const ThreadStatePtr &thread) {
    const std::lock_guard<std::mutex> guard(thread->mutex);
    return thread->status;
}

void park_in_vblank_wait(EmuEnvState &emuenv, const ThreadStatePtr &thread, uint64_t target_vcount) {
    const std::lock_guard<std::mutex> guard(emuenv.display.mutex);
    thread->update_status(ThreadStatus::wait);
    emuenv.display.vblank_wait_infos.push_back({ thread, target_vcount });
}

} // namespace

TEST(AdvanceVblank, IncrementsVcountAndWakesReachedWaiterOnly) {
    EmuEnvState emuenv;
    DisplayState &display = emuenv.display;

    const ThreadStatePtr reached = make_thread(emuenv);
    const ThreadStatePtr pending = make_thread(emuenv);

    const uint64_t initial_vcount = display.vblank_count.load();
    park_in_vblank_wait(emuenv, reached, initial_vcount + 1);
    park_in_vblank_wait(emuenv, pending, initial_vcount + 3);

    advance_vblank(emuenv);

    EXPECT_EQ(display.vblank_count.load(), initial_vcount + 1);
    EXPECT_EQ(status_of(reached), ThreadStatus::run);
    EXPECT_EQ(status_of(pending), ThreadStatus::wait);

    {
        const std::lock_guard<std::mutex> guard(display.mutex);
        ASSERT_EQ(display.vblank_wait_infos.size(), std::size_t(1));
        EXPECT_EQ(display.vblank_wait_infos[0].target_thread, pending);
        EXPECT_EQ(display.vblank_wait_infos[0].target_vcount, initial_vcount + 3);
    }

    // The pending waiter is woken once its own target vcount is reached.
    advance_vblank(emuenv);
    EXPECT_EQ(display.vblank_count.load(), initial_vcount + 2);
    EXPECT_EQ(status_of(pending), ThreadStatus::wait);

    advance_vblank(emuenv);
    EXPECT_EQ(display.vblank_count.load(), initial_vcount + 3);
    EXPECT_EQ(status_of(pending), ThreadStatus::run);
    EXPECT_TRUE(display.vblank_wait_infos.empty());
}

TEST(AdvanceVblank, CurrentVcountTargetIsWokenByNextTick) {
    EmuEnvState emuenv;
    DisplayState &display = emuenv.display;

    // A waiter whose target equals the current vblank count (as produced by
    // sceCtrl/sceTouch style last_vcount + 1 bookkeeping after a tick) must be
    // woken by the next tick, not the current one.
    const ThreadStatePtr waiter = make_thread(emuenv);
    park_in_vblank_wait(emuenv, waiter, display.vblank_count.load());

    advance_vblank(emuenv);
    EXPECT_EQ(status_of(waiter), ThreadStatus::run);
    EXPECT_TRUE(display.vblank_wait_infos.empty());
}

TEST(WaitVblank, AlreadyReachedTargetReturnsImmediately) {
    EmuEnvState emuenv;
    DisplayState &display = emuenv.display;

    const ThreadStatePtr thread = make_thread(emuenv);

    // target <= current vblank count: must return without parking the thread.
    wait_vblank(emuenv, thread, display.vblank_count.load(), false);

    EXPECT_EQ(status_of(thread), ThreadStatus::dormant);
    EXPECT_TRUE(display.vblank_wait_infos.empty());
    EXPECT_EQ(display.vblank_count.load(), 0);
}

TEST(WaitVblank, NullThreadIsIgnored) {
    EmuEnvState emuenv;

    wait_vblank(emuenv, nullptr, 1, false);

    EXPECT_TRUE(emuenv.display.vblank_wait_infos.empty());
}

TEST(WaitVblank, BlockingWaitIsWokenByAdvanceVblank) {
    EmuEnvState emuenv;
    DisplayState &display = emuenv.display;

    const ThreadStatePtr thread = make_thread(emuenv);
    const uint64_t target_vcount = display.vblank_count.load() + 1;
    std::atomic<bool> returned{ false };

    // The real blocking path: park on status_cond like an HLE caller on the
    // emulator host thread, while this test thread plays the vblank thread.
    std::thread waiter([&] {
        wait_vblank(emuenv, thread, target_vcount, false);
        returned.store(true);
    });

    // Wait until wait_vblank has parked the thread, then advance the clock.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        bool parked = false;
        {
            const std::lock_guard<std::mutex> guard(display.mutex);
            parked = !display.vblank_wait_infos.empty();
        }
        if (parked)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    advance_vblank(emuenv);
    waiter.join();

    EXPECT_TRUE(returned.load());
    EXPECT_EQ(status_of(thread), ThreadStatus::run);
    EXPECT_TRUE(display.vblank_wait_infos.empty());
    EXPECT_EQ(display.vblank_count.load(), target_vcount);
}
