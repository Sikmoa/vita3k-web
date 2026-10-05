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

Touch controls and gamepads take presses while the game loads; whatever is
held when it starts reaches it at once.
Input is released on pointer cancellation/lost capture, blur, backgrounding,
viewport changes, Stop, and opening a guest dialog or text-entry field. Hiding
the touch overlay releases its touches. Guest dialogs have tappable response
buttons, and the guest text-entry field uses the phone's keyboard. This is a
virtual **controller**; it does not emulate Vita front/rear touch surfaces.

While the game starts, the overlay reports its phase with byte-level
progress: the runtime `.wasm` download, then each staged file — **Reading**
it from persistent storage or **Downloading** it, with counts, percentage
and elapsed time behind a progress bar — then the launch. With `LIMBO_AOT`
the AOT download is metered the same way, followed by the device-side
compile (which exposes no progress events, so the bar holds full while it
works). The byte fraction counts the whole title, so the read/download
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

The **Title** picker stays visible below the game screen and lists both,
tagging them `· server` or `· package`;
switching titles reloads with `?title=<id>`, which is also the link to share.
A title the server stages and this browser also holds as a package is listed
twice; the package boots with `?source=package` (firmware and patches still
come from the server). The two are stored apart — the package under
`<title>/<app>`, the server's copy under `<title>/<app>.server` — so booting
one never rewrites the other. A server copy cached before that split (every
file carrying a server version) keeps serving from the package key and is
not listed as a package.
Each title keeps its own persistent cache, and the AOT image is used only for
the title it was built for.

Uploaded packages use seedless AOT by default with the JIT backend: the
runtime builds an image from the loaded game and firmware code at launch.
This adds startup time, and the generated image is currently rebuilt on each
launch. Code outside the image uses the JIT fallback. `?buildAot=0` disables
building at launch; `?buildAot=1` also enables it for server-staged titles.

## Game packages & offline content

The upload takes a `.zip` of the game once. Its `ux0/app/<id>` (or bare
`app/<id>`) directory names the title, and every file it ships is stored in
persistent browser storage (OPFS) under that title — so one upload is all a
homebrew or a dump needs to become playable, including titles the server has
never seen. Accepted layouts: `ux0/…`, `app/<id>/…` and either of those under
wrapper folders; anything outside the title's own directory (firmware the
package happens to carry, for instance) is kept at its device root.

**Firmware comes from the server or its own upload** (`os0`/`vs0`): it is
console system software, not part of any game package. From the server it
downloads on a title's first boot and is cached like everything else. The
**Firmware** upload takes a `.zip` holding the `os0` and `vs0` folders of
an installed firmware (desktop Vita3K keeps them in its data directory),
stores it once and lends it to every title; the server's or a package's own
copy of a file wins over it. Each
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

## Static hosting (GitHub Pages)

`.github/workflows/pages.yml` builds both runtimes with
`browser/pages/build.sh` (in the flake's dev shell) and publishes the site
`browser/pages/assemble.sh` makes from the dist: the player as `index.html`,
a `player-config.json` of `{"static": true}` and an empty `manifest.json`.
Enable it under Settings → Pages → Source: GitHub Actions.

A static site ships no game, firmware or AOT image. Visitors upload the
firmware and then a game; the first game reloads the page into
`?title=<id>`, and later visits open the first stored title. Uploaded games
build their AOT image at launch. The firmware must be installed already (a
desktop Vita3K install's `os0`/`vs0`); games may be encrypted dumps (below).

## Encrypted games

An upload holding a NoNpDrm dump — its app folder with `sce_pfs/` and the
license `sce_sys/package/work.bin`, as dumping tools and PSN downloads
unpack — is decrypted as it is stored, the way desktop Vita3K installs one:
`decrypt_worker.js` runs `dist/decrypt/vita3k_decrypt` (browser/decrypt:
psvpfstools' PFS parser and Vita3K's SELF decryption over OpenSSL's
libcrypto, built from the release tarball with Emscripten). It decrypts one
file per call and streams each into storage through a synchronous access
handle, so memory holds one SELF at most, whatever the game's size. Stored
zip entries are read in place; deflated ones are inflated into storage first.
`.pkg` files with a zRIF, and the firmware's `PSVUPDAT.PUP`, are not read.

GitHub Pages sends no headers, so `coi.js` registers `coi_sw.js`, a service
worker that adds the cross-origin isolation headers to every same-origin
response, and reloads once: `SharedArrayBuffer`, and with it `?threads=1`,
then works. A page already isolated (the dev server) skips it.

```sh
nix develop --command browser/pages/build.sh   # build/pages
PAGES_DIR=build/pages PAGES_FIRMWARE_ZIP=firmware.zip PAGES_GAME_ZIP=game.zip \
  node browser/tests/pages_static_chromium.mjs
```

The test serves the site under `/vita3k-web/` with no special headers,
uploads both archives through the page and boots until frames present
(`?threads=1` by default, `PAGES_PARAMS=` for the single-threaded build).

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

**GPU back-pressure.** Field measurements showed hundreds of submissions
outstanding during phone freezes. The renderer limits outstanding GPU
submissions with `?maxInFlight=N` (default 6; integer 0 disables the limit).
This includes scene, canvas presentation, and readback submissions. A canvas
blit and its optional pixel readback use one submission together.

When full, the C++ producer suspends through Asyncify, letting the Worker
handle browser events and completion callbacks. It then retries the same
scene or presentation. Commands and texture uploads are never discarded.
Load-preserving render targets and textures can retain data across frames;
dropping a scene corrupts those dependencies, and suppressing a single
presentation cannot repair them. The guest also receives no successful
scene notification until the scene has actually been submitted.

There is only one outstanding completion probe. It covers submissions made
before its registration; its resolution immediately starts another probe
for any uncovered tail. CPU execution continues synchronously whenever
capacity is available. Device loss, a rejected completion, or a probe with
no observed completion for 15 seconds releases the wait with an error.

The 5 s `[gxm-scene] stats` line and `sceneStats()` include:

| Field | Meaning |
| --- | --- |
| `submitSerial`, `completedSerial`, `inFlight` | All submitted batches vs observed completion; not frame counts |
| `maxInFlight`, `peakInFlight` | Configured cap and highest observed number outstanding |
| `throttledScenes`, `throttledPresents` | Busy attempts retried after waiting; **not drops** |
| `queueWaits`, `queueWaitMs`, `queueWaitMaxMs` | Count, total time, and longest producer wait |
| `completionProbes`, `completionLastMs`, `completionAvgMs`, `completionMaxMs` | Probe resolutions and request-to-callback latency |
| `completionPendingMs` | Age of the current probe |
| `submitMs`, `uploadMs` | Synchronous JS encoding/upload time, excluding queue waits |

Probe latency includes GPU work, browser/IPC scheduling and Worker event-loop
delay; it is **not GPU execution time**. Compare the same gameplay interval
at `?scale=1&maxInFlight=2`, `6`, and `12`, using counter deltas for cumulative
timings. FPS increasing with a larger window suggests sensitivity to the
window/probe latency, but does not by itself prove where the GPU time goes.
The cap stays explicit rather than automatically increasing on a slow phone.
C++ `[gxm]` reports `gpu_wait_ms` separately from `js_submit_ms` (the former
includes scene and presentation waits; `scene_ms` includes scene waits).

**Rebuild required:** this changes the C++/JS submission protocol. Rebuild
`vita3k_web_dist` for both deployed memory models, then merge their dists.
The source-serving dev server loads JS edits immediately, so stop it during
the rebuild or expect a renderer/runtime mismatch until Wasm is updated.
Old runtimes fail at graphics initialization with a rebuild message.
The guest AOT image does not need regeneration for this host renderer change.

```sh
cmake --build build/web --target vita3k_web_dist
cmake --build build/web64 --target vita3k_web_dist
cp -a build/web/dist/. deploy/
cp -a build/web64/dist/. deploy/
```

`?cores=1` and `?hleProfile=1` are now forwarded from the player to the
Worker, along with the other diagnostic switches understood by `worker.js`.

## Validation

```sh
node --experimental-vm-modules browser/tests/gxm_queue_test.mjs
node browser/tests/gxm_queue_chromium.mjs
node browser/tests/player_controls_chromium.mjs
```

The queue tests cover exact completion watermarks, ordered retries without
GPU writes when busy, presentation/readback accounting, and loss/rejection/
timeout recovery. The Chromium queue test checks actual persistent target
pixels under forced saturation. These fixtures do not measure phone FPS or
validate the rebuilt C++/Asyncify path with a retail title.

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
