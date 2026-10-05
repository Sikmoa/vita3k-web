#!/usr/bin/env bash
# assemble.sh <dist> <out>: the static player site (GitHub Pages) from a
# vita3k_web_dist directory. A static host has no game, firmware or AOT
# image: player-config.json says so, and the player boots what the visitor
# uploads into browser storage (browser/PLAYER.md, "Static hosting").
set -euo pipefail
dist=${1:?dist directory}
out=${2:?output directory}
rm -rf "$out"
mkdir -p "$out"
cp -a "$dist"/. "$out"/
# The player is the site's front page; the old bootstrap page stays reachable.
mv "$out/index.html" "$out/bootstrap.html"
cp "$out/player.html" "$out/index.html"
printf '{"static": true, "titles": []}\n' > "$out/player-config.json"
printf '[]\n' > "$out/manifest.json"
touch "$out/.nojekyll"
for file in wasm64/vita3k_web_jit.wasm wasm64/vita3k_web_jit_mt.wasm shaders/naga.wasm coi_sw.js; do
  [[ -f "$out/$file" ]] || { echo "assemble.sh: missing $file in $dist" >&2; exit 1; }
done
echo "assembled $(du -sh "$out" | cut -f1) in $out"
