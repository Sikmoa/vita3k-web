// Browser retail-app launch path: VFS-backed multi-module load.
//
// run_vita() (vita_runtime.cpp) boots ONE homebrew image with no filesystem.
// Real games need the desktop load order instead: VFS device paths, title
// license, load_module() for eboot + firmware/app sysmodules (which also
// runs each file through decrypt_fself), module_start for preloaded
// libraries, then the main thread. This TU implements exactly that chain
// against the staged content directory and NOTHING else: no package
// installer, no PFS crypto, no firmware download. Staging (host tool or JS
// MEMFS upload) must provide, under <vita_fs>/:
//
//   ux0/app/<title>/eboot.bin + sce_module/*.suprx + game data
//   vs0/sys/external/*.suprx + os0:kd/*.skprx (from the user's firmware)
//   ux0/license/<title>/*.rif (or the 16-byte klic via the setter below)
//
// SELF segments must already be decrypted offline (see
// vita_self_decrypt.cpp); still-encrypted files fail loudly at load.
//
// Backend-agnostic: thread creation, run_loop(true) and HLE dispatch are the
// production paths shared with run_vita(), so this works under both
// InterpreterCPU and WasmJitCPU.
#include <cpu/functions.h>
#include <cpu/impl/interpreter_cpu.h>
#ifdef VITA3K_USE_WASM_JIT
#include <cpu/impl/wasm_jit_cpu.h>
#include "guest_thread_runtime.h"
#endif
#include <display/state.h>
#include <emuenv/state.h>
#include <io/functions.h>
#include <io/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <modules/module_parent.h>
#include <nids/functions.h>
#include <packages/license.h>
#include <emscripten/emscripten.h>

#include "vita_runtime.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Null audio sink (hle_audio_null.cpp): no device in the web runtime.
void vita3k_web_install_null_audio(struct AudioState &audio);

namespace {

// Staged launch configuration. Plain POD + std::string; set from JS/bench
// before vita3k_web_run_app(). One app per process, like the desktop flow.
struct AppLaunchConfig {
    std::string vita_fs = "/vita";
    std::string title_id;
    std::string app_path;
    std::uint8_t klic[16] = {};
    bool has_klic = false;
};

AppLaunchConfig &launch_config() {
    static AppLaunchConfig config;
    return config;
}

// Guest dispatch budget for the browser run loop. The desktop-shaped default
// is the bench harness budget; the browser harness raises it so a retail app
// can reach its first presented frame instead of stopping mid-load. The
// VITA3K_BENCH_DISPATCHES environment variable still wins when set (the Node
// bench path), because it is the declared override for that harness.
std::size_t &dispatch_budget_slot() {
    static std::size_t budget = 100000;
    return budget;
}

std::size_t dispatch_budget() {
    if (const char *env = std::getenv("VITA3K_BENCH_DISPATCHES")) {
        const unsigned long parsed = std::strtoul(env, nullptr, 10);
        if (parsed > 0) return static_cast<std::size_t>(parsed);
    }
    return dispatch_budget_slot();
}

// Desktop preload order (interface.cpp load_app_impl, minus taihen/patches):
// HLE-only modules (libnet, np_*, libime, ...) self-skip inside load_module,
// so attempting them unconditionally matches desktop behavior.
constexpr const char *kPreloadChain[] = {
    "app0:eboot.bin",
    "os0:kd/bootimage.skprx",
    "os0:kd/sysmodule.skprx",
    "app0:sce_module/libc.suprx",
    "app0:sce_module/libfios2.suprx",
    "vs0:sys/external/libSceFt2.suprx",
    "vs0:sys/external/libpvf.suprx",
    "vs0:sys/external/libhttp.suprx",
    "vs0:sys/external/libssl.suprx",
};

// Desktop runs module_start for every preloaded library except the main
// executable (run_app starts those after thread creation).
bool needs_module_start(const char *path) {
    return std::strcmp(path, "app0:eboot.bin") != 0
        && std::strcmp(path, "os0:kd/bootimage.skprx") != 0;
}

// Cooperative equivalent of start_module(): desktop spawns an SDL host thread
// per module via KernelState::create_thread + run_guest_function, but the
// browser target has no host threads (SDL_CreateThread/WaitSemaphore would
// spin forever). Drive the start entry synchronously on a temporary thread
// with the same init/start/run_loop(true) sequence as the main thread.
std::uint32_t run_module_entry(EmuEnvState &env, const SceKernelModuleInfo &info,
    Ptr<const void> entry, SceSize args, Ptr<const void> argp) {
    auto module_thread = std::make_shared<ThreadState>(
        env.kernel.get_next_uid(), env.kernel, env.mem);
    if (module_thread->init(info.module_name, entry,
            SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
            SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr)
        < 0) {
        std::printf("[vita3k-web] module thread init failed for %s\n", info.module_name);
        return 0xDEADDEAD;
    }
    env.kernel.threads.emplace(module_thread->id, module_thread);
#ifdef VITA3K_USE_WASM_JIT
    // Module-start threads must use the same backend as the main process;
    // ThreadState::init creates the default CPU implementation first.
    const auto initial = save_context(*module_thread->cpu);
    const auto tls = read_tpidruro(*module_thread->cpu);
    const auto core = get_processor_id(*module_thread->cpu);
    module_thread->cpu->cpu = std::make_unique<WasmJitCPU>(module_thread->cpu.get(), core);
    load_context(*module_thread->cpu, initial);
    write_tpidruro(*module_thread->cpu, tls);
#endif
    if (module_thread->start(args, argp.cast<void>()) < 0) {
        std::printf("[vita3k-web] module thread start failed for %s\n", info.module_name);
        env.kernel.threads.erase(module_thread->id);
        return 0xDEADDEAD;
    }
    // Dynamic module starts nest inside the importing thread's run_loop.
    // Restore its current-CPU pointer before its HLE dispatcher resumes.
    auto *previous_cpu = get_current_cpu_state();
    module_thread->run_loop(true);
    set_current_cpu_state(previous_cpu);
    const std::uint32_t result = module_thread->returned_value;
    env.kernel.threads.erase(module_thread->id);
    module_thread.reset();
    return result;
}

} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE
void vita3k_web_set_app_paths(const char *vita_fs, const char *title_id, const char *app_path) {
    auto &config = launch_config();
    if (vita_fs && *vita_fs) config.vita_fs = vita_fs;
    config.title_id = title_id ? title_id : "";
    config.app_path = app_path && *app_path ? app_path : config.title_id;
    std::printf("[vita3k-web] app paths: vita_fs=%s title=%s app=%s\n",
        config.vita_fs.c_str(), config.title_id.c_str(), config.app_path.c_str());
}

extern "C" EMSCRIPTEN_KEEPALIVE
void vita3k_web_set_dispatch_budget(std::uint32_t budget) {
    if (!budget) return;
    dispatch_budget_slot() = budget;
    std::printf("[vita3k-web] guest dispatch budget: %u\n", budget);
}

extern "C" EMSCRIPTEN_KEEPALIVE
void vita3k_web_set_license_key(const std::uint8_t *key16) {
    auto &config = launch_config();
    if (!key16) {
        config.has_klic = false;
        std::memset(config.klic, 0, sizeof(config.klic));
        std::puts("[vita3k-web] license key cleared (pre-decrypted content only)");
        return;
    }
    std::memcpy(config.klic, key16, sizeof(config.klic));
    config.has_klic = true;
    std::puts("[vita3k-web] license klic staged");
}

// Implementation shared by the exported entry point below, which reports its
// result through the host exit hook.
static int run_app_impl() {
    const auto &config = launch_config();
    if (config.title_id.empty()) {
        std::fprintf(stderr, "[vita3k-web] run_app: no title staged (call vita3k_web_set_app_paths first)\n");
        return -1;
    }
    auto env = std::make_unique<EmuEnvState>();
    if (!init(env->mem, true)) return -2;
    env->vita_fs_path = config.vita_fs;
    env->io.title_id = config.title_id;
    env->io.app_path = config.app_path;
    env->io.user_id = "00";
    env->io.savedata = config.title_id;
    auto &license = env->license.rif[config.title_id];
    license = SceNpDrmLicense{};
    vita3k_web_install_null_audio(env->audio);
    if (config.has_klic) std::memcpy(license.key, config.klic, sizeof(license.key));

    ThreadStatePtr thread;
#ifdef VITA3K_USE_WASM_JIT
    vita3k::web::GuestThreadRuntime runtime;
#endif
    // Same vblank headroom as run_vita: without it the guest spends real time
    // in frame pacing instead of executing.
    env->display.fast_vblank = vita3k_web_fast_vblank_enabled();
    bool exited = false;
    int exit_code = 0;
    unsigned imports = 0;
    // Wall time charged to the HLE callback (import dispatch + module body).
    // Subtracting it, the JIT phase counters and the wall clock separates
    // "import handling" from "JIT compile" and "JIT dispatch".
    double hle_ms = 0.0;
    std::unordered_map<std::uint32_t, std::pair<unsigned, double>> hle_nids;
    const char *trace_option = std::getenv("VITA3K_TRACE_HLE");
    const bool trace_hle = trace_option && std::strcmp(trace_option, "1") == 0;
#ifdef VITA3K_USE_WASM_JIT
    // Guest-rate diagnosis. The retail rate is the product of JIT compilation
    // (emit/install) and execution (run_js_calls), so report instructions over
    // wall time along with the counters that explain a stall: compiled blocks
    // and regions, cache hits, invalidations (self-modifying code) and the
    // slow-path memory breakdown in the per-CPU profile.
    const auto jit_started = std::chrono::steady_clock::now();
    const auto jit_report = [&](const char *tag, bool verbose) {
        std::uint64_t total = 0, hottest = 0;
        for (const auto &[id, active] : env->kernel.threads) {
            if (!active || !active->cpu || !active->cpu->cpu) continue;
            const auto count = static_cast<WasmJitCPU &>(*active->cpu->cpu).instructions_executed();
            total += count;
            if (count > hottest) hottest = count;
        }
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - jit_started).count();
        std::printf("[vita3k-web] jit[%s] elapsed=%.1fs insns=%llu rate_mips=%.2f imports=%u threads=%zu hle_ms=%.1f\n",
            tag, seconds, static_cast<unsigned long long>(total),
            seconds > 0.0 ? static_cast<double>(total) / seconds / 1e6 : 0.0,
            imports, env->kernel.threads.size(), hle_ms);
        if (!verbose) return;
        // Which import owns the wall clock, and is it many cheap calls (spin) or
        // few expensive ones (blocking/decompression)? Sorted by total ms.
        std::vector<std::pair<std::uint32_t, std::pair<unsigned, double>>> ranked(
            hle_nids.begin(), hle_nids.end());
        std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
            return a.second.second > b.second.second;
        });
        // Ascending on purpose: the log tail keeps the end of the block, so the
        // largest consumer is the last line printed.
        const std::size_t shown = ranked.size() < 8 ? ranked.size() : 8;
        for (std::size_t i = shown; i > 0; --i) {
            const auto &[nid, stats] = ranked[i - 1];
            std::printf("[vita3k-web] jit hle NID=%08x %-28s calls=%u ms=%.1f avg_us=%.0f\n",
                nid, import_name(nid), stats.first, stats.second,
                stats.first ? stats.second * 1000.0 / stats.first : 0.0);
        }
        for (const auto &[id, active] : env->kernel.threads) {
            if (!active || !active->cpu || !active->cpu->cpu) continue;
            auto &jit = static_cast<WasmJitCPU &>(*active->cpu->cpu);
            if (jit.instructions_executed() == 0) continue;
            std::printf("[vita3k-web] jit thread=%d %s insns=%llu blocks=%llu regions=%llu hits=%llu invalidated=%llu\n",
                id, active->name.c_str(),
                static_cast<unsigned long long>(jit.instructions_executed()),
                static_cast<unsigned long long>(jit.compiled_blocks()),
                static_cast<unsigned long long>(jit.regions_formed()),
                static_cast<unsigned long long>(jit.cache_hits()),
                static_cast<unsigned long long>(jit.invalidated_blocks()));
            if (jit.instructions_executed() == hottest)
                std::printf("[vita3k-web] jit profile %d: %s\n", id, jit.get_profile().c_str());
        }
    };
#endif
    struct Cleanup {
        EmuEnvState &env;
        ThreadStatePtr &thread;
#ifdef VITA3K_USE_WASM_JIT
        vita3k::web::GuestThreadRuntime &runtime;
#endif
        ~Cleanup() {
#ifdef VITA3K_USE_WASM_JIT
            // Never release guest memory while a suspended HLE frame refers to it.
            if (!runtime.shutdown()) std::terminate();
#endif
            if (thread) { env.kernel.threads.erase(thread->id); thread.reset(); }
            env.kernel.deinit(env.mem);
            deinit_mem(env.mem);
        }
    } cleanup{*env, thread
#ifdef VITA3K_USE_WASM_JIT
        , runtime
#endif
    };
    try {
        if (!env->kernel.init(env->mem, [&](CPUState &cpu, uint32_t nid, SceUID tid) {
                const unsigned import_sequence = ++imports;
                if (trace_hle) {
                    std::fprintf(stderr, "[vita3k-web] HLE enter #%u tid=%d NID=%08x PC=%08x name=%s\n",
                        import_sequence, tid, nid, read_pc(cpu), import_name(nid));
                    std::fflush(stderr);
                }
                if (imports < 400 || imports % 500 == 0) {
                    std::printf("[vita3k-web] Vita import #%u: %s NID=%08x PC=%08x\n",
                        imports, import_name(nid), nid, read_pc(cpu));
#ifdef VITA3K_USE_WASM_JIT
                    jit_report("progress", true);
#endif
                }
                const auto hle_started = std::chrono::steady_clock::now();
                ::call_import(*env, cpu, nid, tid);
                const double hle_cost = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - hle_started).count();
                hle_ms += hle_cost;
                auto &hle_slot = hle_nids[nid];
                ++hle_slot.first;
                hle_slot.second += hle_cost;
                if (trace_hle) {
                    std::fprintf(stderr, "[vita3k-web] HLE return #%u tid=%d NID=%08x PC=%08x\n",
                        import_sequence, tid, nid, read_pc(cpu));
                    std::fflush(stderr);
                }
                if (nid == 0x7A410B64 /* sceDisplaySetFrameBuf */
                    || nid == 0xF51523CB /* _sceDisplaySetFrameBuf */)
                    vita3k_web_present_frame(*env);
                // Module-start imports run before the main thread exists.
                // Stop the importing thread, not a possibly-null main thread.
                if (!env->missing_nids.empty()) {
                    const auto active = env->kernel.threads.find(tid);
                    if (active != env->kernel.threads.end()) active->second->exit_delete(false);
                }
            }, false)) return -3;
        env->kernel.process_exit_callback = [&](int status, std::optional<AppLaunchRequest>) {
            exited = true;
            exit_code = status;
            for (auto &[id, active] : env->kernel.threads)
                active->exit_delete(false);
        };
        env->kernel.run_module_entry = [&](const SceKernelModuleInfo &info,
            Ptr<const void> entry, SceSize args, Ptr<const void> argp) {
            return run_module_entry(*env, info, entry, args, argp);
        };
#ifdef VITA3K_USE_WASM_JIT
        if (!runtime.attach(*env)) return -11;
#endif
        init_device_paths(env->io);
        init_savedata_app_path(env->io, env->vita_fs_path);
        init_libraries(*env);
        init_exported_vars(*env);

        SceUID eboot_uid = -1;
        for (const char *path : kPreloadChain) {
            const SceUID uid = load_module(*env, path);
            if (uid < 0) {
                std::printf("[vita3k-web] load_module %s failed: %08x (continuing, desktop-tolerant)\n",
                    path, static_cast<std::uint32_t>(uid));
                if (!std::strcmp(path, "app0:eboot.bin")) return -4;
                continue;
            }
            std::printf("[vita3k-web] load_module %s -> uid %d (%s)\n",
                path, uid, env->kernel.loaded_modules[uid]->info.module_name);
            if (!std::strcmp(path, "app0:eboot.bin")) eboot_uid = uid;
            if (needs_module_start(path)) {
                const auto &info = env->kernel.loaded_modules[uid]->info;
                if (info.start_entry) {
                    const std::uint32_t result = start_module(*env, info);
                    std::printf("[vita3k-web] module_start %s returned %08x\n", info.module_name, result);
                    if (!env->missing_nids.empty()) {
                        for (const auto nid : env->missing_nids)
                            std::printf("[vita3k-web] missing NID=%08x (%s)\n", nid, import_name(nid));
                        return -8;
                    }
                    if (exited) return exit_code;
                    if (result == 0xDEADDEAD || static_cast<std::int32_t>(result) < 0)
                        return -10;
                }
            }
        }
        const auto &module = env->kernel.loaded_modules.at(eboot_uid)->info;
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
#ifdef VITA3K_USE_WASM_JIT
        thread = env->kernel.create_thread(env->mem, config.title_id.c_str(), module.start_entry,
            priority, affinity, stack_size, nullptr);
        if (!thread) return -6;
        std::puts("[vita3k-web] CPU backend: WasmJitCPU with guest fibers (single logical CPU)");
#else
        thread = std::make_shared<ThreadState>(env->kernel.get_next_uid(), env->kernel, env->mem);
        if (thread->init(config.title_id.c_str(), module.start_entry, priority, affinity, stack_size, nullptr) < 0) return -6;
        std::puts("[vita3k-web] CPU backend: InterpreterCPU");
        env->kernel.threads.emplace(thread->id, thread);
#endif
        env->main_thread_id = thread->id;
        if (thread->start(0, Ptr<void>{}, true) < 0) return -7;
#ifdef VITA3K_USE_WASM_JIT
        vita3k::web::GuestThreadRuntime::Progress progress;
        std::size_t dispatched = 0;
        const std::size_t budget = dispatch_budget();
        std::size_t pc_sample_every = 0;
        if (const char *sample_env = std::getenv("VITA3K_BENCH_PC_SAMPLE")) {
            const unsigned long parsed = std::strtoul(sample_env, nullptr, 10);
            if (parsed > 0) pc_sample_every = static_cast<std::size_t>(parsed);
        }
        std::size_t pc_sample_next = pc_sample_every;
        do {
            progress = runtime.resume(256);
            dispatched += progress.dispatches;
            if (pc_sample_every && dispatched >= pc_sample_next) {
                pc_sample_next = dispatched + pc_sample_every;
                std::fprintf(stderr, "[vita3k-web] pc-sample dispatched=%zu threads=", dispatched);
                for (const auto &[tid, t] : env->kernel.threads) {
                    if (t && t->cpu)
                        std::fprintf(stderr, " %d:%08x", tid, read_pc(*t->cpu));
                }
                std::fprintf(stderr, "\n");
            }
        } while (!exited && env->missing_nids.empty() && !progress.failed
            && !progress.idle && dispatched < budget);
        std::printf("[vita3k-web] Guest scheduler: dispatches=%zu runnable=%zu waiting=%zu dormant=%zu failed=%zu idle=%d\n",
            dispatched, progress.runnable, progress.waiting, progress.dormant, progress.failed, progress.idle);
#else
        thread->run_loop(true);
#endif
#ifdef VITA3K_USE_WASM_JIT
        jit_report("final", true);
#endif
        std::printf("[vita3k-web] Vita result: process_exit=%d code=%d imports=%u missing_nids=%zu PC=%08x\n",
            exited, exit_code, imports, env->missing_nids.size(), read_pc(*thread->cpu));
        for (const auto nid : env->missing_nids)
            std::printf("[vita3k-web] missing NID=%08x (%s)\n", nid, import_name(nid));
        return exited && env->missing_nids.empty() ? exit_code : -8;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[vita3k-web] Vita app runtime error: %s\n", error.what());
        return -9;
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE
int vita3k_web_run_app() {
    // The retail path suspends inside its run loop (ASYNCIFY), so the JS call
    // site does not receive this return value: report the outcome through the
    // same host hook the homebrew path uses (vita3kWebOnExit -> worker.js
    // 'vita-exit'). Without it the host waits out its whole deadline after the
    // guest has already stopped, and the real exit code is invisible.
    const int code = run_app_impl();
    vita3k_web_notify_exit(code);
    return code;
}
