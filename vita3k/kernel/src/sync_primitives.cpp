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

#include <cpu/functions.h>
#include <kernel/state.h>
#include <kernel/sync_primitives.h>

#include <kernel/types.h>
#include <util/lock_and_find.h>
#include <util/log.h>

#include <cstdio>
#include <cstdlib>
#include <limits>

static constexpr bool LOG_SYNC_PRIMITIVES = false;

// ***********
// * Helpers *
// ***********

inline static int unknown_mutex_id(const char *export_name, SyncWeight weight) {
    if (weight == SyncWeight::Light)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_LW_MUTEX_ID);
    return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID);
}

inline static int unknown_cond_id(const char *export_name, SyncWeight weight) {
    if (weight == SyncWeight::Light)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_LW_COND_ID);
    return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_COND_ID);
}

inline static MutexPtrs &get_mutexes(KernelState &kernel, SyncWeight weight) {
    return weight == SyncWeight::Light ? kernel.lwmutexes : kernel.mutexes;
}

inline static CondvarPtrs &get_condvars(KernelState &kernel, SyncWeight weight) {
    return weight == SyncWeight::Light ? kernel.lwcondvars : kernel.condvars;
}

inline static int find_mutex(MutexPtr &mutex_out, MutexPtrs **mutexes_out, KernelState &kernel, const char *export_name, SceUID mutexid, SyncWeight weight) {
    MutexPtrs &mutexes = get_mutexes(kernel, weight);
    mutex_out = lock_and_find(mutexid, mutexes, kernel.mutex);
    if (!mutex_out) {
        return unknown_mutex_id(export_name, weight);
    }

    if (mutexes_out)
        *mutexes_out = &mutexes;

    return SCE_KERNEL_OK;
}

inline static int find_condvar(CondvarPtr &condvar_out, CondvarPtrs **condvars_out, KernelState &kernel, const char *export_name, SceUID condid, SyncWeight weight) {
    CondvarPtrs &condvars = get_condvars(kernel, weight);
    condvar_out = lock_and_find(condid, condvars, kernel.mutex);
    if (!condvar_out) {
        return unknown_cond_id(export_name, weight);
    }

    if (condvars_out)
        *condvars_out = &condvars;

    return SCE_KERNEL_OK;
}

// TODO: Write remaining time to timeout ptr when it's successfully signaled
// Assumes primitive_lock is locked and thread_lock is unlocked
inline static int handle_timeout(KernelState &kernel, const ThreadStatePtr &thread, std::unique_lock<std::mutex> &thread_lock,
    std::unique_lock<std::mutex> &primitive_lock, WaitingThreadQueuePtr &queue,
    const ThreadDataQueueInterator<WaitingThreadData> &data_it, const char *export_name,
    SceUInt *const timeout) {
    // Only semaphore/mutex waits opt into handle_cooperative_wait below.
    // Roll back the queue/status before returning an explicit unsupported-context
    // error; never fall through to a host condition-variable wait in the browser.
    if (kernel.execution_host) {
        queue->erase(data_it);
        thread_lock.lock();
        thread->update_status(ThreadStatus::run);
        thread_lock.unlock();
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
    }
    if (timeout) {
        bool status = false;
        auto start = std::chrono::steady_clock::now();
        if (*timeout > 0) {
            status = thread->status_cond.wait_for(primitive_lock, std::chrono::microseconds{ *timeout }, [&] { return thread->status == ThreadStatus::run; });
        }

        if (!status) {
            *timeout = 0; // Time run out, so remaining time is 0

            thread_lock.lock();
            thread->update_status(ThreadStatus::run, ThreadStatus::wait);
            thread_lock.unlock();

            queue->erase(data_it);

            return RET_ERROR(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
        } else {
            auto end = std::chrono::steady_clock::now();
            uint32_t real_timeout = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
            if (real_timeout > *timeout) {
                *timeout = 0;
            } else {
                *timeout = *timeout - real_timeout;
            }
        }
    } else {
        thread->status_cond.wait(primitive_lock, [&] { return thread->status == ThreadStatus::run; });
    }

    return SCE_KERNEL_OK;
}

// Opt-in only: primitive_lock is held, thread_lock is not. The caller's
// cancellation flag (if any) lives on the parked fiber until we unlink it.
inline static int handle_cooperative_wait(KernelState &kernel, const ThreadStatePtr &thread,
    std::unique_lock<std::mutex> &thread_lock, std::unique_lock<std::mutex> &primitive_lock,
    WaitingThreadQueuePtr &queue, SceUInt *timeout, KernelExecutionHost::WaitResult *wait_result = nullptr) {
    const auto start = std::chrono::steady_clock::now();
    const auto duration = timeout ? std::optional<uint32_t>(*timeout) : std::nullopt;
    primitive_lock.unlock();
    KernelExecutionHost::WaitResult result;
    try {
        result = kernel.execution_host->wait_sync(*thread, duration);
    } catch (...) {
        // Never retain a pointer to the unwinding fiber's cancellation flag.
        primitive_lock.lock();
        const auto pending = queue->find(thread);
        if (pending != queue->end())
            queue->erase(pending);
        thread_lock.lock();
        thread->update_status(ThreadStatus::run);
        thread_lock.unlock();
        throw;
    }
    if (wait_result)
        *wait_result = result;
    primitive_lock.lock();
    // Unlock/signal/cancel may already have erased the original iterator.
    const auto pending = queue->find(thread);
    const bool still_queued = pending != queue->end();
    if (still_queued)
        queue->erase(pending);
    thread_lock.lock();
    thread->update_status(ThreadStatus::run);
    thread_lock.unlock();
    if (timeout) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        *timeout = elapsed >= *duration ? 0 : *duration - static_cast<uint32_t>(elapsed);
    }
    if (still_queued)
        return result == KernelExecutionHost::WaitResult::timeout
            ? SCE_KERNEL_ERROR_WAIT_TIMEOUT : SCE_KERNEL_ERROR_WAIT_CANCEL;
    return SCE_KERNEL_OK; // caller checks its explicit cancellation flag
}

// *****************
// * Simple events *
// *****************

SceUID simple_event_create(KernelState &kernel, MemState &mem, const char *export_name, const char *name, SceUID thread_id, SceUInt32 attr, SceUInt32 init_pattern) {
    if ((strlen(name) > 31) && ((attr & 0x80) == 0x80)) {
        return RET_ERROR(SCE_KERNEL_ERROR_UID_NAME_TOO_LONG);
    }

    const SceUID uid = kernel.get_next_uid();

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} pattern: {:#b}",
            export_name, uid, thread_id, name, attr, init_pattern);
    }

    const SimpleEventPtr event = std::make_shared<SimpleEvent>();
    event->uid = uid;
    event->pattern = init_pattern;
    strncpy(event->name, name, KERNELOBJECT_MAX_NAME_LENGTH);
    event->attr = attr;
    if (event->attr & SCE_KERNEL_ATTR_TH_PRIO) {
        event->waiting_threads = std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();
    } else {
        event->waiting_threads = std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
    }

    event->last_user_data = 0;
    event->auto_reset = (event->attr & SCE_KERNEL_EVENT_ATTR_AUTO_RESET);
    event->cb_wakeup_only = (event->attr & SCE_KERNEL_ATTR_NOTIFY_CB_WAKEUP_ONLY);

    const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);
    kernel.simple_events.emplace(uid, event);

    return uid;
}

SceInt32 simple_event_waitorpoll(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, SceUInt32 wait_pattern, SceUInt32 *result_pattern, SceUInt64 *user_data, SceUInt32 *timeout, bool is_wait) {
    const SimpleEventPtr event = lock_and_find(event_id, kernel.simple_events, kernel.mutex);
    if (!event) {
        // this may also be a timer event
        return timer_waitorpoll(kernel, export_name, thread_id, event_id, wait_pattern, result_pattern, user_data, timeout, is_wait);
    }

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} existing_pattern: {:#b} wait_pattern: {:#b} timeout: {}"
                  " waiting_threads: {}",
            export_name, event->uid, thread_id, event->name, event->attr, event->pattern, wait_pattern, timeout ? *timeout : 0,
            event->waiting_threads->size());
    }

    const ThreadStatePtr thread = kernel.get_thread(thread_id);

    std::unique_lock<std::mutex> event_lock(event->mutex);

    if (result_pattern)
        *result_pattern = event->pattern;

    if (event->pattern & wait_pattern) {
        if (event->auto_reset)
            // all common bits are zeroed
            event->pattern &= ~wait_pattern;

        if (user_data)
            *user_data = event->last_user_data;

        return SCE_KERNEL_OK;
    } else if (is_wait) {
        std::unique_lock<std::mutex> thread_lock(thread->mutex);
        thread->update_status(ThreadStatus::wait, ThreadStatus::run);

        WaitingThreadData data;
        data.thread = thread;
        data.result_pattern = result_pattern;
        data.user_data = user_data;
        data.pattern = wait_pattern;
        data.priority = thread->priority;

        const auto data_it = event->waiting_threads->push(data);
        thread_lock.unlock();

        const int err = handle_timeout(kernel, thread, thread_lock, event_lock, event->waiting_threads, data_it, export_name, timeout);
        if (err < 0) {
            // set it only if a timeout occurs
            // otherwise set in simple_event_setorpulse
            if (user_data)
                *user_data = event->last_user_data;
            if (result_pattern)
                *result_pattern = event->pattern;
        }
        return err;
    } else {
        return SCE_KERNEL_ERROR_EVENT_COND;
    }
}

SceInt32 simple_event_setorpulse(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, SceUInt32 pattern, SceUInt64 user_data, bool is_set) {
    const SimpleEventPtr event = lock_and_find(event_id, kernel.simple_events, kernel.mutex);
    if (!event) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_EVENT_ID);
    }

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} existing_pattern: {:#b} set_pattern: {:#b}"
                  " waiting_threads: {}",
            export_name, event->uid, thread_id, event->name, event->attr, event->pattern, pattern,
            event->waiting_threads->size());
    }

    const SceUInt32 old_pattern = event->pattern;
    const SceUInt64 old_user_data = event->last_user_data;
    const SceUInt32 new_pattern = event->pattern | pattern;

    const std::lock_guard<std::mutex> event_lock(event->mutex);
    event->pattern = new_pattern;
    event->last_user_data = user_data;

    for (auto it = event->waiting_threads->begin(); it != event->waiting_threads->end();) {
        const auto waiting_thread_data = *it;
        const auto waiting_thread = waiting_thread_data.thread;
        const auto waiting_pattern = waiting_thread_data.pattern;

        if (event->pattern & waiting_pattern) {
            if (waiting_thread_data.result_pattern)
                *waiting_thread_data.result_pattern = new_pattern;

            if (waiting_thread_data.user_data)
                *waiting_thread_data.user_data = event->last_user_data;

            if (event->auto_reset)
                // all common bit are zeroed
                event->pattern &= ~waiting_pattern;

            const std::lock_guard<std::mutex> waiting_thread_lock(waiting_thread->mutex);

            waiting_thread->update_status(ThreadStatus::run, ThreadStatus::wait);

            event->waiting_threads->erase(it++);
        } else {
            ++it;
        }
    }

    if (!is_set) {
        event->pattern = old_pattern;
        event->last_user_data = old_user_data;
    }

    return SCE_KERNEL_OK;
}

SceInt32 simple_event_clear(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, SceUInt32 clear_pattern) {
    const SimpleEventPtr event = lock_and_find(event_id, kernel.simple_events, kernel.mutex);
    if (!event) {
        // this may also be a timer event
        return timer_clear(kernel, export_name, thread_id, event_id, clear_pattern);
    }

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} clear_pattern: {:#b}",
            export_name, event_id, thread_id, clear_pattern);
    }

    const std::lock_guard<std::mutex> event_lock(event->mutex);

    event->pattern &= clear_pattern;

    return SCE_KERNEL_OK;
}

SceInt32 simple_event_delete(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id) {
    const SimpleEventPtr event = lock_and_find(event_id, kernel.simple_events, kernel.mutex);
    if (!event) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_EVENT_ID);
    }

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} existing_pattern: {:#b} waiting_threads: {}",
            export_name, event->uid, thread_id, event->name, event->attr, event->pattern, event->waiting_threads->size());
    }

    if (event->waiting_threads->empty()) {
        const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);
        kernel.eventflags.erase(event_id);
    } else {
        // TODO:
        LOG_WARN("Can't delete sync object, it has waiting threads.");
    }

    return SCE_KERNEL_OK;
}

// *********
// * Timer *
// *********

inline uint64_t get_current_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch())
        .count();
}

SceUID timer_create(KernelState &kernel, MemState &mem, const char *export_name, const char *name, SceUID thread_id, SceUInt32 attr) {
    if ((strlen(name) > 31) && ((attr & 0x80) == 0x80)) {
        return RET_ERROR(SCE_KERNEL_ERROR_UID_NAME_TOO_LONG);
    }

    const SceUID uid = kernel.get_next_uid();

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {}",
            export_name, uid, thread_id, name, attr);
    }

    const TimerPtr timer = std::make_shared<Timer>();
    timer->uid = uid;
    timer->next_event = std::numeric_limits<uint64_t>::max();
    strncpy(timer->name, name, KERNELOBJECT_MAX_NAME_LENGTH);
    timer->attr = attr;
    if (attr & SCE_KERNEL_ATTR_TH_PRIO) {
        timer->waiting_threads = std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();
    } else {
        timer->waiting_threads = std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
    }

    const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);
    kernel.timers.emplace(uid, timer);

    return uid;
}

SceUID timer_find(KernelState &kernel, const char *export_name, const char *pName) {
    if (strlen(pName) > KERNELOBJECT_MAX_NAME_LENGTH)
        return RET_ERROR(SCE_KERNEL_ERROR_UID_NAME_TOO_LONG);

    if (LOG_SYNC_PRIMITIVES)
        LOG_DEBUG("{}: name: \"{}\"", export_name, pName);

    const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);

    const auto it = std::find_if(kernel.timers.begin(), kernel.timers.end(), [=](const auto &timer) {
        return strncmp(timer.second->name, pName, KERNELOBJECT_MAX_NAME_LENGTH) == 0;
    });

    if (it != kernel.timers.end())
        return it->first;

    return RET_ERROR(SCE_KERNEL_ERROR_UID_CANNOT_FIND_BY_NAME);
}

static void timer_schedule_event(TimerPtr &timer) {
    uint64_t curr_time = get_current_time();
    timer->next_event = curr_time + timer->event_interval;

    timer->condvar.notify_all();
}

SceInt32 timer_set(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID timer_handle, SceUID type, SceKernelSysClock *interval, SceInt32 repeats) {
    TimerPtr timer = lock_and_find(timer_handle, kernel.timers, kernel.mutex);
    if (!timer)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_TIMER_ID);

    if (!interval)
        return RET_ERROR(SCE_KERNEL_ERROR_INVALID_ARGUMENT);

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} type: {} interval: {} repeats: {}"
                  " waiting_threads: {}",
            export_name, timer->uid, thread_id, timer->name, timer->attr, type, *interval,
            repeats, timer->waiting_threads->size());
    }

    const std::lock_guard<std::mutex> timer_lock(timer->mutex);
    timer->is_pulse = type != 0;
    timer->is_repeat = repeats != 0;
    timer->event_interval = *interval;

    if (timer->is_started)
        timer_schedule_event(timer);

    return SCE_KERNEL_OK;
}

// this function is actually only called by simple_event_waitorpoll
// as the only way to wait for a timer is using the event function (a timer is an event)
SceInt32 timer_waitorpoll(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, SceUInt32 bit_pattern, SceUInt32 *result_pattern, SceUInt64 *user_data, SceUInt32 *timeout, bool is_wait) {
    TimerPtr timer = lock_and_find(event_id, kernel.timers, kernel.mutex);
    if (!timer) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_EVENT_ID);
    }

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} timeout: {}"
                  " waiting_threads: {}",
            export_name, timer->uid, thread_id, timer->name, timer->attr, timeout ? *timeout : 0,
            timer->waiting_threads->size());
    }

    if (timeout)
        LOG_WARN_ONCE("Ignoring timeout");

    const ThreadStatePtr thread = kernel.get_thread(thread_id);

    std::unique_lock<std::mutex> lock(timer->mutex);

    if (result_pattern)
        *result_pattern = SCE_KERNEL_EVENT_TIMER;
    if (user_data)
        *user_data = 0;

    uint64_t current_time = get_current_time();
    auto set_next_event = [&]() {
        if (timer->is_repeat) {
            // the event repeats every timer->event_interval, go to the next one after current_time
            timer->next_event += ((current_time - timer->next_event - 1) / timer->event_interval + 1) * timer->event_interval;
        } else {
            timer->next_event = std::numeric_limits<uint64_t>::max();
        }
    };

    if (timer->next_event < current_time) {
        if (!timer->is_pulse) {
            // we can reach pulse event only by waiting
            timer->event_set = true;
        }

        set_next_event();
    }

    if (timer->event_set) {
        if (timer->attr & SCE_KERNEL_EVENT_ATTR_AUTO_RESET) {
            timer->event_set = false;
        }

        return SCE_KERNEL_OK;
    } else if (is_wait) {
        if (kernel.execution_host)
            return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
        thread->update_status(ThreadStatus::wait, ThreadStatus::run);

        WaitingThreadData data;
        data.thread = thread;
        data.result_pattern = result_pattern;
        data.user_data = user_data;
        data.priority = thread->priority;

        const auto data_it = timer->waiting_threads->push(data);

        bool got_event = false;
        while (!got_event) {
            uint64_t wait_time = timer->next_event - current_time;
            // wait before we got an event and we are the first thread in the waiting list
            timer->condvar.wait_for(lock, std::chrono::microseconds(wait_time), [&] {
                return thread->status == ThreadStatus::run
                    || (*timer->waiting_threads->begin()).thread->id == thread_id;
            });
            if (thread->status == ThreadStatus::run) {
                timer->waiting_threads->erase(data_it);
                timer->condvar.notify_all();
                return SCE_KERNEL_ERROR_WAIT_CANCEL;
            }
            current_time = get_current_time();
            got_event = timer->event_set || current_time > timer->next_event;
        }

        timer->waiting_threads->pop();
        thread->update_status(ThreadStatus::run, ThreadStatus::wait);

        timer->event_set = !timer->is_pulse && !(timer->attr & SCE_KERNEL_EVENT_ATTR_AUTO_RESET);
        set_next_event();
        // notify the other waiting threads
        timer->condvar.notify_all();

        return SCE_KERNEL_OK;
    } else {
        return SCE_KERNEL_ERROR_EVENT_COND;
    }
}

// this function is actually only called by simple_event_clear
SceInt32 timer_clear(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, SceUInt32 clear_pattern) {
    TimerPtr timer = lock_and_find(event_id, kernel.timers, kernel.mutex);
    if (!timer) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_EVENT_ID);
    }

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {}",
            export_name, event_id, thread_id);
    }

    std::lock_guard<std::mutex> guard(timer->mutex);
    timer->event_set = false;
    return SCE_KERNEL_OK;
}

SceInt32 timer_start(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID timer_handle) {
    TimerPtr timer = lock_and_find(timer_handle, kernel.timers, kernel.mutex);
    if (!timer)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_TIMER_ID);

    if (timer->is_started)
        return 1;

    const std::lock_guard<std::mutex> guard(timer->mutex);
    timer->is_started = true;
    timer->time = get_current_time();

    if (timer->event_interval != 0)
        timer_schedule_event(timer);

    return SCE_KERNEL_OK;
}

SceInt32 timer_stop(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID timer_handle) {
    const TimerPtr timer = lock_and_find(timer_handle, kernel.timers, kernel.mutex);
    if (!timer)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_TIMER_ID);

    const std::lock_guard<std::mutex> timer_lock(timer->mutex);
    bool was_stopped = !timer->is_started;
    timer->is_started = false;
    timer->time = get_current_time();
    timer->next_event = std::numeric_limits<uint64_t>::max();

    return static_cast<int>(was_stopped);
}

// *********
// * Mutex *
// *********

namespace {
// Keep the guest workarea words used by the generated probe an explicit ABI.
static_assert(sizeof(SceKernelLwMutexWork) == 32);
static_assert(offsetof(SceKernelLwMutexWork, owner) == 0);
static_assert(offsetof(SceKernelLwMutexWork, lockCount) == 8);
static_assert(offsetof(SceKernelLwMutexWork, attr) == 12);
static_assert(offsetof(SceKernelLwMutexWork, uid) == 16);
static_assert(SCE_KERNEL_MUTEX_ATTR_RECURSIVE == vita3k::wasmjit::kInlineMutexRecursive);

// These helpers are only used between generated phases, on the cooperative
// host's single OS thread. Never overwrite dirty state to "recover": doing so
// would let HLE/scheduling observe stale kernel owners after an inline update.
[[noreturn]] void mutex_inline_corruption(const char *reason) noexcept {
    std::fprintf(stderr, "[inline-mutex] invariant failure: %s\n", reason);
    std::abort();
}

vita3k::wasmjit::InlineMutexEntry *find_inline_mutex_entry(KernelState &kernel, const Mutex &mutex) noexcept {
    if (!kernel.execution_host || !kernel.inline_mutex_table || !mutex.workarea)
        return nullptr;
    auto &table = *kernel.inline_mutex_table;
    if (table.dirty_head)
        mutex_inline_corruption("HLE entered before committing generated updates");
    auto &entry = table.entries[vita3k::wasmjit::inline_mutex_index(mutex.workarea.address())];
    // A suspended old guard may outlive deletion and reuse of this slot. A
    // refresh must neither resurrect that lifetime nor overwrite a collision.
    if (entry.uid != static_cast<uint32_t>(mutex.uid) || entry.workarea != mutex.workarea.address())
        return nullptr;
    if (entry.dirty || entry.next_dirty)
        mutex_inline_corruption("dirty slot at an HLE boundary");
    return &entry;
}

void refresh_inline_mutex(KernelState &kernel, const Mutex &mutex) noexcept {
    auto *entry = find_inline_mutex_entry(kernel, mutex);
    if (!entry)
        return;
    entry->enabled = 0;
    entry->owner = mutex.owner ? static_cast<uint32_t>(mutex.owner->id) : vita3k::wasmjit::kInlineMutexNoOwner;
    entry->count = static_cast<uint32_t>(mutex.lock_count);
    entry->attr = mutex.attr;
    // Preserve unusual HLE counts/owners exactly, but never accelerate them.
    entry->enabled = mutex.inline_access_depth == 0 && mutex.waiting_threads
        && mutex.waiting_threads->empty() && mutex.lock_count >= 0
        && ((mutex.lock_count == 0) == !mutex.owner)
        && (!mutex.owner || mutex.owner->id > 0);
}

void register_inline_mutex(KernelState &kernel, const Mutex &mutex) noexcept {
    if (!kernel.execution_host || !kernel.inline_mutex_table || !mutex.workarea || mutex.uid <= 0)
        return;
    auto &table = *kernel.inline_mutex_table;
    if (table.dirty_head)
        mutex_inline_corruption("mutex created before committing generated updates");
    auto &entry = table.entries[vita3k::wasmjit::inline_mutex_index(mutex.workarea.address())];
    if (entry.uid != 0)
        return; // Never evict a live slot, even if its inline access is disabled.
    if (entry.dirty || entry.next_dirty)
        mutex_inline_corruption("dirty slot registered as a new mutex");
    entry = {};
    entry.workarea = mutex.workarea.address();
    entry.uid = static_cast<uint32_t>(mutex.uid);
    refresh_inline_mutex(kernel, mutex);
}

void clear_inline_mutex(KernelState &kernel, const Mutex &mutex) noexcept {
    if (auto *entry = find_inline_mutex_entry(kernel, mutex))
        *entry = {}; // Clear both lifetime tags and enabled before map erasure.
}

class InlineMutexAccessGuard {
    KernelState &kernel;
    Mutex &mutex;
    const bool active;

public:
    // The caller holds mutex.mutex on entry and again at destruction. Only
    // handle_cooperative_wait releases it meanwhile; this guard stays on the
    // parked fiber, so a second HLE operation cannot prematurely re-enable it.
    InlineMutexAccessGuard(KernelState &kernel, Mutex &mutex) noexcept
        : kernel(kernel)
        , mutex(mutex)
        , active(kernel.execution_host && kernel.inline_mutex_table) {
        if (!active)
            return; // Desktop does not even change the access depth.
        if (mutex.inline_access_depth == std::numeric_limits<unsigned>::max())
            mutex_inline_corruption("HLE access depth overflow");
        ++mutex.inline_access_depth;
        if (auto *entry = find_inline_mutex_entry(kernel, mutex))
            entry->enabled = 0;
    }

    ~InlineMutexAccessGuard() noexcept {
        if (!active)
            return;
        if (mutex.inline_access_depth == 0)
            mutex_inline_corruption("unbalanced HLE access depth");
        --mutex.inline_access_depth;
        refresh_inline_mutex(kernel, mutex);
    }

    InlineMutexAccessGuard(const InlineMutexAccessGuard &) = delete;
    InlineMutexAccessGuard &operator=(const InlineMutexAccessGuard &) = delete;
};
} // namespace

void mutex_inline_commit(KernelState &kernel, const ThreadStatePtr &running_thread) noexcept {
    auto *table = kernel.inline_mutex_table.get();
    if (!table || !table->dirty_head)
        return;
    if (!kernel.execution_host || !running_thread || running_thread->id <= 0)
        mutex_inline_corruption("generated updates without a running cooperative thread");

    // No allocation, locks, callbacks or stack switches: no observer may run
    // until ALL dirty slots are committed. Work is proportional to distinct
    // changed slots, not inline calls or table capacity. Clearing dirty also
    // detects duplicate/cyclic links without a table-sized visited scan.
    uint32_t remaining = vita3k::wasmjit::kInlineMutexEntries;
    while (table->dirty_head) {
        const uint32_t link = table->dirty_head;
        if (remaining == 0 || link > vita3k::wasmjit::kInlineMutexEntries)
            mutex_inline_corruption("invalid or cyclic dirty list");
        --remaining;
        auto &entry = table->entries[link - 1];
        if (entry.dirty != 1 || entry.enabled != 1 || entry.uid == 0
            || entry.uid > static_cast<uint32_t>(std::numeric_limits<SceUID>::max())
            || !entry.workarea || vita3k::wasmjit::inline_mutex_index(entry.workarea) != link - 1
            || entry.next_dirty > vita3k::wasmjit::kInlineMutexEntries)
            mutex_inline_corruption("invalid dirty slot metadata");

        const auto found = kernel.lwmutexes.find(static_cast<SceUID>(entry.uid));
        if (found == kernel.lwmutexes.end() || !found->second)
            mutex_inline_corruption("dirty slot refers to a deleted mutex");
        auto &mutex = *found->second;
        if (static_cast<uint32_t>(mutex.uid) != entry.uid || mutex.workarea.address() != entry.workarea
            || mutex.attr != entry.attr || mutex.inline_access_depth != 0
            || !mutex.waiting_threads || !mutex.waiting_threads->empty())
            mutex_inline_corruption("dirty slot lifetime or HLE state mismatch");

        const bool free = entry.owner == vita3k::wasmjit::kInlineMutexNoOwner;
        if ((!free && entry.owner != static_cast<uint32_t>(running_thread->id))
            || (entry.count == 0) != free
            || entry.count > static_cast<uint32_t>(std::numeric_limits<int>::max())
            || mutex.lock_count < 0 || ((mutex.lock_count == 0) != !mutex.owner)
            || (mutex.owner && mutex.owner != running_thread))
            mutex_inline_corruption("invalid inline owner or count");

        mutex.lock_count = static_cast<int>(entry.count);
        mutex.owner = free ? ThreadStatePtr{} : running_thread;
        table->dirty_head = entry.next_dirty;
        entry.dirty = 0;
        entry.next_dirty = 0;
    }
}

SceUID mutex_create(SceUID *uid_out, KernelState &kernel, MemState &mem, const char *export_name, const char *mutex_name, SceUID thread_id, SceUInt attr, int init_count, Ptr<SceKernelLwMutexWork> workarea, SyncWeight weight) {
    if ((strlen(mutex_name) > 31) && ((attr & 0x80) == 0x80)) {
        return RET_ERROR(SCE_KERNEL_ERROR_UID_NAME_TOO_LONG);
    }
    if (init_count < 0) {
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_COUNT);
    }
    if (init_count > 1 && (attr & SCE_KERNEL_MUTEX_ATTR_RECURSIVE)) {
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_COUNT);
    }

    const MutexPtr mutex = std::make_shared<Mutex>();
    const SceUID uid = kernel.get_next_uid();
    mutex->uid = uid;
    mutex->init_count = init_count;
    mutex->lock_count = init_count;
    mutex->workarea = workarea;
    strncpy(mutex->name, mutex_name, KERNELOBJECT_MAX_NAME_LENGTH);
    mutex->attr = attr;
    mutex->owner = nullptr;
    if (init_count > 0) {
        const ThreadStatePtr thread = kernel.get_thread(thread_id);
        mutex->owner = thread;
    }
    if (mutex->attr & SCE_KERNEL_ATTR_TH_PRIO) {
        mutex->waiting_threads = std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();
    } else {
        mutex->waiting_threads = std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
    }

    if (weight == SyncWeight::Light) {
        SceKernelLwMutexWork *workarea_mem = workarea.get(mem);
        workarea_mem->lockCount = init_count;
        if (workarea_mem->lockCount)
            workarea_mem->owner = thread_id;
        else if (kernel.execution_host)
            workarea_mem->owner = static_cast<uint32_t>(-1);
        workarea_mem->attr = attr;
    }

    const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);
    auto &mutexes = get_mutexes(kernel, weight);
    mutexes.emplace(uid, mutex);

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} init_count: {}",
            export_name, uid, thread_id, mutex_name, attr, init_count);
    }

    if (uid_out) {
        *uid_out = uid;
    }

    if (weight == SyncWeight::Light && kernel.execution_host && kernel.inline_mutex_table) {
        // The module normally aliases uid_out to workarea.uid. Also initialize
        // it for direct kernel callers before publishing the slot's lifetime.
        workarea.get(mem)->uid = uid;
        register_inline_mutex(kernel, *mutex);
    }

    return SCE_KERNEL_OK;
}

SceUID mutex_find(KernelState &kernel, const char *export_name, const char *pName) {
    if (strlen(pName) > KERNELOBJECT_MAX_NAME_LENGTH)
        return RET_ERROR(SCE_KERNEL_ERROR_UID_NAME_TOO_LONG);

    if (LOG_SYNC_PRIMITIVES)
        LOG_DEBUG("{}: name: \"{}\"", export_name, pName);

    const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);

    const auto it = std::find_if(kernel.mutexes.begin(), kernel.mutexes.end(), [=](const auto &mutex) {
        return strncmp(mutex.second->name, pName, KERNELOBJECT_MAX_NAME_LENGTH) == 0;
    });

    if (it != kernel.mutexes.end())
        return it->first;

    return RET_ERROR(SCE_KERNEL_ERROR_UID_CANNOT_FIND_BY_NAME);
}

inline static void mutex_release_locked(KernelState &kernel, Mutex &mutex, int unlock_count);

inline static int mutex_lock_impl(KernelState &kernel, MemState &mem, const char *export_name, SceUID thread_id, int lock_count, MutexPtr &mutex, SyncWeight weight, SceUInt *timeout, bool only_try) {
    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} lock_count: {} timeout: {} waiting_threads: {}",
            export_name, mutex->uid, thread_id, mutex->name, mutex->attr, mutex->lock_count, timeout ? *timeout : 0,
            mutex->waiting_threads->size());
    }

    const ThreadStatePtr thread = kernel.get_thread(thread_id);

    std::unique_lock<std::mutex> mutex_lock(mutex->mutex);
    const InlineMutexAccessGuard inline_access(kernel, *mutex);

    bool is_recursive = (mutex->attr & SCE_KERNEL_MUTEX_ATTR_RECURSIVE);

    // Uncontended fast path: the mutex is free, so take ownership inline.
    // No wait-queue entry, no thread-status transition and — on the
    // cooperative (browser) runtime — no fiber park/wake round-trip through
    // the scheduler. The workarea mirror update is plain guest-memory writes.
    // A free mutex has no waiters at an HLE boundary (mutex_unlock always
    // transfers ownership directly to the next waiter instead of freeing),
    // so taking it here cannot disturb wake order.
    if (mutex->lock_count == 0) {
        mutex->lock_count += lock_count;
        mutex->owner = thread;

        if (weight == SyncWeight::Light) {
            mutex->workarea.get(mem)->lockCount = mutex->lock_count;
            if (mutex->owner == thread) {
                mutex->workarea.get(mem)->owner = thread_id;
            }
        }

        return SCE_KERNEL_OK;
    }

    // Owned by ourselves: recursive take (or a recursion error on a
    // non-recursive mutex). Also completes inline, same as above.
    if (mutex->owner == thread) {
        if (is_recursive) {
            mutex->lock_count += lock_count;
            if (weight == SyncWeight::Light)
                mutex->workarea.get(mem)->lockCount += lock_count;

            return SCE_KERNEL_OK;
        }
        if (weight == SyncWeight::Light)
            return RET_ERROR(SCE_KERNEL_ERROR_LW_MUTEX_RECURSIVE);

        return RET_ERROR(SCE_KERNEL_ERROR_MUTEX_RECURSIVE);
    }

    // Contended slow path: held by another thread. Semantics unchanged:
    // only_try fails without sleeping, otherwise the thread is queued
    // (FIFO/priority order) and parked until mutex_unlock hands ownership
    // to it directly. Wake order is owned entirely by this queue.
    // Don't sleep if only_try is set
    if (only_try) {
        if (weight == SyncWeight::Light)
            return RET_ERROR(SCE_KERNEL_ERROR_LW_MUTEX_FAILED_TO_OWN);

        return RET_ERROR(SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN);
    }

    // Sleep thread!
    std::unique_lock<std::mutex> thread_lock(thread->mutex);
    thread->update_status(ThreadStatus::wait, ThreadStatus::run);

    WaitingThreadData data;
    data.thread = thread;
    data.lock_count = lock_count;
    data.priority = thread->priority;
    bool was_canceled = false;
    data.was_canceled = &was_canceled;

    const auto data_it = mutex->waiting_threads->push(data);
    thread_lock.unlock();

    auto wait_result = KernelExecutionHost::WaitResult::ready;
    int res = kernel.execution_host
        ? handle_cooperative_wait(kernel, thread, thread_lock, mutex_lock, mutex->waiting_threads, timeout, &wait_result)
        : handle_timeout(kernel, thread, thread_lock, mutex_lock, mutex->waiting_threads, data_it, export_name, timeout);
    if (kernel.execution_host && was_canceled)
        res = SCE_KERNEL_ERROR_WAIT_CANCEL;
    // Unlock may hand ownership to this waiter after it was deleted but
    // before it resumed; a dying thread must not keep the mutex.
    if (kernel.execution_host && wait_result == KernelExecutionHost::WaitResult::cancelled) {
        if (mutex->owner == thread)
            mutex_release_locked(kernel, *mutex, lock_count);
        res = SCE_KERNEL_ERROR_WAIT_CANCEL;
    }

    if (weight == SyncWeight::Light) {
        auto *work = mutex->workarea.get(mem);
        work->lockCount = mutex->lock_count;
        if (mutex->owner == thread)
            work->owner = thread_id;
        else if (kernel.execution_host) // the release above may have handed it on
            work->owner = mutex->owner ? static_cast<uint32_t>(mutex->owner->id) : static_cast<uint32_t>(-1);
    }

    return res;
}

int mutex_lock(KernelState &kernel, MemState &mem, const char *export_name, SceUID thread_id, SceUID mutexid, int lock_count, unsigned int *timeout, SyncWeight weight) {
    assert(mutexid >= 0);

    MutexPtr mutex;
    if (auto error = find_mutex(mutex, nullptr, kernel, export_name, mutexid, weight))
        return error;

    return mutex_lock_impl(kernel, mem, export_name, thread_id, lock_count, mutex, weight, timeout, false);
}

int mutex_try_lock(KernelState &kernel, MemState &mem, const char *export_name, SceUID thread_id, SceUID mutexid, int lock_count, SyncWeight weight) {
    assert(mutexid >= 0);

    MutexPtr mutex;
    if (auto error = find_mutex(mutex, nullptr, kernel, export_name, mutexid, weight))
        return error;

    return mutex_lock_impl(kernel, mem, export_name, thread_id, lock_count, mutex, weight, nullptr, true);
}

// The caller holds mutex.mutex and an InlineMutexAccessGuard, and has checked
// that the count does not underflow.
inline static void mutex_release_locked(KernelState &kernel, Mutex &mutex, int unlock_count) {
    mutex.lock_count -= unlock_count;

    if (mutex.lock_count == 0) {
        mutex.owner = nullptr;

        while (!mutex.waiting_threads->empty()) {
            const auto waiting_thread_data = *mutex.waiting_threads->begin();
            const auto waiting_thread = waiting_thread_data.thread;
            const auto waiting_lock_count = waiting_thread_data.lock_count;

            const std::lock_guard<std::mutex> waiting_thread_lock(waiting_thread->mutex);
            // Deletion wakes a parked waiter before its HLE continuation
            // unlinks itself. Skip it and keep searching: ownership must
            // never be handed to a cancelled waiter, nor lost behind it.
            if (kernel.execution_host && waiting_thread->status != ThreadStatus::wait) {
                *waiting_thread_data.was_canceled = true;
                mutex.waiting_threads->pop();
                continue;
            }
            if (!kernel.execution_host)
                waiting_thread->update_status(ThreadStatus::run, ThreadStatus::wait);
            mutex.waiting_threads->pop();
            mutex.lock_count += waiting_lock_count;
            mutex.owner = waiting_thread;
            if (kernel.execution_host)
                waiting_thread->update_status(ThreadStatus::run, ThreadStatus::wait);
            break;
        }
    }
}

inline static int mutex_unlock_impl(KernelState &kernel, const char *export_name, SceUID thread_id, int unlock_count, MutexPtr &mutex) {
    const ThreadStatePtr current_thread = kernel.get_thread(thread_id);

    const std::lock_guard<std::mutex> mutex_lock(mutex->mutex);
    const InlineMutexAccessGuard inline_access(kernel, *mutex);

    if (current_thread == mutex->owner) {
        if (unlock_count > mutex->lock_count) {
            return RET_ERROR(SCE_KERNEL_ERROR_LW_MUTEX_UNLOCK_UDF);
        }

        mutex_release_locked(kernel, *mutex, unlock_count);
    }

    return SCE_KERNEL_OK;
}

int mutex_unlock(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID mutexid, int unlock_count, SyncWeight weight) {
    assert(mutexid >= 0);

    MutexPtr mutex;
    if (auto error = find_mutex(mutex, nullptr, kernel, export_name, mutexid, weight))
        return error;

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} lock_count: {} waiting_threads: {}",
            export_name, mutexid, thread_id, mutex->name, mutex->attr, mutex->lock_count, unlock_count,
            mutex->waiting_threads->size());
    }

    const int result = mutex_unlock_impl(kernel, export_name, thread_id, unlock_count, mutex);
    if (kernel.execution_host && weight == SyncWeight::Light) {
        // The unlock API has no MemState argument. The calling CPU belongs to
        // this runtime's memory. No swap occurs during unlock or notification.
        const auto thread = kernel.get_thread(thread_id);
        if (thread) {
            const std::lock_guard<std::mutex> lock(mutex->mutex);
            auto *work = mutex->workarea.get(*thread->cpu->mem);
            work->lockCount = mutex->lock_count;
            work->owner = mutex->owner ? static_cast<uint32_t>(mutex->owner->id) : static_cast<uint32_t>(-1);
        }
    }
    return result;
}

int mutex_delete(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID mutexid, SyncWeight weight) {
    assert(mutexid >= 0);

    MutexPtr mutex;
    MutexPtrs *mutexes;
    if (auto error = find_mutex(mutex, &mutexes, kernel, export_name, mutexid, weight))
        return error;

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} lock_count: {} waiting_threads: {}",
            export_name, mutexid, thread_id, mutex->name, mutex->attr, mutex->lock_count,
            mutex->waiting_threads->size());
    }

    // Production deletion does not implement waking live mutex waiters.
    if (kernel.execution_host && !mutex->waiting_threads->empty())
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
    if (mutex->waiting_threads->empty()) {
        const std::lock_guard<std::mutex> kernel_guard(kernel.mutex);
        if (weight == SyncWeight::Light)
            clear_inline_mutex(kernel, *mutex);
        mutexes->erase(mutexid);
    } else {
        // TODO:
        LOG_WARN("Can't delete sync object, it has waiting threads.");
    }

    return SCE_KERNEL_OK;
}

MutexPtr mutex_get(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID mutexid, SyncWeight weight) {
    assert(mutexid >= 0);

    MutexPtr mutex;
    MutexPtrs *mutexes;
    if (auto error = find_mutex(mutex, &mutexes, kernel, export_name, mutexid, weight))
        return nullptr;

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} lock_count: {} waiting_threads: {}",
            export_name, mutexid, thread_id, mutex->name, mutex->attr, mutex->lock_count,
            mutex->waiting_threads->size());
    }
    return mutex;
}

// **************
// * RWLock *
// **************

SceUID rwlock_create(KernelState &kernel, MemState &mem, const char *export_name, const char *name, SceUID thread_id, SceUInt32 attr) {
    if ((strlen(name) > 31) && ((attr & 0x80) == 0x80)) {
        return RET_ERROR(SCE_KERNEL_ERROR_UID_NAME_TOO_LONG);
    }

    const RWLockPtr rwlock = std::make_shared<RWLock>();
    const SceUID uid = kernel.get_next_uid();
    rwlock->uid = uid;
    strncpy(rwlock->name, name, KERNELOBJECT_MAX_NAME_LENGTH);
    rwlock->attr = attr;

    if (rwlock->attr & SCE_KERNEL_ATTR_TH_PRIO) {
        rwlock->waiting_threads = std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();
    } else {
        rwlock->waiting_threads = std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
    }

    const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);
    kernel.rwlocks.emplace(uid, rwlock);

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {}",
            export_name, uid, thread_id, name, attr);
    }

    return uid;
}

SceInt32 rwlock_lock(KernelState &kernel, MemState &mem, const char *export_name, SceUID thread_id, SceUID lock_id, uint32_t *timeout, bool is_write) {
    const ThreadStatePtr thread = kernel.get_thread(thread_id);
    const RWLockPtr rwlock = lock_and_find(lock_id, kernel.rwlocks, kernel.mutex);

    if (!rwlock)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID);

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} timeout: {} waiting_threads: {}",
            export_name, lock_id, thread_id, rwlock->name, rwlock->attr, timeout ? *timeout : 0,
            rwlock->waiting_threads->size());
    }

    std::unique_lock<std::mutex> rwlock_lock(rwlock->mutex);

    // if it is a read lock, it is always recursive
    bool is_recursive = !is_write || (rwlock->attr & SCE_KERNEL_MUTEX_ATTR_RECURSIVE);

    // cases where we don't need to wait :
    if (rwlock->state == RWLockState::Unlocked // the lock is unlocked
        || (!is_write && rwlock->state == RWLockState::ReadLocked) // we want a read lock when the lock is readlocked
        || (is_recursive && rwlock->owners.contains(thread))) { // the thread asking has already locked this lock

        auto it = rwlock->owners.find(thread);
        if (it != rwlock->owners.end()) {
            // increase the count
            it->second++;
        } else {
            rwlock->owners.emplace(thread, 1);
        }

        rwlock->state = is_write ? RWLockState::WriteLocked : RWLockState::ReadLocked;

        return SCE_KERNEL_OK;
    } else if (!is_recursive && rwlock->owners.contains(thread)) {
        return RET_ERROR(SCE_KERNEL_ERROR_RW_LOCK_RECURSIVE);
    } else {
        // we need to wait

        std::unique_lock<std::mutex> thread_lock(thread->mutex);
        thread->update_status(ThreadStatus::wait, ThreadStatus::run);

        WaitingThreadData data;
        data.thread = thread;
        data.is_write = is_write;
        data.priority = thread->priority;

        const auto data_it = rwlock->waiting_threads->push(data);
        thread_lock.unlock();

        return handle_timeout(kernel, thread, thread_lock, rwlock_lock, rwlock->waiting_threads, data_it, export_name, timeout);
    }
}

SceInt32 rwlock_unlock(KernelState &kernel, MemState &mem, const char *export_name, SceUID thread_id, SceUID lock_id, bool is_write) {
    const ThreadStatePtr current_thread = kernel.get_thread(thread_id);
    const RWLockPtr rwlock = lock_and_find(lock_id, kernel.rwlocks, kernel.mutex);

    if (!rwlock)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID);

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} waiting_threads: {}",
            export_name, lock_id, thread_id, rwlock->name, rwlock->attr,
            rwlock->waiting_threads->size());
    }

    const std::lock_guard<std::mutex> rwlock_lock(rwlock->mutex);

    auto it = rwlock->owners.find(current_thread);
    if (it == rwlock->owners.end()) {
        return RET_ERROR(SCE_KERNEL_ERROR_RW_LOCK_FAILED_TO_UNLOCK);
    }

    // decrease the lock count
    it->second--;
    if (it->second == 0)
        rwlock->owners.erase(current_thread);

    // if it is still locked
    if (!rwlock->owners.empty())
        return SCE_KERNEL_OK;

    rwlock->state = RWLockState::Unlocked;

    if (!rwlock->waiting_threads->empty()) {
        for (auto it = rwlock->waiting_threads->begin(); it != rwlock->waiting_threads->end();) {
            const auto &waiting_thread_data = *it;
            const auto waiting_thread = waiting_thread_data.thread;
            const auto waiting_is_write = waiting_thread_data.is_write;

            if (rwlock->state == RWLockState::ReadLocked && waiting_is_write) {
                // only awaken read threads
                ++it;
                continue;
            }

            auto old_it = it;
            ++it;
            rwlock->waiting_threads->erase(old_it);

            const std::lock_guard<std::mutex> waiting_thread_lock(waiting_thread->mutex);
            waiting_thread->update_status(ThreadStatus::run, ThreadStatus::wait);
            rwlock->owners.emplace(waiting_thread, 1);

            if (waiting_is_write) {
                rwlock->state = RWLockState::WriteLocked;
                break;
            }
            rwlock->state = RWLockState::ReadLocked;
        }
    }

    return SCE_KERNEL_OK;
}

SceInt32 rwlock_delete(KernelState &kernel, MemState &mem, const char *export_name, SceUID thread_id, SceUID lock_id) {
    const ThreadStatePtr thread = kernel.get_thread(thread_id);
    const RWLockPtr rwlock = lock_and_find(lock_id, kernel.rwlocks, kernel.mutex);

    if (!rwlock)
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID);

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} waiting_threads: {}",
            export_name, lock_id, thread_id, rwlock->name, rwlock->attr,
            rwlock->waiting_threads->size());
    }

    if (rwlock->waiting_threads->empty()) {
        const std::lock_guard<std::mutex> kernel_guard(kernel.mutex);
        kernel.rwlocks.erase(lock_id);
    } else {
        // TODO:
        LOG_WARN("Can't delete sync object, it has waiting threads.");
    }

    return SCE_KERNEL_OK;
}

// **************
// * Semaphore *
// **************

SceUID semaphore_create(KernelState &kernel, const char *export_name, const char *name, SceUID thread_id, SceUInt attr, int init_val, int max_val) {
    if ((strlen(name) > 31) && ((attr & 0x80) == 0x80)) {
        return RET_ERROR(SCE_KERNEL_ERROR_UID_NAME_TOO_LONG);
    }

    const SemaphorePtr semaphore = std::make_shared<Semaphore>();
    const SceUID uid = kernel.get_next_uid();
    semaphore->uid = uid;
    semaphore->init_val = init_val;
    semaphore->val = init_val;
    semaphore->max = max_val;
    semaphore->attr = attr;
    strncpy(semaphore->name, name, KERNELOBJECT_MAX_NAME_LENGTH);

    if (semaphore->attr & SCE_KERNEL_ATTR_TH_PRIO) {
        semaphore->waiting_threads = std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();
    } else {
        semaphore->waiting_threads = std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
    }

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} init_val: {} max_val: {}",
            export_name, uid, thread_id, name, attr, init_val, max_val);
    }

    const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);
    kernel.semaphores.emplace(uid, semaphore);

    return uid;
}

SceUID semaphore_find(KernelState &kernel, const char *export_name, const char *pName) {
    if (strlen(pName) > KERNELOBJECT_MAX_NAME_LENGTH)
        return RET_ERROR(SCE_KERNEL_ERROR_UID_NAME_TOO_LONG);

    if (LOG_SYNC_PRIMITIVES)
        LOG_DEBUG("{}: name: \"{}\"", export_name, pName);

    const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);

    const auto it = std::find_if(kernel.semaphores.begin(), kernel.semaphores.end(), [=](const auto &sema) {
        return strncmp(sema.second->name, pName, KERNELOBJECT_MAX_NAME_LENGTH) == 0;
    });

    if (it != kernel.semaphores.end())
        return it->first;

    return RET_ERROR(SCE_KERNEL_ERROR_UID_CANNOT_FIND_BY_NAME);
}

SceInt32 semaphore_wait(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID semaId, SceInt32 needCount, SceUInt32 *pTimeout) {
    assert(semaId >= 0);

    // TODO Don't lock twice.
    const SemaphorePtr semaphore = lock_and_find(semaId, kernel.semaphores, kernel.mutex);
    if (!semaphore) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID);
    }

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} val: {} timeout: {} waiting_threads: {}",
            export_name, semaphore->uid, thread_id, semaphore->name, semaphore->attr, semaphore->val,
            pTimeout ? *pTimeout : 0, semaphore->waiting_threads->size());
    }

    const ThreadStatePtr thread = kernel.get_thread(thread_id);

    std::unique_lock<std::mutex> semaphore_lock(semaphore->mutex);

    if (semaphore->val < needCount) {
        std::unique_lock<std::mutex> thread_lock(thread->mutex);
        thread->update_status(ThreadStatus::wait, ThreadStatus::run);

        WaitingThreadData data;
        data.thread = thread;
        data.priority = thread->priority;
        data.signal = needCount;

        bool was_canceled = false;
        data.was_canceled = &was_canceled;

        const auto data_it = semaphore->waiting_threads->push(data);
        thread_lock.unlock();

        int res;
        if (kernel.execution_host) {
            res = handle_cooperative_wait(kernel, thread, thread_lock, semaphore_lock, semaphore->waiting_threads, pTimeout);
        } else {
            res = handle_timeout(kernel, thread, thread_lock, semaphore_lock, semaphore->waiting_threads, data_it, export_name, pTimeout);
        }
        if (was_canceled)
            res = SCE_KERNEL_ERROR_WAIT_CANCEL;
        return res;
    } else {
        semaphore->val -= needCount;
    }

    return SCE_KERNEL_OK;
}

int semaphore_signal(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID semaid, int signal) {
    assert(semaid >= 0);

    // TODO Don't lock twice.
    const SemaphorePtr semaphore = lock_and_find(semaid, kernel.semaphores, kernel.mutex);
    if (!semaphore) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID);
    }

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} val: {} signal: {} waiting_threads: {}",
            export_name, semaphore->uid, thread_id, semaphore->name, semaphore->attr, semaphore->val, signal,
            semaphore->waiting_threads->size());
    }

    const std::lock_guard<std::mutex> semaphore_lock(semaphore->mutex);

    if (semaphore->val + signal > semaphore->max) {
        return RET_ERROR(SCE_KERNEL_ERROR_SEMA_OVF);
    }
    semaphore->val += signal;

    while (!semaphore->waiting_threads->empty()) {
        const auto waiting_thread_data = *semaphore->waiting_threads->begin();
        const auto waiting_thread = waiting_thread_data.thread;
        const auto waiting_signal_count = waiting_thread_data.signal;

        // A deletion can wake a browser waiter before its continuation gets a
        // dispatch to unlink itself. Do not consume a permit or assert wait.
        if (kernel.execution_host && waiting_thread->status != ThreadStatus::wait) {
            if (waiting_thread_data.was_canceled)
                *waiting_thread_data.was_canceled = true;
            semaphore->waiting_threads->pop();
            continue;
        }
        if (semaphore->val < waiting_signal_count)
            break;

        const std::unique_lock<std::mutex> waiting_thread_lock(waiting_thread->mutex);

        waiting_thread->update_status(ThreadStatus::run, ThreadStatus::wait);

        semaphore->waiting_threads->pop();
        semaphore->val -= waiting_signal_count;
    }

    return SCE_KERNEL_OK;
}

int semaphore_delete(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID semaid) {
    assert(semaid >= 0);

    // TODO: Don't lock twice
    const SemaphorePtr semaphore = lock_and_find(semaid, kernel.semaphores, kernel.mutex);
    if (!semaphore) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID);
    }

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} val: {} waiting_threads: {}",
            export_name, semaphore->uid, thread_id, semaphore->name, semaphore->attr, semaphore->val,
            semaphore->waiting_threads->size());
    }

    // Deletion with live waiters is not implemented by the production queue.
    // Do not report fake success on the cooperative path.
    if (kernel.execution_host && !semaphore->waiting_threads->empty())
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
    if (semaphore->waiting_threads->empty()) {
        const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);
        kernel.semaphores.erase(semaid);
    } else {
        // TODO:
        LOG_WARN("Can't delete sync object, it has waiting threads.");
    }

    return SCE_KERNEL_OK;
}

int semaphore_cancel(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID semaid, SceInt32 setCount, SceUInt32 *pNumWaitThreads) {
    assert(semaid >= 0);

    // TODO: Don't lock twice
    const SemaphorePtr semaphore = lock_and_find(semaid, kernel.semaphores, kernel.mutex);
    if (!semaphore) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID);
    }

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} val: {} waiting_threads: {}",
            export_name, semaphore->uid, thread_id, semaphore->name, semaphore->attr, semaphore->val,
            semaphore->waiting_threads->size());
    }

    SceUInt32 nb_threads = 0;
    const std::lock_guard<std::mutex> semaphore_lock(semaphore->mutex);
    if (kernel.execution_host && setCount > semaphore->max)
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_COUNT);
    while (!semaphore->waiting_threads->empty()) {
        const auto &waiting_thread_data = *semaphore->waiting_threads->begin();
        const auto waiting_thread = waiting_thread_data.thread;

        const std::lock_guard<std::mutex> waiting_thread_lock(waiting_thread->mutex);

        if (waiting_thread_data.was_canceled) {
            *waiting_thread_data.was_canceled = true;
        }

        if (kernel.execution_host)
            waiting_thread->update_status(ThreadStatus::run);
        else
            waiting_thread->update_status(ThreadStatus::run, ThreadStatus::wait);

        semaphore->waiting_threads->erase(semaphore->waiting_threads->begin());
        nb_threads++;
    }

    if (!kernel.execution_host && semaphore->val < setCount) {
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    }
    if (setCount < 0) {
        semaphore->val = semaphore->init_val;
    } else {
        semaphore->val = setCount;
    }
    if (pNumWaitThreads)
        *pNumWaitThreads = nb_threads;
    return SCE_KERNEL_OK;
}

// **********************
// * Condition Variable *
// **********************

SceUID condvar_create(SceUID *uid_out, KernelState &kernel, const char *export_name, const char *name, SceUID thread_id, SceUInt attr, SceUID assoc_mutexid, SyncWeight weight) {
    if ((strlen(name) > 31) && ((attr & 0x80) == 0x80)) {
        return RET_ERROR(SCE_KERNEL_ERROR_UID_NAME_TOO_LONG);
    }
    MutexPtr assoc_mutex;
    if (auto error = find_mutex(assoc_mutex, nullptr, kernel, export_name, assoc_mutexid, weight))
        return error;

    const SceUID uid = kernel.get_next_uid();

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} assoc_mutexid: {}",
            export_name, uid, thread_id, name, attr, assoc_mutexid);
    }

    const CondvarPtr condvar = std::make_shared<Condvar>();
    condvar->attr = attr;
    condvar->associated_mutex = std::move(assoc_mutex);
    strncpy(condvar->name, name, KERNELOBJECT_MAX_NAME_LENGTH);

    if (condvar->attr & SCE_KERNEL_ATTR_TH_PRIO) {
        condvar->waiting_threads = std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();
    } else {
        condvar->waiting_threads = std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
    }

    const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);
    auto &condvars = get_condvars(kernel, weight);
    condvars.emplace(uid, condvar);

    if (uid_out)
        *uid_out = uid;
    return SCE_KERNEL_OK;
}

int condvar_wait(KernelState &kernel, MemState &mem, const char *export_name, SceUID thread_id, SceUID condid, SceUInt *timeout, SyncWeight weight) {
    assert(condid >= 0);

    CondvarPtr condvar;
    CondvarPtrs *condvars;
    if (auto error = find_condvar(condvar, &condvars, kernel, export_name, condid, weight))
        return error;

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} name: \"{}\" attr: {} assoc_mutexid: {} timeout: {} waiting_threads: {}",
            export_name, condvar->uid, condvar->name, condvar->attr, condvar->associated_mutex->uid,
            timeout ? *timeout : 0, condvar->waiting_threads->size());
    }

    const ThreadStatePtr thread = kernel.get_thread(thread_id);

    std::unique_lock<std::mutex> condition_variable_lock(condvar->mutex);

    if (auto error = mutex_unlock_impl(kernel, export_name, thread_id, 1, condvar->associated_mutex))
        return error;
    if (kernel.execution_host && weight == SyncWeight::Light) {
        // The inline fast paths compare this mirror with the kernel object;
        // publish the release as the public unlock does.
        auto &mutex = *condvar->associated_mutex;
        const std::lock_guard<std::mutex> mutex_lock(mutex.mutex);
        auto *work = mutex.workarea.get(mem);
        work->lockCount = mutex.lock_count;
        work->owner = mutex.owner ? static_cast<uint32_t>(mutex.owner->id) : static_cast<uint32_t>(-1);
    }

    std::unique_lock<std::mutex> thread_lock(thread->mutex);
    thread->update_status(ThreadStatus::wait, ThreadStatus::run);

    WaitingThreadData data;
    data.thread = thread;
    data.priority = thread->priority;

    const auto data_it = condvar->waiting_threads->push(data);
    thread_lock.unlock();

    if (kernel.execution_host) {
        // Signal unlinks the waiter and sets it running; a waiter still
        // queued timed out or was cancelled. Like the desktop path, neither
        // re-acquires the mutex. A thread deleted after its signal must not
        // take the mutex with it either.
        auto result = KernelExecutionHost::WaitResult::ready;
        if (auto error = handle_cooperative_wait(kernel, thread, thread_lock, condition_variable_lock, condvar->waiting_threads, timeout, &result))
            return error;
        if (result == KernelExecutionHost::WaitResult::cancelled)
            return SCE_KERNEL_ERROR_WAIT_CANCEL;
    } else if (auto error = handle_timeout(kernel, thread, thread_lock, condition_variable_lock, condvar->waiting_threads, data_it, export_name, timeout))
        return error;

    condition_variable_lock.unlock();
    return mutex_lock_impl(kernel, mem, export_name, thread_id, 1, condvar->associated_mutex, weight, timeout, false);
}

int condvar_signal(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID condid, Condvar::SignalTarget signal_target, SyncWeight weight) {
    assert(condid >= 0);

    CondvarPtr condvar;
    CondvarPtrs *condvars;
    if (auto error = find_condvar(condvar, &condvars, kernel, export_name, condid, weight))
        return error;

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} name: \"{}\" attr: {} assoc_mutexid: {} waiting_threads: {}",
            export_name, condvar->uid, condvar->name, condvar->attr, condvar->associated_mutex->uid,
            condvar->waiting_threads->size());
    }

    const auto target_type = signal_target.type;

    const std::lock_guard<std::mutex> condvar_lock(condvar->mutex);
    auto &waiting_threads = condvar->waiting_threads;

    if (target_type == Condvar::SignalTarget::Type::Specific) {
        ThreadStatePtr waiting_thread = lock_and_find(signal_target.thread_id, kernel.threads, kernel.mutex);
        // Search for specified waiting thread
        auto waiting_thread_iter = waiting_threads->find(waiting_thread);
        if (waiting_thread_iter != waiting_threads->end()) {
            const std::lock_guard<std::mutex> waiting_thread_lock(waiting_thread->mutex);

            waiting_thread->update_status(ThreadStatus::run, ThreadStatus::wait);
            waiting_threads->erase(waiting_thread_iter);
        } else {
            LOG_ERROR("{}: Target thread {} not found", export_name, waiting_thread->name);
        }
    } else {
        while (!waiting_threads->empty()) {
            const auto waiting_thread_data = *waiting_threads->begin();
            auto waiting_thread = waiting_thread_data.thread;
            const std::unique_lock<std::mutex> waiting_thread_lock(waiting_thread->mutex, std::try_to_lock);
            if (!waiting_thread_lock)
                continue;

            // Deletion wakes a parked waiter before its HLE continuation
            // unlinks itself; it must not consume the signal.
            if (kernel.execution_host && waiting_thread->status != ThreadStatus::wait) {
                waiting_threads->pop();
                continue;
            }
            waiting_thread->update_status(ThreadStatus::run, ThreadStatus::wait);
            waiting_threads->pop();
            if (target_type == Condvar::SignalTarget::Type::Any)
                break;
        }
    }

    return SCE_KERNEL_OK;
}

int condvar_delete(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID condid, SyncWeight weight) {
    assert(condid >= 0);

    CondvarPtr condvar;
    CondvarPtrs *condvars;
    if (auto error = find_condvar(condvar, &condvars, kernel, export_name, condid, weight))
        return error;

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} assoc_mutexid: {} waiting_threads: {}",
            export_name, condvar->uid, thread_id, condvar->name, condvar->attr, condvar->associated_mutex->uid,
            condvar->waiting_threads->size());
    }

    // Production deletion does not implement waking live condition waiters.
    if (kernel.execution_host && !condvar->waiting_threads->empty())
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
    if (condvar->waiting_threads->empty()) {
        const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);
        condvars->erase(condid);
    } else {
        // TODO:
        LOG_WARN("Can't delete sync object, it has waiting threads.");
    }

    return SCE_KERNEL_OK;
}

// **************
// * Event Flag *
// **************

SceUID eventflag_clear(KernelState &kernel, const char *export_name, SceUID evfId, SceUInt32 bitPattern) {
    const EventFlagPtr event = lock_and_find(evfId, kernel.eventflags, kernel.mutex);
    if (!event) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_EVF_ID);
    }

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} bitPattern: {:#b}",
            export_name, evfId, bitPattern);
    }

    const std::lock_guard<std::mutex> event_lock(event->mutex);

    event->flags &= bitPattern;

    return SCE_KERNEL_OK;
}

SceUID eventflag_create(KernelState &kernel, const char *export_name, SceUID thread_id, const char *pName, SceUInt32 attr, SceUInt32 initPattern) {
    if (((attr & 0x80) == 0x80) && (strlen(pName) > KERNELOBJECT_MAX_NAME_LENGTH)) {
        return RET_ERROR(SCE_KERNEL_ERROR_UID_NAME_TOO_LONG);
    }

    const SceUID uid = kernel.get_next_uid();

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} bitPattern: {:#b}",
            export_name, uid, thread_id, pName, attr, initPattern);
    }

    const EventFlagPtr event = std::make_shared<EventFlag>();
    event->uid = uid;
    event->flags = initPattern;
    strncpy(event->name, pName, KERNELOBJECT_MAX_NAME_LENGTH);
    event->attr = attr;
    if (event->attr & SCE_KERNEL_ATTR_TH_PRIO) {
        event->waiting_threads = std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();
    } else {
        event->waiting_threads = std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
    }

    const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);
    kernel.eventflags.emplace(uid, event);

    return uid;
}

SceUID eventflag_find(KernelState &kernel, const char *export_name, const char *pName) {
    if (strlen(pName) > KERNELOBJECT_MAX_NAME_LENGTH)
        return RET_ERROR(SCE_KERNEL_ERROR_UID_NAME_TOO_LONG);

    if (LOG_SYNC_PRIMITIVES)
        LOG_DEBUG("{}: name: \"{}\"", export_name, pName);

    const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);

    const auto it = std::find_if(kernel.eventflags.begin(), kernel.eventflags.end(), [=](const auto &evf) {
        return strncmp(evf.second->name, pName, KERNELOBJECT_MAX_NAME_LENGTH) == 0;
    });

    if (it != kernel.eventflags.end())
        return it->first;

    return RET_ERROR(SCE_KERNEL_ERROR_UID_CANNOT_FIND_BY_NAME);
}

static int eventflag_waitorpoll(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, unsigned int flags, unsigned int wait, unsigned int *outBits, SceUInt *timeout, bool dowait) {
    assert(event_id >= 0);

    // TODO Don't lock twice.
    const EventFlagPtr event = lock_and_find(event_id, kernel.eventflags, kernel.mutex);
    if (!event) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_EVF_ID);
    }

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} existing_flags: {:#b} wait_flags: {:#b} timeout: {}"
                  " waiting_threads: {}",
            export_name, event->uid, thread_id, event->name, event->attr, event->flags, flags, timeout ? *timeout : 0,
            event->waiting_threads->size());
    }

    if ((event->attr & 0x1000) == 0 && event->waiting_threads->size() > 0) {
        return RET_ERROR(SCE_KERNEL_ERROR_EVF_MULTI);
    }

    const ThreadStatePtr thread = kernel.get_thread(thread_id);

    std::unique_lock<std::mutex> event_lock(event->mutex);

    bool condition;
    if (wait & SCE_EVENT_WAITOR) {
        condition = event->flags & flags;
    } else {
        condition = (event->flags & flags) == flags;
    }

    if (outBits) {
        *outBits = event->flags;
    }

    if (condition) {
        if (wait & SCE_EVENT_WAITCLEAR) {
            event->flags = 0;
        }

        if (wait & SCE_EVENT_WAITCLEAR_PAT) {
            event->flags &= ~flags;
        }

        return SCE_KERNEL_OK;
    } else if (dowait) {
        std::unique_lock<std::mutex> thread_lock(thread->mutex);
        thread->update_status(ThreadStatus::wait, ThreadStatus::run);

        WaitingThreadData data;
        data.thread = thread;
        data.wait = wait;
        data.flags = flags;
        data.outBits = outBits;
        data.priority = thread->priority;

        bool was_canceled = false;
        data.was_canceled = &was_canceled;

        const auto data_it = event->waiting_threads->push(data);
        thread_lock.unlock();

        // Browser fiber runtime: park cooperatively instead of blocking the
        // host thread (same contract as semaphore/mutex waits). The
        // set/cancel paths unlink the queue entry and flip status to run,
        // which both resumes the fiber (via update_status -> notify) and
        // reports readiness back through handle_cooperative_wait.
        int err;
        if (kernel.execution_host)
            err = handle_cooperative_wait(kernel, thread, thread_lock, event_lock, event->waiting_threads, timeout);
        else
            err = handle_timeout(kernel, thread, thread_lock, event_lock, event->waiting_threads, data_it, export_name, timeout);
        if (err < 0 && outBits) {
            // set it only if a timeout occurs
            // otherwise set in eventflag_set
            *outBits = event->flags;
        }
        if (was_canceled)
            err = SCE_KERNEL_ERROR_WAIT_CANCEL;

        return err;
    } else {
        return SCE_KERNEL_ERROR_EVF_COND;
    }
}

SceInt32 eventflag_wait(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID evfId, SceUInt32 bitPattern, SceUInt32 waitMode, SceUInt32 *pResultPat, SceUInt32 *pTimeout) {
    return eventflag_waitorpoll(kernel, export_name, thread_id, evfId, bitPattern, waitMode, pResultPat, pTimeout, true);
}

int eventflag_poll(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, unsigned int flags, unsigned int wait, unsigned int *outBits) {
    return eventflag_waitorpoll(kernel, export_name, thread_id, event_id, flags, wait, outBits, 0, false);
}

SceInt32 eventflag_set(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID evfId, SceUInt32 bitPattern) {
    assert(evfId >= 0);

    // TODO Don't lock twice.
    const EventFlagPtr event = lock_and_find(evfId, kernel.eventflags, kernel.mutex);
    if (!event) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_EVF_ID);
    }

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} existing_flags: {:#b} set_flags: {:#b}"
                  " waiting_threads: {}",
            export_name, event->uid, thread_id, event->name, event->attr, event->flags, bitPattern,
            event->waiting_threads->size());
    }

    const std::lock_guard<std::mutex> event_lock(event->mutex);
    event->flags |= bitPattern;

    for (auto it = event->waiting_threads->begin(); it != event->waiting_threads->end();) {
        const auto waiting_thread_data = *it;
        const auto waiting_thread = waiting_thread_data.thread;
        const auto waiting_flags = waiting_thread_data.flags;

        bool condition;
        if (waiting_thread_data.wait & SCE_EVENT_WAITOR) {
            condition = event->flags & waiting_flags;
        } else {
            condition = (event->flags & waiting_flags) == waiting_flags;
        }

        if (condition) {
            if (waiting_thread_data.outBits) {
                *waiting_thread_data.outBits = event->flags;
            }

            if (waiting_thread_data.wait & SCE_EVENT_WAITCLEAR) {
                event->flags = 0;
            }

            if (waiting_thread_data.wait & SCE_EVENT_WAITCLEAR_PAT) {
                event->flags &= ~waiting_flags;
            }

            const std::lock_guard<std::mutex> waiting_thread_lock(waiting_thread->mutex);

            waiting_thread->update_status(ThreadStatus::run, ThreadStatus::wait);

            event->waiting_threads->erase(it++);
        } else {
            ++it;
        }
    }

    return 0;
}

SceInt32 eventflag_cancel(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, SceUInt32 pattern, SceUInt32 *num_wait_threads) {
    assert(event_id >= 0);

    const EventFlagPtr event = lock_and_find(event_id, kernel.eventflags, kernel.mutex);
    if (!event) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_EVF_ID);
    }

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} existing_flags: {:#b} waiting_threads: {}",
            export_name, event->uid, thread_id, event->name, event->attr, event->flags, event->waiting_threads->size());
    }

    SceUInt32 nb_threads = 0;

    const std::lock_guard<std::mutex> event_lock(event->mutex);

    while (!event->waiting_threads->empty()) {
        const auto &waiting_thread_data = *event->waiting_threads->begin();
        const auto waiting_thread = waiting_thread_data.thread;

        const std::lock_guard<std::mutex> waiting_thread_lock(waiting_thread->mutex);

        *waiting_thread_data.was_canceled = true;
        if (waiting_thread_data.outBits)
            *waiting_thread_data.outBits = pattern;

        waiting_thread->update_status(ThreadStatus::run, ThreadStatus::wait);

        event->waiting_threads->erase(event->waiting_threads->begin());
        nb_threads++;
    }

    event->flags = pattern;

    if (num_wait_threads)
        *num_wait_threads = nb_threads;

    return SCE_KERNEL_OK;
}

int eventflag_delete(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id) {
    assert(event_id >= 0);

    const EventFlagPtr event = lock_and_find(event_id, kernel.eventflags, kernel.mutex);
    if (!event) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_EVF_ID);
    }

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {} existing_flags: {:#b} waiting_threads: {}",
            export_name, event->uid, thread_id, event->name, event->attr, event->flags, event->waiting_threads->size());
    }

    if (event->waiting_threads->empty()) {
        const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);
        kernel.eventflags.erase(event_id);
    } else {
        // TODO:
        LOG_WARN("Can't delete sync object, it has waiting threads.");
    }

    return SCE_KERNEL_OK;
}

// *************
// * Msg Pipe  *
// *************

SceUID msgpipe_create(KernelState &kernel, const char *export_name, const char *name, SceUID thread_id, SceUInt attr, SceSize bufSize) {
    if ((strlen(name) > 31) && ((attr & 0x80) == 0x80)) {
        return RET_ERROR(SCE_KERNEL_ERROR_UID_NAME_TOO_LONG);
    }

    const SceUID uid = kernel.get_next_uid();

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {}",
            export_name, uid, thread_id, name, attr);
    }

    const MsgPipePtr msgpipe = std::make_shared<MsgPipe>(bufSize);

    msgpipe->attr = attr;
    msgpipe->uid = uid;
    strncpy(msgpipe->name, name, KERNELOBJECT_MAX_NAME_LENGTH);

    if (msgpipe->attr & SCE_KERNEL_ATTR_TH_PRIO) {
        msgpipe->receivers = std::make_unique<PriorityThreadDataQueue<WaitingThreadData>>();
    } else {
        msgpipe->receivers = std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();
    }

    // TODO do senders respect priority?
    msgpipe->senders = std::make_unique<FIFOThreadDataQueue<WaitingThreadData>>();

    const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);
    kernel.msgpipes.emplace(uid, msgpipe);

    return uid;
}

SceUID msgpipe_find(KernelState &kernel, const char *export_name, const char *pName) {
    if (strlen(pName) > KERNELOBJECT_MAX_NAME_LENGTH)
        return RET_ERROR(SCE_KERNEL_ERROR_UID_NAME_TOO_LONG);

    if (LOG_SYNC_PRIMITIVES)
        LOG_DEBUG("{}: name: \"{}\"", export_name, pName);

    const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);

    const auto it = std::find_if(kernel.msgpipes.begin(), kernel.msgpipes.end(), [=](const auto &msg_pipe) {
        return strncmp(msg_pipe.second->name, pName, KERNELOBJECT_MAX_NAME_LENGTH) == 0;
    });

    if (it != kernel.msgpipes.end())
        return it->first;

    return RET_ERROR(SCE_KERNEL_ERROR_UID_CANNOT_FIND_BY_NAME);
}

SceSize msgpipe_recv(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID msgPipeId, SceUInt32 waitMode, void *pRecvBuf, SceSize recvSize, SceUInt32 *pTimeout) {
    assert(msgPipeId >= 0);

    const bool ASAP = !(waitMode & SCE_KERNEL_MSG_PIPE_MODE_FULL);

    const MsgPipePtr msgpipe = lock_and_find(msgPipeId, kernel.msgpipes, kernel.mutex);
    if (!msgpipe) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID);
    }

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" pipe attr: {} wait_mode: {:#b} ({})"
                  " senders: {} receivers: {}",
            export_name, msgpipe->uid, thread_id, msgpipe->name, msgpipe->attr, waitMode, ASAP ? "ASAP" : "FULL",
            msgpipe->senders->size(), msgpipe->receivers->size());
    }

    if (recvSize > msgpipe->data_buffer.Capacity())
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_SIZE);

    const auto copyOut = [&] {
        if (waitMode & SCE_KERNEL_MSG_PIPE_MODE_DONT_REMOVE) {
            return msgpipe->data_buffer.Peek(pRecvBuf, recvSize);
        } else {
            return msgpipe->data_buffer.Remove(pRecvBuf, recvSize);
        }
    };

    const ThreadStatePtr thread = kernel.get_thread(thread_id);
    std::unique_lock msgpipe_lock(msgpipe->mutex);
    // check in case of delete happens while waiting (un)lock
    if (msgpipe->beingDeleted) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID);
    }

    const auto wakeup_senders = [&] {
        if (!msgpipe->senders->empty()) {
            for (auto it = msgpipe->senders->begin(); it != msgpipe->senders->end(); ++it) {
                auto threadInfo = (*it);
                if (threadInfo.mp.request_size <= msgpipe->data_buffer.Free()) { // Found a thread we can service
                    threadInfo.thread->status = ThreadStatus::run;
                    threadInfo.thread->status_cond.notify_one();

                    msgpipe->senders->erase(it); // Erase other thread's info - done here to avoid race
                    break; // Should we try to signal other threads, too?
                }
            }
        }
    };

    std::size_t availableSize = msgpipe->data_buffer.Used();
    if ((availableSize >= recvSize) || (ASAP && availableSize >= 1)) { // Copy out and return.
        SceSize copied_size = (SceSize)copyOut();
        wakeup_senders();
        return copied_size;
    } else if (waitMode & SCE_KERNEL_MSG_PIPE_MODE_DONT_WAIT) {
        return 0;
    } else { // sleep until we can insert
        if (kernel.execution_host)
            return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
        WaitingThreadData wait_data;
        wait_data.thread = thread;
        wait_data.priority = thread->priority;
        wait_data.mp.request_size = (ASAP) ? 1 : recvSize; // If ASAP, we can read as low as 1 byte

        msgpipe->receivers->push(wait_data);

        std::unique_lock thread_lock(thread->mutex); // Lock thread - needed for condition variable
        thread->update_status(ThreadStatus::wait, ThreadStatus::run); // Mark ourselves as sleeping

        const auto finish = [&] {
            thread->update_status(ThreadStatus::run); // Wake up

            SceSize readSize = (SceSize)copyOut();
            // msgpipe->receivers->erase(wait_data); //we've already been erased by the sender
            wakeup_senders();
            return readSize;
        };

        if (!pTimeout) { // No timeout - loop forever until we can fill the buffer
            do {
                // FIXME sleep on SimpleEvent
                msgpipe_lock.unlock(); // Unlock message pipe object, else we'll deadlock
                thread->status_cond.wait(thread_lock, [&] {
                    return thread->status == ThreadStatus::run;
                });
                if (msgpipe->beingDeleted) { // if beingDeleted then message pipe is locked, so we can't lock again
                    std::atomic_fetch_add(&msgpipe->remainingThreads, static_cast<size_t>(-1));
                    return SCE_KERNEL_ERROR_WAIT_DELETE;
                }
                msgpipe_lock.lock(); // Lock message pipe again
                availableSize = msgpipe->data_buffer.Used();
            } while (!((availableSize >= recvSize) || (ASAP && (availableSize > 0))));

            return finish();
        } else { // There's a timeout - wait until we can fill buffer or timeout
            msgpipe_lock.unlock(); // Unlock message pipe object, else we'll deadlock
            auto status = thread->status_cond.wait_for(thread_lock, std::chrono::microseconds{ *pTimeout }, [&] {
                return thread->status == ThreadStatus::run;
            });
            if (msgpipe->beingDeleted) {
                std::atomic_fetch_add(&msgpipe->remainingThreads, static_cast<size_t>(-1));
                return SCE_KERNEL_ERROR_WAIT_DELETE;
            }

            if (!status) { // Timed out and buffer hasn't been touched
                thread->update_status(ThreadStatus::run, ThreadStatus::wait);
                return RET_ERROR(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
            }
            msgpipe_lock.lock(); // Lock message pipe again
            return finish();
        }
    }
}

// FIXME this should be SendVector!
SceSize msgpipe_send(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID msgPipeId, SceUInt32 waitMode, const void *pSendBuf, SceSize sendSize, SceUInt32 *pTimeout) {
    assert(msgPipeId >= 0);

    const bool ASAP = !(waitMode & SCE_KERNEL_MSG_PIPE_MODE_FULL);

    const MsgPipePtr msgpipe = lock_and_find(msgPipeId, kernel.msgpipes, kernel.mutex);
    if (!msgpipe) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID);
    }

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" pipe attr: {} wait_mode: {:#b}"
                  " senders: {} receivers: {}",
            export_name, msgpipe->uid, thread_id, msgpipe->name, msgpipe->attr, waitMode,
            msgpipe->senders->size(), msgpipe->receivers->size());
    }

    if (sendSize > msgpipe->data_buffer.Capacity())
        return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_SIZE);

    const auto wakeup_receivers = [&] { // TODO is this correct?
        if (!msgpipe->receivers->empty()) {
            for (auto it = msgpipe->receivers->begin(); it != msgpipe->receivers->end(); ++it) {
                if ((*it).mp.request_size <= msgpipe->data_buffer.Used()) { // Found a thread we can service
                    (*it).thread->update_status(ThreadStatus::run, ThreadStatus::wait);

                    msgpipe->receivers->erase(it); // Erase other thread's info - done here to avoid race
                    break; // Should we try to signal other threads, too?
                }
            }
        }
    };

    const ThreadStatePtr thread = kernel.get_thread(thread_id);
    std::unique_lock<std::mutex> msgpipe_lock(msgpipe->mutex);
    // check in case of delete happens while waiting (un)lock
    if (msgpipe->beingDeleted) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID);
    }

    // If ASAP and there's at least 1 free byte, or FULL and there's enough space, copy and return directly.
    std::size_t freeSize = msgpipe->data_buffer.Free();
    if ((freeSize >= sendSize) || (ASAP && (freeSize >= 1))) {
        SceSize copied_size = (SceSize)msgpipe->data_buffer.Insert(pSendBuf, sendSize);

        wakeup_receivers();

        return copied_size;
    } else if (waitMode & SCE_KERNEL_MSG_PIPE_MODE_DONT_WAIT) {
        return 0;
    } else { // Go to sleep until there's more space
        if (kernel.execution_host)
            return RET_ERROR(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
        WaitingThreadData wait_data;
        wait_data.thread = thread;
        wait_data.priority = thread->priority;
        wait_data.mp.request_size = (ASAP) ? 1 : sendSize; // If ASAP, we can insert as low as 1 byte

        msgpipe->senders->push(wait_data);

        std::unique_lock thread_lock(thread->mutex); // Lock thread - needed for condition variable
        thread->update_status(ThreadStatus::wait, ThreadStatus::run); // Mark ourselves as sleeping

        const auto finish = [&] {
            thread->update_status(ThreadStatus::run); // Wake up

            SceSize insertedSize = (SceSize)msgpipe->data_buffer.Insert(pSendBuf, sendSize);
            // msgpipe->senders->erase(wait_data); //Don't erase ourselves - recv will do it
            wakeup_receivers();
            return (int)insertedSize;
        };

        if (!pTimeout) { // No timeout - loop forever until we can fill the buffer
            do {
                // FIXME sleep on SimpleEvent
                msgpipe_lock.unlock(); // Unlock message pipe object, else we'll deadlock
                thread->status_cond.wait(thread_lock, [&] {
                    return thread->status == ThreadStatus::run;
                });
                if (msgpipe->beingDeleted) { // if beingDeleted then message pipe is locked, so we can't lock again
                    std::atomic_fetch_add(&msgpipe->remainingThreads, static_cast<size_t>(-1));
                    return SCE_KERNEL_ERROR_WAIT_DELETE;
                }
                msgpipe_lock.lock(); // Lock message pipe before read from data_buffer
                freeSize = msgpipe->data_buffer.Free();
            } while (!((freeSize >= sendSize) || (ASAP && (freeSize >= 1))));

            // Message pipe is still locked here, so we can read from data_buffer in finish()
            return finish();
        } else { // There's a timeout - wait until we can fill buffer or timeout
            msgpipe_lock.unlock(); // Unlock message pipe object, else we'll deadlock
            auto status = thread->status_cond.wait_for(thread_lock, std::chrono::microseconds{ *pTimeout }, [&] {
                return thread->status == ThreadStatus::run;
            });
            if (msgpipe->beingDeleted) {
                std::atomic_fetch_add(&msgpipe->remainingThreads, static_cast<size_t>(-1));
                return SCE_KERNEL_ERROR_WAIT_DELETE;
            }

            if (!status) { // Timed out and buffer hasn't been touched
                thread->update_status(ThreadStatus::run, ThreadStatus::wait);
                return RET_ERROR(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
            }
            msgpipe_lock.lock(); // Lock message pipe before read from data_buffer in finish()
            return finish();
        }
    }
}

SceInt32 msgpipe_delete(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID msgpipe_id) {
    assert(msgpipe_id >= 0);

    const MsgPipePtr msgpipe = lock_and_find(msgpipe_id, kernel.msgpipes, kernel.mutex);
    if (!msgpipe) {
        return RET_ERROR(SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID);
    }

    if (LOG_SYNC_PRIMITIVES) {
        LOG_DEBUG("{}: uid: {} thread_id: {} name: \"{}\" attr: {}",
            export_name, msgpipe->uid, thread_id, msgpipe->name, msgpipe->attr);
    }

    if (!msgpipe->receivers->empty() || !msgpipe->senders->empty()) {
        const std::lock_guard<std::mutex> event_lock(msgpipe->mutex);
        msgpipe->remainingThreads = (msgpipe->senders->size() + msgpipe->receivers->size());
        msgpipe->beingDeleted = true;
        std::atomic_thread_fence(std::memory_order_release);

        // Wake up every thread
        for (auto it : *msgpipe->senders) {
            it.thread->update_status(ThreadStatus::run, ThreadStatus::wait);
        }
        for (auto it : *msgpipe->receivers) {
            it.thread->update_status(ThreadStatus::run, ThreadStatus::wait);
        }
        while (std::atomic_load(&msgpipe->remainingThreads) != 0) // FIXME busy loop bad
            std::this_thread::yield();
    }

    const std::lock_guard<std::mutex> kernel_lock(kernel.mutex);
    kernel.msgpipes.erase(msgpipe->uid);

    return SCE_KERNEL_OK;
}
