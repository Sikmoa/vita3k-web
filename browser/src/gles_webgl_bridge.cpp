// SPDX-License-Identifier: GPL-2.0-or-later
// Opt-in HLE of PVR PSP2's user-mode EGL/GLES libraries. The guest engine still
// runs on the ARM CPU; GL commands execute on WebGL instead of the unimplemented
// PowerVR kernel driver. GXM and native Vita3K are not changed by this adapter.
#include "gles_webgl_bridge.h"

#include <cpu/functions.h>
#include <emuenv/state.h>
#include <kernel/state.h>
#include <mem/functions.h>
#include <emscripten/emscripten.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
struct Function {
    const char *name;
    uint32_t nid;
    uint32_t library;
    const char *signature;
};
constexpr Function functions[] = {
#define GLES_FUNCTION(name, nid, library, signature) { #name, nid, library, signature },
#include "gles_functions.inc"
#undef GLES_FUNCTION
};
constexpr uint32_t max_transfer = 128 * 1024 * 1024;
struct State {
    EmuEnvState *env = nullptr;
    bool enabled = false;
    bool active = false;
    Address trampolines = 0;
    std::vector<uint8_t> scratch;
    std::unordered_set<SceUID> replaced;
    std::array<uint8_t, 1004> app_hint{};
};
State state;

const Function *find_function(uint32_t nid) {
    static const auto table = [] {
        std::unordered_map<uint32_t, const Function *> result;
        for (const auto &function : functions) result.emplace(function.nid, &function);
        return result;
    }();
    const auto it = table.find(nid);
    return it == table.end() ? nullptr : it->second;
}
const Function *find_function(std::string_view name) {
    for (const auto &function : functions)
        if (name == function.name) return &function;
    return nullptr;
}

// A copied view is required even on Memory64: Wasm can grow and an adapter call
// can issue several independent guest reads. All ranges pass mem_read/mem_write,
// so sparse wasm32 pages and permissions have exactly the core memory semantics.
} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE
const uint8_t *vita3k_web_gles_read(uint32_t address, uint32_t length) {
    if (!state.env || length > max_transfer) return nullptr;
    state.scratch.resize(length);
    return mem_read(state.env->mem, address, state.scratch.data(), length)
        ? state.scratch.data() : nullptr;
}
extern "C" EMSCRIPTEN_KEEPALIVE
uint8_t *vita3k_web_gles_scratch(uint32_t length) {
    if (!state.env || length > max_transfer) return nullptr;
    state.scratch.resize(length);
    return state.scratch.data();
}
extern "C" EMSCRIPTEN_KEEPALIVE
int vita3k_web_gles_commit(uint32_t address, uint32_t length) {
    return state.env && length <= state.scratch.size()
        && mem_write(state.env->mem, address, state.scratch.data(), length);
}
extern "C" EMSCRIPTEN_KEEPALIVE
uint32_t vita3k_web_gles_alloc(uint32_t length) {
    if (!state.env || !length || length > max_transfer) return 0;
    return alloc(state.env->mem, length, "GLES guest return data");
}

EM_ASYNC_JS(int, web_gles_init, (), {
    try {
        const { createGlesBridge } = await import(new URL('gles_webgl.js', globalThis.location.href).href);
        const memory = {
            read(address, length) {
                if (!Number.isSafeInteger(length) || length < 0 || length > 134217728)
                    throw new RangeError('invalid GLES guest read length');
                if (!length) return new Uint8Array();
                const pointer = Module['_vita3k_web_gles_read'](address >>> 0, length);
                if (!pointer) throw new RangeError('GLES read of unmapped guest memory at 0x' + (address >>> 0).toString(16));
                return Module['vita3kHostBytes'](pointer, length).slice();
            },
            write(address, bytes) {
                const length = bytes.byteLength;
                if (!length) return;
                if (!Number.isSafeInteger(length) || length < 0 || length > 134217728)
                    throw new RangeError('invalid GLES guest write length');
                const pointer = Module['_vita3k_web_gles_scratch'](length);
                if (!pointer) throw new RangeError('GLES guest write is too large');
                Module['vita3kHostBytes'](pointer, length).set(bytes);
                if (!Module['_vita3k_web_gles_commit'](address >>> 0, length))
                    throw new RangeError('GLES write of unmapped guest memory at 0x' + (address >>> 0).toString(16));
            },
            alloc(length) {
                if (!Number.isSafeInteger(length) || length <= 0 || length > 134217728)
                    throw new RangeError('invalid GLES guest allocation');
                const address = Module['_vita3k_web_gles_alloc'](length) >>> 0;
                if (!address) throw new Error('GLES guest allocation failed');
                return address;
            },
        };
        const bridge = createGlesBridge({ memory, logger: message => err(message),
            onFrame: (generation, width, height, pixels) => {
                globalThis.vita3kWebOnGpuFrame?.(generation, width, height, pixels);
            } });
        Module['vita3kGles'] = bridge;
        globalThis.vita3kGlesReady?.(bridge);
        return 0;
    } catch (error) {
        err('[gles-webgl] initialization failed: ' + (error.stack || error));
        return -1;
    }
});
EM_JS(int, web_gles_supports, (const char *name), {
    return Module['vita3kGles']?.supports(UTF8ToString(Module['vita3kHostOffset'](name))) ? 1 : 0;
});
EM_JS(int, web_gles_call, (const char *name, const double *args, uint32_t count, uint32_t *result), {
    const callName = UTF8ToString(Module['vita3kHostOffset'](name));
    try {
        const offset = Module['vita3kHostOffset'](args, count * 8);
        // Do not retain views across guest callbacks (which can grow Wasm).
        const values = Array.from(new Float64Array(wasmMemory.buffer, offset, count));
        const value = Module['vita3kGles'].call(callName, values);
        const dest = Module['vita3kHostBytes'](result, 4);
        new DataView(dest.buffer, dest.byteOffset, 4).setUint32(0, Number(value || 0) >>> 0, true);
        return 0;
    } catch (error) {
        err('[gles-webgl] ' + callName + ' failed: ' + (error.stack || error));
        return -1;
    }
});
EM_ASYNC_JS(int, web_gles_swap, (), {
    try {
        await Module['vita3kGles'].swapBuffers();
        return 1;
    } catch (error) {
        err('[gles-webgl] presentation failed: ' + (error.stack || error));
        return 0;
    }
});

namespace {
void ensure_bridge() {
    if (state.active) return;
    if (web_gles_init() != 0)
        throw std::runtime_error("EGL/GLES WebGL adapter initialization failed (see graphics log)");
    state.active = true;
}
Address trampoline(EmuEnvState &env, const Function &function) {
    if (!state.trampolines) {
        constexpr size_t bytes = std::size(functions) * 3 * sizeof(uint32_t);
        state.trampolines = alloc(env.mem, bytes, "GLES import trampolines");
        if (!state.trampolines) throw std::runtime_error("Cannot allocate GLES import trampolines");
        std::array<uint32_t, std::size(functions) * 3> words{};
        for (size_t i = 0; i < std::size(functions); ++i) {
            words[i * 3] = 0xEF000000; // ARM svc #0
            words[i * 3 + 1] = 0xE1A0F00E; // mov pc, lr
            words[i * 3 + 2] = functions[i].nid;
        }
        if (!mem_write(env.mem, state.trampolines, words.data(), bytes))
            throw std::runtime_error("Cannot write GLES import trampolines");
    }
    return state.trampolines + (&function - functions) * 12;
}
std::string guest_name(EmuEnvState &env, Address address) {
    std::string name;
    for (unsigned i = 0; i < 256; ++i) {
        char c;
        if (!address || address > UINT32_MAX - i || !mem_read(env.mem, address + i, &c, 1))
            throw std::runtime_error("Invalid EGL procedure-name pointer");
        if (!c) return name;
        name += c;
    }
    throw std::runtime_error("EGL procedure name exceeds 255 bytes");
}
uint32_t initialize_app_hint(EmuEnvState &env, Address address) {
    if (!address) return 0;
    // Public PVRSRV_PSP2_APPHINT ABI: ten words, three 256-byte paths,
    // thirty-three common GLES words, sixteen GLES2 words (1004 bytes).
    // The WebGL driver does not consume PowerVR tuning, but callers can read
    // and customize this real structure before CreateVirtualAppHint stores it.
    std::array<uint8_t, 1004> hint{};
    const uint32_t common[] = { 50 * 1024, 16 * 1024 * 1024, 4 * 1024 * 1024,
        0, 0, 0, 0, 0, 0, 1 };
    const uint32_t gles[] = { 1, 1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0,
        200 * 1024, 800 * 1024, 200 * 1024, 50 * 1024, 80 * 1024,
        50 * 1024, 20 * 1024, 1, 0, 4 * 1024, 256 * 1024, 1, 1,
        1, 70, 0, 256, 1000000, 1000, 0 };
    const uint32_t gles2[] = { 0, 0, 1, 0x3F800000, 0x3F800000, 0, 0, 1,
        0, 0, 32, 1, 1, 1, 0, 0x7FFFFFFF };
    static_assert(sizeof(common) == 40 && sizeof(gles) == 132 && sizeof(gles2) == 64);
    std::memcpy(hint.data(), common, sizeof(common));
    std::strcpy(reinterpret_cast<char *>(hint.data() + 40), "app0:module/libpvrPSP2_WSEGL.suprx");
    std::strcpy(reinterpret_cast<char *>(hint.data() + 296), "app0:module/libGLESv1_CM.suprx");
    std::strcpy(reinterpret_cast<char *>(hint.data() + 552), "app0:module/libGLESv2.suprx");
    std::memcpy(hint.data() + 808, gles, sizeof(gles));
    std::memcpy(hint.data() + 940, gles2, sizeof(gles2));
    return mem_write(env.mem, address, hint.data(), hint.size()) ? 1 : 0;
}
} // namespace

namespace browser::gles {
Session::Session(EmuEnvState &env) {
    state = {};
    state.env = &env;
    const char *option = std::getenv("VITA3K_GLES_BRIDGE");
    state.enabled = option && std::strcmp(option, "1") == 0;
}
Session::~Session() {
    state = {};
}

bool replace_module(EmuEnvState &env, const SceKernelModuleInfo &info) {
    if (!state.enabled || state.env != &env) return false;
    const std::string_view name(info.module_name, strnlen(info.module_name, sizeof(info.module_name)));
    uint32_t library = 0;
    if (name == "libgpu_es4_ext") library = 0x34C4AB62;
    else if (name == "libIMGEGL") library = 0xB4B37D2D;
    else if (name == "libGLESv2") library = 0x626F9AB6;
    else return false;
    if (state.replaced.contains(info.modid)) return true;
    ensure_bridge();
    // Loading a guest library has already linked its exports. Replace only this
    // library's known API bindings with HLE trampolines, using the same cache
    // invalidation as load_func_exports. Never overwrite neighboring guest code
    // (some exported entry points are shorter than the 12-byte SVC sequence).
    for (const auto &function : functions) {
        if (function.library != library) continue;
        const auto address = trampoline(env, function);
        const auto key = lib_export_key(library, function.nid);
        env.kernel.export_nids_by_lib.insert_or_assign(key, address);
        const auto owner = env.kernel.export_nid_owners.find(function.nid);
        if (owner == env.kernel.export_nid_owners.end() || owner->second == library) {
            env.kernel.export_nids.insert_or_assign(function.nid, address);
            env.kernel.export_nid_owners.insert_or_assign(function.nid, library);
        }
        const auto [begin, end] = env.kernel.func_binding_infos.equal_range(function.nid);
        for (auto it = begin; it != end; ++it) {
            if (it->second.library_nid != library) continue;
            const auto stub_address = it->second.entry_address;
            auto *stub = Ptr<uint32_t>(stub_address).get(env.mem);
            stub[0] = 0xEF000000;
            stub[1] = 0xE1A0F00E;
            stub[2] = function.nid;
            mem_mark_written(env.mem, stub_address, 12);
            env.kernel.invalidate_jit_cache(stub_address, 12);
        }
    }
    state.replaced.insert(info.modid);
    std::printf("[gles-webgl] HLE library %.*s (guest EGL/GLES -> browser WebGL)\n",
        static_cast<int>(name.size()), name.data());
    return true;
}

bool call_import(EmuEnvState &env, CPUState &cpu, uint32_t nid) {
    if (!state.enabled || state.env != &env) return false;
    const Function *function = find_function(nid);
    if (!function) return false;
    ensure_bridge();
    uint32_t result = 0;
    if (nid == 0x249A431A) { // eglGetProcAddress: only supported functions exist.
        const auto name = guest_name(env, read_reg(cpu, 0));
        const auto *procedure = find_function(name);
        if (procedure && web_gles_supports(procedure->name))
            result = trampoline(env, *procedure);
    } else if (nid == 0x1DEFDAB5) {
        result = initialize_app_hint(env, read_reg(cpu, 0));
    } else if (nid == 0x90FD340D) {
        const auto address = read_reg(cpu, 0);
        result = address && mem_read(env.mem, address, state.app_hint.data(), state.app_hint.size());
    } else if (nid == 0x02AB000E) { // eglSwapBuffers
        result = web_gles_swap();
        if (!result) throw std::runtime_error("GLES presentation failed (see graphics log)");
    } else {
        // Vita's AAPCS-VFP allocates floats independently of integer/pointer
        // registers. Do not reinterpret float bits as integers or consume rN
        // for a float; mixed signatures such as glUniform4f depend on this.
        std::array<double, 12> args{};
        const size_t count = std::strlen(function->signature);
        size_t integer = 0, fp = 0;
        for (size_t i = 0; i < count; ++i) {
            if (function->signature[i] == 'f') {
                args[i] = read_float_reg(cpu, fp++);
            } else if (integer < 4) {
                args[i] = read_reg(cpu, integer++);
            } else {
                uint32_t value = 0;
                const uint64_t stack = static_cast<uint64_t>(read_sp(cpu)) + (integer++ - 4) * 4;
                if (stack > UINT32_MAX || !mem_read(env.mem, static_cast<Address>(stack), &value, sizeof(value)))
                    throw std::runtime_error("Invalid GLES stack argument");
                args[i] = value;
            }
        }
        if (web_gles_call(function->name, args.data(), count, &result) != 0)
            throw std::runtime_error(std::string("GLES call failed: ") + function->name + " (see graphics log)");
    }
    write_reg(cpu, 0, result);
    return true;
}

const char *import_name(uint32_t nid) {
    if (nid == 0x228252A2) return "PVRSRVConnect";
    const auto *function = find_function(nid);
    return function ? function->name : nullptr;
}
} // namespace browser::gles
