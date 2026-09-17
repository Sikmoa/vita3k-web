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

Expected: guest `exit 42`, 56 imports, zero missing NIDs,
`missingSceGxmBridge=false`, two `GXM WebGPU GXP indexed draw readback completed`
log lines, and `CPU backend: WasmJitCPU (no fallback)`.

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
  guest surface between draws. Unsupported depth, blending, textures, instancing,
  non-default viewport/clip and other commands are rejected, not silently ignored.
- C1 adds an unverified bounded pipeline cache and ordered guest draw/fill/fence
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
  has separate host-driven browser tests, but guest texture state, depth, blending,
  display queue integration and retail-app scheduling remain unfinished.
  `rendererComplete=false` in the probe output remains intentional.
