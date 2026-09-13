// Browser byte transport into Vita3K's production loader, thread and HLE paths.
// This is a synchronous, non-graphical launch entrypoint, not another kernel.
#include <cpu/functions.h>
#include <emuenv/state.h>
#include <kernel/load_self.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <modules/module_parent.h>
#include <nids/functions.h>
#include <emscripten/emscripten.h>

#include <cstdio>
#include <memory>
#include <optional>

extern "C" EMSCRIPTEN_KEEPALIVE
int vita3k_web_run_vita(const uint8_t *bytes, uint32_t size) {
    if (!bytes || !size) return -1;
    auto env = std::make_unique<EmuEnvState>();
    if (!init(env->mem, true)) return -2;
    ThreadStatePtr thread;
    bool exited = false;
    int exit_code = 0;
    unsigned imports = 0;
    struct Cleanup {
        EmuEnvState &env;
        ThreadStatePtr &thread;
        ~Cleanup() {
            // This host used a cooperative ThreadState, not an SDL host thread.
            // It has returned from run_loop before cleanup and must be removed
            // before kernel teardown waits for host-thread deletion notifications.
            if (thread) { env.kernel.threads.erase(thread->id); thread.reset(); }
            env.kernel.deinit(env.mem);
            deinit_mem(env.mem);
        }
    } cleanup{*env, thread};
    try {
        if (!env->kernel.init(env->mem, [&](CPUState &cpu, uint32_t nid, SceUID tid) {
                ++imports;
                std::printf("[vita3k-web] Vita import: %s NID=%08x PC=%08x\n", import_name(nid), nid, read_pc(cpu));
                ::call_import(*env, cpu, nid, tid);
                if (!env->missing_nids.empty()) thread->exit_delete(false);
            }, false)) return -3;
        env->kernel.process_exit_callback = [&](int status, std::optional<AppLaunchRequest>) {
            exited = true;
            exit_code = status;
            // request_process_exit is delivered from the real ExitProcess HLE.
            // Never synchronously join threads inside the HLE callback.
            thread->exit_delete(false);
        };
        init_libraries(*env);
        init_exported_vars(*env);
        const auto uid = load_self_sized(env->kernel, env->mem, bytes, size, "app0:eboot.bin", {});
        if (uid < 0) return -4;
        const auto &module = env->kernel.loaded_modules.at(uid)->info;
        std::printf("[vita3k-web] Vita module: %.28s entry=%08x\n", module.module_name, module.start_entry.address());
        if (!module.start_entry) return -5;
        SceInt32 priority = SCE_KERNEL_DEFAULT_PRIORITY_USER;
        SceInt32 stack_size = SCE_KERNEL_STACK_SIZE_USER_MAIN;
        SceInt32 affinity = SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT;
        if (const auto *param = env->kernel.process_param.get(env->mem)) {
            if (param->main_thread_priority) priority = *Ptr<SceInt32>(param->main_thread_priority).get(env->mem);
            if (param->main_thread_stacksize) stack_size = *Ptr<SceInt32>(param->main_thread_stacksize).get(env->mem);
            if (param->main_thread_cpu_affinity_mask) affinity = *Ptr<SceInt32>(param->main_thread_cpu_affinity_mask).get(env->mem);
        }
        thread = std::make_shared<ThreadState>(env->kernel.get_next_uid(), env->kernel, env->mem);
        if (thread->init("vita-homebrew-main", module.start_entry, priority, affinity, stack_size, nullptr) < 0) return -6;
        env->kernel.threads.emplace(thread->id, thread);
        env->main_thread_id = thread->id;
        if (thread->start(0, Ptr<void>{}, true) < 0) return -7;
        thread->run_loop(true);
        std::printf("[vita3k-web] Vita result: process_exit=%d code=%d imports=%u missing_nids=%zu PC=%08x\n",
            exited, exit_code, imports, env->missing_nids.size(), read_pc(*thread->cpu));
        return exited && env->missing_nids.empty() ? exit_code : -8;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[vita3k-web] Vita runtime error: %s\n", error.what());
        return -9;
    }
}
