#!/usr/bin/env bash
# assemble.sh <dist> <out> [app dist]: the static player site (GitHub Pages)
# from a vita3k_web_dist directory and the app's build (browser/app/dist). A static host has no game, firmware or AOT
# image: player-config.json says so, and the player boots what the visitor
# uploads into browser storage (browser/PLAYER.md, "Static hosting").
set -euo pipefail
dist=${1:?dist directory}
out=${2:?output directory}
app=${3:-}
rm -rf "$out"
mkdir -p "$out"
cp -a "$dist"/. "$out"/
# Games run on the JIT: the interpreter module and the development pages
# that use it (the bootstrap index.html, display.html and its guest.elf) stay
# out. The app is the site's front page (the player page without it); the
# player page stays at player.html.
rm -f "$out"/wasm64/vita3k_web.{js,wasm} "$out"/{index.html,display.html,display.js,guest.elf}
if [[ -n "$app" ]]; then
  [[ -f "$app/index.html" ]] || { echo "assemble.sh: no app build in $app (bun run build in browser/app)" >&2; exit 1; }
  cp -a "$app"/. "$out"/
else
  cp "$out/player.html" "$out/index.html"
fi
printf '{"static": true, "titles": []}\n' > "$out/player-config.json"
printf '[]\n' > "$out/manifest.json"
touch "$out/.nojekyll"
for file in wasm64/vita3k_web_jit.wasm wasm64/vita3k_web_jit_mt.wasm shaders/naga.wasm decrypt/vita3k_decrypt.wasm coi_sw.js \
    library.js vita_session.js player.html; do
  [[ -f "$out/$file" ]] || { echo "assemble.sh: missing $file in $dist" >&2; exit 1; }
done
echo "assembled $(du -sh "$out" | cut -f1) in $out"
