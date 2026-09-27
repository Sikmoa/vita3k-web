# GXM/WebGPU renderer boundary

The consumer `browser/web/gxm_renderer.js` is the WGSL-level renderer behind
`browser/web/webgpu.js`, exercised by `gxm_webgpu_smoke.mjs`. The runtime does
not use it: `browser/src/gxm_webgpu_bridge.cpp` emits one GXS1 scene stream per
command list, consumed by `browser/web/gxm_scene.js`. See
GXM_GUEST_INTEGRATION.md for the historical evidence. The consumer itself
accepts WGSL; it is not the shader translator.

## Existing supported boundary

- RGBA8 offscreen targets and indexed triangles: triangle lists (uint16/uint32)
  and triangle fans. WebGPU has no fan topology, so the native bridge expands a
  fan to the equivalent triangle list (same index buffer, centre = the first
  guest index); cull mode is restricted to NONE, so the expansion cannot change
  which faces are drawn.
- One interleaved vertex stream. The guest attribute format travels in the
  GXS1 scene stream and is translated in `gxm_scene.js`: U8N/S8N/U16N/S16N with 2 or 4 components,
  F16 with 2 or 4, and F32 with 1 to 4.
- Explicit WGSL.
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
**Guest SceGxm unit-zero LINEAR texture state is wired and verified**: ABGR8
(`0x0c000000`, byte-identical to RGBA8) and 16-bit swizzled U8U8 GRRR
(`0x07002000`, RGB = the low byte replicated, A = the high byte), the format the
retail Limbo frame's sprite draws use.
The Memory64 JIT guest probe performs six textured GXP draws, including padded
rows, filtering/wrapping, pixel mutation without rebinding and data-address
replacement. See `GXM_GUEST_INTEGRATION.md` for the exact bounded descriptor
contract and nine-readback fixture (exit 42).

Per-draw viewports map NDC to a target sub-rect (see C2); omitted viewports
cover the full target. Viewport rects are dynamic draw state, never pipeline
key material. Negative/non-finite rects reject; outside-viewport texels keep
their initial values.

`createProgram` additionally accepts the guest fixed-function fragment state as
WebGPU values: `writeMask` (the translated guest color mask) and `blend` with a
color and an alpha component (`operation`, `srcFactor`, `dstFactor`). Both are
pipeline key material. A descriptor missing a component, an unknown operation
or factor, a saturate factor outside the color source slot, or a write mask
outside 0..15 rejects. `dst-alpha-saturate` is not a WebGPU factor at all and
is rejected earlier, at program creation on the native side.

`createTarget(width, height, { depthFormat })` adds a depth-stencil attachment
(`depth16unorm`, `depth32float`, or `depth24plus-stencil8`), and
`createProgram({ depthStencil: { format, depthCompare, depthWriteEnabled } })`
adds the matching pipeline state. `submit(..., { depth })` supplies the pass
attachment: it is always clear-on-load with the guest background depth and
`storeOp: 'discard'`, because the native producer rejects the guest
force-load/force-store states that would need guest depth contents. The
attachment is therefore per draw, and depth continuity across the draws of one
scene is only promised when the guest asks for it with force-load (which
rejects instead of rendering with silently lost contents); the desktop GL
backend clears per draw the same way. A depth
pipeline submitted to a target without a depth attachment (or the reverse), a
format mismatch, and an out-of-range clear value all reject.

Still unsupported: MSAA, multiple
vertex streams, vertex attribute formats outside the JS table above,
non-default region-clip/scissor, additional target formats, stencil state or
stencil writeback. Two-sided depth state is approximated with the front face
(see C2); each remaining unsupported state rejects rather than rendering
implicitly. The guest fixture
verifies only the bounded texture path, not these remaining features. Each draw still
reads back; this is not a performance result or a completed retail renderer.

## Presentation (verified)

The display queue is how a frame reaches the screen, and it is now wired end to
end. `browser::gxm_initialize` creates the same `SceGxmDisplayQueue` guest
thread the desktop backend creates (null entry, standard priority; a dormant
thread parks in `run_loop` without executing the null entry), but no host
`std::thread`. `sceGxmDisplayQueueAddEntry` therefore drains the queue inline
under `VITA3K_BROWSER_GXM` by calling the production `display_entry_thread`,
whose loop gains one browser-only check that stops on an empty queue instead of
blocking on a condition variable no other thread can signal. The desktop
`wait_empty()` for `displayQueueMaxPendingCount == 1` is skipped for the same
reason: the entry has already been drained by the time the call returns.

Draining runs the guest display callback through
`ThreadState::run_guest_function` on the display queue thread. The browser
execution host implements this cooperatively (`guest_thread_runtime.cpp`): the
calling fiber parks and the scheduler dispatches the display thread, exactly
like the desktop host thread. The callback's `sceDisplaySetFrameBuf`
(`0x7A410B64`) / `_sceDisplaySetFrameBuf` (`0xF51523CB`) is the presentation
trigger: `browser/src/vita_app.cpp` calls `vita3k_web_present_frame` after that
import, and `vita_display_bridge.cpp` tightens the real `sce_frame` rows and
posts them through the host hook. `NewFrame` still records the predicted frame
for renderer state; it is not the presentation path.

Verified with retail Limbo: the title screen presents real content
(`framesPresented: 3` in a 240 s probe, first frame 960x544, saved as
`.limbo_work/limbo_frame_000001_960x544.png`). The frame arrives early in the
run; the probe then waits out its deadline because the app keeps loading, not
because presentation is slow.

## C2 — display-queue/sync commands, viewport state, state-driven render info

Status: implemented and verified end-to-end against retail Limbo, which
presented its first frames (verification and mechanism in the Presentation
section above). The HLE allowlist additions it unblocked are selected in
`browser/runtime_hle.cmake` (`sceGxmPadHeartbeat`, `sceGxmDisplayQueueAddEntry`);
the remaining inventory is in `GXM_GUEST_INTEGRATION.md`.

Native (`browser/src/gxm_webgpu_bridge.cpp`):

- Accepts `SignalSyncObject` (EndScene fragment completion via
  `subject_done`), `WaitSyncObject` (BeginScene fragment wait via `wishlist`;
  steady state is already signaled, genuine backpressure blocks with desktop
  semantics instead of skipping), and `NewFrame` (display-queue entry records
  the predicted frame into display state and sets `should_display`; the pixels
  themselves are presented by the display-queue drain, see above). All other
  new opcodes (notably `MidSceneFlush`,
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

Guest blend, depth, stencil, viewport and vertex-format words travel in the
GXS1 scene stream and are translated to WebGPU in `browser/web/gxm_scene.js`;
the guest blend descriptor is retained on the WebGPU fragment program at
creation time (`browser/src/gxm_webgpu_program.h`) because the guest pointer is
not kept.

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
`{"checks":68,"backend":"WebGPU","translatedGuestShader":false,"pipelineCache":true}`.
The eight added texture assertions cover quadrant pixels with snapshot ownership,
texel changes on later draws, pipeline reuse, missing/partial texture rejection,
unsupported samplers, rejection of ignored textures, and resource-layout cache keys.
These use test WGSL, not a guest texturing fixture.

The 27 blend/depth assertions use real blended and depth-tested fragments, not
mocks: src-alpha ADD over an opaque destination at quarter alpha (with a
one-step unorm tolerance, since the blend unit's rounding mode is unspecified),
`max` blending, the write mask with blending both disabled and enabled, blend
state sharing and differentiating the pipeline key, four malformed blend
descriptors; then far/near ordering, near-first rejection of the farther quad,
depth-write-disabled keeping the earlier depth value (with the write-enabled
control), `greater` compare against a zero clear value (with its control),
`depth16unorm`, depth-state pipeline reuse, and six rejected depth
configurations.
No game assets or shader compiler are needed. Unavailable WebGPU is a failure
to verify, never a successful skip.

The historical baseline was 18 checks (37 after C1). Four new assertions cover
viewport clipping to a sub-rect, initial-surface preservation outside the
viewport, and explicit rejection of negative/non-finite viewport rects.
Earlier assertions cover equivalent
descriptors sharing a pipeline but not handles, handle lifetime, changed shader
pixels, changed vertex layout, changed binding size/visibility, named unsupported
state rejection, failed compilation not entering the cache, successful reuse
after failure, and disposal.
Pixels are produced by real WebGPU, not mocks. Before C1 there is no cache API,
unknown top-level state is ignored and a rejected guest packet does not prevent
an independent fence from succeeding. Cache eviction/device-loss behavior and
successful guest GXP draws still require integration verification; this small
smoke does not certify them or gameplay. See VERIFICATION_QUEUE.md.
