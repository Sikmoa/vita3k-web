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

Regression baseline: `gxm_webgpu_smoke.mjs` (18 checks),
`gxp_webgpu_smoke.mjs` (8 checks), `gxp_translation_smoke.mjs` (22 checks),
and `gxm_guest_probe_chromium.mjs` all pass in Chromium/SwiftShader.
Host-only shader/renderer checks do not substitute for guest execution.

## Integration blockers / notes

- Device acquisition must happen before the wasm64 runtime loads
  (`gxm-probe-worker.js` imports `gxm_hle_bridge.js` first). Acquiring inside the
  suspended (Asyncify) stack intermittently reports "WebGPU adapter unavailable".
- The guest draw path currently supports ABGR8 linear surfaces, one interleaved
  F32 vertex stream, U16/U32 indexed triangle lists, default viewport/clip,
  and metadata-packed vertex/fragment uniform buffers. It preserves the initial
  guest surface between draws. The bounded GXM2 texture path below is guest
  GPU-verified. Unsupported depth, blending, other textures, instancing,
  non-default viewport/clip and other commands are rejected, not silently ignored.
- C1 adds a bounded pipeline cache (reuse verified; eviction/device-loss tests
  outstanding) and ordered guest draw/fill/fence
  queue (GXM_WEBGPU.md). Each draw still translates GXP, uploads buffers and
  creates/reads back its target. Shader translation caching and persistent
  surface ownership remain to be implemented.
- Production distribution of compiler, Naga and WASI shim assets and ordinary
  worker integration still need packaging work; the historical test server is
  not evidence of a complete app launcher.
- `vita3k/renderer/src/sync.cpp` intentionally NOT linked (single-threaded, GPU-fenced
  bridge replaces its queue thread); `wishlist` / `subject_done` reimplemented locally.
- `browser/web/gxm_context.js` is owned by main; the bridge uses the separate
  `gxm_hle_bridge.js` instead.
- Full renderer completeness and Limbo gameplay are NOT verified. Textured GXP
  has separate host-driven browser tests. Bounded guest texture wiring is now
  GPU-verified; depth, blending, display queue integration
  and retail-app scheduling remain unfinished.
  `rendererComplete=false` in the probe output remains intentional.

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
- GXM2 (`0x47584d32`) replaces GXM1, including untextured draws. Following the
  old ten u32 words comes count 0/1; if 1, eight u32s are width, height, GXM
  format, min, mag, U, V, packed byte length. Then old render-info, attributes,
  six payloads, followed by packed texture bytes. No guest/native pointers
  or row padding cross into JS. JS checks version/count/size/enums/truncation
  and trailing bytes before compilation. C++ and JS snapshots happen before
  suspension; texture validation/readback failures cannot signal later batch
  notifications. Native and JS must be deployed together.
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
regression also passed all 37 checks. The worker loads `vita3k_web_jit.js`, NOT
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
