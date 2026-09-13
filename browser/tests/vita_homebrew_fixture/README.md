# Genuine VitaSDK startup fixture

`main.c` returns 42. The SDK's **default crt0 and libc** must run before that
return can reach the real `sceKernelExitProcess` import. This is not a tiny
hand-written instruction probe. No startup code, imports, ELF headers, or SELF
segments are synthesized by the test.

## Build only the guest (no native/Qt dependencies)

From the repository root:

```sh
cmake -S browser/tests/vita_homebrew_fixture -B build/vita-homebrew-fixture \
  -DVITASDK=/opt/vitasdk/vitasdk
cmake --build build/vita-homebrew-fixture --target vita3k_vita_homebrew_fixture
```

All generated files (`main.o`, `fixture.elf`, `fixture.velf`, `eboot.bin`) go in
the binary directory. Existing artifacts beside `main.c` are not inputs. The
recipe deliberately preserves the SDK defaults, including linking with `-Wl,-q`
so `vita-elf-create` can process relocations. Rebuild cleanly after changing the
SDK's libraries/linker scripts. No downloaded or copyrighted firmware is needed.

## Native interpreter launch probe

```sh
cmake -S . -B build/native-interpreter -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=/opt/qt/6.11.2/gcc_64 \
  -DQt6_ROOT=/opt/qt/6.11.2/gcc_64 \
  -DUSE_DISCORD_RICH_PRESENCE=OFF \
  -DVITA3K_INTERPRETER_CPU=ON \
  -DVITA3K_VITASDK_LAUNCH_TEST=ON \
  -DVITASDK=/opt/vitasdk/vitasdk
cmake --build build/native-interpreter --target vita3k_vitasdk_launch_test -j 1
ctest --test-dir build/native-interpreter -R '^vita3k_vitasdk_launch$' \
  --output-on-failure -V
```

This target builds its own guest at
`build/native-interpreter/vita3k/vita_homebrew_fixture/eboot.bin`.
The probe constructs the production `EmuEnvState`, initializes memory/kernel and
exported variables (including the production HLE library-state initializers),
loads the full image with `load_self_sized`, then starts the
loader-selected entry via `KernelState::create_thread` / `ThreadState::start`.
The production SDL host thread runs `ThreadState::run_loop`; its import callback
forwards every call to `::call_import`. The harness only observes the existing
`KernelState::process_exit_callback`. It does not implement any HLE imports.

Success requires a real process-exit request with status **42** and no missing
imports. A CPU error, ordinary thread return, load failure, or timeout is a
failure, not an expected-pass/skip. The whole process (including teardown) has a
10-second watchdog, and CTest has a separate 15-second timeout. The watchdog
exits with 124; completed-but-unsuccessful execution returns 1; setup/input
errors return 2.

On a CPU error the probe reads registers only after `run_loop` parks dormant and
prints PC/CPSR/LR/SP, r0-r12, and disassembly at the stopped PC. The current
interpreter restores the faulting PC on failure and emits its own opcode/reason
trace. These diagnostics never patch the guest, retry an instruction, or skip
ahead.

The current minimal interpreter may fail in crt0 before the first import. That
failure is the intended **bring-up evidence**, not proof that the fixture ran to
completion. This native probe exercises the real runtime; it does not itself
claim an Emscripten/browser execution or rendering result.
