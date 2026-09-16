# GXM/WebGPU renderer bring-up

## Implemented boundary

`browser/web/gxm_renderer.js` is a narrow WebGPU command consumer, exposed as
`createWebGPUBridge().renderer` after device acquisition. It is **not yet wired
to guest SceGxm calls**, and it does not translate GXP/USSE.

Supported:
- RGBA8 offscreen targets; triangle-list indexed draws (uint16/uint32).
- One interleaved vertex stream (float scalar/vectors, unorm8x4 attributes).
- Explicit WGSL vertex/fragment programs; optional group 0/binding 0 uniform buffer.
- Checked, owned upload snapshots, including safe BigInt Memory64 pointer conversion.
- GPU-completed, tightly packed RGBA8 readback with 256-byte GPU row alignment.
- Explicit resource deletion; rejection of concurrent operations, invalid indices,
  unsupported vertex formats, wrong uniform sizes, and shader validation errors.

No textures/samplers, depth/stencil, blending, MSAA, viewport/scissor commands,
multiple streams, pipeline cache, guest shader conversion or display queue yet.
Submission intentionally serializes and reads back every frame: correctness-first,
not a performance result. Producer code must reject unsupported GXM states rather
than silently omit them. Producer must await `submit()` before signaling guest
sync objects or executing display callbacks. Inputs are copied before the first
await; output pixels are owned and remain valid after unmapping.

## Test

From repository root:

```sh
PLAYWRIGHT_MODULE_URL=file://$PWD/build/playwright/node_modules/playwright/index.mjs \
  timeout -s KILL 55s node browser/tests/gxm_webgpu_smoke.mjs
```

Requires installed Playwright Chromium with WebGPU. Runs headless using SwiftShader
(software GPU), not a JavaScript mock. Unavailable WebGPU is a failure, not a skip.
Expected: `{"checks":18,"backend":"WebGPU","translatedGuestShader":false}`.
The 18 assertions cover Memory64 snapshot bounds, 2x2 upload/readback, a 65x33
indexed triangle (nonaligned rows and six-byte index upload), input mutation after
submission, uniform and vertex changes, 32-bit indices, lifecycle, invalid input,
invalid shaders and uncaptured GPU errors. Test shaders are explicit WGSL.

## Next integration milestone

A real VitaSDK indexed-triangle fixture must issue SceGxm calls, use its own GXP
shaders and vertex/uniform buffers, and present only after completion. Reuse native
GXM validation/state and shader translation where possible; don't replace arbitrary
guest programs with hardcoded WGSL. Native GXM initialization/display queues spawn
host threads, so the bridge also needs cooperative Asyncify-aware scheduling.
Limbo remains blocked on this and broader HLE/GXM/shader coverage.
