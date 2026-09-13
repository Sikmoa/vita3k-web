# Real MemState standalone tests and interpreter API

These tests compile **`../src/mem.cpp` and `../src/allocator.cpp`**, not a browser
memory substitute. They require restored `external/boost`, `external/fmt`, and
`external/spdlog` headers. No CPU/kernel/browser CMake changes are needed.

From the repository root:

```sh
bash vita3k/mem/tests/run_mem_state_tests.sh all
# Or: native / wasm
# CXX=clang++ (default), EMXX=em++, MEM_TEST_OUT=/tmp/vita3k-mem-state-tests
```

Native tests use UBSan with recovery disabled and exercise both direct and
page-table modes, including a real OS write-protection fault. Wasm tests use
`em++`, embed the actual wasm module in a Node launcher (`SINGLE_FILE`), then
**execute it with Node**. The heap starts at 32 MiB, may grow to 64 MiB, and malloc
must return failure rather than abort (`ABORTING_MALLOC=0`). C++ exceptions are
enabled for metadata-allocation failure handling. The tests explicitly grow the
heap to 48 MiB, verify pointer stability, then intentionally exhaust the host
heap and check guest-allocation rollback. The resulting `Cannot enlarge memory`
messages are expected, not failed assertions.

## API handoff (CPU/loader integration is outside this change)

Declared in `mem/functions.h`, implemented once in `mem/src/mem.cpp`:

```cpp
bool mem_read(const MemState &, Address, void *destination, size_t);
bool mem_write(MemState &, Address, const void *source, size_t);
bool mem_fetch(const MemState &, Address, void *destination, size_t);
bool mem_set_permissions(MemState &, Address, size_t, MemPerm);
bool mem_host_to_guest(const MemState &, const void *, Address &);
```

- **Use `mem_read` / `mem_write` / `mem_fetch` in interpreter callbacks**, handling
  `false` as a guest access fault. Fetch checks `Execute`, independently of Read.
  No host signal, automatic unprotection, or watch callback occurs on failure.
- Helpers preflight the entire range (allocation, permission, 32-bit guest address
  overflow). They handle unaligned and cross-page accesses, including adjacent
  but separately backed allocations. A rejected request leaves the destination
  unchanged. Zero bytes is a successful no-op, even with null pointers/addresses.
  Host buffers must be valid for the requested size and must not overlap the
  guest range (the copying contract is `memcpy`, not `memmove`).
- `MemPerm` bits are Read=1, Write=2, Execute=4. Named combinations are
  `None`, `ReadOnly`, `WriteOnly`, `ReadWrite`, `Execute`, `ReadExecute`, and
  `ReadWriteExecute`. Fresh allocations default to **RWX** for existing loader
  compatibility. A loader should set final segment permissions explicitly after
  copying/relocating. These tests do not prove that any loader already does so.
- Permission changes round outward to guest pages. `mem_set_permissions` rejects
  unallocated/overflowing ranges before changing anything. The legacy
  `protect_inner` / `unprotect_inner` also update metadata; unprotect restores
  RWX. Watch protection callbacks remain explicit/native-fault driven.
- `Ptr::get` and `Ptr::atomic_compare_and_swap` remain **unchecked trusted HLE /
  native paths**, not substitutes for interpreter access validation. A Wasm bulk
  `Ptr(mem)+length` is contiguous within one allocation, or one external mapping;
  it is not guaranteed across separate allocations or partial external remaps.
- Wasm uses one owned contiguous buffer per allocation and a flat 1,048,576-entry
  page lookup. Its guest metadata/bitmap allocator are shared with native. The
  fixed Wasm tables cost approximately **9.125 MiB**, plus allocation buffers and
  per-allocation map nodes; there is no 4 GiB host reservation/backing. Aligned
  front trimming retains one owner and invalidates freed front-page entries.
- Native retains its 4 GiB OS reservation, direct `memory + guest_address` path,
  optional absolute-bias page table, and OS protection/fault handling. No native
  signal-context or mmap code is compiled into the Wasm translation unit.
- Reverse pointer conversion validates live mappings using integer host ranges,
  not subtraction of unrelated pointers. Unrelated, freed, null, trimmed-front,
  and shadowed original backing pointers fail (`addr = 0`). Like any raw-pointer
  API, it cannot detect a stale pointer whose host address has since been reused.
- Access/lifecycle/protection changes must be externally serialized, as with
  existing `Ptr` use. External mapping buffers are caller-owned and must outlive
  their mapping, including teardown. Native external buffers/ranges must be
  host-page aligned; Wasm does not require that host alignment.

## Coverage

The focused executable covers null reservation, accounting, uninitialized and
reinitialized state, allocation size/rounding overflow, the last guest byte at
`0xffffffff`, full multi-page bulk copies and zero fill, stacks/alignment and
front reuse, overlap rejection, checked transfers across independent allocations,
permission failures without partial copies, RX and execute-only fetch, safe
reverse translation, native and explicit watch callback handling, high-page
protection, CAS, external remap/copyback/free/teardown, Wasm growth, and OOM rollback.

Existing allocator tests can also be run directly:

```sh
clang++ -std=c++20 -O1 -fsanitize=undefined -fno-sanitize-recover=all -pthread \
  -Ivita3k/mem/include -Iexternal/googletest/googletest/include \
  -Iexternal/googletest/googletest \
  vita3k/mem/src/allocator.cpp vita3k/mem/tests/allocator_tests.cpp \
  external/googletest/googletest/src/gtest-all.cc \
  external/googletest/googletest/src/gtest_main.cc \
  -o /tmp/vita3k-mem-state-tests/allocator-native
/tmp/vita3k-mem-state-tests/allocator-native
```

## Validation record

- Focused native executable (Clang 21.1.8 and GCC 15.2.0, UBSan): **517 checks
  passed per compiler**, both modes; no sanitizer diagnostics. GCC additionally
  warned about the existing `static_cast<const uint32_t>` in the bitmap allocator
  (`-Wignored-qualifiers`).
- Focused wasm32 executable (`em++` 3.1.69, Node 22.22.1): **291 checks passed**.
  A second direct em++ build with `-fsanitize=undefined
  -fno-sanitize-recover=all` also **passed all 291 checks**, without sanitizer
  diagnostics.
  Heap grew **33,554,432 -> 50,331,648 bytes**. Deliberate oversized allocation
  attempts produced expected heap-cap diagnostics and preserved allocator state.
- Existing allocator suite (Clang, UBSan): **13/13 tests passed**, including the
  1,024,000-iteration randomized battle test. Vendored GoogleTest emitted a
  `char8_t`/`char32_t` character-conversion warning; no sanitizer diagnostics.
- The initial separate GCC/em++ object-compilation probes both exceeded their
  **120-second tool timeout** under concurrent builds; they produced warnings,
  not C++ compilation errors. Subsequent complete builds/runs used a longer
  timeout and succeeded. The focused recipe suppresses vendored fmt/spdlog
  deprecation warnings, not runtime failures.

These results establish focused production MemState behavior on Linux native and
actual wasm32 under Node. They are **not** evidence of a complete VitaSDK game
boot, browser integration, Windows/macOS behavior, concurrent access correctness,
or interpreter/loader adoption of the new helpers.
