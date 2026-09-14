// Browser presentation bridge for the real SceDisplay framebuffer.
// This is transport only: the framebuffer metadata comes from Vita3K's real
// sceDisplaySetFrameBuf state (emuenv.display.sce_frame). Guest A8B8G8R8 rows
// are tightened/converted to RGBA in Wasm; JS never touches guest memory.
#include <emuenv/state.h>
#include <display/state.h>
#include <mem/functions.h>

// Narrow benchmark seam (M14c): the runtime's own instruction counter
// (vita3k_web_last_run_instructions) is only published after the run loop
// returns, so per-frame guest-instruction snapshots otherwise do not exist.
// The bridge already sits on the one presentation seam; it records the main
// thread's cumulative backend counter at each presented frame and exposes it
// read-only below. Nothing about presentation or the frame contract changes.
#include <cpu/impl/interpreter_cpu.h>
#ifdef VITA3K_USE_WASM_JIT
#include <cpu/impl/wasm_jit_cpu.h>
#endif
#include <kernel/state.h>

#include <cstdint>
#include <mutex>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

#include "vita_runtime.h"

namespace {

struct DisplayBridgeState {
    std::mutex mutex;
    std::vector<uint8_t> rgba; // tight RGBA scratch, resized on dimension change
    uint32_t posted_generation = 0;
    uint64_t last_frame_instructions = 0; // cumulative guest instructions at the last presented frame
    bool hooked = false;
};

DisplayBridgeState &bridge_state() {
    static DisplayBridgeState state;
    return state;
}

// Cumulative guest instructions of the main thread's CPU backend. Mirrors the
// counter read the runtime itself performs after the run loop; 0 when the main
// thread or its backend is absent (also before the first presented frame).
static uint64_t guest_instructions_executed(const EmuEnvState &emuenv) {
    const auto thread = emuenv.kernel.threads.find(emuenv.main_thread_id);
    if (thread == emuenv.kernel.threads.end() || !thread->second || !thread->second->cpu)
        return 0;
    const CPUInterface *backend = thread->second->cpu->cpu.get();
    if (const auto *interpreter = dynamic_cast<const InterpreterCPU *>(backend))
        return interpreter->instructions_executed();
#ifdef VITA3K_USE_WASM_JIT
    if (const auto *jit = dynamic_cast<const WasmJitCPU *>(backend))
        return jit->instructions_executed();
#endif
    return 0;
}

} // namespace

void vita3k_web_present_frame(EmuEnvState &emuenv) {
    DisplayBridgeState &bridge = bridge_state();
    DisplayFrameInfo info;
    {
        // SceDisplay updates sce_frame under display_info_mutex; match it.
        std::lock_guard<std::mutex> guard(emuenv.display.display_info_mutex);
        info = emuenv.display.sce_frame;
    }
    if (!info.base || info.image_size.x == 0 || info.image_size.y == 0)
        return;
    if (info.pixelformat != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8) {
        // The real HLE only accepts A8B8G8R8; anything else is a bug upstream
        // of this bridge. Refuse silently rather than presenting garbage.
        return;
    }

    const uint32_t width = static_cast<uint32_t>(info.image_size.x);
    const uint32_t height = static_cast<uint32_t>(info.image_size.y);
    const uint32_t pitch = info.pitch != 0 ? info.pitch : width; // pitch is in pixels
    const size_t frame_bytes = size_t(width) * height * 4;
    if (bridge.rgba.size() != frame_bytes)
        bridge.rgba.assign(frame_bytes, 0);

    // A8B8G8R8 in a little-endian 32-bit guest word is R,G,B,A in memory —
    // byte-identical to canvas RGBA8 — so no channel swizzle is needed. The
    // only per-row work is pitch tightening (guest pitch may exceed width).
    for (uint32_t y = 0; y < height; ++y) {
        const Address row_addr = info.base.address() + Address(size_t(y) * pitch * 4);
        if (!mem_read(emuenv.mem, row_addr, bridge.rgba.data() + size_t(y) * width * 4, size_t(width) * 4))
            return; // unmapped/invalid framebuffer: skip this frame entirely
    }

    // Record before the hook so a synchronous JS reader (Node bench driver
    // sampling inside vita3kWebOnFrame) sees this frame's cumulative count.
    // Presentation is single-threaded (Worker/main thread), like the rest of
    // this bridge's state.
    bridge.last_frame_instructions = guest_instructions_executed(emuenv);
    ++bridge.posted_generation;
    vita3k_web_post_frame_hook(static_cast<int>(bridge.posted_generation),
        static_cast<int>(width), static_cast<int>(height),
        reinterpret_cast<int>(bridge.rgba.data()));
}

extern "C" EMSCRIPTEN_KEEPALIVE
uint64_t vita3k_web_last_frame_instructions() {
    // Benchmark seam: cumulative guest instructions at the most recent
    // presented frame; 0 before the first frame of a run. Presentation itself
    // never reads or depends on this value.
    return bridge_state().last_frame_instructions;
}
