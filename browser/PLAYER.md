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

**Sound on/off** mutes output without suspending guest audio. **Fullscreen**
keeps the toolbar and controls with the display; browsers without element
fullscreen support use an expanded in-page player. **Exit full** leaves either
mode. Diagnostics and the sound test are collapsed by default. Runtime logs
retain their last 200 messages and update in batches.

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
