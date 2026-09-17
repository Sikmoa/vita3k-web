# Verification queue

These batches were written under a **no execution** constraint. Nothing below
has been run for these changes. Historical passing results in other documents
are baselines, not results for this revision. Run commands from the repo root
in the Linux/Emscripten verification environment. Never use proprietary assets
as committed fixtures.

## Preparation

```sh
export SCRATCH="$(mktemp -d /tmp/vita3k-web-verify.XXXXXX)"
mkdir -p "$SCRATCH/tmp" "$SCRATCH/emcache"
# These commands assume build/web and build/web64 are configured JIT builds.
# Use a compatible nvm Node binary for Memory64, not the system Node:
export NODE64="$NVM_BIN/node"
```

The runner must supply a complete writable Emscripten cache for the configured
Memory64 toolchain under `$SCRATCH/emcache`. Do not reuse an incomplete or
read-only system cache. A missing sysroot/port is a preparation failure, not a
passing or skipped test.

## A1 — nonblocking HLE clocks and queries

```sh
mkdir -p $SCRATCH/tmp && timeout -s KILL 180s env TMPDIR=$SCRATCH/tmp EMCC_CORES=1 ninja -C build/web -j1 vita3k_jit_backend_test_node vita3k_web_app_bench
rg 'sceKernel(GetProcessTime|GetSystemTimeWide|GetThreadCurrentPriority|GetThreadExitStatus|GetThreadCpuAffinityMask|GetSemaInfo|TryLockMutex)|sceKernelLibc(Clock|Time)' build/web/browser/runtime-hle-generated/startup_nids.inc
cat build/web/browser/runtime-hle-generated/startup_libraries.inc
```

Expected: build exit 0; all eleven new exports (plus the pre-existing Low
variant) appear with their authoritative NIDs. GetProcessTimeWide is B110C123.
Library list stays SceSysmem only. Configure failure means a selection or source
mapping mismatch; undefined link symbols mean a newly reachable dependency is
missing. There must be no new invented library initializer symbols.

```sh
timeout -s KILL 180s env TMPDIR=$SCRATCH/tmp EM_CACHE=$SCRATCH/emcache EM_FROZEN_CACHE=0 EMCC_CORES=1 ninja -C build/web64 -j1 vita3k_jit_backend_test_node vita3k_web_app_bench
timeout -s KILL 40s "$NODE64" build/web64/browser/vita3k_jit_backend_test_node.js
```

Expected: build/test exit 0. A Memory64-only failure points to address types,
layout, toolchain/cache configuration, or bridge ABI; investigate before retail.

Asset-holder integration (cannot run without decrypted content and firmware):

```sh
# Supply STAGE: root containing ux0/app/PCSE00268, vs0 and os0 trees.
: "${STAGE:?Set STAGE to the asset-holder supplied decrypted-content root}"
timeout -s KILL 30s node build/web/browser/vita3k_web_app_bench.js $STAGE PCSE00268
```

Expected A1 signal: no missing NID B110C123; execution advances past that import.
Record the next exact PC/bytes/NID/reason and import count privately. A timeout,
another unsupported instruction/import, or `process_exit=0` is not gameplay.
In a source-owned guest probe, bracket GetProcessTime/Low and LibcClock between
Wide reads (compare modulo 2^32 for Low/Clock); bracket LibcTime between RTC
seconds reads; verify null optional time pointers, invalid thread/semaphore IDs,
NOT_DORMANT from a running thread and nonblocking mutex contention errors.
These clock/query behavior checks are still pending a dedicated fixture.

## Baseline JIT and renderer regressions

```sh
timeout -s KILL 40s node build/web/browser/vita3k_jit_backend_test_node.js
PLAYWRIGHT_MODULE_URL=file://$PWD/build/playwright/node_modules/playwright/index.mjs timeout -s KILL 55s node browser/tests/gxm_webgpu_smoke.mjs
```

Expected: JIT exit 0, no FAIL or missing-family diagnostics; renderer exit 0 and
`{"checks":18,"backend":"WebGPU","translatedGuestShader":false}`. Chromium
and WebGPU are required; lack of either is a failure to verify, not a skip.
The renderer probe needs no game assets and does not prove retail rendering.
