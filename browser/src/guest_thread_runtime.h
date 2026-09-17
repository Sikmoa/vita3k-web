#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

struct EmuEnvState;
struct KernelState;
struct MemState;

namespace vita3k::web {

// Single-threaded Emscripten/Asyncify execution host; JIT only, no SDL threads.
// Attach AFTER kernel/memory init, BEFORE creating threads. The kernel must be
// empty; manually registered/native threads cannot be adopted. Kernel, memory,
// and captured import callbacks must outlive this object (or successful shutdown).
// One attached runtime per process, called only on its owning OS thread.
// Do not replace hooks while attached.
//
// Create/start using production KernelState::create_thread / ThreadState::start.
// Creation and start enqueue only. resume/execute run until idle or the dispatch
// budget is reached; they never sleep the browser event loop. Call again after
// a signal or the reported deadline. Each CPU invocation is slice-bounded;
// lifecycle callbacks may run additional invocations before the next switch.
// Synchronous HLE/compilation wall time is NOT preemptible.
//
// Supported waits: ordinary semaphore waits, production signal/cancel queues,
// relative microsecond timeout, thread deletion/shutdown cancellation. Semaphore
// deletion with waiters returns ILLEGAL_CONTEXT. CB notification processing,
// other wait families, device waits and host-blocking HLE are NOT implemented.
// Known wait imports are rejected with ILLEGAL_CONTEXT, never fake success.
// Custom import callbacks must neither block nor switch with a lock held.
// Thread start/end event handlers and nested run_callback execute on the same
// fiber; notification Callback::execute (which holds a mutex) is excluded.
//
// Not Vita multicore: one logical CPU (processor ID 0), affinity is metadata.
// Dispatch uses creation-time priority, lower numeric first/FIFO equals, with
// no priority inheritance or dynamic reprioritization. Semaphore ordering is
// the existing production FIFO/priority queue, including its priority ordering.
// To isolate the process-global JS JIT dispatch hints, compiled caches are
// conservatively invalidated on every suspension and terminal exit (not fast).
// The legacy synchronous run_guest_function/run_module_entry host API pumps at
// most 4096 dispatches, then requests deletion and returns ILLEGAL_CONTEXT if
// unfinished. Prefer create/start + resume for asynchronous host launches.
class GuestThreadRuntime final {
public:
    struct Progress {
        std::size_t dispatches = 0;
        std::size_t runnable = 0;
        std::size_t waiting = 0;
        std::size_t dormant = 0;
        std::size_t failed = 0; // cumulative failed/faulted threads
        bool idle = true; // no runnable work, NOT necessarily all threads done
        // Monotonic steady-clock microseconds, same epoch as now_us().
        std::optional<std::uint64_t> next_deadline_us;
    };

    explicit GuestThreadRuntime(std::uint64_t instructions_per_slice = 32768,
        std::size_t c_stack_bytes = 256 * 1024,
        std::size_t asyncify_stack_bytes = 256 * 1024);
    ~GuestThreadRuntime();
    GuestThreadRuntime(const GuestThreadRuntime &) = delete;
    GuestThreadRuntime &operator=(const GuestThreadRuntime &) = delete;

    bool attach(EmuEnvState &env);
    bool attach(KernelState &kernel, MemState &mem);
    bool attached() const noexcept;
    static std::uint64_t now_us() noexcept;
    Progress resume(std::size_t max_dispatches = 256);
    Progress execute(std::size_t max_dispatches = 256) { return resume(max_dispatches); }

    // Root only. Request deletion without end callbacks; wake/drain actual
    // continuations before releasing any stacks. False means budget exhausted
    // (or nested call); retain this object and call shutdown again. Success
    // restores previous import/module hooks and detaches. Destructor attempts
    // bounded shutdown and fails fast rather than freeing live continuations.
    bool shutdown(std::size_t max_dispatches = 4096);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vita3k::web
