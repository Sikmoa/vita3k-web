# GXM/WebGPU renderer

The runtime renders GXM on the GPU through one GXS1 scene stream per command
list. `browser/src/gxm_webgpu_bridge.cpp` walks the production
`renderer::Command` list and the recorded guest state and encodes passes,
draws, fixed-function state, textures and fills into the stream;
`browser/web/gxm_scene.js`, running in the Worker, is its only consumer. It owns
the WebGPU device (acquired in `sceGxmInitialize`), translates GXP programs
in-browser through `browser/web/gxp_shader_adapter.js` (see
GXP_TRANSLATION.md), keeps render targets and textures on the GPU, and presents
a target to the OffscreenCanvas the page transferred, reading pixels back only
when asked (`?readback=N`, or every frame when no canvas is attached).

Display-sized targets render at an internal resolution scale
(`VITA3K_RESOLUTION_SCALE`, worker `?scale=N`, 1-4, default 2); smaller
intermediate targets stay at guest resolution (reason next to
`resolution_scale()` in the bridge). The supported guest formats and states are
the translation tables in those two files. A draw they cannot represent is
skipped and its reason logged once as `[gxm-skip]`; an unsupported command fails
its command list. Nothing is rendered implicitly.

## Frame path

- `sceGxmInitialize` (`browser::gxm_initialize`) acquires the device through
  `gxm_scene.js` `init()` (`web_gxm_init`) and creates the `SceGxmDisplayQueue`
  guest thread; there are no host threads.
- Each command list is encoded and submitted synchronously; its completions
  (notifications, sync objects) are published right after the submission.
  `sceGxmFinish` returns at once: nothing guest-visible waits on the GPU.
- Surface sync is opt-in (`VITA3K_SURFACE_SYNC=1`; Worker `?surfaceSync=1`,
  `limbo_serve.mjs` `?surfaceSync=1`, probe `LIMBO_SURFACE_SYNC=1`): a scene
  that drew into a linear, full-size color surface is read back into guest
  memory before its completions are published, for code that reads rendered
  pixels with the CPU. Other surfaces are skipped with a `[gxm-skip]` reason.
- `sceGxmDisplayQueueAddEntry` drains the queue inline (`display_entry_thread`
  on the display queue thread, which runs the guest callback). Its
  `sceDisplaySetFrameBuf` leads to `vita3k_web_present_frame`;
  `vita_display_bridge.cpp` presents the GPU target at the frame buffer
  address, or the guest rows for a CPU-drawn frame.

Not covered by a test: viewport sub-rects and negative viewport x scale.
