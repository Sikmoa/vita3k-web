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

## B1 — ordinary NEON integer arithmetic

First perform the A1 build commands on both ABIs. Then:

```sh
timeout -s KILL 40s node build/web/browser/vita3k_jit_backend_test_node.js
timeout -s KILL 40s "$NODE64" build/web64/browser/vita3k_jit_backend_test_node.js
for flags in 0 1; do
  for accounting in 0 1; do
    VITA3K_WASMJIT_PROMOTE_FLAGS=$flags VITA3K_WASMJIT_PROMOTE_ACCOUNTING=$accounting timeout -s KILL 40s node build/web/browser/vita3k_jit_backend_test_node.js
    VITA3K_WASMJIT_PROMOTE_FLAGS=$flags VITA3K_WASMJIT_PROMOTE_ACCOUNTING=$accounting timeout -s KILL 40s "$NODE64" build/web64/browser/vita3k_jit_backend_test_node.js
  done
done
```

Expected on each run: exit 0, `NEON integer arithmetic: 25 independent IR
fixtures passed`, and the final `WasmJit backend: ... checks passed (real
memory, no interpreter)` line. Do not expect the old exact 99,716 total, since
new checks were added. No Vector family missing / FAIL / NEON guest failed
messages. Emission failures identify missing dispatch; Wasm validation failures
identify stack/local type errors; lane assertions identify carry contamination,
word ordering or truncation; unchanged-register failures identify D/Q aliasing
or accidental status writes.

```sh
: "${VITASDK:?Set VITASDK to the VitaSDK root}"
timeout -s KILL 15s "$VITASDK/bin/arm-vita-eabi-as" browser/tests/jit_vector_integer_encodings.S -o "$SCRATCH/neon-integer.o"
"$VITASDK/bin/arm-vita-eabi-objdump" -d "$SCRATCH/neon-integer.o"
python3 browser/tests/jit_coverage_inventory.py --format markdown --output "$SCRATCH/JIT_COVERAGE_INVENTORY.md"
python3 browser/tests/jit_coverage_inventory.py --format json --output "$SCRATCH/jit-coverage.json"
python3 -m unittest discover -s browser/tests -p 'test_jit_coverage_inventory.py'
```

Compare each ARM encoding with the C++ guest fixture's field formula (and
Thumb halfwords with `thumb_word`). Representative expected ARM words:
VADD.I8 q15,q15,q1 = f24ee8c2; VSUB.I64 d31,d2,d31 = f372f82f;
VMUL.I16 q15,q15,d3[3] = f3dee8eb.
Assembler disagreement is a fixture problem to investigate, never a reason
to bypass a rejected guest instruction. Inventory should add eleven dispatch
routes while retaining VectorMultiply64 as missing. The inventory is a
generated report; it is not committed.

## B2 — NEON integer comparison/min/max/absolute family

Repeat the B1 build/test/mode commands on both ABIs. Expected additional line:
`NEON integer comparisons: 81 independent IR fixtures passed`, followed by
full backend success. B1's 25-fixture line must still appear. Raw IR and
guest tests compare all four result words, untouched registers and status.

```sh
timeout -s KILL 15s "$VITASDK/bin/arm-vita-eabi-as" browser/tests/jit_vector_compare_encodings.S -o "$SCRATCH/neon-compare.o"
"$VITASDK/bin/arm-vita-eabi-objdump" -d "$SCRATCH/neon-compare.o"
python3 browser/tests/jit_coverage_inventory.py --format markdown --output "$SCRATCH/JIT_COVERAGE_INVENTORY.md"
python3 -m unittest discover -s browser/tests -p 'test_jit_coverage_inventory.py'
```

Compare assembler words against `guest_comparisons` and `thumb_word`. Predicates
must produce lane-wide ones, signed/unsigned order must differ at the sign bit,
abs(INT_MIN) must retain its bits without QC, and signed extreme distance must
be lane-wide ones. Inventory should add 27 routes on top of B1's eleven (38
over the committed old snapshot). Regenerate the committed inventory only
after reviewing both batches, not just B1. No complete-NEON claim follows.

## B3 — scalar byte reversal and packed-lane stack correction

Repeat the B1 build/test/mode commands on both ABIs. Expected additional line:
`Byte reversal: 6 independent IR fixtures passed`, followed by full backend
success. B1's 25-fixture and B2's 81-fixture lines must still appear. The
fixtures compare `ByteReverseWord`, `ByteReverseHalf` and `ByteReverseDual`
against independent byte references, plus ARM REV/REV16/REVSH execution.

```sh
timeout -s KILL 15s "$VITASDK/bin/arm-vita-eabi-as" browser/tests/jit_byte_reverse_encodings.S -o "$SCRATCH/byte-reverse.o"
"$VITASDK/bin/arm-vita-eabi-objdump" -d "$SCRATCH/byte-reverse.o"
```

Compare the assembler output with the ARM words in `guest_reversal`; verify
the Thumb spellings are accepted by the same assembler. A Wasm validation
failure in this batch most likely means the i32 packed-lane accumulator was
accidentally changed back to the i64 scratch local. REVSH sign errors indicate
missing U16 masking or sign extension; REV64 word-order errors indicate a
missing swap. Inventory should add three scalar routes after regeneration.
Do not count the stale committed inventory until the generator has been run
and reviewed.

## Combined JIT regressions

```sh
timeout -s KILL 40s node build/web/browser/vita3k_jit_backend_test_node.js
```

Expected: exit 0, no FAIL or missing-family diagnostics.
