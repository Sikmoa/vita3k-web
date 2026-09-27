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
//   os0/kd/registry.db0 (firmware registry template behind SceRegMgr reads)
//   ux0/license/<title>/*.rif (or the 16-byte klic via the setter below)
//
// SELF segments must already be decrypted offline (see
// vita_self_decrypt.cpp); still-encrypted files fail loudly at load.
//
// Backend-agnostic: thread creation, run_loop(true) and HLE dispatch are the
// production paths shared with run_vita(), so this works under both
// InterpreterCPU and WasmJitCPU.
#include <patch/patch.h>
#include <cpu/functions.h>
#include <cpu/impl/interpreter_cpu.h>
#ifdef VITA3K_USE_WASM_JIT
#include <cpu/impl/wasm_jit_cpu.h>
#include "guest_thread_runtime.h"
#include "gxm_webgpu_bridge.h"
#endif
#include <ctrl/state.h>
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
#include <regmgr/functions.h>
#include <emscripten/emscripten.h>

#include "ime_bridge.h"
#include "msg_dialog_bridge.h"
#include "vita_runtime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Null audio sink (hle_audio_null.cpp): no device in the web runtime.
void vita3k_web_install_null_audio(struct AudioState &audio);

// One event-loop turn without setTimeout's clamping: a Worker only commits its
// OffscreenCanvas frame and resolves GPU map requests between tasks.
EM_ASYNC_JS(void, web_yield_to_event_loop, (), {
    await new Promise(resolve => {
        const channel = new MessageChannel();
        channel.port1.onmessage = () => { channel.port1.close(); resolve(); };
        channel.port2.postMessage(0);
    });
});

// Host input (worker.js 'input'): SCE_CTRL_* button mask and stick axes in
// [-1, 1] (lx, ly, rx, ry), set between event-loop turns and applied to the
// guest pad by the run loop.
static std::uint32_t g_host_buttons = 0;
static std::array<float, 4> g_host_axes{};
static bool g_host_input_changed = false;

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

#ifdef VITA3K_USE_WASM_JIT
// Offline AOT build (VITA3K_AOT_BUILD=<out.wasm>): after the same module
// loads the app performs, collect executable ranges and function roots and
// write one ahead-of-time module (vita3k/cpu/src/wasmjit/AOT.md).
static int build_aot_image(EmuEnvState &env, const char *out_path) {
    WasmJitCPU::AotBuildSpec spec;
    const auto executable = [&](Address address) {
        return env.mem.page_permissions
            && (static_cast<uint8_t>(env.mem.page_permissions[address >> 12]) & static_cast<uint8_t>(MemPerm::Execute));
    };
    std::vector<uint64_t> seed_values;
    if (const char *seed_path = std::getenv("VITA3K_AOT_SEEDS")) {
        FILE *in = std::fopen(seed_path, "r");
        if (!in) {
            std::fprintf(stderr, "[vita3k-web] AOT seeds %s cannot be opened\n", seed_path);
            return -12;
        }
        unsigned long long value = 0;
        while (std::fscanf(in, "%llx", &value) == 1)
            seed_values.push_back(value);
        std::fclose(in);
    }
    // Code = each module's text segment (segment 0) up to its .ARM.exidx
    // end-of-code sentinel or its module info, past which the segment holds
    // read-only data (WasmJitCPU::aot_code_size).
    // Page permissions cannot tell text from data here (data pages are
    // mapped executable too), and a module with no unwind table, entry point
    // or executed seed (e.g. the bootimage container) contributes no code.
    for (const auto &[uid, module] : env.kernel.loaded_modules) {
        const auto &info = module->info;
        const auto &segment = info.segments[0];
        const Address base = segment.vaddr.address();
        if (!segment.memsz || !executable(base))
            continue;
        const uint32_t size = WasmJitCPU::aot_code_size(env.mem, base, static_cast<uint32_t>(segment.memsz),
                                  base + info.exidx_top.address(), base + info.exidx_btm.address(),
                                  module->info_segment_address.address() + module->info_offset)
            & ~1u;
        const bool has_seed = std::any_of(seed_values.begin(), seed_values.end(),
            [&](uint64_t value) { return static_cast<uint32_t>(value) - base < size; });
        const bool has_exidx = info.exidx_top.address() < info.exidx_btm.address();
        if (!has_seed && !has_exidx && !info.start_entry) {
            std::printf("[vita3k-web] AOT skips %s [%08x,+%x): no unwind table, entry or seed\n",
                info.module_name, base, size);
            continue;
        }
        spec.code.push_back({base, size});
        std::printf("[vita3k-web] AOT code %s [%08x,+%x)\n", info.module_name, base, size);
    }
    const auto in_code = [&](Address address) {
        for (const auto &range : spec.code)
            if (address - range.base < range.size)
                return true;
        return false;
    };
    std::size_t exidx_roots = 0, relocation_roots = 0, export_roots = 0, entry_roots = 0;
    const auto add = [&](Address address, std::size_t &counter) {
        const Address pc = address & ~1u;
        if (!in_code(pc) || ((address & 1) == 0 && (pc & 3)))
            return;
        spec.function_roots.push_back(WasmJitCPU::aot_location(address));
        ++counter;
    };
    for (const auto &[uid, module] : env.kernel.loaded_modules) {
        const auto &info = module->info;
        if (info.start_entry)
            add(info.start_entry.address(), entry_roots);
        if (info.stop_entry)
            add(info.stop_entry.address(), entry_roots);
        // .ARM.exidx function starts; the module info stores the table bounds
        // relative to the text segment.
        const Address text = info.segments[0].vaddr.address();
        for (const uint32_t function : WasmJitCPU::aot_exidx_functions(env.mem,
                 text + info.exidx_top.address(), text + info.exidx_btm.address()))
            add(function, exidx_roots);
        // Thumb code pointers: the absolute values the module's relocations
        // wrote (vtables, callback tables, literal pools, MOVW/MOVT pairs).
        // Words that merely look like Thumb code addresses are not pointers:
        // instruction halfword pairs, constants and non-module data such as
        // the bootimage container match too, and turn data into roots. ARM
        // targets cannot be told from data pointers into code; they come
        // from exidx, exports and seeds.
        for (const Address value : module->absolute_relocations)
            if (value & 1)
                add(value, relocation_roots);
    }
    // Function exports only: export_nids also maps variable exports, which
    // are data addresses.
    for (const auto &[key, address] : env.kernel.export_nids_by_lib)
        add(address, export_roots);
    spec.extra_entries = seed_values;
    const std::size_t seeds = seed_values.size();
    // Import stubs [svc #0; mov pc, lr; nid] whose NID this build cannot
    // service: every one is a future missing-NID stop once the guest calls it.
    std::map<std::uint32_t, unsigned> unserviced;
    for (const auto &range : spec.code) {
        std::vector<std::uint32_t> words(range.size / 4);
        if (!mem_read(env.mem, range.base, words.data(), words.size() * 4))
            continue;
        for (std::size_t i = 0; i + 2 < words.size(); ++i)
            if (words[i] == 0xEF000000u && words[i + 1] == 0xE1A0F00Eu && !has_hle_implementation(words[i + 2]))
                ++unserviced[words[i + 2]];
    }
    std::printf("[vita3k-web] AOT scan: %zu imported NIDs without an HLE implementation in this build\n", unserviced.size());
    for (const auto &[nid, stubs] : unserviced)
        std::printf("[vita3k-web]   unserviced NID=%08x %s\n", nid, import_name(nid));
    std::printf("[vita3k-web] AOT roots: entries=%zu exidx=%zu relocations=%zu exports=%zu seeds=%zu\n",
        entry_roots, exidx_roots, relocation_roots, export_roots, seeds);
    std::vector<std::uint8_t> image;
    std::string report;
    const auto started = std::chrono::steady_clock::now();
    const bool built = WasmJitCPU::build_aot(env.mem, spec, image, report);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("[vita3k-web] AOT build %s in %.1fs: %s\n", built ? "ok" : "FAILED", seconds, report.c_str());
    if (!built)
        return -12;
    FILE *out = std::fopen(out_path, "wb");
    if (!out || std::fwrite(image.data(), 1, image.size(), out) != image.size() || std::fclose(out) != 0) {
        std::fprintf(stderr, "[vita3k-web] AOT image %s cannot be written\n", out_path);
        return -12;
    }
    std::printf("[vita3k-web] AOT image -> %s (%zu bytes)\n", out_path, image.size());
    return 0;
}
#endif

// Implementation shared by the exported entry point below, which reports its
// result through the host exit hook.
extern "C" void vita3k_web_set_pad(std::uint32_t buttons, float lx, float ly, float rx, float ry);

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
    // The Vita gives applications three CPU cores. VITA3K_GUEST_CORES=1
    // restores the single-core scheduler (A/B measurement).
    unsigned guest_cores = 3;
    if (const char *cores = std::getenv("VITA3K_GUEST_CORES"))
        guest_cores = static_cast<unsigned>(std::strtoul(cores, nullptr, 10));
    vita3k::web::GuestThreadRuntime runtime(32768, 256 * 1024, 256 * 1024, guest_cores);
#endif
    // Same vblank headroom as run_vita: without it the guest spends real time
    // in frame pacing instead of executing.
    env->display.fast_vblank = vita3k_web_fast_vblank_enabled();
    // Desktop Vita3K's fps-hack option: display waits of N vblanks wait one,
    // so titles that pace at 30 FPS through the display API present at 60.
    if (const char *fps_hack = std::getenv("VITA3K_FPS_HACK"))
        env->display.fps_hack = std::strcmp(fps_hack, "1") == 0;
    bool exited = false;
    int exit_code = 0;
    unsigned imports = 0;
    // Wall time charged to the HLE callback (import dispatch + module body).
    // Subtracting it, the JIT phase counters and the wall clock separates
    // "import handling" from "JIT compile" and "JIT dispatch".
    double hle_ms = 0.0;
    unsigned frames_presented = 0;
    std::unordered_map<std::uint32_t, std::pair<unsigned, double>> hle_nids;
    // Last import (NID, PC) per thread under VITA3K_HLE_PROFILE: where a parked thread waits.
    std::unordered_map<SceUID, std::pair<std::uint32_t, Address>> last_import;
    // The last imports of every thread, printed when a guest thread fails.
    struct ImportRecord { unsigned sequence; SceUID tid; std::uint32_t nid; Address pc, lr; };
    std::array<ImportRecord, 128> recent_imports{};
    // Wall time the root loop spends asleep (every fiber parked) and in event-loop turns.
    double idle_ms = 0, yield_ms = 0;
    // Per-NID call counts and inclusive wall time (VITA3K_HLE_PROFILE=1): two
    // clock reads and a map update per import, so off by default.
    const bool hle_profile = std::getenv("VITA3K_HLE_PROFILE") != nullptr;
    auto last_report = std::chrono::steady_clock::now();
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
        double guest_ms = 0;
        for (const auto &[id, active] : env->kernel.threads)
            if (active && active->cpu && active->cpu->cpu)
                guest_ms += static_cast<WasmJitCPU &>(*active->cpu->cpu).run_ms();
        std::printf("[vita3k-web] jit[%s] elapsed=%.1fs insns=%llu rate_mips=%.2f imports=%u threads=%zu hle_ms=%.1f frames=%u guest_ms=%.0f idle_ms=%.0f yield_ms=%.0f\n",
            tag, seconds, static_cast<unsigned long long>(total),
            seconds > 0.0 ? static_cast<double>(total) / seconds / 1e6 : 0.0,
            imports, env->kernel.threads.size(), hle_ms, frames_presented, guest_ms, idle_ms, yield_ms);
        browser::gxm_timing_report();
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
        for (const auto &[id, active] : env->kernel.threads) {
            const auto last = last_import.find(id);
            if (!active || last == last_import.end()) continue;
            std::printf("[vita3k-web] thread=%d %s status=%d last_import=%s PC=%08x\n", id,
                active->name.c_str(), static_cast<int>(active->status),
                import_name(last->second.first), last->second.second);
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
                recent_imports[import_sequence % recent_imports.size()] = { import_sequence, tid, nid, read_pc(cpu), read_lr(cpu) };
                if (trace_hle) {
                    std::fprintf(stderr, "[vita3k-web] HLE enter #%u tid=%d NID=%08x PC=%08x name=%s args=%08x,%08x,%08x,%08x LR=%08x\n",
                        import_sequence, tid, nid, read_pc(cpu), import_name(nid),
                        read_reg(cpu, 0), read_reg(cpu, 1), read_reg(cpu, 2), read_reg(cpu, 3), read_lr(cpu));
                    std::fflush(stderr);
                }
                // Boot imports are traced one by one; afterwards progress is
                // reported on a wall-clock period, not per import (retail
                // titles make 100k+ imports per second).
                bool report = imports < 400;
                if (!report && (imports & 1023) == 0) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_report >= std::chrono::seconds(5)) {
                        last_report = now;
                        report = true;
                    }
                }
                if (report) {
                    std::printf("[vita3k-web] Vita import #%u: %s NID=%08x PC=%08x\n",
                        imports, import_name(nid), nid, read_pc(cpu));
#ifdef VITA3K_USE_WASM_JIT
                    jit_report("progress", true);
#endif
                }
                if (hle_profile) {
                    last_import[tid] = { nid, read_pc(cpu) };
                    const auto hle_started = std::chrono::steady_clock::now();
                    ::call_import(*env, cpu, nid, tid);
                    const double hle_cost = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - hle_started).count();
                    hle_ms += hle_cost;
                    auto &hle_slot = hle_nids[nid];
                    ++hle_slot.first;
                    hle_slot.second += hle_cost;
                } else {
                    ::call_import(*env, cpu, nid, tid);
                }
                browser::sync_message_dialog(*env);
                browser::sync_ime(*env);
                if (trace_hle) {
                    std::fprintf(stderr, "[vita3k-web] HLE return #%u tid=%d NID=%08x PC=%08x r0=%08x\n",
                        import_sequence, tid, nid, read_pc(cpu), read_reg(cpu, 0));
                    std::fflush(stderr);
                }
                if (nid == 0x7A410B64 /* sceDisplaySetFrameBuf */
                    || nid == 0xF51523CB /* _sceDisplaySetFrameBuf */) {
                    vita3k_web_present_frame(*env);
                    ++frames_presented;
                }
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
        // SceRegMgr values: firmware template defaults, then vd0/registry/system.dreg.
        regmgr::init_regmgr(env->regmgr, env->vita_fs_path);
        init_libraries(*env);
        init_exported_vars(*env);

        SceUID eboot_uid = -1;
        for (const char *path : kPreloadChain) {
            // Everything but the executable is a process preload or a
            // sysmodule load: a system load.
            const SceUID uid = load_module(*env, path, std::strcmp(path, "app0:eboot.bin") != 0);
            if (uid < 0) {
                std::printf("[vita3k-web] load_module %s failed: %08x (continuing, desktop-tolerant)\n",
                    path, static_cast<std::uint32_t>(uid));
                if (!std::strcmp(path, "app0:eboot.bin")) return -4;
                continue;
            }
            std::printf("[vita3k-web] load_module %s -> uid %d (%s)\n",
                path, uid, env->kernel.loaded_modules[uid]->info.module_name);
            if (!std::strcmp(path, "app0:eboot.bin")) {
                eboot_uid = uid;
                env->kernel.process_program_authority_id = env->kernel.loaded_modules[uid]->program_authority_id;
            }
            if (needs_module_start(path)) {
                auto &module = *env->kernel.loaded_modules[uid];
                const auto &info = module.info;
                if (info.start_entry) {
                    const std::uint32_t result = start_module(*env, module);
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
#ifdef VITA3K_USE_WASM_JIT
        if (const char *aot_out = std::getenv("VITA3K_AOT_BUILD"))
            return build_aot_image(*env, aot_out);
        {
            std::string aot_report;
            const int aot = WasmJitCPU::load_aot(env->mem, aot_report);
            std::printf("[vita3k-web] AOT %s: %s\n", aot > 0 ? "on" : aot == 0 ? "off" : "REJECTED",
                aot_report.c_str());
        }
#endif
        // Title patches, desktop Vita3K's patch directory format, staged at
        // <vita fs>/patch. Applied after the AOT image was checked against the
        // unpatched code; the AOT functions covering patched bytes retire.
        if (fs::path patch_dir = env->vita_fs_path / "patch"; fs::is_directory(patch_dir)) {
            const Patches patches = get_patches(patch_dir, env->io.title_id, "app0:eboot.bin");
            std::vector<PatchSegment> segments;
            for (const auto &segment : module.segments)
                segments.push_back({ segment.vaddr.address(), static_cast<uint32_t>(segment.memsz) });
            const auto written = apply_patches(env->mem, patches, segments);
            for (const auto &range : written) {
#ifdef VITA3K_USE_WASM_JIT
                WasmJitCPU::retire_aot(range.address, range.size);
#endif
                env->kernel.invalidate_jit_cache(range.address, range.size);
            }
            std::printf("[vita3k-web] patches for %s: %zu applied\n", env->io.title_id.c_str(), written.size());
        }
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
        // The main thread is the module's entry: the module counts as started
        // before its thread can reach sceKernelCallModuleExit.
        auto &main_module = *env->kernel.loaded_modules.at(eboot_uid);
        main_module.started = true;
        if (thread->start(0, Ptr<void>{}, true) < 0) {
            main_module.started = false;
            return -7;
        }
#ifdef VITA3K_USE_WASM_JIT
        vita3k::web::GuestThreadRuntime::Progress progress;
        std::size_t dispatched = 0;
        std::size_t pc_sample_every = 0;
        if (const char *sample_env = std::getenv("VITA3K_BENCH_PC_SAMPLE")) {
            const unsigned long parsed = std::strtoul(sample_env, nullptr, 10);
            if (parsed > 0) pc_sample_every = static_cast<std::size_t>(parsed);
        }
        std::size_t pc_sample_next = pc_sample_every;
        // Benchmark bound: stop the pump after this many wall seconds and fall
        // through to the final report instead of being killed mid-run.
        double bench_seconds = 0;
        if (const char *limit = std::getenv("VITA3K_BENCH_SECONDS"))
            bench_seconds = std::strtod(limit, nullptr);
        bool bench_expired = false;
        // Diagnostic: VITA3K_AOT_UNTIL=<seconds> leaves the AOT module after
        // that much wall time (boot on AOT, the rest on the lazy JIT).
        double aot_until = 0;
        if (const char *until = std::getenv("VITA3K_AOT_UNTIL"))
            aot_until = std::strtod(until, nullptr);
        // VITA3K_BENCH_INPUT="<ms>:<input>[+<input>]:<hold ms>,...": scripted
        // pad input for runs without a page (the Chromium probe's LIMBO_INPUT
        // syntax), so a Node run can reach gameplay.
        struct ScriptedInput { double at_ms, hold_ms; std::uint32_t buttons; std::array<float, 4> axes; };
        std::vector<ScriptedInput> input_script;
        if (const char *script = std::getenv("VITA3K_BENCH_INPUT")) {
            static const std::pair<const char *, std::uint32_t> buttons[] = {{"select", 0x1}, {"start", 0x8},
                {"up", 0x10}, {"right", 0x20}, {"down", 0x40}, {"left", 0x80}, {"l", 0x100}, {"r", 0x200},
                {"triangle", 0x1000}, {"circle", 0x2000}, {"cross", 0x4000}, {"square", 0x8000}};
            static const std::tuple<const char *, int, float> sticks[] = {{"lstick-left", 0, -1.0f},
                {"lstick-right", 0, 1.0f}, {"lstick-up", 1, -1.0f}, {"lstick-down", 1, 1.0f}};
            std::string entries = script;
            for (std::size_t begin = 0; begin < entries.size();) {
                const std::size_t end = std::min(entries.find(',', begin), entries.size());
                const std::string entry = entries.substr(begin, end - begin);
                begin = end + 1;
                const std::size_t first = entry.find(':'), second = entry.find(':', first + 1);
                if (first == std::string::npos) {
                    std::fprintf(stderr, "[vita3k-web] VITA3K_BENCH_INPUT: bad entry '%s'\n", entry.c_str());
                    return -10;
                }
                ScriptedInput input{std::strtod(entry.c_str(), nullptr),
                    second == std::string::npos ? 200.0 : std::strtod(entry.c_str() + second + 1, nullptr), 0, {}};
                const std::string names = entry.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
                for (std::size_t n = 0; n <= names.size();) {
                    const std::size_t plus = std::min(names.find('+', n), names.size());
                    const std::string name = names.substr(n, plus - n);
                    n = plus + 1;
                    bool known = false;
                    for (const auto &[label, mask] : buttons)
                        if (name == label) { input.buttons |= mask; known = true; }
                    for (const auto &[label, axis, value] : sticks)
                        if (name == label) { input.axes[axis] = value; known = true; }
                    if (!known) {
                        std::fprintf(stderr, "[vita3k-web] VITA3K_BENCH_INPUT: unknown input '%s'\n", name.c_str());
                        return -10;
                    }
                }
                input_script.push_back(input);
            }
        }
        std::uint32_t scripted_buttons = 0;
        std::array<float, 4> scripted_axes{};
        unsigned frames_yielded = 0;
        double last_yield_ms = emscripten_get_now();
        do {
            if (!input_script.empty()) {
                const double now_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - jit_started).count();
                std::uint32_t buttons = 0;
                std::array<float, 4> axes{};
                for (const auto &input : input_script) {
                    if (now_ms < input.at_ms || now_ms >= input.at_ms + input.hold_ms)
                        continue;
                    buttons |= input.buttons;
                    for (int axis = 0; axis < 4; ++axis)
                        if (input.axes[axis] != 0)
                            axes[axis] = input.axes[axis];
                }
                if (buttons != scripted_buttons || axes != scripted_axes) {
                    scripted_buttons = buttons;
                    scripted_axes = axes;
                    vita3k_web_set_pad(buttons, axes[0], axes[1], axes[2], axes[3]);
                }
            }
            if (bench_seconds > 0 && std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - jit_started).count() >= bench_seconds) {
                bench_expired = true;
                break;
            }
            if (g_host_input_changed) {
                g_host_input_changed = false;
                const std::lock_guard<std::mutex> guard(env->ctrl.mutex);
                auto &pad = env->ctrl.keyboard_state;
                pad.buttons = pad.buttons_ext = g_host_buttons;
                std::copy(g_host_axes.begin(), g_host_axes.end(), pad.axes);
            }
            if (aot_until > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - jit_started).count() >= aot_until) {
                WasmJitCPU::disable_aot();
                std::printf("[vita3k-web] AOT disabled after %.0fs (VITA3K_AOT_UNTIL)\n", aot_until);
                aot_until = 0;
            }
            progress = runtime.resume(256);
            dispatched += progress.dispatches;
            // Page messages (pad input, dialog answers) arrive only in event
            // loop turns: take one per presented frame, and at least every
            // 50 ms for a guest that runs without presenting.
            const double now_ms = emscripten_get_now();
            if (frames_presented != frames_yielded || now_ms - last_yield_ms >= 50.0) {
                frames_yielded = frames_presented;
                web_yield_to_event_loop();
                last_yield_ms = emscripten_get_now();
                yield_ms += last_yield_ms - now_ms;
            }
            if (pc_sample_every && dispatched >= pc_sample_next) {
                pc_sample_next = dispatched + pc_sample_every;
                std::fprintf(stderr, "[vita3k-web] pc-sample dispatched=%zu threads=", dispatched);
                for (const auto &[tid, t] : env->kernel.threads) {
                    if (t && t->cpu)
                        std::fprintf(stderr, " %d:%08x", tid, read_pc(*t->cpu));
                }
                std::fprintf(stderr, "\n");
            }
            // Every thread is parked but one waits on a timeout: the guest is
            // idle until then (e.g. sceKernelDelayThread), not finished.
            if (progress.idle && progress.next_deadline_us && !exited && !progress.failed) {
                const auto now = vita3k::web::GuestThreadRuntime::now_us();
                if (*progress.next_deadline_us > now) {
                    const double started = emscripten_get_now();
                    emscripten_sleep(static_cast<unsigned>((*progress.next_deadline_us - now + 999) / 1000));
                    idle_ms += emscripten_get_now() - started;
                }
                progress.idle = false;
            }
        } while (!exited && env->missing_nids.empty() && !progress.failed
            && !progress.idle);
        std::printf("[vita3k-web] Guest scheduler: dispatches=%zu runnable=%zu waiting=%zu dormant=%zu failed=%zu idle=%d\n",
            dispatched, progress.runnable, progress.waiting, progress.dormant, progress.failed, progress.idle);
        if (progress.failed) {
            std::vector<ImportRecord> ordered(recent_imports.begin(), recent_imports.end());
            std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) { return a.sequence < b.sequence; });
            for (const auto &r : ordered)
                if (r.sequence)
                    std::printf("[vita3k-web] recent import #%u tid=%d %s PC=%08x LR=%08x\n", r.sequence, r.tid, import_name(r.nid), r.pc, r.lr);
        }
#else
        thread->run_loop(true);
#endif
#ifdef VITA3K_USE_WASM_JIT
        jit_report("final", true);
        browser::gxm_survey_report();
        if (bench_expired)
            std::printf("[vita3k-web] benchmark deadline reached after %.1fs\n", bench_seconds);
        if (const char *seeds = std::getenv("VITA3K_AOT_SEEDS_OUT"))
            std::printf("[vita3k-web] AOT seeds -> %s: %s\n", seeds,
                WasmJitCPU::dump_aot_seeds(seeds) ? "ok" : "FAILED");
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
void vita3k_web_set_pad(std::uint32_t buttons, float lx, float ly, float rx, float ry) {
    g_host_buttons = buttons;
    g_host_axes = { lx, ly, rx, ry };
    g_host_input_changed = true;
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
