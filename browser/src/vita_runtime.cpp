// Browser byte transport into Vita3K's production loader, thread and HLE paths.
// This is a synchronous, non-graphical launch entrypoint, not another kernel.
#include <cpu/functions.h>
#include <cpu/impl/interpreter_cpu.h>
#include <display/state.h>
#ifdef VITA3K_USE_WASM_JIT
#include <cpu/impl/wasm_jit_cpu.h>
#endif
#include <emuenv/state.h>
#include <kernel/load_self.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <modules/module_parent.h>
#include <nids/functions.h>
#include <emscripten/emscripten.h>

#include "vita_runtime.h"

#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>

static_assert(sizeof(SceSize) == 4 && sizeof(SceUIntPtr) == 4 && sizeof(SceIntPtr) == 4,
    "The Vita HLE ABI must not inherit the host pointer/size width");

// Completion callback. With ASYNCIFY, a suspended call's return value is not
// delivered to the original JS call site, so the runtime reports its final
// exit code through this host hook instead. Hosts that receive the return
// value directly (non-suspending Node builds) may ignore it.
EM_JS(void, vita3k_web_notify_exit, (int code), {
    if (typeof vita3kWebOnExit === 'function') vita3kWebOnExit(code);
});

// Frame presentation hook (contract for the display bridge):
// - generation: monotonically increasing frame/update generation
// - data: TIGHT RGBA rows (width*height*4 bytes) at a Wasm scratch pointer.
// The receiving host must copy the view synchronously; the scratch buffer is
// reused by the next frame. Pixel format is fixed RGBA8; A8B8G8R8 guest
// framebuffers are converted/tightened in Wasm before this call.
EM_JS(void, vita3k_web_post_frame_hook, (int generation, int width, int height, const uint8_t *ptr), {
    if (typeof vita3kWebOnFrame === 'function')
        vita3kWebOnFrame(generation, width, height, Module['vita3kHostBytes'](ptr, width * height * 4));
});

// Fixed-width input length, native host pointer. Dedicated exports avoid the
// special Number wrappers Emscripten may apply to its built-in malloc/free.
extern "C" EMSCRIPTEN_KEEPALIVE
uint8_t *vita3k_web_alloc_input(uint32_t size) {
    return static_cast<uint8_t *>(std::malloc(size));
}
extern "C" EMSCRIPTEN_KEEPALIVE
void vita3k_web_free_input(uint8_t *pointer) {
    std::free(pointer);
}

// Total guest instructions of the most recent run (benchmarking).
static uint64_t vita3k_web_bench_instructions = 0;
static bool vita3k_web_trace_cpu = false;

extern "C" EMSCRIPTEN_KEEPALIVE
void vita3k_web_set_trace(int enabled) { vita3k_web_trace_cpu = enabled != 0; }

// Debug-only vblank headroom mode, mirrored into DisplayState per run.
// See DisplayState::fast_vblank; default off, enabled by ?fastvblank=1.
static bool vita3k_web_fast_vblank = false;
extern "C" EMSCRIPTEN_KEEPALIVE
void vita3k_web_set_fast_vblank(int enabled) { vita3k_web_fast_vblank = enabled != 0; }

static void trace_cpu(CPUState &cpu, uint32_t nid) {
    if (!vita3k_web_trace_cpu) return;
    const auto context = save_context(cpu);
    std::printf("[vita3k-web] CPU state: {\"nid\":%u,\"r\":[", nid);
    for (unsigned i = 0; i < 16; ++i)
        std::printf("%s%u", i ? "," : "", context.cpu_registers[i]);
    std::printf("],\"cpsr\":%u,\"fpscr\":%u,\"tpidruro\":%u,\"svc\":%u,\"fpu\":[",
        context.cpsr, context.fpscr, read_tpidruro(cpu), cpu.svc);
    for (unsigned i = 0; i < 64; ++i)
        std::printf("%s%u", i ? "," : "", std::bit_cast<uint32_t>(context.fpu_registers[i]));
    std::puts("]}");
}

extern "C" EMSCRIPTEN_KEEPALIVE
uint64_t vita3k_web_last_run_instructions() {
    return vita3k_web_bench_instructions;
}

static int run_vita(const uint8_t *bytes, uint32_t size) {
    if (!bytes || !size) return -1;
    auto env = std::make_unique<EmuEnvState>();
    if (!init(env->mem, true)) return -2;
    std::printf("[vita3k-web] memory model: %s; host pointer bits=%zu\n",
        env->mem.direct_host_memory ? "wasm64-direct" : "wasm32-sparse", sizeof(void *) * 8);
    if (env->mem.direct_host_memory)
        std::printf("[vita3k-web] guest window: [0x%llx,0x%llx); runtime allocation below 4 GiB\n",
            static_cast<unsigned long long>(vita3k::memory::guest_window_base),
            static_cast<unsigned long long>(vita3k::memory::guest_window_end));
    env->display.fast_vblank = vita3k_web_fast_vblank;
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
                trace_cpu(cpu, nid);
                ::call_import(*env, cpu, nid, tid);
                // Present exactly once per real sceDisplaySetFrameBuf call: one
                // frame/update generation, matching the real API semantics.
                if (nid == 0x7A410B64 /* sceDisplaySetFrameBuf */
                    || nid == 0xF51523CB /* _sceDisplaySetFrameBuf */)
                    vita3k_web_present_frame(*env);
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
#ifdef VITA3K_USE_WASM_JIT
        // Keep production ThreadState initialization, then transfer its CPU
        // state into the explicitly selected experimental backend.
        const auto initial = save_context(*thread->cpu);
        const auto tls = read_tpidruro(*thread->cpu);
        const auto core = get_processor_id(*thread->cpu);
        thread->cpu->cpu = std::make_unique<WasmJitCPU>(thread->cpu.get(), core);
        load_context(*thread->cpu, initial);
        write_tpidruro(*thread->cpu, tls);
        std::puts("[vita3k-web] CPU backend: WasmJitCPU (no fallback)");
#else
        std::puts("[vita3k-web] CPU backend: InterpreterCPU");
#endif
        env->kernel.threads.emplace(thread->id, thread);
        env->main_thread_id = thread->id;
        if (thread->start(0, Ptr<void>{}, true) < 0) return -7;
        thread->run_loop(true);
        if (const auto *interp = dynamic_cast<const InterpreterCPU *>(thread->cpu->cpu.get()))
            vita3k_web_bench_instructions = interp->instructions_executed();
#ifdef VITA3K_USE_WASM_JIT
        if (const auto *jit = dynamic_cast<const WasmJitCPU *>(thread->cpu->cpu.get())) {
            vita3k_web_bench_instructions = jit->instructions_executed();
            std::printf("[vita3k-web] JIT stats: instructions=%llu compiled=%llu hits=%llu invalidated=%llu regions=%llu\n",
                (unsigned long long)jit->instructions_executed(), (unsigned long long)jit->compiled_blocks(),
                (unsigned long long)jit->cache_hits(), (unsigned long long)jit->invalidated_blocks(),
                (unsigned long long)jit->regions_formed());
            std::printf("[vita3k-web] JIT profile: %s\n", jit->get_profile().c_str());
        }
#endif
        std::printf("[vita3k-web] Vita result: process_exit=%d code=%d imports=%u missing_nids=%zu PC=%08x\n",
            exited, exit_code, imports, env->missing_nids.size(), read_pc(*thread->cpu));
        return exited && env->missing_nids.empty() ? exit_code : -8;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[vita3k-web] Vita runtime error: %s\n", error.what());
        return -9;
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE
int vita3k_web_run_vita(const uint8_t *bytes, uint32_t size) {
    const auto started = std::chrono::steady_clock::now();
    vita3k_web_bench_instructions = 0;
    const int code = run_vita(bytes, size);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::printf("[vita3k-web] Vita benchmark: instructions=%llu elapsed_ms=%lld\n",
        static_cast<unsigned long long>(vita3k_web_bench_instructions),
        static_cast<long long>(elapsed));
    vita3k_web_notify_exit(code);
    return code;
}
