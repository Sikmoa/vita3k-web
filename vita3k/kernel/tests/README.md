# Sized SELF/VELF loader tests

These tests call the **production** `load_self_sized`, `load_self`, and
`unload_self`, with the native memory allocator, relocation engine and linker.
There is no mock parser or parallel loader. Guest instructions are not executed.

## Run

Use an already configured **Ninja** native build with
`VITA3K_INTERPRETER_CPU=ON` and `CMAKE_EXPORT_COMPILE_COMMANDS=ON`. The existing
`build/native-interpreter` configuration in this checkout works. Build its runtime
test target first so the production libraries and headers agree:

```sh
cmake --build build/native-interpreter --target vita3k_interpreter_runtime_tests -j 1

python3 vita3k/kernel/tests/run_sized_loader_tests.py \
  --build-dir build/native-interpreter --output-dir /tmp/vita-sized-tests

ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
python3 vita3k/kernel/tests/run_sized_loader_tests.py \
  --build-dir build/native-interpreter --output-dir /tmp/vita-sized-tests-asan \
  --sanitize --ndebug
```

The helper uses the existing compile database and runtime-test link rule, but
compiles the current `load_self.cpp`, `relocation.cpp`, and test source explicitly
into the output directory. It never modifies project CMake or source files. The
sanitizer option instruments those three translation units, **not all prebuilt
libraries**. `--ndebug` disables assertions in those units; test checks remain
active. Run the two builds serially on memory-constrained hosts.

Required fixtures:

- `browser/tests/vita_homebrew_fixture/fixture.velf`
- `browser/tests/vita_homebrew_fixture/eboot.bin`

The helper also uses `/opt/vitasdk/vitasdk/bin/vita-make-fself -c` to produce a
compressed SELF from the real VELF in the temporary output directory. Override
the SDK root with `--sdk`. It does not alter the existing fixtures.

## Coverage and observed results

Both normal and ASan+UBSan+NDEBUG runs passed **2,973 checks**, including six
positive genuine-fixture loads: raw VELF, existing uncompressed SELF, and
SDK-compressed SELF, each through the sized and legacy APIs. Sized positives
also use byte-unaligned input and enable ELF dumping. Checks verify module
registration, segment sizes, entry points, real import-stub linking, dump ELF
magic and unload. The synthetic image additionally checks BSS initialization.

Negative tests exercise:

- Null/zero-size input and every truncated prefix of mandatory synthetic input.
- ELF/SELF header and table offsets, count/stride mismatches, integer boundaries.
- Segment payload spans, `PT_LOAD filesz > memsz`, allocation/address wrap.
- Compressed input length versus decompressed `p_filesz`, invalid zlib streams,
  short/oversized output, including the real SDK compressed relocation segment.
- Missing/non-loadable/truncated/unaligned module info; import/export ranges;
  start/stop offsets, TLS and exception-table ranges.
- Short/long import records, zero/unknown/truncated record strides, count
  overflow, unsupported TLS imports, NID/entry tables, function stub extent,
  variable reference-table extent, names, export targets and process-param
  fields actually read by the loader.
- Truncated top-level relocation records.
- Rejected cases leave no module, binding/export registration or leaked segment
  allocation (checked against the native allocator's allocation-name map).

## Deliberate limits

This is **parser and immediate linker-metadata validation, not a security
boundary for hostile binaries**. The existing relocation engine is still
responsible for execution of relocation records. Top-level record framing is
checked, but relocation segment references, patch targets, opcode semantics,
variable/late-binding relocation record interpretation and transactional rollback
of linker side effects have **not** been comprehensively hardened. A malicious
relocation can still access outside its intended segment, including before the
post-relocation metadata validation. Table aliasing/self-modifying linker data
and allocation/decompression resource-exhaustion policies are also not addressed.

The sized API does not authenticate SELF content or validate unused ELF section
headers/SELF control records. Checks of process parameters cover fields read
while loading, not every pointer followed later by guest/runtime code. The
legacy wrapper intentionally retains its trusted unknown-input-size contract.
Native tests and partial sanitizer instrumentation are not browser execution
coverage or proof of whole-loader memory safety.
