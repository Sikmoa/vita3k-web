# Browser player and touch controls

The interactive launcher (`browser/tests/limbo_serve.mjs`) serves the player
from `browser/web/player.html`, `player.css`, `player.js`, and `pad_input.js`.
Changes to these files are served from source without rebuilding Wasm. The
existing `display.html` homebrew probe and its test interface are unchanged.
The `vita3k_web_dist` target also copies these assets as part of `web/`.

Start the existing launcher with your built runtime and staged files:

```sh
GXM_RUNTIME_DIST=build/web64/dist \
LIMBO_STAGE=.limbo_work/stage \
LIMBO_AOT=/absolute/path/to/title.aot.wasm \
node browser/tests/limbo_serve.mjs
```

`LIMBO_AOT` is optional. All existing launcher environment variables and query
parameters still apply. The server supplies title/app/AOT configuration through
`/player-config.json`; `player.html` needs this endpoint and the existing runtime,
manifest, stage, and AOT routes. Use HTTPS for a phone connecting over the network
because WebGPU requires a secure context.

## Playing on a phone

Press **Play**, then use **START** or **×** at the game's title screen. Touch
controls appear automatically on touch-capable devices. The **Touch** toolbar
button toggles them; **Controls & preferences** offers Automatic, Always show,
and Hidden modes, plus opacity and size. These preferences are stored locally
when browser storage is available. Size is capped on narrow/short phones to
keep the controls separate.

The overlay includes the four face buttons, a D-pad, both analog sticks, L/R,
and Start/Select. Multiple fingers can hold different controls simultaneously.
Drag the D-pad for diagonals or drag a stick for analog input; the D-pad also
drives the left stick, matching the existing arrow-key behavior. Keyboard
bindings are unchanged and listed in the preferences panel. Keyboard and touch
holds are independent, so releasing one does not release the other.

Input is released on pointer cancellation/lost capture, blur, backgrounding,
viewport changes, Stop, and opening a guest dialog or text-entry field. Hiding
the touch overlay releases its touches. Guest dialogs have tappable response
buttons, and the guest text-entry field uses the phone's keyboard. This is a
virtual **controller**; it does not emulate Vita front/rear touch surfaces.

While the game starts, the overlay reports its phase: the runtime module,
then each staged file — **Reading** it from persistent storage or
**Downloading** it, with counts, percentage and elapsed time behind a
progress bar — then the launch (and the AOT compile when `LIMBO_AOT` is
set). The byte fraction counts the whole title, so the read/download
counters are the ones that say how much actually crosses the network. The
UI has no borders; the rounded corners stay.

## Titles

Two ways a title becomes bootable:

* **Staged on the server** — `<stage>/ux0/app/<id>/eboot.bin`. The server owns
  the manifest (app directory, its patch, trophy data) and serves the bytes.
  `LIMBO_TITLE`/`LIMBO_APP` pick the default.
* **A package this browser holds** — upload a `.zip` of the game and the
  package names its own title (see below). No server-side staging, no
  restart.

The **Title** picker lists both, tagging them `· server` or `· package`;
switching titles reloads with `?title=<id>`, which is also the link to share.
Each title keeps its own persistent cache, and the AOT image is used only for
the title it was built for.

## Game packages & offline content

The upload takes a `.zip` of the game once. Its `ux0/app/<id>` (or bare
`app/<id>`) directory names the title, and every file it ships is stored in
persistent browser storage (OPFS) under that title — so one upload is all a
homebrew or a dump needs to become playable, including titles the server has
never seen. Accepted layouts: `ux0/…`, `app/<id>/…` and either of those under
wrapper folders; anything outside the title's own directory (firmware the
package happens to carry, for instance) is kept at its device root.

**Firmware is the one thing that must come from the server** (`os0`/`vs0`, a
few MiB): it is console system software, not part of any package. It
downloads on a title's first boot and is cached like everything else. Each
boot merges two sources — the server's manifest and the stored files for that
title — and the staging line says which is which (`Reading <path> from
storage` vs `Downloading <path>`, with read/download counters). A file the
server has and the package does not is downloaded; a file only the package
has comes from storage, and a miss there is an error rather than a doomed
fetch. Patches always come from the server. An unreadable or empty archive is
rejected before anything stored is touched.

**Sound on/off** mutes output without suspending guest audio. **Fullscreen**
keeps the toolbar and controls with the display; browsers without element
fullscreen support use an expanded in-page player. **Exit full** leaves either
mode. Debug and the sound test are collapsed by default. Runtime logs
retain their last 200 messages and update in batches.

## Frozen frames (phone)

Emulation and sound keep running when the graphics device is lost, so a
lost device looks like a frozen picture over a live game. The player now
names it instead: the renderer reports `[gxm-device] lost` with the reason,
the status becomes **Graphics device lost**, and the notice offers the
recovery (Stop, then Play — a fresh run gets a fresh device). If frames
stop for 15 s after the first one without a device report, a watchdog says
so in the notice and the log. The 5 s `[gxm-scene] stats` line carries the
device state, submitted-vs-completed queue serials, dropped scenes and
present failures; compare `scenes`/`submitSerial` against `presents` to
tell a wedged queue (submits continue, presents stop) from device loss.
When reporting a freeze, paste the adapter line, the last stats line and
any `[gxm-device]` lines from Debug.

## Slow frames (phone)

Each draw's WebGPU calls cross from the worker into the browser/GPU
process, which costs real time on a phone CPU, so the draw loop skips
re-emitting unchanged pipeline, bind groups, buffers, viewport, scissor
and stencil state (`stateSkips` counts the skipped calls). `uploadMs` vs
`submitMs` in the stats line tells buffer uploads apart from call/encode
overhead. If the GPU side is the bottleneck instead, `?scale=1` renders
at 960x544 (a quarter of the default 1920x1088 pixels).

**GPU back-pressure.** WebGPU's queue is unbounded, and a phone GPU that
falls behind never catches up: field measurements showed 300-500 encoded
scenes queued with `completedSerial` falling further behind every second.
A permanently backlogged vendor driver stalls the whole display stack on
Android — the *system* UI freezes, status-bar clock included — so the
renderer now refuses to queue more than `maxInFlight` scenes (default 6).
Past that limit scenes are dropped (`throttledScenes`), the page says so
and suggests `?scale=1`, and encoding resumes automatically once the queue
drains. `?maxInFlight=N` tunes the limit (`0` disables it); the 5 s stats
line carries `inFlight`, `maxInFlight`, `throttledScenes`,
`droppedScenes` and `completedSerial` for diagnosis.

## Validation

```sh
node browser/tests/player_controls_chromium.mjs
```

Requires Playwright and Chromium. `PLAYWRIGHT_MODULE_URL` and
`PLAYWRIGHT_CHROMIUM_EXECUTABLE` can select existing installations;
`PLAYER_SCREENSHOTS=/path/to/output` saves layout screenshots.

The test loads the real player with a small Worker fixture and uses Chromium
touch events. It checks Vita masks/axes, simultaneous inputs, release behavior,
dialogs, IME, restart, fullscreen fallback, and phone layouts. It does not
validate the emulator or retail game execution. A real-game check additionally
requires the built `vita3k_web_jit.js`/`.wasm` pair and shader dependencies in the
runtime dist, plus the staged game and any matching AOT module.

```sh
node browser/tests/zip_content_test.mjs          # zip reader + OPFS cache, no browser
node browser/tests/content_cache_chromium.mjs    # upload once, boot from storage
```

The unit test covers stored/deflated/empty/unicode entries, zip-slip and
junk rejection, CRC, manifest handling, `app/`-without-`ux0/` packages and
partial unpacks. The Chromium test serves a tiny fake title, uploads it as
a partial zip, then reboots with game downloads blocked and requires
staging to complete with zero stage hits.
