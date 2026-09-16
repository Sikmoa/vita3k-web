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
multiple streams or pipeline cache in this consumer yet. Separate prototypes
exercise offline guest shader conversion and a JavaScript completion queue
(see below); neither establishes an integrated guest renderer.
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

## Additional prototypes (not guest ABI integration)

`gxp_webgpu_smoke.mjs` uses the production native USSE recompiler to convert the
repository's `color_v.gxp` and `color_f.gxp` to SPIR-V, then Naga to WGSL. It renders
these translated programs in Chromium and asserts vertex/color/uniform-driven
pixels. **8 checks pass; `guestExecution` remains `false`.** This is an offline
conversion experiment, not yet a browser shader compiler.

`gxm_context.js` snapshots draw payloads, serializes scenes and waits for GPU
readback/writeback before completion hooks. Its display callback API is supplied
by a JavaScript caller, not by Vita3K's guest scheduler. The corresponding
`gxm_context_smoke.mjs` passes **19 checks**, including snapshot ownership,
completion ordering, rejection propagation and destroyed-context invalidation.
It likewise reports `guestExecution: false`.

Both tests currently require `.limbo_work/gxm/gxp_compile` (linked using
`browser/tests/build_gxp_compiler.sh` against existing native shader libraries)
and Naga CLI at `.limbo_work/gxm/node_modules/naga-wasi-cli/bin/naga.mjs` or
`NAGA_CLI`. Run each with the same Playwright environment and hard timeout as the
baseline test. They are not self-contained clean-checkout tests yet.

`gxm_guest_probe_chromium.mjs --expect-missing` diagnoses the current missing
`sceGxmInitialize` bridge using a real VitaSDK executable under Memory64 WASM JIT.
Passing that diagnostic means the missing bridge was observed, **not** that GXM
initialization or rendering succeeded.

## Next integration milestone

A real VitaSDK indexed-triangle fixture must issue SceGxm calls, use its own GXP
shaders and vertex/uniform buffers, and present only after completion. Reuse native
GXM validation/state and shader translation where possible; don't replace arbitrary
guest programs with hardcoded WGSL. Native GXM initialization/display queues spawn
host threads, so the bridge also needs cooperative Asyncify-aware scheduling.
Limbo remains blocked on this and broader HLE/GXM/shader coverage.
