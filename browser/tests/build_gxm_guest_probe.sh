#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
export TMPDIR="$PWD/.limbo_work/tmp"
mkdir -p "$TMPDIR" .limbo_work/gxm
sdk=${VITASDK:-/opt/vitasdk/vitasdk}
# Embed repository-owned GXP binaries, never game assets or generated WGSL.
python3 - <<'PY'
from pathlib import Path
out = Path('.limbo_work/gxm/probe_shaders.h')
with out.open('w') as f:
    for name in ('color_v', 'color_f'):
        data = Path(f'tools/native-tool/src/shaders/{name}.gxp').read_bytes()
        f.write(f'static const unsigned char {name}[] __attribute__((aligned(16))) = {{')
        f.write(','.join(str(b) for b in data))
        f.write('};\n')
PY
timeout -s KILL 30s "$sdk/bin/arm-vita-eabi-gcc" -I.limbo_work/gxm browser/tests/vita_homebrew_fixture/gxm_probe.c \
  -Wl,-q -lSceGxm_stub -o .limbo_work/gxm/guest_probe.elf
timeout -s KILL 10s "$sdk/bin/vita-elf-create" .limbo_work/gxm/guest_probe.elf .limbo_work/gxm/guest_probe.velf
timeout -s KILL 10s "$sdk/bin/vita-make-fself" .limbo_work/gxm/guest_probe.velf .limbo_work/gxm/guest_probe.bin
