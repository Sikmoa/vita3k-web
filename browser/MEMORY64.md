# Opt-in browser Memory64 backend

This checkout contains an opt-in `VITA3K_WEB_MEMORY64` browser configuration.
It is disabled by default.  The existing browser configuration remains the
wasm32 sparse backend and native builds keep the native contiguous mapping.

The direct backend treats a Vita address as `uint32_t` and stores its bytes at
the fixed linear-memory offset

```text
guest_window_base = 0x1_0000_0000
guest_window_end  = 0x2_0000_0000   (exclusive)
```

`Address`, guest registers, guest PCs, function pointers, relocation values and
HLE-visible pointers remain 32-bit.  Only a host pointer and a generated Wasm
memory effective address become 64-bit.  Permission, allocation, ownership,
code-page and SMC metadata continue to be indexed by the original guest page.
The page-table lookup and physical backing allocation disappear from the direct
JIT path; validity, permissions, page-boundary checks and code-page handling do
not.

## Build contract (UNBUILT / UNVALIDATED)

The root option adds Emscripten's `-sMEMORY64=1` setting before CMake's compiler
and dependency probes.  Emscripten 3.1.69 accepts this setting but rejects
`-m64` during final link; a newer toolchain can replace it with `-m64` once
that link path is supported.  The browser graph propagates the same ABI to its
targets.  The browser memory configuration sets a fixed 8 GiB WebAssembly
memory (`INITIAL_MEMORY` and
`MAXIMUM_MEMORY`), disables growth, enables `WASM_BIGINT`, and keeps the current
unshared memory model.  A fresh build directory is required when switching the
option.  The hand-written JIT modules import the one runtime memory with a
Memory64 memory type (64-bit page limits, maximum present); they do not create
another memory.  The separately created region `funcref` table keeps ordinary
i32 slot numbers.  Emscripten's compiler-owned native function table is left to
the toolchain's wasm64 table ABI.

The direct configuration currently fails closed for shared Emscripten memory.
Shared-memory Memory64 flags and atomic encodings have not been needed by the
current single-worker cooperative runtime and remain a later toolchain task.

## Runtime heap boundary

`browser/src/memory64_heap.cpp` supplies the Emscripten libc `sbrk`, `_sbrk64`,
`brk`, and `emscripten_get_sbrk_ptr` interface used by the existing dlmalloc
configuration.  The break starts at the linker-provided `__heap_base` and may
grow only while it is strictly below `0x1_0000_0000`; requests that would cross
that boundary fail without changing the break.  Shrinking cannot move below
`__heap_base`.  This is a bounded morecore guard, not a replacement allocator
and not an attempt to shrink WebAssembly memory.  The object is linked directly
into each final browser executable so the stock unbounded `sbrk.c` archive member
cannot silently win; a duplicate strong definition is intended to fail the
link.

This source assumes static data, stack and the linker-selected `__heap_base`
are below the boundary, that the current Emscripten dlmalloc build uses sbrk
morecore with mmap disabled, and that the final link keeps the strong symbols.
Those assumptions need a real Emscripten link audit.  A fixed 8 GiB addressable
memory does not imply either a particular RSS or a particular physical-memory
commit policy; startup, reservation and allocation failure behavior remain
runtime questions.

## MemState and loader behavior

The Vita bitmap allocator and allocation records remain unchanged.  Direct-mode
allocation marks the same guest pages and permission bytes, then zeroes the
fixed window in place.  Free removes metadata and permissions but does not
shrink the WebAssembly memory or promise stale bytes are cleared.  Guest-to-host
conversion checks the guest mapping and returns `base + uint64(address)`.
Host-to-guest conversion accepts only pointers in the fixed window and rejects
all runtime pointers, so no wasm64 pointer is silently truncated.  Whole-range
read, write, fetch, memset and copy helpers use widened endpoint arithmetic;
an end exactly equal to 4 GiB is valid, while a crossing range is rejected.

Loader segment copies, BSS initialization and relocations therefore write the
same guest addresses through the direct helper.  Relocation destination
accumulators are widened before they are narrowed back to a checked 32-bit Vita
address.  External aliases are rejected in direct mode because the current
browser source has no supported renderer alias that should create a second
sparse backing.

Executable-page writes still update the existing code-page reference counts,
`smc_dirty` and dispatch epochs.  Host/HLE/loader writes are unchecked by the
ordinary `Ptr` access path, so a direct-mode host entry revalidates all cached
region bytes and executable permissions and bumps the global dispatch epoch
before publishing a new dispatch set.  This preserves invalidation at the cost
of a host-entry revalidation point rather than a per-access hook.

## JavaScript and generated Wasm ABI

`host_abi.js` is preprocessed by Emscripten and centralizes exact pointer
conversion.  Typed-array views use a checked Number offset in the runtime area;
raw wasm64 pointer arguments are passed as BigInts.  Uploaded files use custom
raw-pointer allocation exports, and the framebuffer hook receives the original
32-bit guest address after C++ has copied it into a runtime scratch buffer.
JIT module bytes are copied through the checked host view.  `memory64_post.js`
forces the Emscripten table-index conversion shim to BigInt for the validated
3.1.69/new-Node combination.  Guest addresses are never represented as BigInts
merely because the host build is wasm64.

## Preferred configuration and browser fallback

Memory64 (`wasm64-direct`) is the preferred browser configuration; the
sparse wasm32 backend remains as the automatic fallback. Selection happens at
runtime in `browser/web/worker.js` (no CMake default was changed, so existing
wasm32 consumers are unaffected):

- `display.html?memory=w64` forces the Memory64 build (fails loudly when the
  browser cannot run it); `?memory=w32` forces the wasm32 reference.
- Default (`auto`) probes `WebAssembly.validate` on a minimal Memory64
  module (`capabilities.js detectMemory64Support`), attempts the preferred
  module first, and falls back to wasm32 on any load/instantiation error.
  The fallback also covers a failed 8 GiB reservation on constrained devices
  (support probe and capacity are different things).
- The `ready` diagnostics report `memoryRequested`, `memoryFallback`, and the
  effective `memoryModel`, so any run is attributable after the fact.

Serving layout: `moduleDirectories` in `worker.js` names each memory model's
module directory next to the Worker (wasm64 in `wasm64/`, wasm32 beside it),
and the `vita3k_web_dist` target stages each build's modules into its
directory; a deployment offering both merges the two builds' `dist/`.
`locateFile` is based per attempt so each module resolves its own `.wasm`. Browser
requirement: Chrome 128+ (Memory64 unflagged). Firefox/Safari behavior is
deployment-tested via the fallback path, not assumed.

## Validation status (updated: validated September 2026)

The source-only caveat below is SUPERSEDED. Validated on Emscripten 3.1.69
with `-sMEMORY64=1` (note: `-m64` does not link on this toolchain), a local
unfrozen sysroot cache, and Node 24 for wasm64 execution:

- wasm64 backend suite 9157/9157; exit-42 fixture exit 42; display fixture
  156,399,069 instructions, exit 77, counter-identical to wasm32.
- Dispatch-map anomaly found and fixed in-tree (per-entry epoch bump made
  every pump re-entry miss: 717 vs 71); post-fix `host_miss` 71,
  `js_calls` 217, `tx_wasm` bit-identical to wasm32 on both modes.
- Container A/B (4 interleaved pairs): generated-Wasm time median -10%
  (4/4 pair wins); laptop Firefox uncapped: 77 -> 89.3 FPS (+16%).
- Remaining: Safari/Firefox deployment matrix, RSS/reservation telemetry,
  and the lazy-flags structural project. None of those are implied here.
