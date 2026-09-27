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

#pragma once

#include <kernel/thread/thread_data_queue.h>
#include <kernel/types.h>
#include <util/byte_ring_buffer.h>

#include <atomic>

struct KernelState;

struct WaitingThreadData {
    ThreadStatePtr thread;
    int32_t priority;
    // Where a waker that dequeues this entry reports a failure (cancel or
    // deletion) to the waiter; the waiter's frame owns it. Null: no report.
    SceInt32 *wake_error = nullptr;

    // additional fields for each primitive
    union {
        struct { // mutex
            int32_t lock_count;
            // Condition variable whose waiter re-acquires the mutex (0: none).
            SceUID relock_cond;
        };
        struct { // rwlock
            bool is_write;
        };
        struct { // semaphore
            int32_t signal;
        };
        struct { // simple events
            int32_t pattern;
            uint32_t *result_pattern;
            uint64_t *user_data;
        };
        struct { // event flags
            int32_t wait;
            int32_t flags;
            uint32_t *outBits;
        };
        // struct { }; // condvar
        struct { // msgpipe
            SceSize request_size;
        } mp;
    };

    bool operator<(const WaitingThreadData &rhs) const {
        return priority < rhs.priority;
    }

    bool operator>(const WaitingThreadData &rhs) const {
        return priority > rhs.priority;
    }

    bool operator==(const WaitingThreadData &rhs) const {
        return thread == rhs.thread;
    }

    bool operator==(const ThreadStatePtr &rhs) const {
        return thread == rhs;
    }
};

typedef std::unique_ptr<ThreadDataQueue<WaitingThreadData>> WaitingThreadQueuePtr;

struct SyncPrimitive {
    // The creating handle. Opened handles are further keys of the same object
    // in its class map.
    SceUID uid{};
    // Keys of the object in its class map: the creating uid and every opened
    // one. Guarded by the kernel mutex; the object is destroyed with its last
    // handle.
    unsigned handles = 1;
    uint32_t attr{};
    std::mutex mutex;
    char name[KERNELOBJECT_MAX_NAME_LENGTH + 1];
    // Set under `mutex` on deletion: holders of a stale pointer must not use it.
    // Atomic so that a deleted timer, whose opened handles stay in the class
    // map, is recognised without its mutex, which a desktop waiter holds.
    std::atomic<bool> deleted = false;
    virtual ~SyncPrimitive() = default;
};

struct SimpleEvent : SyncPrimitive {
    WaitingThreadQueuePtr waiting_threads;
    SceUInt32 pattern;
    SceUInt64 last_user_data;

    bool auto_reset;
    bool cb_wakeup_only;
};

typedef std::shared_ptr<SimpleEvent> SimpleEventPtr;
typedef std::map<SceUID, SimpleEventPtr> SimpleEventPtrs;

struct Timer : SyncPrimitive {
    WaitingThreadQueuePtr waiting_threads;
    std::condition_variable condvar;
    // Event bits other than SCE_KERNEL_EVENT_TIMER, which event_set holds,
    // and the user data the last of them came with.
    SceUInt32 pattern = 0;
    SceUInt64 last_user_data = 0;

    bool is_started = false;
    bool is_repeat = false;
    bool is_pulse = false;
    bool event_set = false;
    uint64_t time = 0;
    uint64_t next_event;
    uint64_t event_interval = 0;
};

typedef std::shared_ptr<Timer> TimerPtr;
typedef std::map<SceUID, TimerPtr> TimerPtrs;

struct Semaphore : SyncPrimitive {
    WaitingThreadQueuePtr waiting_threads;
    int max;
    int val;
    int init_val;
};

typedef std::shared_ptr<Semaphore> SemaphorePtr;
typedef std::map<SceUID, SemaphorePtr> SemaphorePtrs;

struct Mutex : SyncPrimitive {
    int init_count;
    int lock_count;
    ThreadStatePtr owner;
    WaitingThreadQueuePtr waiting_threads;
    Ptr<SceKernelLwMutexWork> workarea;
    // With SCE_KERNEL_MUTEX_ATTR_CEILING: the priority the owner runs at least.
    int ceiling_priority = 0;
    // Overlapping cooperative HLE operations, including parked continuations.
    // Inline access is disabled until every operation has completed.
    unsigned inline_access_depth = 0;
};

typedef std::shared_ptr<Mutex> MutexPtr;
typedef std::map<SceUID, MutexPtr> MutexPtrs;

enum class RWLockState {
    Unlocked,
    ReadLocked,
    WriteLocked,
};

// the int value is the lock count for recursive locks
typedef std::map<ThreadStatePtr, int> RWLockOwners;

struct RWLock : SyncPrimitive {
    RWLockState state;
    RWLockOwners owners;
    WaitingThreadQueuePtr waiting_threads;
};

typedef std::shared_ptr<RWLock> RWLockPtr;
typedef std::map<SceUID, RWLockPtr> RWLockPtrs;

struct EventFlag : SyncPrimitive {
    WaitingThreadQueuePtr waiting_threads;
    int flags;
    SceUInt32 init_pattern;
};

typedef std::shared_ptr<EventFlag> EventFlagPtr;
typedef std::map<SceUID, EventFlagPtr> EventFlagPtrs;

struct Condvar : SyncPrimitive {
    struct SignalTarget {
        enum class Type {
            Any, // signal any one waiting thread
            Specific, // signal a specific waiting thread (target_thread)
            All, // signal all waiting threads
        } type;

        SceUID thread_id; // for Type::One

        explicit SignalTarget(Type type)
            : type(type)
            , thread_id(0) {}
        SignalTarget(Type type, SceUID thread_id)
            : type(type)
            , thread_id(thread_id) {}
    };

    WaitingThreadQueuePtr waiting_threads;
    // Null once the mutex is deleted: firmware dissociates the condition.
    MutexPtr associated_mutex;
    // Lightweight only. The mutex workarea stays reported after the mutex is deleted.
    Ptr<SceKernelLwCondWork> workarea;
    Ptr<SceKernelLwMutexWork> lwmutex_workarea;
};
typedef std::shared_ptr<Condvar> CondvarPtr;
typedef std::map<SceUID, CondvarPtr> CondvarPtrs;

struct MsgPipe : SyncPrimitive {
    MsgPipe(std::size_t bufSize)
        : data_buffer(bufSize) {}

    WaitingThreadQueuePtr senders;
    WaitingThreadQueuePtr receivers;
    ByteRingBuffer data_buffer;

    bool beingDeleted = false;
    std::atomic<std::size_t> remainingThreads = { 0 };

    ~MsgPipe() override = default;
};

typedef std::shared_ptr<MsgPipe> MsgPipePtr;
typedef std::map<SceUID, MsgPipePtr> MsgPipePtrs;

enum class SyncWeight {
    Light, // lightweight
    Heavy // 'heavy'weight
};

// Which handle a ThreadMgr Delete or Close takes (SceKernelThreadMgr 3.74).
// Titles built before SDK 3.10 may use Delete and Close on any handle.
enum class HandleClose {
    Delete, // the creating handle
    Close, // an opened handle
    Any, // any handle: condition variables and kernel callers
};

// Whether `uid` is an opened handle rather than the creating one.
inline bool is_opened_handle(const SyncPrimitive &object, SceUID uid) {
    return uid != object.uid;
}

// simple events
SceUID simple_event_create(KernelState &kernel, MemState &mem, const char *export_name, const char *name, SceUID thread_id, SceUInt32 attr, SceUInt32 init_pattern);
SceInt32 simple_event_waitorpoll(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, SceUInt32 wait_pattern, SceUInt32 *result_pattern, SceUInt64 *user_data, SceUInt32 *timeout, bool is_wait);
SceInt32 simple_event_setorpulse(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, SceUInt32 pattern, SceUInt64 user_data, bool is_set);
SceInt32 simple_event_clear(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, SceUInt32 clear_pattern);
SceUID simple_event_open(KernelState &kernel, const char *export_name, SceUID thread_id, const char *name);
SceInt32 simple_event_close(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, HandleClose how);

// Timer
SceUID timer_create(KernelState &kernel, MemState &mem, const char *export_name, const char *name, SceUID thread_id, SceUInt32 attr);
SceUID timer_open(KernelState &kernel, const char *export_name, SceUID thread_id, const char *pName);
// A deleted timer's other handles stay open but no longer resolve.
TimerPtr timer_find(KernelState &kernel, SceUID timer_handle);
// Delete destroys the timer whatever other handles are open.
SceInt32 timer_close(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID timer_handle, HandleClose how);
SceInt32 timer_waitorpoll(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, SceUInt32 bit_pattern, SceUInt32 *result_pattern, SceUInt64 *user_data, SceUInt32 *timeout, bool is_wait);
SceInt32 timer_clear(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, SceUInt32 clear_pattern);
SceInt32 timer_set(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID timer_handle, SceUID type, SceKernelSysClock *interval, SceInt32 repeats);
SceInt32 timer_start(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID timer_handle);
SceInt32 timer_stop(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID timer_handle);

// Mutex
// Single-host-thread boundary: commit generated updates before any kernel/HLE
// observer runs. A broken dirty list/lifetime/owner is fatal, not a fallback to
// stale kernel state. Only running_thread may have changed ownership inline.
// Desktop (no table) is a no-op; callers must hold no kernel/primitive locks.
void mutex_inline_commit(KernelState &kernel, const ThreadStatePtr &running_thread) noexcept;
SceUID mutex_create(SceUID *uid_out, KernelState &kernel, MemState &mem, const char *export_name, const char *name, SceUID thread_id, SceUInt attr, int init_count, Ptr<SceKernelLwMutexWork> workarea, SyncWeight weight, int ceiling_priority = 0);
SceUID mutex_open(KernelState &kernel, const char *export_name, const char *pName);
int mutex_lock(KernelState &kernel, MemState &mem, const char *export_name, SceUID thread_id, SceUID mutexid, int lock_count, unsigned int *timeout, SyncWeight weight);
int mutex_try_lock(KernelState &kernel, MemState &mem, const char *export_name, SceUID thread_id, SceUID mutexid, int lock_count, SyncWeight weight);
int mutex_unlock(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID mutexid, int unlock_count, SyncWeight weight);
// Lightweight mutexes have only their creating handle.
int mutex_close(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID mutexid, SyncWeight weight, HandleClose how);
// Heavy mutexes only. Writes the number of woken waiters only on success.
int mutex_cancel(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID mutexid, int new_count, SceUInt32 *num_wait_threads);
MutexPtr mutex_get(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID mutexid, SyncWeight weight);

// RWLock
SceUID rwlock_create(KernelState &kernel, MemState &mem, const char *export_name, const char *name, SceUID thread_id, SceUInt32 attr);
SceInt32 rwlock_lock(KernelState &kernel, MemState &mem, const char *export_name, SceUID thread_id, SceUID lock_id, uint32_t *timeout, bool is_write);
SceInt32 rwlock_unlock(KernelState &kernel, MemState &mem, const char *export_name, SceUID thread_id, SceUID lock_id, bool is_write);
SceUID rwlock_open(KernelState &kernel, const char *export_name, const char *pName);
SceInt32 rwlock_close(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID lock_id, HandleClose how);

// Semaphore
SceUID semaphore_create(KernelState &kernel, const char *export_name, const char *name, SceUID thread_id, SceUInt attr, int init_val, int max_val);
SceUID semaphore_open(KernelState &kernel, const char *export_name, const char *pName);
SceInt32 semaphore_wait(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID semaId, SceInt32 needCount, SceUInt32 *pTimeout);
int semaphore_signal(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID semaid, int signal);
int semaphore_close(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID semaid, HandleClose how);
int semaphore_cancel(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID semaid, SceInt32 setCount, SceUInt32 *pNumWaitThreads);

// Condition Variable
SceUID condvar_create(SceUID *uid_out, KernelState &kernel, const char *export_name, const char *name, SceUID thread_id, SceUInt attr, SceUID assoc_mutexid, Ptr<SceKernelLwCondWork> workarea, SyncWeight weight);
int condvar_wait(KernelState &kernel, MemState &mem, const char *export_name, SceUID thread_id, SceUID condid, SceUInt *timeout, SyncWeight weight);
int condvar_signal(KernelState &kernel, MemState &mem, const char *export_name, SceUID thread_id, SceUID condid, Condvar::SignalTarget signal_target, SyncWeight weight);
SceUID condvar_open(KernelState &kernel, const char *export_name, const char *pName);
// Closes any handle: DeleteCond and CloseCond are the same call.
int condvar_delete(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID condid, SyncWeight weight);

// Event Flag
SceUID eventflag_clear(KernelState &kernel, const char *export_name, SceUID evfId, SceUInt32 bitPattern);
SceUID eventflag_create(KernelState &kernel, const char *export_name, SceUID thread_id, const char *pName, SceUInt32 attr, SceUInt32 initPattern);
SceUID eventflag_open(KernelState &kernel, const char *export_name, const char *pName);
SceInt32 eventflag_wait(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID evfId, SceUInt32 bitPattern, SceUInt32 waitMode, SceUInt32 *pResultPat, SceUInt32 *pTimeout);
int eventflag_poll(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, unsigned int flags, unsigned int wait, unsigned int *outBits);
SceInt32 eventflag_set(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID evfId, SceUInt32 bitPattern);
SceInt32 eventflag_cancel(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, SceUInt32 pattern, SceUInt32 *num_wait_threads);
int eventflag_close(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, HandleClose how);

// Message Pipe
SceUID msgpipe_create(KernelState &kernel, const char *export_name, const char *name, SceUID thread_id, SceUInt attr, SceSize bufSize);
SceUID msgpipe_open(KernelState &kernel, const char *export_name, const char *pName);
SceSize msgpipe_recv(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID msgPipeId, SceUInt32 waitMode, void *pRecvBuf, SceSize recvSize, SceUInt32 *pTimeout);
SceSize msgpipe_send(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID msgPipeId, SceUInt32 waitMode, const void *pSendBuf, SceSize sendSize, SceUInt32 *pTimeout);
SceInt32 msgpipe_close(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID msgpipe_id, HandleClose how);
