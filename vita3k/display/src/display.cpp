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

#include <display/functions.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <dialog/state.h>
#include <motion/functions.h>
#include <touch/functions.h>
#endif
#include <display/state.h>
#include <emuenv/state.h>
#include <kernel/callback.h>
#include <kernel/state.h>
#include <renderer/state.h>

#include <chrono>
#include <cstdint>
#include <vector>

// The vblank clock is read on every guest thread switch in the browser. There,
// libc++ reaches the same monotonic clock (performance.now()) through a
// clock_gettime call that JS exceptions wrap in an allocating invoke_*
// thunk; read it directly instead.
static std::chrono::steady_clock::time_point vblank_clock_now() noexcept {
#ifdef __EMSCRIPTEN__
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(
        static_cast<std::int64_t>(emscripten_get_now() * 1e6)));
#else
    return std::chrono::steady_clock::now();
#endif
}

// Code heavily influenced by PPSSSPP's SceDisplay.cpp

static constexpr int TARGET_FPS = 60;
static constexpr int64_t TARGET_MICRO_PER_FRAME = 1000000LL / TARGET_FPS;
// how many cycles do we need to see before we start predicting the next frame
static constexpr int predict_threshold = 3;
static constexpr int max_expected_swapchain_size = 6;

// One emulated vblank tick: increments the vblank count, notifies the vblank
// callbacks and wakes the threads whose target vcount was reached. On native
// builds this runs on the host vblank thread (see vblank_sync_thread below);
// single-threaded hosts such as the browser Worker drive it cooperatively from
// wait_vblank instead, on the emulator's only thread.
void advance_vblank(EmuEnvState &emuenv) {
    DisplayState &display = emuenv.display;

    std::unique_lock<std::mutex> guard(display.mutex);

    {
        const std::lock_guard<std::mutex> guard_info(display.display_info_mutex);
        ++display.vblank_count;

#ifndef __EMSCRIPTEN__
        // Native-only: this both dereferences the native renderer (absent in
        // the browser runtime) and depends on the host pause/common-dialog
        // state that only exists with a frontend.
        // in this case, even though no new game frames are being rendered, we still need to update the screen
        if (emuenv.kernel.is_threads_paused() || (emuenv.common_dialog.status == SCE_COMMON_DIALOG_STATUS_RUNNING))
            // only display the UI/common dialog at 30 fps
            // this is necessary so that the command buffer processing doesn't get starved
            // with vsync enabled and a screen with a refresh rate of 60Hz or less
            if (display.vblank_count % 2 == 0)
                emuenv.renderer->should_display = true;
#endif
    }

#ifndef __EMSCRIPTEN__
    // Native-only per-vblank host input sampling; the browser Worker has no
    // host touch/motion event sources to sample here.
    // maybe we should also use a mutex for this part, but it shouldn't be an issue
    touch_vsync_update(emuenv);
    refresh_motion(emuenv.motion, emuenv.ctrl);
#endif

    // Notify Vblank callback in each VBLANK start
    std::vector<SceUID> notified;
    for (auto &[_, cb] : display.vblank_callbacks) {
        cb->event_notify(cb->get_notifier_id());
        notified.push_back(cb->get_owner_thread_id());
    }

    std::vector<ThreadStatePtr> woken;
    for (std::size_t i = 0; i < display.vblank_wait_infos.size();) {
        auto &vblank_wait_info = display.vblank_wait_infos[i];
        if (vblank_wait_info.target_vcount <= display.vblank_count) {
            woken.push_back(vblank_wait_info.target_thread);
            display.vblank_wait_infos.erase(display.vblank_wait_infos.begin() + i);
        } else {
            i++;
        }
    }
    // Waking takes the thread's lock, which wait_vblank holds while it takes
    // display.mutex.
    guard.unlock();
    for (const ThreadStatePtr &target_wait : woken) {
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_SHARED_MEMORY__)
        // With host threads the waiter checks its status under its own lock
        // before it sleeps. Setting it without that lock can land between the
        // check and the sleep, and the wakeup is lost for good (the thread is
        // no longer in vblank_wait_infos).
        const std::lock_guard<std::mutex> thread_guard(target_wait->mutex);
#endif
        target_wait->update_status(ThreadStatus::run);
    }
    for (const SceUID owner : notified)
        wake_callback_wait(emuenv.kernel, owner);
}

static void vblank_sync_thread(EmuEnvState &emuenv) {
    DisplayState &display = emuenv.display;

    while (!display.abort.load()) {
        advance_vblank(emuenv);

        const auto time_ms = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto time_left = TARGET_MICRO_PER_FRAME - (time_ms % TARGET_MICRO_PER_FRAME);
        std::this_thread::sleep_for(std::chrono::microseconds(time_left));
    }
}

void start_sync_thread(EmuEnvState &emuenv) {
    emuenv.display.vblank_thread = std::make_unique<std::thread>(vblank_sync_thread, std::ref(emuenv));
}

// Browser clock driver, shared by the cooperative fiber runtime and the
// legacy self-driven wait below. See display/functions.h for the contract.
bool service_vblank(EmuEnvState &emuenv) {
    DisplayState &display = emuenv.display;
    const auto before = display.vblank_count.load();
    if (display.fast_vblank) {
        // Headroom mode has no wall clock to follow: vblanks happen only for
        // threads that wait for one.
        if (display.vblank_wait_infos.empty())
            return false;
        // Headroom mode: one vblank per service pass, no wall-clock gating.
        // next_vblank_time tracks now so leaving fast mode (or concurrent
        // readers) never sees a stale deadline.
        advance_vblank(emuenv);
        display.next_vblank_time = vblank_clock_now();
        return display.vblank_count.load() != before;
    }
    const auto now = vblank_clock_now();
    if (display.next_vblank_time.time_since_epoch().count() == 0) {
        // First wait: seed the cadence so the first vblank lands one full
        // period from now, like arriving just after a vblank start on native.
        display.next_vblank_time = now + std::chrono::microseconds(TARGET_MICRO_PER_FRAME);
    }
    // Advance by every fully elapsed period, mirroring the native vblank
    // thread's steady wall-clock cadence (a slow guest sees its vcount jump by
    // the number of missed vblanks, as on native). This runs with no thread
    // waiting too: games also poll sceDisplayGetVcount (Persona 4 Golden's
    // display callback spins on it), and a clock that only moved for waiters
    // left that poll, and every thread spinning on its result, stuck.
    // Only a waiter or a vblank callback can be woken, so a tick without them
    // does not count as a wake for the scheduler (it would spin when idle).
    const bool wakes = !display.vblank_wait_infos.empty() || !display.vblank_callbacks.empty();
    while (now >= display.next_vblank_time) {
        advance_vblank(emuenv);
        display.next_vblank_time += std::chrono::microseconds(TARGET_MICRO_PER_FRAME);
    }
    return wakes && display.vblank_count.load() != before;
}

void wait_vblank(EmuEnvState &emuenv, const ThreadStatePtr &wait_thread, const uint64_t target_vcount, const bool is_cb) {
    DisplayState &display = emuenv.display;

    if (!wait_thread) {
        return;
    }

    {
        auto thread_lock = std::unique_lock(wait_thread->mutex);

        {
            const std::lock_guard<std::mutex> guard(display.mutex);

            if (target_vcount <= display.vblank_count)
                return;

            wait_thread->update_status(ThreadStatus::wait);
            display.vblank_wait_infos.push_back({ wait_thread, target_vcount });
        }

        // The threaded browser build (shared memory) has the desktop vblank thread.
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_SHARED_MEMORY__)
        // Browser Worker path: there is no host vblank thread, so nothing would
        // ever wake status_cond. Two shapes, one clock service:
        //
        // * With a cooperative execution host (the fiber runtime the browser
        //   app installs) the waiting fiber parks through wait_sync, exactly
        //   like the kernel sync primitives do, and the runtime's service pass
        //   calls service_vblank to advance the emulated clock and wake it.
        //   Parking is what makes this usable: the Worker has one thread, so
        //   every OTHER guest fiber keeps running while this one waits out its
        //   vblank. The previous sleep loop instead froze the whole Worker for
        //   up to a full frame per wait, which was over half the wall clock as
        //   soon as a title paced itself with sceDisplayWaitSetFrameBuf.
        // * Without one (a bare Emscripten build with no fiber runtime) the
        //   waiting thread keeps driving the clock itself, yielding to the
        //   browser event loop between ticks.
        if (emuenv.kernel.execution_host) {
            thread_lock.unlock();
            emuenv.kernel.execution_host->wait_sync(*wait_thread, std::nullopt);
            thread_lock.lock();
        } else {
            while (wait_thread->status != ThreadStatus::run && !display.abort.load()) {
                service_vblank(emuenv);
                thread_lock.unlock();
                emscripten_sleep(1);
                thread_lock.lock();
            }
        }
#else
        wait_thread->status_cond.wait(thread_lock, [&]() {
            return wait_thread->status == ThreadStatus::run;
        });
#endif
    }

    if (is_cb) {
        for (auto &[_, cb] : display.vblank_callbacks) {
            if (cb->get_owner_thread_id() == wait_thread->id) {
                std::string name = cb->get_name();
                cb->execute(emuenv.kernel, [name]() {
                });
            }
        }
    }
}

static void reset_swapchain_cycle(DisplayState &display, Address sync_object) {
    display.predicted_frames.resize(1);
    display.predicted_frames[0].sync_object = sync_object;
    display.predicted_frame_position = 0;
    display.predicted_cycles_seen = 0;
}

DisplayFrameInfo *predict_next_image(EmuEnvState &emuenv, Address sync_object) {
    auto &display = emuenv.display;
    std::lock_guard<std::mutex> lock(display.display_info_mutex);

    if (display.predicted_cycles_seen >= predict_threshold) {
        // just check that the next sync_object in line is the one we expect
        display.predicted_frame_position = (display.predicted_frame_position + 1) % display.predicted_frames.size();
        if (display.predicted_frames[display.predicted_frame_position].sync_object != sync_object)
            // bad, this isn't what we expect
            reset_swapchain_cycle(display, sync_object);

    } else if (display.predicted_cycles_seen >= 1) {
        display.predicted_frame_position = (display.predicted_frame_position + 1) % display.predicted_frames.size();

        if (display.predicted_frame_position == 0)
            display.predicted_cycles_seen++;

        if (display.predicted_frames[display.predicted_frame_position].sync_object != sync_object)
            // bad, this isn't what we expect
            reset_swapchain_cycle(display, sync_object);
    } else {
        // check if we have a cycle
        bool has_cycle = false;
        for (int idx = 0; idx < display.predicted_frames.size(); idx++) {
            if (display.predicted_frames[idx].sync_object == sync_object) {
                // we found a cycle
                has_cycle = true;
                display.predicted_frames.erase(display.predicted_frames.begin(), display.predicted_frames.begin() + idx);
                display.predicted_frame_position = 0;
                display.predicted_cycles_seen = 1;
                break;
            }
        }

        if (!has_cycle) {
            // predicted_frame_position is initialized to -1, so this is fine
            display.predicted_frame_position++;
            if (display.predicted_frame_position == display.predicted_frames.size()) {
                // keep the last max_expected_swapchain_size frames for the swapchain cycle
                if (display.predicted_frames.size() == max_expected_swapchain_size) {
                    display.predicted_frame_position = 0;
                } else {
                    display.predicted_frames.resize(display.predicted_frames.size() + 1);
                }
            }
            display.predicted_frames[display.predicted_frame_position].sync_object = sync_object;
        }
    }

    bool predict = display.predicted_cycles_seen >= predict_threshold;
    DisplayFrameInfo *frame = nullptr;
    if (predict) {
        // set the next framebuffer image here
        frame = new DisplayFrameInfo;
        *frame = display.predicted_frames[display.predicted_frame_position].frame_info;
    }

    return frame;
}

void update_prediction(EmuEnvState &emuenv, DisplayFrameInfo &frame) {
    auto &display = emuenv.display;
    std::lock_guard<std::mutex> lock(display.display_info_mutex);
    Address sync_object = display.current_sync_object;

    if (!display.predicting) {
        display.next_rendered_frame = frame;
        // The browser runtime has no renderer; presentation happens through the
        // display state itself (sce_frame / next_rendered_frame) instead.
        if (emuenv.renderer)
            emuenv.renderer->should_display = true;
    }

    for (auto &pred_frame : display.predicted_frames) {
        if (pred_frame.sync_object != sync_object)
            continue;

        if (memcmp(&pred_frame.frame_info, &frame, sizeof(DisplayFrameInfo)) == 0)
            // we got what we expected, fine
            return;

        pred_frame.frame_info = frame;
        break;
    }

    if (display.predicting) {
        LOG_TRACE("Mispredicted the next swapchain image");
        display.next_rendered_frame = frame;
        // No renderer exists in the browser runtime (see above).
        if (emuenv.renderer)
            emuenv.renderer->should_display = true;
    }

    // let predict_next_image reset the cycle if necessary
    display.predicted_cycles_seen = std::min(display.predicted_cycles_seen, 1U);
}

void DisplayState::deinit() {
    abort = true;
    if (vblank_thread && vblank_thread->joinable())
        vblank_thread->join();

    vblank_thread.reset();
    abort = false;

    {
        const std::lock_guard<std::mutex> guard(mutex);
        vblank_wait_infos.clear();
        vblank_callbacks.clear();
    }

    {
        const std::lock_guard<std::mutex> guard(display_info_mutex);
        sce_frame = {};
        next_rendered_frame = {};
    }

    predicted_frames.clear();
    predicted_frame_position = static_cast<uint32_t>(-1);
    predicted_cycles_seen = 0;
    predicting = false;
    current_sync_object = 0;

    vblank_count = 0;
    last_setframe_vblank_count = 0;
    next_vblank_time = {};

    fps_hack = false;
    // pretty sure we set this on game boot
    fullscreen = false;
}