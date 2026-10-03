#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace vita3k::web {

// Single OS thread, cooperative Emscripten fibers. Build all reachable code with
// -sASYNCIFY=1 and -fexceptions (including at link time). No pthreads, JS async
// suspension, or switching while a C++ exception is being handled/unwound.
// Stack sizes are fixed capacities: callers must budget for their callback depth.
class GuestFiberScheduler final {
public:
    using TaskId = std::uint64_t;
    using Function = void (*)(GuestFiberScheduler &, void *);
    static constexpr TaskId invalid_task = 0;
    enum class State { runnable, parked, completed };
    struct Status {
        State state;
        bool failed; // Callback threw; exception was caught on its own fiber.
    };

    // `cores` > 1 emulates that many CPUs on the one OS thread: every dispatch
    // assigns ready tasks to cores by priority (within each task's core mask)
    // and serves the cores in rotation, so tasks on different cores make
    // progress together as they would in parallel.
    static constexpr unsigned max_cores = 8;
    explicit GuestFiberScheduler(std::size_t c_stack_bytes = 64 * 1024,
        std::size_t asyncify_stack_bytes = 64 * 1024, unsigned cores = 1);
    // Fail-fast if live tasks remain; never implicitly cancel continuations.
    ~GuestFiberScheduler();
    GuestFiberScheduler(const GuestFiberScheduler &) = delete;
    GuestFiberScheduler &operator=(const GuestFiberScheduler &) = delete;
    GuestFiberScheduler(GuestFiberScheduler &&) = delete;
    GuestFiberScheduler &operator=(GuestFiberScheduler &&) = delete;

    // Enqueue only. Lower numeric priority wins; equals rotate FIFO on dispatch.
    // Argument is borrowed until completion. Null function returns invalid_task.
    // Allocation/invalid stack size exceptions are ordinary caller-side errors.
    // `core_mask` bit n allows core n; 0 allows every core.
    TaskId enqueue(int priority, Function function, void *argument = nullptr, unsigned core_mask = 0);
    std::optional<Status> status(TaskId id) const noexcept;

    // Root only; nested dispatch (even of another scheduler) returns zero.
    // One budget unit = one root->task dispatch and its task->root return.
    // This is NOT preemption: a callback must yield/park/return to bound latency.
    std::size_t resume(std::size_t max_swaps) noexcept;
    // Only the active callback can suspend. Root calls return false.
    // Active tasks count as runnable. Resumption returns true at the same frame.
    // `aged`: the task used up its slice without blocking; it loses
    // effective priority until it next parks (see kAgingStep).
    bool yield(bool aged = false) noexcept;
    bool park() noexcept;
    // Active task only: a ready task has a strictly better effective priority,
    // so it should run now. Equal ones wait for the slice to run out.
    bool should_yield() const noexcept;
    // Active task only: charge `units` of work (guest instructions) to the
    // logical core it runs on. The next dispatch serves the claimed core that
    // has done the least work, so a busy-waiting thread with long slices
    // cannot crowd out threads on other cores whose turns are short.
    void charge(std::uint64_t units) noexcept;
    // Only parked->runnable; append behind existing equals, never execute eagerly.
    bool wake(TaskId id) noexcept;
    // Guest priority change (sceKernelChangeThreadPriority): takes effect for
    // the next dispatch decision, reordering the task if it is ready.
    bool set_priority(TaskId id, int priority) noexcept;
    // Guest affinity change (sceKernelChangeThreadCpuAffinityMask).
    bool set_core_mask(TaskId id, unsigned core_mask) noexcept;

    // Root only. False leaves everything intact if any task is live (including
    // never-started or parked). True frees only terminal, non-resumable tasks.
    // Completed records/stacks remain owned until this call or destruction.
    // Reusable after success; IDs are not recycled during this object's lifetime.
    bool teardown() noexcept;
    // Root only; release one terminal record without disturbing live peers.
    // Never frees an active/parked/never-started continuation.
    bool reap(TaskId id) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vita3k::web
