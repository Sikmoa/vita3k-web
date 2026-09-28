# GXM/WebGPU renderer

The runtime renders GXM on the GPU through one GXS1 scene stream per command
list. `browser/src/gxm_webgpu_bridge.cpp` walks the production
`renderer::Command` list and the recorded guest state and encodes passes,
draws, fixed-function state, textures and transfer writes into the stream;
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
- Each command list is encoded and submitted in order; its completions
  (notifications, sync objects) are published right after accepted submission.
  If the GPU queue is full, the producer suspends through Asyncify and retries
  the same stream when capacity returns. No scene, upload or target update
  is dropped. `sceGxmFinish` does not add a GPU completion fence.
  Presentation/readback submissions share the same bounded queue. See
  `../PLAYER.md` for limits, telemetry and the required Wasm rebuild.
- Surface sync is opt-in (`VITA3K_SURFACE_SYNC=1`; Worker `?surfaceSync=1`,
  `limbo_serve.mjs` `?surfaceSync=1`, probe `LIMBO_SURFACE_SYNC=1`): a scene
  that drew into a color surface is read back into guest memory, in the
  surface's layout (linear, tiled or swizzled), before its completions are
  published, for code that reads rendered pixels with the CPU.
- GPU rendering is layout-independent; what touches guest memory goes through
  the surface's layout: surface sync, textures over a rendered surface (the
  surface itself, or a pre-pass copy of the rectangle a texture covers, when
  layout, pitch and texel format match; otherwise guest memory, like desktop),
  and transfers (fill, copy, downscale), which run on the CPU, read a rendered
  source back first and write the texels they changed into the GPU copy.
- A downscaled surface (`SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE`) renders
  at twice its size and is box-filtered wherever its texels are read. A
  multisampled render target renders one pixel per sample, as on desktop.
- `sceGxmDisplayQueueAddEntry` drains the queue inline (`display_entry_thread`
  on the display queue thread, which runs the guest callback). Its
  `sceDisplaySetFrameBuf` leads to `vita3k_web_present_frame`;
  `vita_display_bridge.cpp` presents the GPU target at the frame buffer
  address, or the guest rows for a CPU-drawn frame.

`browser/tests/gxm_surface_chromium.mjs` runs the VitaSDK fixture
`vita_gxm_surface_fixture` (tiled, swizzled, downscaled and multisampled
surfaces, textures over them, transfers) with and without surface sync.

Not covered by a test: viewport sub-rects and negative viewport x scale.
