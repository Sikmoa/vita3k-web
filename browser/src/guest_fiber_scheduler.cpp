#include "guest_fiber_scheduler.h"

#include <emscripten/fiber.h>

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

        Task(Impl &owner, TaskId id, int priority, Function function, void *argument)
            : owner(owner), id(id), priority(priority), function(function), argument(argument),
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
    // Deliberately process-global: this implementation supports ONE OS thread.
    static Impl *dispatch_owner;

    Impl(GuestFiberScheduler &api, std::size_t c_bytes, std::size_t a_bytes)
        : api(api), c_stack_bytes(c_bytes), root_stack(a_bytes) {
        if (c_bytes < 1024 || c_bytes % sizeof(StackBlock) != 0)
            throw std::invalid_argument("fiber stack size must be >=1024 and a multiple of 16");
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
        while (*position && (*position)->priority <= task->priority)
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
        emscripten_fiber_swap(&task.fiber, &task.owner.root);
        std::abort(); // A completed fiber must NEVER be resumed.
    }

    bool suspend(State state) noexcept {
        if (dispatch_owner != this || !active || std::uncaught_exceptions() != 0)
            return false;
        active->status.state = state;
        emscripten_fiber_swap(&active->fiber, &root);
        return true;
    }
};

GuestFiberScheduler::Impl *GuestFiberScheduler::Impl::dispatch_owner = nullptr;

GuestFiberScheduler::GuestFiberScheduler(std::size_t c_bytes, std::size_t a_bytes)
    : impl_(std::make_unique<Impl>(*this, c_bytes, a_bytes)) {}

GuestFiberScheduler::~GuestFiberScheduler() {
    if (!teardown())
        std::terminate(); // Refuse destruction before unique_ptr can free live stacks.
}

GuestFiberScheduler::TaskId GuestFiberScheduler::enqueue(int priority, Function function, void *argument) {
    if (!function)
        return invalid_task;
    if (impl_->next_id == std::numeric_limits<TaskId>::max())
        throw std::overflow_error("fiber task IDs exhausted");
    auto task = std::make_unique<Impl::Task>(*impl_, impl_->next_id, priority, function, argument);
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
    emscripten_fiber_init_from_current_context(&self.root, self.root_stack.data.get(), self.root_stack.bytes);
    std::size_t swaps = 0;
    while (swaps < max_swaps && self.ready) {
        auto *task = self.ready;
        self.ready = task->next;
        task->next = nullptr;
        self.active = task;
        emscripten_fiber_swap(&self.root, &task->fiber);
        self.active = nullptr;
        ++swaps;
        if (task->status.state == State::runnable)
            self.queue(task);
    }
    Impl::dispatch_owner = nullptr;
    return swaps;
}

bool GuestFiberScheduler::yield() noexcept { return impl_->suspend(State::runnable); }
bool GuestFiberScheduler::park() noexcept { return impl_->suspend(State::parked); }

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
