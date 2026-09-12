#include <cpu/functions.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <mem/ptr.h>

#include <cstdio>
#include <cstdlib>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (false)

int main() {
    MemState memory;
    CHECK(::init(memory, true));
    const Address code = try_alloc_at(memory, 0x81000000, 0x1000, "interpreter runtime code");
    CHECK(code == 0x81000000);
    auto *instructions = Ptr<uint32_t>(code).get(memory);
    CHECK(instructions != nullptr);
    instructions[0] = 0xe3a0002au; // mov r0, #42
    instructions[1] = 0xe3a07000u; // mov r7, #0 (test-only service marker)
    instructions[2] = 0xef000000u; // svc #0
    // The instruction after SVC is the normal Vita import/NID slot.
    instructions[3] = 0x12345678u;

    bool svc_seen = false;
    uint32_t svc_number = 0;
    bool import_nid_ok = false;
    bool register_ok = false;
    KernelState kernel;
    ThreadStatePtr thread;
    CHECK(kernel.init(memory, [&](CPUState &cpu, uint32_t nid, SceUID) {
        svc_seen = true;
        svc_number = cpu.svc;
        import_nid_ok = nid == 0x12345678u;
        register_ok = read_reg(cpu, 0) == 42;
        thread->exit_delete(false);
    }, false));

    thread = std::make_shared<ThreadState>(kernel.get_next_uid(), kernel, memory);
    CHECK(thread->init("interpreter-runtime", Ptr<const void>(code), SCE_KERNEL_DEFAULT_PRIORITY,
        SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_DEFAULT, nullptr) == 0);
    CHECK(thread->cpu != nullptr);
    CHECK(read_pc(*thread->cpu) == 0);
    CHECK(thread->start(0, Ptr<void>(0)) == SCE_KERNEL_OK);
    thread->run_loop();
    CHECK(svc_seen && svc_number == 0 && import_nid_ok && register_ok);
    CHECK(read_reg(*thread->cpu, 0) == 42);

    kernel.threads.erase(thread->id);
    thread.reset();
    kernel.deinit(memory);
    deinit_mem(memory);
    std::puts("Interpreter runtime convergence checks passed");
    return 0;
}
