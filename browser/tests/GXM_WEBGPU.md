# GXM/WebGPU renderer boundary

The consumer `browser/web/gxm_renderer.js` is used by the guest bridge in
`gxm_hle_bridge.js`. The existing guest triangle milestone routes production
SceGxm commands through `browser/src/gxm_webgpu_bridge.cpp`, converts GXP with
the production USSE compiler and Naga, and copies GPU-completed pixels back to
guest memory. See GXM_GUEST_INTEGRATION.md for the historical evidence. The
consumer itself accepts WGSL; it is not the shader translator.

## Existing supported boundary

- RGBA8 offscreen targets and indexed triangle lists (uint16/uint32).
- One interleaved vertex stream; float scalar/vectors and unorm8x4 in the
  consumer. The native guest packet currently permits F32 attributes only.
- Explicit WGSL, or WGSL translated from guest GXP by the guest bridge.
- Group-0 uniform/read-only-storage bindings with explicit sizes/visibility;
  the original single uniform-buffer API remains available.
- Owned uploads, Memory64-safe host-pointer snapshots, tightly packed RGBA8
  readback from 256-byte-aligned GPU rows and explicit resource deletion.
- Unsupported indices, formats, sizes, shaders and concurrent direct consumer
  operations reject. Native guest state validation remains in force.

Depth/stencil, blending, MSAA, textures/samplers in this consumer, multiple
vertex streams, non-default viewport/scissor, additional target formats and
resident framebuffer resolve/presentation remain unsupported. Separate texture
translation prototypes are not proof of guest texture support. Each draw still
reads back; this is not a performance result or a completed retail renderer.

## C1 — bounded pipeline cache and ordered guest operations

Status: source changes and fixtures only; **not run** in this session.

The consumer retains at most 64 pipeline entries in LRU order per device. The
key uses exact WGSL strings, entry points, stride, sorted attribute layout,
sorted buffer binding layout (including size/type/visibility), and the fixed
target/primitive/depth/blend/sample state. No lossy shader hash is used. Dynamic
vertex/index/uniform contents are uploaded per draw and do not belong in the
pipeline key. Unsupported top-level pipeline fields reject with their names.
Future supported state must be added to both descriptor and key.

Program handles retain their own pipeline references. Deleting one handle or
evicting a cache entry cannot invalidate another handle. Failed compilation or
validation creates neither a cache entry nor a program. Disposal clears all
references. `pipelineCacheStats()` returns an immutable diagnostic snapshot;
cache misses include failed compilation attempts, entries include successes only.

The guest bridge now retains the consumer across draws, destroying each draw's
target/program handle only after submission/readback settles. It serializes
draws, fills and fences on one Promise chain so the shared renderer's exclusive
operation contract and device error scopes remain valid. A failure poisons
subsequent work with the original error, including queued fences; no later
fence may turn an unsupported draw into apparent success. Recovery currently
requires a new worker/module instance. This is device-work ordering, **not**
cooperative guest-thread scheduling.

Construction references: the existing `createProgram` descriptor/error-scope
path and `submit`/`destroyProgram` lifecycle in `gxm_renderer.js`; guest
`drawGuestSurface` packet snapshots and the native `web_gxm_draw` transport at
`gxm_webgpu_bridge.cpp:49`. Packet and initial pixel bytes are copied before
queueing or any await. Readback remains owned after unmap. Native writeback and
guest completion still happen after the awaited bridge call returns.

Assumptions to verify: equivalent descriptor keys produce interchangeable GPU
pipelines; translating identical GXP produces stable WGSL for useful cache hits;
the worker owns one device; the native producer preserves its sequential await/
writeback/signalling path. No target residency or shader-translation cache was
added. GXP is still translated per draw, and production compiler/WASI packaging
and ordinary worker startup remain separate gaps.

## Verification

From repository root, with Playwright Chromium and WebGPU:

```sh
PLAYWRIGHT_MODULE_URL=file://$PWD/build/playwright/node_modules/playwright/index.mjs \
  timeout -s KILL 55s node browser/tests/gxm_webgpu_smoke.mjs
```

Expected **pending** result:
`{"checks":29,"backend":"WebGPU","translatedGuestShader":false,"pipelineCache":true}`.
No game assets or shader compiler are needed. Unavailable WebGPU is a failure
to verify, never a successful skip.

The historical baseline was 18 checks. Eleven new assertions cover equivalent
descriptors sharing a pipeline but not handles, handle lifetime, changed shader
pixels, changed vertex layout, changed binding size/visibility, named unsupported
state rejection, failed compilation not entering the cache, successful reuse
after failure, disposal, and queue failure propagation to both fill and fence.
Pixels are produced by real WebGPU, not mocks. Before C1 there is no cache API,
unknown top-level state is ignored and a rejected guest packet does not prevent
an independent fence from succeeding. Cache eviction/device-loss behavior and
successful guest GXP draws still require integration verification; this small
smoke does not certify them or gameplay. See VERIFICATION_QUEUE.md.
