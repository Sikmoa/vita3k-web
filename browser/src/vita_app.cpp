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
#endif
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

#include <bit>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

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
std::uint32_t run_module_start(EmuEnvState &env, const SceKernelModuleInfo &info) {
    auto module_thread = std::make_shared<ThreadState>(
        env.kernel.get_next_uid(), env.kernel, env.mem);
    if (module_thread->init(info.module_name, info.start_entry,
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
    if (module_thread->start(0, Ptr<void>{}) < 0) {
        std::printf("[vita3k-web] module thread start failed for %s\n", info.module_name);
        env.kernel.threads.erase(module_thread->id);
        return 0xDEADDEAD;
    }
    module_thread->run_loop(true);
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

extern "C" EMSCRIPTEN_KEEPALIVE
int vita3k_web_run_app() {
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
    if (config.has_klic) std::memcpy(license.key, config.klic, sizeof(license.key));

    ThreadStatePtr thread;
    bool exited = false;
    int exit_code = 0;
    unsigned imports = 0;
    struct Cleanup {
        EmuEnvState &env;
        ThreadStatePtr &thread;
        ~Cleanup() {
            if (thread) { env.kernel.threads.erase(thread->id); thread.reset(); }
            env.kernel.deinit(env.mem);
            deinit_mem(env.mem);
        }
    } cleanup{*env, thread};
    try {
        if (!env->kernel.init(env->mem, [&](CPUState &cpu, uint32_t nid, SceUID tid) {
                ++imports;
                if (imports < 400 || imports % 500 == 0)
                    std::printf("[vita3k-web] Vita import #%u: %s NID=%08x PC=%08x\n",
                        imports, import_name(nid), nid, read_pc(cpu));
                ::call_import(*env, cpu, nid, tid);
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
                    const std::uint32_t result = run_module_start(*env, info);
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
        thread = std::make_shared<ThreadState>(env->kernel.get_next_uid(), env->kernel, env->mem);
        if (thread->init(config.title_id.c_str(), module.start_entry, priority, affinity, stack_size, nullptr) < 0) return -6;
#ifdef VITA3K_USE_WASM_JIT
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
