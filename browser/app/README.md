# Vita3K Web app

The site's front page: a Library of the games in this browser, Files (the
site's private storage, OPFS, as folders: browse, upload, download, rename,
remove), Settings and About, and a full-window player. Vue 3, UnoCSS (`presetWind3`, Lucide icons via
`presetIcons`) and VueUse, built with Vite; packages with Bun.

```sh
bun install
bun run dev        # http://localhost:5173, the runtime proxied from the dev server
bun run build      # dist/, which browser/pages/assemble.sh puts at the site's root
bun run typecheck
```

`bun run dev` needs the dev server running (`browser/tests/limbo_serve.mjs`,
`http://localhost:8080` unless `VITA3K_RUNTIME` says otherwise): it serves the
runtime the app loads.

## How it fits

The emulator is not bundled. The app loads it at run time from its own
directory (`src/runtime.ts`), where the site keeps it next to `worker.js`:

- `library.js` — imports (games, encrypted dumps, `.pkg` + zRIF, firmware
  `.PUP`s), the title list, firmware status, saves;
- `vita_session.js` — a running game: staging, the worker, audio, saves, the
  guest's dialogs and keyboard, the pad;
- `pad_input.js` — touch controls and gamepads; `capabilities.js` — the
  device checks on About.

The player page (`player.html`, `player.js`) uses the same modules.

## Rules the code keeps

- **Relative URLs only** (`base: './'`): GitHub Pages serves the site under
  `/<repo>/`. Routes are hashes (`#/library`, `#/settings`, `#/about`,
  `#/files/<folder>/…`, `#/play/<title>`), so no server rewrites are needed.
- **Cross-origin isolation** for the threaded runtime: `coi.js` runs first in
  `index.html`; nothing loads from other origins.
- **A canvas hands its control to one worker only**: the session asks the
  player for a fresh `<canvas>` on every run.
- **Audio starts from a user gesture**: opening a game from its card is one.
- Colours are the theme in `uno.config.ts`; no borders, blur or glow.

## Layout

```
src/
  App.vue, main.ts, style.css
  router.ts       hash routes
  runtime.ts      the runtime modules and their types
  settings.ts     settings (localStorage) and what a session gets from them
  library.ts      the library and the import in progress
  opfs.ts         the Files page's storage operations
  components/     navigation, dialogs, controls, game card, import dialog
  pages/          Library, Files, Settings, About, Play
  player/         touch controls, the game's dialogs and keyboard
```
