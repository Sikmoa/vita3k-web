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

The consumer additionally supports optional fragment texture unit zero: group 3
bindings 0 (2D RGBA8 texture) and 1 (sampler), matching `webgpu_spirv.h`.
`createProgram({ fragmentTexture: true, ... })` requires each draw to supply
`fragmentTexture: { width, height, pixels, format: 'rgba8unorm', sampler }`.
One mip only; nearest/linear min/mag filtering and clamp/repeat/mirror-repeat
U/V addressing are accepted. Texels and sampler state are snapshotted before
await, uploaded per draw, and texture resources are destroyed after readback.
Texture layout participates in the pipeline key; texels and sampler values do
not. Missing data, wrong sizes and unsupported sampler/texture fields reject.
**Guest SceGxm unit-zero LINEAR ABGR8 texture state is wired and verified.**
The Memory64 JIT guest probe performs six textured GXP draws, including padded
rows, filtering/wrapping, pixel mutation without rebinding and data-address
replacement. See `GXM_GUEST_INTEGRATION.md` for the exact bounded descriptor
contract and nine-readback fixture (exit 42).

Per-draw viewports map NDC to a target sub-rect (see C2); omitted viewports
cover the full target. Viewport rects are dynamic draw state, never pipeline
key material. Negative/non-finite rects reject; outside-viewport texels keep
their initial values.

Depth/stencil, blending, MSAA, multiple
vertex streams, non-default region-clip/scissor, additional target formats and
resident framebuffer resolve/presentation remain unsupported. The guest fixture
verifies only the bounded texture path, not these remaining features. Each draw still
reads back; this is not a performance result or a completed retail renderer.

## C2 — display-queue/sync commands, viewport state, state-driven render info

Status: implemented, NOT yet run (parent verifies serially). Renderer-side
slice only; the HLE allowlist additions it unblocks are listed for the HLE
owner in `GXM_GUEST_INTEGRATION.md` and are not selected here.

Native (`browser/src/gxm_webgpu_bridge.cpp`):

- Accepts `SignalSyncObject` (EndScene fragment completion via
  `subject_done`), `WaitSyncObject` (BeginScene fragment wait via `wishlist`;
  steady state is already signaled, genuine backpressure blocks with desktop
  semantics instead of skipping), and `NewFrame` (display-queue entry records
  the predicted frame into display state and sets `should_display`; pixels are
  NOT presented). All other new opcodes (notably `MidSceneFlush`,
  `TransferCopy/Downscale`, `MemoryMap/Unmap`) still reject in preflight.
- `SignalNotification` and `SyncSurfaceData` now publish under
  `notification_mutex` and `notify_all`, mirroring `sync.cpp`
  `handle_notification` and `scene.cpp` `signal_notifications`. Previously
  values were written with no wakeup, so a selected
  `sceGxmNotificationWait` could never observe them. Failed batches still
  publish nothing.
- Arbitrary GXM viewports are forwarded instead of rejected. `SetState`
  Viewport handling mirrors `state_set.cpp` record fields (flip, z
  offset/scale; flat forces flip `(1,-1,1,1)` and z `(0,1)`); draws without
  any recorded viewport state reject rather than render implicitly.
- The vertex render-info block now carries the real flip/flag/screen/z from
  record state (previously the flag was hardcoded to 1, mis-describing flat
  viewports to the translated shader). Screen dimensions remain the color
  surface size, matching `gl/draw.cpp`.
- Non-default region clip still rejects explicitly; there is no scissor stage.
  Indexed/default vertex/fragment uniform buffers need no change: they already
  flow through `set_uniform_buffer` into the packed draw payload.

Packet GXM3 (`0x47584d33`, replaces GXM2; native and JS deploy together):
fixed words as before, then viewport flat u32 and six f32 bits
(xOffset,yOffset,zOffset,xScale,yScale,zScale), then render info, attributes,
payloads and texture bytes. GXM2 now fails loudly in the decoder.

JS (`gxm_hle_bridge.js`): `decodeGuestDrawPacket` validates the viewport
words (flat must be 0/1, floats finite) and exports pure `gxmViewportRect`,
which mirrors `vulkan/sync_viewport_real` (res_multiplier 1) with
negative-height normalization; flat covers the full target. The rect is
computed before any await and passed per draw to `submit`, which applies
`setViewport(x, y, w, h, 0, 1)` per draw. Depth stays `[0, 1]` because the
translated WebGPU shader (`is_vulkan` path) applies z offset/scale itself.

Assumptions to verify: a WebGPU viewport rect renders the same NDC mapping
as the equivalent Vulkan viewport (affine-equivalent by construction; the
full-target case is already pixel-verified, the sub-rect case is covered by
the new smoke checks); `emscripten_sleep` wishlist waits are reachable from
the HLE submit path; negative xScale games render with abs like both
production backends.

## C1 — bounded pipeline cache and ordered guest operations

Status: real Chromium/WebGPU smoke passed, including pipeline-cache checks and
the consumer texture extension below. Cache eviction and device-loss scenarios
are not covered by this smoke test.

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

Verified result (exit 0):
`{"checks":41,"backend":"WebGPU","translatedGuestShader":false,"pipelineCache":true}`.
The eight added texture assertions cover quadrant pixels with snapshot ownership,
texel changes on later draws, pipeline reuse, missing/partial texture rejection,
unsupported samplers, rejection of ignored textures, and resource-layout cache keys.
These use test WGSL, not a guest texturing fixture.
No game assets or shader compiler are needed. Unavailable WebGPU is a failure
to verify, never a successful skip.

The historical baseline was 18 checks (37 after C1). Four new assertions cover
viewport clipping to a sub-rect, initial-surface preservation outside the
viewport, and explicit rejection of negative/non-finite viewport rects.
Eleven earlier assertions cover equivalent
descriptors sharing a pipeline but not handles, handle lifetime, changed shader
pixels, changed vertex layout, changed binding size/visibility, named unsupported
state rejection, failed compilation not entering the cache, successful reuse
after failure, disposal, and queue failure propagation to both fill and fence.
Pixels are produced by real WebGPU, not mocks. Before C1 there is no cache API,
unknown top-level state is ignored and a rejected guest packet does not prevent
an independent fence from succeeding. Cache eviction/device-loss behavior and
successful guest GXP draws still require integration verification; this small
smoke does not certify them or gameplay. See VERIFICATION_QUEUE.md.
