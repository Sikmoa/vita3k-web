// Contracts between the browser runtime entrypoint, the display bridge, and
// the Wasm->JS hooks. Browser-side only; no emulator types leak into here.
#pragma once

#include <cstdint>

struct EmuEnvState;

// Called after guest execution yields (frame boundary): converts and posts the
// current real sceDisplaySetFrameBuf state to the host. Defined in
// vita_display_bridge.cpp.
void vita3k_web_present_frame(EmuEnvState &emuenv);

// Implemented in vita_runtime.cpp via EM_JS (C linkage, EMSCRIPTEN-only).
extern "C" {
void vita3k_web_post_frame_hook(int generation, int width, int height, const uint8_t *data_ptr);
void vita3k_web_notify_exit(int code);
}
