#include "guest_thread_runtime.h"
#include "guest_fiber_scheduler.h"

#include <cpu/disasm/functions.h>
#include <cpu/functions.h>
#include <cpu/impl/wasm_jit_cpu.h>
#include <emuenv/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <nids/functions.h>

#include <chrono>
#include <cstdio>
#include <exception>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace vita3k::web {
namespace {
constexpr uint32_t context_error = static_cast<uint32_t>(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);

// These paths use host waits outside sync_primitives.cpp, or notification
// callbacks holding Callback::_mutex. Guard aliases by their canonical name.
// Custom/device HLE must obey the host contract too; this is not a sandbox.
bool unsupported_import(uint32_t nid) {
    const char *name = import_name(nid);
    const std::string_view n = name ? name : "";
    if (n.find("WaitSema") != n.npos && n.find("CB") == n.npos)
        return false;
    // eventflag_wait parks on the fiber runtime (sync_primitives
    // execution_host branch); the CB variant still runs host callbacks.
    if (n.find("WaitEventFlag") != n.npos && n.find("CB") == n.npos)
        return false;
    // delay_thread parks until its deadline (SceThreadmgr execution_host
    // branch); the CB variant still runs host callbacks first.
    if (n.find("DelayThread") != n.npos && n.find("CB") == n.npos)
        return false;
    return n.find("Wait") != n.npos || n.find("DelayThread") != n.npos
        || n.find("CheckCallback") != n.npos || n.find("CB") != n.npos
        || n.find("ReceiveMsgPipe") != n.npos || n.find("SendMsgPipe") != n.npos;
}
}

struct GuestThreadRuntime::Impl final : KernelExecutionHost {
    using Scheduler = GuestFiberScheduler;
    struct Record {
        Impl &owner;
        ThreadStatePtr thread;
        Scheduler::TaskId task = 0;
        bool deleting = false;
        bool faulted = false;
        std::optional<uint64_t> deadline;
        ThreadStatePtr joining;
        Record(Impl &owner, ThreadStatePtr thread) : owner(owner), thread(std::move(thread)) {}
    };

    Scheduler scheduler;
    uint64_t slice;
    KernelState *kernel = nullptr;
    MemState *mem = nullptr;
    std::map<SceUID, std::unique_ptr<Record>> records;
    Record *active = nullptr;
    Record *last_dispatched = nullptr; // deferred JIT-cache retirement owner
    CPUState *root_cpu = nullptr;
    bool dispatching = false;
    bool stopping_ = false;
    std::size_t failures = 0;
    CallImportFunc saved_import;
    decltype(KernelState::run_module_entry) saved_module;
    static Impl *attached_owner;

    Impl(uint64_t slice, std::size_t c, std::size_t a) : scheduler(c, a), slice(slice) {
        if (slice < 128)
            throw std::invalid_argument("guest slice must fit a conservative 128-tick block");
    }

    CPUStatePtr make_cpu(SceUID id, MemState &memory) override {
        CPUStatePtr cpu(new CPUState(), [](CPUState *p) { delete p; });
        cpu->mem = &memory;
        cpu->thread_id = id;
        cpu->svc_called = false;
        cpu->svc = 0;
        if (!init(cpu->disasm))
            return {};
        cpu->cpu = std::make_unique<WasmJitCPU>(cpu.get(), 0);
        return cpu;
    }

    bool created(const ThreadStatePtr &thread) override {
        if (stopping_ || thread->cpu->mem != mem)
            return false;
        auto record = std::make_unique<Record>(*this, thread);
        auto *pointer = record.get();
        const auto [it, inserted] = records.emplace(thread->id, std::move(record));
        if (!inserted)
            return false;
        try {
            pointer->task = scheduler.enqueue(thread->priority, &entry, pointer);
        } catch (...) {
            records.erase(it);
            return false;
        }
        return true;
    }

    void activate(Record &r) {
        // Called both on first entry AND on return from a suspended stack.
        // entry() alone misses A -> B -> A once both fibers have started.
        if (last_dispatched && last_dispatched != &r)
            invalidate_jit_cache(*last_dispatched->thread->cpu, 0, UINT32_MAX);
        last_dispatched = &r;
        active = &r;
    }

    static void entry(Scheduler &, void *argument) {
        auto &r = *static_cast<Record *>(argument);
        auto &self = r.owner;
        self.activate(r);
        try {
            // Persistent top-level frame: dormant/suspended states park instead
            // of losing lifecycle/callback state. Only deletion returns it.
            r.thread->run_loop();
        } catch (...) {
            r.faulted = true;
            r.thread->returned_value = 0xDEADDEAD;
            r.thread->update_status(ThreadStatus::dormant);
        }
        // A terminal exit is also a CPU switch. Retained external ThreadState
        // references may keep this CPU alive after its kernel record is erased.
        clear_exclusive(*r.thread->cpu);
        invalidate_jit_cache(*r.thread->cpu, 0, UINT32_MAX);
        if (self.last_dispatched == &r)
            self.last_dispatched = nullptr;
        // No exception is active at the scheduler's terminal swap.
        self.active = nullptr;
        set_current_cpu_state(self.root_cpu);
    }

    void notify(ThreadState &thread, bool deleting) noexcept override {
        const auto it = records.find(thread.id);
        if (it == records.end())
            return;
        auto &r = *it->second;
        r.deleting = r.deleting || deleting;
        if (r.deleting || thread.status == ThreadStatus::run)
            scheduler.wake(r.task); // enqueue only, safe under production locks
    }

    void suspend(bool parked) {
        auto *r = active;
        if (!r || !dispatching)
            throw std::logic_error("guest continuation invoked outside its runtime fiber");
        auto *cpu = get_current_cpu_state();
        clear_exclusive(*r->thread->cpu);
        // The JS dispatch map has no CPU owner in its key. Invalidation is
        // DEFERRED to the next dispatch: re-running the same CPU (the hot
        // polling path) keeps its compiled state, a different CPU triggers the
        // outgoing caches' retirement in activate() before any guest code runs.
        active = nullptr;
        set_current_cpu_state(root_cpu);
        const bool switched = parked ? scheduler.park() : scheduler.yield();
        set_current_cpu_state(cpu);
        activate(*r);
        if (!switched)
            throw std::logic_error("unsafe guest fiber suspension");
    }

    int run_cpu(ThreadState &thread, bool single_step) override {
        if (!active || active->thread.get() != &thread)
            return -1;
        if (stopping_)
            return -1;
        auto &jit = static_cast<WasmJitCPU &>(*thread.cpu->cpu);
        const int result = single_step ? jit.step() : jit.run_slice(slice);
        if (result < 0 && !active->faulted) {
            active->faulted = true;
        }
        return result == WasmJitCPU::slice_yield ? 0 : result;
    }
    void checkpoint(ThreadState &) override { suspend(false); }
    void park(ThreadState &) override { suspend(true); }
    bool stopping() const noexcept override { return stopping_; }

    WaitResult wait_sync(ThreadState &thread, std::optional<uint32_t> timeout) override {
        if (!active || active->thread.get() != &thread)
            return WaitResult::cancelled;
        auto &r = *active;
        r.deadline = timeout ? std::optional<uint64_t>(GuestThreadRuntime::now_us() + *timeout) : std::nullopt;
        WaitResult result;
        for (;;) {
            if (r.deleting || stopping_) { result = WaitResult::cancelled; break; }
            if (thread.status == ThreadStatus::run) { result = WaitResult::ready; break; }
            if (r.deadline && GuestThreadRuntime::now_us() >= *r.deadline) { result = WaitResult::timeout; break; }
            suspend(true);
        }
        r.deadline.reset();
        return result;
    }

    uint32_t run_guest_function(ThreadState &thread, Address entry_address, SceSize args, Ptr<void> argp) override {
        if (!kernel || stopping_ || (active && active->thread.get() == &thread))
            return context_error;
        const auto found = records.find(thread.id);
        if (found == records.end())
            return context_error;
        const Address old_entry = thread.entry_point;
        thread.entry_point = entry_address;
        const int started = thread.start(args, argp);
        thread.entry_point = old_entry;
        if (started < 0)
            return static_cast<uint32_t>(started);
        if (active) {
            auto &caller = *active;
            caller.joining = found->second->thread;
            while (thread.status != ThreadStatus::dormant && records.contains(thread.id)) {
                if (caller.deleting || stopping_) {
                    caller.joining.reset();
                    thread.exit_delete(false);
                    return static_cast<uint32_t>(SCE_KERNEL_ERROR_WAIT_CANCEL);
                }
                suspend(true);
            }
            caller.joining.reset();
        } else {
            // Legacy synchronous module-entry API cannot return a continuation.
            // Bound it; use create/start + resume for asynchronous host launches.
            pump(4096);
            if (thread.status != ThreadStatus::dormant) {
                thread.exit_delete(false);
                return context_error;
            }
        }
        return thread.returned_value;
    }

    uint32_t module_entry(const SceKernelModuleInfo &info, Ptr<const void> entry_address,
        SceSize args, Ptr<const void> argp) {
        auto thread = kernel->create_thread(*mem, info.module_name, entry_address,
            SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
            SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        if (!thread)
            return context_error;
        const auto result = run_guest_function(*thread, entry_address.address(), args, argp.cast<void>());
        thread->exit_delete(false);
        return result;
    }

    void service() {
        const auto now = GuestThreadRuntime::now_us();
        for (auto it = records.begin(); it != records.end();) {
            auto &r = *it->second;
            const auto state = scheduler.status(r.task);
            if (state && state->state == Scheduler::State::completed) {
                if (state->failed || r.faulted)
                    ++failures;
                { // Reap only at root, after the terminal trampoline returned.
                    const std::lock_guard<std::mutex> lock(kernel->mutex);
                    kernel->threads.erase(r.thread->id);
                    kernel->thread_deleted_cond.notify_all();
                }
                if (last_dispatched == &r)
                    last_dispatched = nullptr;
                scheduler.reap(r.task);
                it = records.erase(it);
                continue;
            }
            if (r.deleting || (r.deadline && now >= *r.deadline)
                || (r.joining && (r.joining->status == ThreadStatus::dormant
                    || !kernel->threads.contains(r.joining->id))))
                scheduler.wake(r.task);
            ++it;
        }
    }

    GuestThreadRuntime::Progress pump(std::size_t budget) {
        GuestThreadRuntime::Progress p;
        if (!kernel || dispatching)
            return p;
        root_cpu = get_current_cpu_state();
        dispatching = true;
        struct Restore {
            Impl &self;
            ~Restore() { self.dispatching = false; self.active = nullptr; set_current_cpu_state(self.root_cpu); }
        } restore{*this};
        service();
        while (p.dispatches < budget) {
            const auto count = scheduler.resume(1);
            if (!count)
                break;
            p.dispatches += count;
            service();
        }
        p.failed = failures;
        for (const auto &[id, pointer] : records) {
            const auto &r = *pointer;
            const auto state = scheduler.status(r.task);
            if (state && state->state == Scheduler::State::runnable)
                ++p.runnable;
            else if (r.thread->status == ThreadStatus::dormant)
                ++p.dormant;
            else
                ++p.waiting;
            if (r.faulted)
                ++p.failed;
            if (r.deadline && (!p.next_deadline_us || *r.deadline < *p.next_deadline_us))
                p.next_deadline_us = r.deadline;
        }
        p.idle = p.runnable == 0;
        return p;
    }

    void request_stop() {
        stopping_ = true;
        for (auto &[id, r] : records)
            r->thread->exit_delete(false);
    }
    void process_exit() override {
        if (dispatching)
            throw std::logic_error("KernelState::process_exit requires the host root");
        request_stop();
        pump(4096);
        if (!records.empty())
            throw std::runtime_error("guest shutdown budget exhausted; resume shutdown before kernel deinit");
    }
};

GuestThreadRuntime::Impl *GuestThreadRuntime::Impl::attached_owner = nullptr;
GuestThreadRuntime::GuestThreadRuntime(uint64_t slice, std::size_t c, std::size_t a)
    : impl_(std::make_unique<Impl>(slice, c, a)) {}
GuestThreadRuntime::~GuestThreadRuntime() {
    if (!shutdown())
        std::terminate();
}
uint64_t GuestThreadRuntime::now_us() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool GuestThreadRuntime::attached() const noexcept { return impl_->kernel != nullptr; }
bool GuestThreadRuntime::attach(EmuEnvState &env) { return attach(env.kernel, env.mem); }
bool GuestThreadRuntime::attach(KernelState &kernel, MemState &mem) {
    auto &self = *impl_;
    if (self.kernel || Impl::attached_owner || kernel.execution_host || !kernel.threads.empty()
        || !kernel.halt_instruction_pc || !kernel.call_import)
        return false;
    // Prepare potentially allocating closures before publishing a borrowed host.
    auto import = [&self](CPUState &cpu, uint32_t nid, SceUID tid) {
        if (unsupported_import(nid)) {
            std::fprintf(stderr, "[guest-runtime] unsupported wait/callback NID=%08x (%s)\n", nid, import_name(nid));
            write_reg(cpu, 0, context_error);
            return;
        }
        self.saved_import(cpu, nid, tid);
    };
    decltype(kernel.run_module_entry) module = [&self](const SceKernelModuleInfo &info,
        Ptr<const void> entry, SceSize args, Ptr<const void> argp) {
        return self.module_entry(info, entry, args, argp);
    };
    CallImportFunc wrapped_import = import;
    self.saved_import = std::move(kernel.call_import);
    self.saved_module = std::move(kernel.run_module_entry);
    kernel.call_import = std::move(wrapped_import);
    kernel.run_module_entry = std::move(module);
    self.kernel = &kernel;
    self.mem = &mem;
    self.stopping_ = false;
    self.failures = 0;
    kernel.execution_host = &self;
    Impl::attached_owner = &self;
    return true;
}
GuestThreadRuntime::Progress GuestThreadRuntime::resume(std::size_t budget) { return impl_->pump(budget); }
bool GuestThreadRuntime::shutdown(std::size_t budget) {
    auto &self = *impl_;
    if (!self.kernel)
        return true;
    if (self.dispatching)
        return false;
    self.request_stop();
    self.pump(budget);
    if (!self.records.empty())
        return false;
    if (!self.scheduler.teardown())
        return false;
    self.kernel->call_import = std::move(self.saved_import);
    self.kernel->run_module_entry = std::move(self.saved_module);
    self.kernel->execution_host = nullptr;
    self.kernel = nullptr;
    self.mem = nullptr;
    Impl::attached_owner = nullptr;
    return true;
}

} // namespace vita3k::web
