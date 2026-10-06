#!/usr/bin/env bash
# build.sh [build root] [site dir]: both browser runtimes and the static
# player site, from a clean checkout inside the flake's dev shell
# (nix develop --command browser/pages/build.sh). CI runs exactly this
# (.github/workflows/pages.yml).
#   build root (build/pages-build): web64 (single-threaded, fibers) and
#     web64-mt (threaded), which stages into web64's dist
#   site dir (build/pages): the site, from assemble.sh, with the app
set -euo pipefail
cd "$(dirname "$0")/../.."
root=${1:-build/pages-build}
site=${2:-build/pages}
jobs=${VITA3K_BUILD_JOBS:-$(nproc)}
common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DVITA3K_WEB_MEMORY64=ON -DVITA3K_WEB_JIT_TESTS=ON
  -DVITA3K_WEB_DISPLAY_FIXTURE=OFF -DCMAKE_CXX_SCAN_FOR_MODULES=OFF)
emcmake cmake -S . -B "$root/web64" "${common[@]}"
cmake --build "$root/web64" --target vita3k_web_dist -j "$jobs"
emcmake cmake -S . -B "$root/web64-mt" "${common[@]}" -DCMAKE_C_FLAGS=-pthread -DCMAKE_CXX_FLAGS=-pthread \
  -DVITA3K_WEB_DIST="$PWD/$root/web64/dist"
cmake --build "$root/web64-mt" --target vita3k_web_dist -j "$jobs"
# The app (browser/app): Vue, built with Bun into browser/app/dist.
(cd browser/app && bun install --frozen-lockfile && bun run build)
browser/pages/assemble.sh "$root/web64/dist" "$site" browser/app/dist
