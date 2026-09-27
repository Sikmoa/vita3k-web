# GXM guest integration

Guest GXM calls run the production SceGxm HLE (`vita3k/modules/SceGxm`, the
exports selected in `browser/runtime_hle.cmake`). Its renderer commands go to
`browser/src/gxm_webgpu_bridge.cpp`, which renders them through the GXS1 scene
stream (GXM_WEBGPU.md). Render targets stay on the GPU; guest memory receives
rendered pixels only with surface sync.

## Guest probe

`browser/tests/vita_homebrew_fixture/gxm_probe.c` is a VitaSDK homebrew that
uses repository GXP programs (`tools/native-tool/src/shaders`): initialisation
and its error paths, a transfer fill, color and textured indexed draws, and
teardown. It checks its rendered pixels in guest memory after each
`sceGxmFinish` and exits 42, so it runs with surface sync:

```sh
bash browser/tests/build_gxm_guest_probe.sh      # -> .limbo_work/gxm/guest_probe.bin
cmake --build build/web64 --target vita3k_web_dist
PLAYWRIGHT_MODULE_URL=file://$PWD/build/playwright/node_modules/playwright/index.mjs \
  node browser/tests/gxm_guest_probe_chromium.mjs
```

`rendererComplete=false` in its output is intentional: the probe covers a
bounded feature set, not the renderer.

## Retail apps

Limbo runs through `limbo_serve.mjs` and `limbo_app_chromium.mjs` (SCRIPTS.md).
Message dialogs: the HLE keeps `sceMsgDialog` state as on desktop and
`browser/src/msg_dialog_bridge.cpp` posts it to the page (`vita-dialog`). The
dev server draws the dialog and answers it with the keys; the probe answers
every dialog with `LIMBO_DIALOG` (`cross`, `circle` or `none`).
