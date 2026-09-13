// Integration probe for the authoritative Vita loader and runtime. This stops
// at dispatch; it is NOT an HLE implementation or a homebrew launch success.
#include <cpu/functions.h>
#include <kernel/load_self.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    std::ifstream stream(argv[1], std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(stream)), {});
    if (bytes.empty()) return 2;
    MemState mem;
    if (!init(mem, true)) return 3;
    KernelState kernel;
    ThreadStatePtr thread;
    bool reached = false;
    if (!kernel.init(mem, [&](CPUState &cpu, uint32_t nid, SceUID) {
            std::printf("Vita dispatch boundary: PC=%08x SVC=%u NID=%08x\n", read_pc(cpu), cpu.svc, nid);
            reached = true;
            thread->exit_delete(false);
        }, false)) return 4;
    auto uid = load_self_sized(kernel, mem, bytes.data(), bytes.size(), "app0:eboot.bin", {});
    if (uid < 0) { std::printf("load_self_sized failed: %08x\n", uint32_t(uid)); return 5; }
    const auto &module = kernel.loaded_modules.at(uid)->info;
    std::printf("Vita module: %.28s, entry=%08x\n", module.module_name, module.start_entry.address());
    thread = std::make_shared<ThreadState>(kernel.get_next_uid(), kernel, mem);
    if (thread->init("Vita loader probe", module.start_entry.cast<const void>(), SCE_KERNEL_DEFAULT_PRIORITY,
            SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr) < 0) return 6;
    kernel.threads.emplace(thread->id, thread);
    if (thread->start(0, Ptr<void>{}) < 0) return 7;
    thread->run_loop(true);
    std::printf("Loader probe stopped: PC=%08x r0=%08x dispatch=%d\n", read_pc(*thread->cpu), read_reg(*thread->cpu, 0), reached);
    kernel.threads.erase(thread->id);
    thread.reset();
    kernel.deinit(mem);
    deinit_mem(mem);
    return reached ? 0 : 8;
}
