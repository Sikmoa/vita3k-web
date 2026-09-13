// Browser presentation bridge for the real SceDisplay framebuffer.
// This is transport only: the framebuffer metadata comes from Vita3K's real
// sceDisplaySetFrameBuf state (emuenv.display.sce_frame). Guest A8B8G8R8 rows
// are tightened/converted to RGBA in Wasm; JS never touches guest memory.
#include <emuenv/state.h>
#include <display/state.h>
#include <mem/functions.h>

#include <cstdint>
#include <mutex>
#include <vector>

#include "vita_runtime.h"

namespace {

struct DisplayBridgeState {
    std::mutex mutex;
    std::vector<uint8_t> rgba; // tight RGBA scratch, resized on dimension change
    uint32_t posted_generation = 0;
    bool hooked = false;
};

DisplayBridgeState &bridge_state() {
    static DisplayBridgeState state;
    return state;
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

    ++bridge.posted_generation;
    vita3k_web_post_frame_hook(static_cast<int>(bridge.posted_generation),
        static_cast<int>(width), static_cast<int>(height),
        reinterpret_cast<int>(bridge.rgba.data()));
}
