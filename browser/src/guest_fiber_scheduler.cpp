#include <algorithm>
#include "guest_fiber_scheduler.h"

#include <emscripten/fiber.h>

// A fiber switch unwinds and rewinds through Asyncify; no C++ exception
// crosses it. The same imports declared noexcept (fiber.h cannot say so), so
// the noexcept callers below call them directly: a potentially-throwing
// callee would get a terminate landing pad, which Emscripten's JS exceptions
// turn into an invoke_* wrapper that allocates BigInt arguments on every
// guest thread switch.
extern "C" void fiber_swap(emscripten_fiber_t *old_fiber, emscripten_fiber_t *new_fiber) noexcept
    __asm__("emscripten_fiber_swap");
extern "C" void fiber_init_from_current_context(emscripten_fiber_t *fiber, void *asyncify_stack,
    size_t asyncify_stack_size) noexcept __asm__("emscripten_fiber_init_from_current_context");

#include <cstdlib>
#include <exception>
#include <limits>
#include <stdexcept>
#include <vector>

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
#error "GuestFiberScheduler requires single-threaded Emscripten"
#endif
#if !defined(__cpp_exceptions)
#error "GuestFiberScheduler requires C++ exceptions enabled, including at link time"
#endif

namespace vita3k::web {

struct GuestFiberScheduler::Impl {
    // Explicit alignment and size_t arithmetic work on both wasm32 and Memory64.
    struct alignas(16) StackBlock { unsigned char bytes[16]; };
    struct Stack {
        std::unique_ptr<StackBlock[]> data;
        std::size_t bytes;
        explicit Stack(std::size_t size) : bytes(size) {
            if (size < 1024 || size % sizeof(StackBlock) != 0)
                throw std::invalid_argument("fiber stack size must be >=1024 and a multiple of 16");
            data = std::make_unique<StackBlock[]>(size / sizeof(StackBlock));
        }
    };
    struct Task {
        Impl &owner;
        TaskId id;
        int priority;
        Function function;
        void *argument;
        Status status{State::runnable, false};
        Stack c_stack;
        Stack asyncify_stack;
        emscripten_fiber_t fiber{};
        Task *next = nullptr;
        // Aging penalty added to the priority while the task keeps using up
        // whole slices without blocking; cleared when it parks.
        int penalty = 0;
        int effective() const noexcept { return priority + penalty; }
        unsigned core_mask = 0;
        unsigned last_core = 0;

        Task(Impl &owner, TaskId id, int priority, Function function, void *argument, unsigned core_mask)
            : owner(owner), id(id), priority(priority), function(function), argument(argument), core_mask(core_mask),
              c_stack(owner.c_stack_bytes), asyncify_stack(owner.root_stack.bytes) {
            emscripten_fiber_init(&fiber, &Impl::entry, this,
                c_stack.data.get(), c_stack.bytes, asyncify_stack.data.get(), asyncify_stack.bytes);
        }
    };

    GuestFiberScheduler &api;
    std::size_t c_stack_bytes;
    Stack root_stack;
    emscripten_fiber_t root{};
    std::vector<std::unique_ptr<Task>> tasks;
    Task *ready = nullptr;
    Task *active = nullptr;
    TaskId next_id = 1;
    unsigned cores = 1;
    unsigned next_core = 0; // rotation position among cores with equal work
    // Work charged per logical core (charge()); see take_next.
    std::uint64_t core_work[max_cores] = {};
    // One logical CPU stands in for the Vita's three cores: a task that keeps
    // exhausting slices loses effective priority step by step, so a busy-wait
    // cannot starve the lower-priority thread it is waiting for.
    static constexpr int kAgingStep = 16, kAgingCap = 512;
    // Deliberately process-global: this implementation supports ONE OS thread.
    static Impl *dispatch_owner;

    Impl(GuestFiberScheduler &api, std::size_t c_bytes, std::size_t a_bytes, unsigned core_count)
        : api(api), c_stack_bytes(c_bytes), root_stack(a_bytes), cores(core_count) {
        if (c_bytes < 1024 || c_bytes % sizeof(StackBlock) != 0)
            throw std::invalid_argument("fiber stack size must be >=1024 and a multiple of 16");
        if (core_count == 0 || core_count > max_cores)
            throw std::invalid_argument("fiber scheduler core count out of range");
    }

    unsigned allowed(const Task &task) const noexcept {
        const unsigned all = (1u << cores) - 1;
        return (task.core_mask & all) ? task.core_mask & all : all;
    }

    // Give `task` a free allowed core (its previous one first), or else move
    // already placed tasks along a chain of their other allowed cores to free
    // one (augmenting path; depth is bounded by the core count).
    bool place(Task *task, unsigned &visited, Task *(&claimed)[max_cores]) noexcept {
        const unsigned mask = allowed(*task);
        const auto core_at = [&](unsigned i) { return i == 0 ? task->last_core : (i <= task->last_core ? i - 1 : i); };
        for (unsigned i = 0; i < cores; ++i) {
            const unsigned core = core_at(i);
            if ((mask >> core & 1) && !claimed[core]) {
                claimed[core] = task;
                return true;
            }
        }
        for (unsigned i = 0; i < cores; ++i) {
            const unsigned core = core_at(i);
            if (!(mask >> core & 1) || (visited >> core & 1))
                continue;
            visited |= 1u << core;
            if (place(claimed[core], visited, claimed)) {
                claimed[core] = task;
                return true;
            }
        }
        return false;
    }

    // Next task to dispatch, unlinked from the ready list. Ready tasks claim
    // cores in priority order, rearranging earlier claims when that lets a
    // later task run on an otherwise idle core; the claimed cores are then
    // served round-robin.
    Task *take_next() noexcept {
        Task *claimed[max_cores] = {};
        unsigned filled = 0;
        for (Task *task = ready; task && filled < cores; task = task->next) {
            unsigned visited = 0;
            if (place(task, visited, claimed))
                ++filled;
        }
        // Serve the claimed core that has done the least work (rotation order
        // breaks ties). Serving cores in turn regardless of how long each turn
        // ran let a thread spinning through whole slices take nearly all the
        // time from threads on other cores that block or yield often: Persona
        // 4 Golden's main thread busy-waits for its model threads, which got
        // about 2% of the instructions. An idle core banks no credit: it is
        // raised to the least-loaded claimed core first.
        std::uint64_t least = UINT64_MAX;
        for (unsigned core = 0; core < cores; ++core)
            if (claimed[core])
                least = std::min(least, core_work[core]);
        if (least == UINT64_MAX)
            return nullptr;
        for (unsigned core = 0; core < cores; ++core)
            if (!claimed[core])
                core_work[core] = std::max(core_work[core], least);
        for (unsigned i = 0; i < cores; ++i) {
            const unsigned core = (next_core + i) % cores;
            Task *task = claimed[core];
            if (!task || core_work[core] != least)
                continue;
            next_core = (core + 1) % cores;
            task->last_core = core;
            for (Task **position = &ready; *position; position = &(*position)->next) {
                if (*position == task) {
                    *position = task->next;
                    break;
                }
            }
            task->next = nullptr;
            return task;
        }
        return nullptr;
    }

    Task *find(TaskId id) const noexcept {
        for (const auto &task : tasks)
            if (task->id == id)
                return task.get();
        return nullptr;
    }

    // Intrusive sorted FIFO: suspension and waking never allocate or throw.
    void queue(Task *task) noexcept {
        Task **position = &ready;
        while (*position && (*position)->effective() <= task->effective())
            position = &(*position)->next;
        task->next = *position;
        *position = task;
    }

    static void entry(void *argument) noexcept {
        auto &task = *static_cast<Task *>(argument);
        try {
            task.function(task.owner.api, task.argument);
        } catch (...) {
            task.status.failed = true;
        }
        // The callback and all its RAII frames have finished, including exception
        // cleanup. Only this terminal trampoline remains; never enqueue it again.
        task.status.state = State::completed;
        fiber_swap(&task.fiber, &task.owner.root);
        std::abort(); // A completed fiber must NEVER be resumed.
    }

    bool suspend(State state, bool aged = false) noexcept {
        if (dispatch_owner != this || !active || std::uncaught_exceptions() != 0)
            return false;
        if (state == State::parked)
            active->penalty = 0;
        else if (aged)
            active->penalty = std::min(active->penalty + kAgingStep, kAgingCap);
        active->status.state = state;
        fiber_swap(&active->fiber, &root);
        return true;
    }
};

GuestFiberScheduler::Impl *GuestFiberScheduler::Impl::dispatch_owner = nullptr;

GuestFiberScheduler::GuestFiberScheduler(std::size_t c_bytes, std::size_t a_bytes, unsigned cores)
    : impl_(std::make_unique<Impl>(*this, c_bytes, a_bytes, cores)) {}

GuestFiberScheduler::~GuestFiberScheduler() {
    if (!teardown())
        std::terminate(); // Refuse destruction before unique_ptr can free live stacks.
}

GuestFiberScheduler::TaskId GuestFiberScheduler::enqueue(int priority, Function function, void *argument, unsigned core_mask) {
    if (!function)
        return invalid_task;
    if (impl_->next_id == std::numeric_limits<TaskId>::max())
        throw std::overflow_error("fiber task IDs exhausted");
    auto task = std::make_unique<Impl::Task>(*impl_, impl_->next_id, priority, function, argument, core_mask);
    auto *pointer = task.get();
    impl_->tasks.push_back(std::move(task));
    ++impl_->next_id;
    impl_->queue(pointer);
    return pointer->id;
}

std::optional<GuestFiberScheduler::Status> GuestFiberScheduler::status(TaskId id) const noexcept {
    auto *task = impl_->find(id);
    if (!task)
        return std::nullopt;
    return task->status;
}

std::size_t GuestFiberScheduler::resume(std::size_t max_swaps) noexcept {
    auto &self = *impl_;
    if (Impl::dispatch_owner || max_swaps == 0 || std::uncaught_exceptions() != 0)
        return 0;
    Impl::dispatch_owner = &self;
    // Capture this root invocation, not a constructor frame that already returned.
    fiber_init_from_current_context(&self.root, self.root_stack.data.get(), self.root_stack.bytes);
    std::size_t swaps = 0;
    while (swaps < max_swaps && self.ready) {
        auto *task = self.take_next();
        self.active = task;
        fiber_swap(&self.root, &task->fiber);
        self.active = nullptr;
        ++swaps;
        if (task->status.state == State::runnable)
            self.queue(task);
    }
    Impl::dispatch_owner = nullptr;
    return swaps;
}

bool GuestFiberScheduler::yield(bool aged) noexcept { return impl_->suspend(State::runnable, aged); }
bool GuestFiberScheduler::should_yield() const noexcept {
    const auto &self = *impl_;
    return self.active && self.ready && self.ready->effective() <= self.active->effective();
}
bool GuestFiberScheduler::park() noexcept { return impl_->suspend(State::parked); }
void GuestFiberScheduler::charge(std::uint64_t units) noexcept {
    auto &self = *impl_;
    if (self.active)
        self.core_work[self.active->last_core] += units;
}

bool GuestFiberScheduler::set_priority(TaskId id, int priority) noexcept {
    auto &self = *impl_;
    auto *task = self.find(id);
    if (!task)
        return false;
    if (task->priority == priority)
        return true;
    bool ready = false;
    for (Impl::Task **position = &self.ready; *position; position = &(*position)->next) {
        if (*position == task) {
            *position = task->next;
            task->next = nullptr;
            ready = true;
            break;
        }
    }
    task->priority = priority;
    if (ready)
        self.queue(task);
    return true;
}

bool GuestFiberScheduler::set_core_mask(TaskId id, unsigned core_mask) noexcept {
    auto *task = impl_->find(id);
    if (!task)
        return false;
    task->core_mask = core_mask;
    return true;
}

bool GuestFiberScheduler::wake(TaskId id) noexcept {
    auto *task = impl_->find(id);
    if (!task || task->status.state != State::parked)
        return false;
    task->status.state = State::runnable;
    impl_->queue(task);
    return true;
}

bool GuestFiberScheduler::reap(TaskId id) noexcept {
    if (Impl::dispatch_owner)
        return false;
    for (auto it = impl_->tasks.begin(); it != impl_->tasks.end(); ++it) {
        if ((*it)->id == id && (*it)->status.state == State::completed) {
            impl_->tasks.erase(it);
            return true;
        }
    }
    return false;
}

bool GuestFiberScheduler::teardown() noexcept {
    if (Impl::dispatch_owner)
        return false;
    for (const auto &task : impl_->tasks)
        if (task->status.state != State::completed)
            return false;
    // At root after the final swap: no active or resumable continuation is freed.
    impl_->tasks.clear();
    impl_->ready = nullptr;
    return true;
}

} // namespace vita3k::web
