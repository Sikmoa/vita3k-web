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
full-target case is pixel-verified, the sub-rect case has no test); `emscripten_sleep` wishlist waits are reachable from
the HLE submit path; negative xScale games render with abs like both
production backends.
