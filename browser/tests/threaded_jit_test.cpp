// Real host threads, real emitted ARM code, no staged game assets.
#include <cpu/functions.h>
#include <cpu/disasm/functions.h>
#include <cpu/impl/wasm_jit_cpu.h>
#include <emuenv/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#include "guest_thread_semaphore_fixture.h"

#include <array>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::abort(); } } while (0)

static CPUStatePtr make_cpu(SceUID id, std::size_t core, MemState &mem) {
    CPUStatePtr cpu(new CPUState(), [](CPUState *p) { delete p; });
    cpu->mem = &mem;
    cpu->thread_id = id;
    REQUIRE(init(cpu->disasm));
    cpu->cpu = std::make_unique<WasmJitCPU>(cpu.get(), core);
    return cpu;
}

int main() {
    auto env = std::make_unique<EmuEnvState>();
    REQUIRE(init(env->mem, true));
    const auto code = alloc(env->mem, 4096, "threaded JIT fixture");
    const auto data = alloc(env->mem, 4096, "shared counter");
    REQUIRE(code && data);
    constexpr unsigned count = 4, iterations = 100000;
    *Ptr<uint32_t>(data).get(env->mem) = 0;
    guest_thread_fixture::Arm p(code);
    p.constant(0, data);
    p.constant(3, iterations);
    const auto loop = p.pc();
    p.emit(0xe1901f9f); // ldrex r1,[r0]
    p.emit(0xe2811001); // add r1,r1,#1
    p.emit(0xe1802f91); // strex r2,r1,[r0]
    p.emit(0xe3520000); // cmp r2,#0
    p.emit(0x1a000000 | (((loop - (p.pc() + 8)) >> 2) & 0xffffff));
    p.emit(0xe2533001); // subs r3,r3,#1
    p.emit(0x1a000000 | (((loop - (p.pc() + 8)) >> 2) & 0xffffff));
    p.emit(0xf57ff05f); // dmb sy
    p.emit(0xef000000); // svc: test completion
    p.finish(env->mem);
    std::barrier ready(count);
    std::array<std::thread, count> workers;
    for (unsigned i = 0; i < count; ++i) {
        workers[i] = std::thread([&, i] {
            auto cpu = make_cpu(i + 1, i, env->mem);
            write_pc(*cpu, code);
            ready.arrive_and_wait();
            do {
                REQUIRE(run(*cpu) == 0);
            } while (!cpu->svc_called);
        });
    }
    for (auto &worker : workers) worker.join();
    REQUIRE(*Ptr<uint32_t>(data).get(env->mem) == count * iterations);
    std::puts("4 guest LDREX/STREX loops: exact 400000 increments");

    const auto entry = code + 0x200, callback = code + 0x300, stub = code + 0x400;
    guest_thread_fixture::Arm module(entry);
    module.emit(0xe92d4010); // push {r4,lr}
    module.call(stub);
    module.emit(0xe2800001); // callback result + 1
    module.emit(0xe8bd8010); // pop {r4,pc}
    module.finish(env->mem);
    guest_thread_fixture::Arm cb(callback);
    cb.constant(0, 42);
    cb.emit(0xe12fff1e); // bx lr
    cb.finish(env->mem);
    const uint32_t import[] = {0xef000000, 0xe1a0f00e, 0x12345678};
    REQUIRE(mem_write(env->mem, stub, import, sizeof(import)));
    unsigned callbacks = 0;
    REQUIRE(env->kernel.init(env->mem, [&](CPUState &cpu, uint32_t nid, SceUID tid) {
        REQUIRE(nid == 0x12345678);
        const auto thread = env->kernel.get_thread(tid);
        REQUIRE(thread);
        const auto pc = read_pc(cpu), lr = read_lr(cpu), sp = read_sp(cpu);
        REQUIRE(thread->run_callback(callback, {}) == 42);
        REQUIRE(read_pc(cpu) == pc && read_lr(cpu) == lr && read_sp(cpu) == sp);
        write_reg(cpu, 0, 42);
        ++callbacks;
    }, false));
    env->kernel.make_cpu = make_cpu;
    for (unsigned reuse = 0; reuse < 8; ++reuse) {
        auto thread = env->kernel.create_thread(env->mem, "module start", Ptr<const void>(entry));
        REQUIRE(thread);
        REQUIRE(thread->run_guest_function(entry) == 43);
        REQUIRE(thread->run_guest_function(entry) == 43);
        thread->exit_delete(false);
        std::unique_lock<std::mutex> lock(env->kernel.mutex);
        REQUIRE(env->kernel.thread_deleted_cond.wait_for(lock, std::chrono::seconds(5), [&] {
            return !env->kernel.threads.contains(thread->id);
        }));
        // The CPU's last owner is the loader; its code slots were already
        // released on the guest Worker before the core number was recycled.
    }
    REQUIRE(callbacks == 16);
    env->kernel.deinit(env->mem);
    deinit_mem(env->mem);
    std::puts("module return, nested callback, restart and Worker reuse passed");
}
