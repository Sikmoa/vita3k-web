# GXM Guest Integration (WebGPU bridge)

## What is verified working

- Guest ABI: real VitaSDK SELF fixture exercises `sceGxmInitialize` (incl. NULL validation),
  `sceGxmCreateContext` / `sceGxmDestroyContext`, `sceGxmTransferFill`, `sceGxmFinish`,
  `sceGxmTerminate` through production HLE on the wasm64-direct JIT backend.
- Real command submission: bridge walks the production `renderer::Command` list,
  executes `Nop` (queue fence), `TransferFill`, and the supported
  `SetContext` / `SetState` / `Draw` / `SyncSurfaceData` sequence via
  `EM_ASYNC_JS` + WebGPU. Readback is copied into guest memory before completion.
- Real guest GXP draws: the fixture registers repository-owned `color_v.gxp` /
  `color_f.gxp` through the production shader patcher, submits an indexed triangle,
  and verifies all 1,024 red framebuffer pixels. A second draw changes the guest
  color attributes and WVP matrix; the guest checks a green center and preserved
  red exterior. Shader conversion runs in-browser (Vita3K USSE Wasm → SPIR-V →
  Naga WASI → WGSL), not a shader-name lookup or host-precompiled WGSL shortcut.
- Cooperative completion: `EM_ASYNC_JS` suspends the host HLE stack at the SVC boundary
  (`sceGxmFinish` returns only after `gpu.queue.onSubmittedWorkDone()`).
- 4 GiB host-pointer cap in `browser/src/host_abi.js` lifted to the real buffer bound;
  guest surfaces above the direct-mapping base are reachable.

## Reproduction status

Use VERIFICATION_QUEUE.md for current bounded build/regression commands with
runner-owned scratch/cache directories. The historical guest-probe builder and
server still assume a previous host's shader/WASI asset staging arrangement;
they need portable dependency paths before they are clean-checkout commands.
The asset-free `gxm_webgpu_smoke.mjs` remains directly runnable. No commands
were executed in the C1 pipeline-cache batch.

Historical color-only baseline: guest `exit 42`, 56 imports, zero missing NIDs,
`missingSceGxmBridge=false`, two draw readback log lines. The extended texture
fixture now expects **nine** `GXM WebGPU GXP indexed draw readback completed`
lines and `CPU backend: WasmJitCPU (no fallback)`. Its Memory64/Chromium run
passed: exit 42, 113 imports, zero missing NIDs, and nine readbacks on the
Memory64 WASM JIT in Chromium/SwiftShader. This is the extended fixture's result,
not the old color-only result.

Regression baseline: `gxm_webgpu_smoke.mjs` (68 checks),
`gxp_webgpu_smoke.mjs` (8 checks), `gxp_translation_smoke.mjs` (22 checks),
and `gxm_guest_probe_chromium.mjs` all pass in Chromium/SwiftShader.
Host-only shader/renderer checks do not substitute for guest execution.

## Integration blockers / notes

- Device acquisition must happen before the wasm64 runtime loads
  (`gxm-probe-worker.js` imports `gxm_hle_bridge.js` first). Acquiring inside the
  suspended (Asyncify) stack intermittently reports "WebGPU adapter unavailable".
- The guest draw path currently supports ABGR8 linear surfaces, one interleaved
  F32 vertex stream, U16/U32 indexed triangle lists, arbitrary viewports
  (forwarded to the WebGPU viewport; draws without recorded viewport state
  reject), default region clip, and metadata-packed vertex/fragment uniform
  buffers (indexed and default, both stages: same `set_uniform_buffer` path).
  It preserves the initial guest surface between draws. It also materializes the
  guest fragment blend descriptor (write mask, color/alpha operation and
  factors) and a depth-stencil attachment whose format is an exact WebGPU
  representation of the guest depth format, with the guest depth
  compare/write mode, cleared to the guest background depth. The bounded GXM4
  texture path below is guest GPU-verified. Unsupported F32-only vertex
  attributes, other textures, instancing, non-default region clip, stencil
  state, the force-load/force-store depth modes and other commands are
  rejected, not silently ignored.
- C2 (implemented and verified against retail Limbo) accepts
  `SignalSyncObject`,
  `WaitSyncObject` and `NewFrame`, and wakes `sceGxmNotificationWait` via
  `notification_ready` after `SignalNotification`/`SyncSurfaceData`. NewFrame
  records the predicted frame; the pixels are presented by the display-queue
  drain that `sceGxmDisplayQueueAddEntry` performs (GXM_WEBGPU.md
  "Presentation").
- C1 adds a bounded pipeline cache (reuse verified; eviction/device-loss tests
  outstanding) and ordered guest draw/fill/fence
  queue (GXM_WEBGPU.md). Each draw still translates GXP, uploads buffers and
  creates/reads back its target. Shader translation caching and persistent
  surface ownership remain to be implemented.
- Production distribution of compiler, Naga and WASI shim assets and ordinary
  worker integration still need packaging work. `browser/tests/limbo_serve.mjs`
  now serves the runtime, the shader assets and a staged retail app to an
  ordinary browser (`HOST=0.0.0.0 PORT=... node browser/tests/limbo_serve.mjs`),
  and `browser/tests/limbo_watch.mjs` mirrors the presented frames into the
  workspace headlessly; both are still development tools, not a shipped app.
- `vita3k/renderer/src/sync.cpp` intentionally NOT linked (single-threaded, GPU-fenced
  bridge replaces its queue thread); `wishlist` / `subject_done` reimplemented locally.
- `browser/web/gxm_context.js` is owned by main; the bridge uses the separate
  `gxm_hle_bridge.js` instead.
- Full renderer completeness and Limbo gameplay are NOT verified. Textured GXP
  has separate host-driven browser tests. Bounded guest texture wiring is now
  GPU-verified; the guest `SceGxmAttributeFormat` set U8N/S8N/U16N/S16N/F16/F32
  is translated in JS (GXM5 packet); display-queue integration, retail-app
  scheduling and presentation are implemented and verified against retail Limbo,
  which presented its title/loading frames in a browser (GXM_WEBGPU.md
  "Presentation"). Blend and depth-stencil state are implemented and
  GPU-verified at the renderer level.
  Still unverified: gameplay beyond the loading screen, stencil state, MSAA,
  multi-stream vertex layouts, and every draw still reads back per draw.
  `rendererComplete=false` in the probe output remains intentional.

## Task #20 handoff to the HLE owner (Limbo SceGxm static rows)

Limbo (`dec_out/eboot.bin`, module `Limbo`) statically imports 73 SceGxm
functions. The browser selection already covers 25 of them. The C2 bridge
slice removes every *renderer-side* blocker for the groups below; selecting
them needs only `browser/runtime_hle.cmake` edits (NOT this workstream).
All NIDs are from `vita3k/nids/include/nids/nids.inc`.

Display-queue/sync notification (bridge now handles the commands/notify):

- `sceGxmDisplayQueueAddEntry` 0xEC5C26B5 — emits `NewFrame` (accepted) plus
  the display-queue push. SELECTED and verified: `browser::gxm_initialize`
  creates the display queue guest thread, the entry drains the queue inline
  under `VITA3K_BROWSER_GXM`, the guest display callback runs on that thread and
  its `sceDisplaySetFrameBuf` import is the presentation trigger. Retail Limbo
  presents real frames (GXM_WEBGPU.md "Presentation").
- `sceGxmSyncObjectCreate` 0x6A6013E1 / `sceGxmSyncObjectDestroy` 0x889AE88C —
  backend-agnostic `renderer::create/destroy` already linked via
  `renderer/src/creation.cpp`.
- `sceGxmGetNotificationRegion` 0x8BDE825A — returns the region allocated in
  `gxm_initialize`; no renderer involvement.
- `sceGxmNotificationWait` 0x9F448E79 — now woken by the bridge notify; pure
  HLE wait on `notification_ready` otherwise. Limbo calls it on the render
  thread and the fiber runtime answers it immediately (logged as an unsupported
  wait, non-fatal). Also non-fatal and logged the same way:
  `sceDisplayWaitSetFrameBuf` 0x9423560C and `sceDisplayWaitSetFrameBufMulti`
  0x7D9864A8.
- `sceGxmMapMemory` 0xC61E34FC / `sceGxmUnmapMemory` 0x828C68E8 — HLE-side
  region tracking only (`features.enable_memory_mapping` is false, so no
  renderer `MemoryMap` command is ever sent and preflight is unaffected).
  Needed for the MAX_UB size bounding in `gxmSetUniformBuffers`.
- `sceGxmMapVertexUsseMemory` 0xFA437510 / `sceGxmUnmapVertexUsseMemory`
  0x099134F5 / `sceGxmMapFragmentUsseMemory` 0x008402C6 /
  `sceGxmUnmapFragmentUsseMemory` 0x80CCEDBB — STUBBED upstream (always
  success); already observed in the desktop Limbo boot log.

Viewport state (bridge now forwards; no rejection on non-default):

- `sceGxmSetViewport` 0x3EB3380B, `sceGxmSetViewportEnable` 0x814F61EB.

Uniform buffers (same verified path as the existing default-VB support):

- `sceGxmSetVertexUniformBuffer` 0xC68015E4,
  `sceGxmSetFragmentUniformBuffer` 0xEA0FC310,
  `sceGxmSetFragmentDefaultUniformBuffer` 0xA824EB24. All funnel through
  `gxmSetUniformBuffers` into `GXMState::UniformBuffer`, which the bridge
  packs from program metadata. (`sceGxmSetVertexDefaultUniformBuffer` is
  already selected.)

Shader-patcher/program-metadata queries (pure CPU GXP parsing in the already
linked `shader/src`, `gxm/src`; no renderer commands, no bridge change):

- `sceGxmShaderPatcherReleaseVertexProgram` 0xAC1FF2DA,
  `sceGxmShaderPatcherReleaseFragmentProgram` 0xBE2743D1,
  `sceGxmShaderPatcherUnregisterProgram` 0xF103AF8A,
  `sceGxmShaderPatcherDestroy` 0xEAA5B100,
  `sceGxmShaderPatcherGetProgramFromId` 0xA949A803,
  `sceGxmProgramCheck` 0xED8B6C69,
  `sceGxmProgramGetParameterCount` 0xD5D5FCCD,
  `sceGxmProgramGetParameter` 0x06FF9151,
  `sceGxmProgramFindParameterBySemantic` 0x633CAA54,
  `sceGxmProgramParameterGetCategory` 0x1997DC17,
  `sceGxmProgramParameterGetType` 0x7B9023C3,
  `sceGxmProgramParameterGetName` 0x6AF88A5D,
  `sceGxmProgramParameterGetContainerIndex` 0xBB58267D,
  `sceGxmProgramParameterGetComponentCount` 0xBD2998D1,
  `sceGxmProgramParameterGetArraySize` 0xDBA8D061.

Safe descriptors whose *use* stays explicitly rejected at draw time:

- Pure getters: `sceGxmColorSurfaceGetFormat` 0xF3C1C6C6,
  `sceGxmTextureGetStride` 0xB0BD52F3, `sceGxmTextureGetType` 0xF65D4917.
- Non-LINEAR layouts: `sceGxmTextureInitSwizzled` 0xD572D547,
  `sceGxmTextureInitSwizzledArbitrary` 0x5DBFBA2C,
  `sceGxmTextureInitLinearStrided` 0x6679BEF0, plus
  `sceGxmTextureSetMipFilter` 0x1CA9FE0B, `sceGxmTextureSetLodBias`
  0xB65EE6F7, `sceGxmTextureSetLodMin` 0xB79E43DD (draw still rejects
  anything but unit-zero LINEAR ABGR8).
- Depth: `sceGxmDepthStencilSurfaceInit` 0xCA9D41D1,
  `sceGxmDepthStencilSurfaceSetForceLoadMode` 0x0C44ACD7,
  `sceGxmDepthStencilSurfaceSetForceStoreMode` 0x12AAA7AF (SetContext with a
  depth surface still rejects).
- Fixed-function: `sceGxmSetCullMode` 0xE1CA72AE,
  `sceGxmSetFrontDepthFunc` 0x14BD831F, `sceGxmSetFrontDepthWriteEnable`
  0xF32CBF34, `sceGxmSetFrontPolygonMode` 0xFD93209D (emit state commands the
  bridge explicitly rejects unless default).
- `sceGxmPadHeartbeat` 0x3D25FCE9 (upstream null-checks and returns 0).

Explicitly NOT requested: `sceGxmDisplayQueueFinish` (Limbo does not import
it), `sceGxmBeginSceneEx`, `sceGxmMidSceneFlush`/`sceGxmExecuteCommandList`/
deferred contexts, `sceGxmTransferCopy/Downscale`, `sceGxmSetVertexTexture`,
precomputed paths, `sceGxmSetRegionClip` (not imported; scissor stays
rejected). Limbo imports neither `sceGxmFinish` nor `sceGxmTransferFill`;
its sync is display-queue plus notification waits.

## GXM2 fragment texture wiring (verified bounded path)

- Only fragment unit 0, `SCE_GXM_TEXTURE_LINEAR`, exact
  `SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR`, one effective mip (encoded count 0 or
  15), normalized coordinates, gamma off, neutral LOD bias 31/minimum 0,
  mip filtering off. Point/linear min and mag filters map independently.
  Repeat/mirror/clamp descriptor codes map to repeat/mirror-repeat/clamp-to-edge.
  Reserved fields and palette state must be zero. Other units/stages, layouts,
  swizzles, formats, mip/LOD/border states are rejected, not approximated.
- Dimensions are 1..4096; the padded guest footprint is capped at 16 MiB.
  `gxm::texture_size_first_mip` (`vita3k/gxm/src/textures.cpp`) and the production
  upload loop (`vita3k/renderer/src/texture/cache.cpp`) establish LINEAR row
  pitch `align(width,8)*4`. `gxm::get_stride_in_bytes` is for LINEAR_STRIDED
  **only**, which this bridge rejects. Full padded guest range is checked;
  each row's useful bytes are packed into an owned snapshot on every draw,
  even when no new descriptor command was emitted. No channel swap or Y flip.
- `renderer::set_texture` (`vita3k/renderer/src/renderer.cpp`) sends `uint32_t`
  index then `SceGxmTexture` by value. `sceGxmDraw` in
  `vita3k/modules/SceGxm/SceGxm.cpp` emits dirty textures used by the shader;
  fragment indices start at 0, vertex indices at 16. A context retains unit 0
  across draws/program changes; untextured programs omit the payload even if
  a descriptor remains bound. Nonzero fragment or any vertex texture usage
  is rejected from `ShaderProgram::textures_used` before upload.
- GXM4 (`0x47584d34`) replaces GXM3. After the old fixed words (magic, stride,
  index size, six payload lengths, attribute count) come a blend enabled u32 and
  seven guest blend words, then a depth enabled u32 and, when enabled, the guest
  depth format, depth func, depth write mode, load mode and an f32 clear value,
  then the texture count/header, viewport flat u32 plus six f32 bits
  (xOffset,yOffset,zOffset,xScale,yScale,zScale), render info, attributes, six
  payloads and packed texture bytes. GXM2/GXM3 no longer decode. No guest/native pointers
  or row padding cross into JS. JS checks version/count/size/enums/viewport
  finiteness/truncation and trailing bytes before compilation. C++ and JS
  snapshots happen before suspension; texture validation/readback failures
  cannot signal later batch notifications. Native and JS must be deployed
  together.
- Existing `spirv_recompiler.cpp` uses descriptor set 3 for fragment samplers;
  `vita3k/shader/include/shader/webgpu_spirv.h` splits unit n to texture 2n,
  sampler 2n+1. JS passes explicit ABGR8 translator hints and the consumer's
  existing `createProgram.fragmentTexture` / `draw.fragmentTexture` API.
  Translator and consumer are unchanged.
- HLE selection adds only `sceGxmTextureInitLinear`, `sceGxmSetFragmentTexture`,
  `sceGxmTextureSetData`, min/mag filter and U/V address-mode setters. These
  remain production functions. In particular `verify_texture_mode` rejects
  setting MIRROR on LINEAR: already-encoded MIRROR descriptors are accepted by
  the bridge, but no production setter semantics are changed. The guest probe
  covers clamp/repeat; mirror mapping is covered by the packet test only.

Extended repo-owned GXP fixture: two existing color draws, six textured draws
(4x4 texture with poisoned 8-pixel row padding, distinct channels/alpha, nearest
UV selection, repeat wrapping, linear interpolation, pixel-only mutation without
rebinding, then data-address replacement), and return to an untextured program.
Expected exit 42 and nine completed readbacks. No new shader binaries or WGSL.

Lightweight checks performed: `gxm_hle_packet_test.mjs` passes (36 sampler
combinations, 4x4 payload/ownership, malformed/version/truncation/trailing-byte
rejection); SDK guest build succeeds; shell/JS syntax and `git diff --check`
pass. The packet test emulates depadding and does **not** execute the C++ loop.
Parent verification rebuilt `vita3k_web_jit` and passed the real Memory64 guest
Chromium probe (exit 42, nine draw readbacks, zero missing NIDs). The consumer
regression also passed all 37 checks (41 after the C2 viewport extension,
unverified). The worker loads `vita3k_web_jit.js`, NOT
`vita3k_web.js`: rebuilding the latter leaves a stale JIT packet producer.
On the verification host, overriding Chromium's TMPDIR to the build scratch
caused adapter acquisition to fail; the default temporary directory worked.
Use scratch/cache overrides for compilation, not for the browser invocation.

Parent verification, **serially** (existing compiler/Naga staging required):

```sh
node --experimental-default-type=module browser/tests/gxm_hle_packet_test.mjs
# With the build environment's writable Emscripten cache configured:
timeout -s KILL 180s cmake --build build/web64 --target vita3k_web_jit -j1
bash browser/tests/build_gxm_guest_probe.sh
PLAYWRIGHT_MODULE_URL="file://$PWD/build/playwright/node_modules/playwright/index.mjs" \
  timeout -s KILL 60s node browser/tests/gxm_guest_probe_chromium.mjs
# Separate subsequent regression, never parallel with the Memory64 guest:
PLAYWRIGHT_MODULE_URL="file://$PWD/build/playwright/node_modules/playwright/index.mjs" \
  timeout -s KILL 60s node browser/tests/gxm_webgpu_smoke.mjs
```

Native rejection branches (invalid guest range, unsupported descriptor, missing
binding/nonzero unit) still need runtime negative tests. This is bounded texture
integration, not full renderer support or proof of retail gameplay.
