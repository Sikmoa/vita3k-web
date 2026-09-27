// Ahead-of-time image build and load for both launch paths; see vita_aot.h.
#ifdef VITA3K_USE_WASM_JIT
#include "vita_aot.h"
#include <cpu/impl/wasm_jit_cpu.h>
#include <kernel/state.h>
#include <mem/functions.h>
#include <modules/module_parent.h>
#include <nids/functions.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

int build_aot_image(EmuEnvState &env, const char *out_path) {
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

int load_aot_image(EmuEnvState &env) {
    std::string report;
    const int aot = WasmJitCPU::load_aot(env.mem, report);
    std::printf("[vita3k-web] AOT %s: %s\n", aot > 0 ? "on" : aot == 0 ? "off" : "REJECTED", report.c_str());
    return aot;
}
#endif
