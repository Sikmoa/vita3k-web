# Vita loader reuse audit

This is the boundary map for the first browser increment that crosses from the
standalone ARM harness into Vita3K's real loader. It intentionally does not
introduce a second Vita loader or import resolver.

## Native call chain

```text
interface.cpp:load_app_impl
  -> modules/module_parent.cpp:load_module
     -> VFS read + SELF decryption
     -> kernel/load_self.cpp:load_self
        -> SELF envelope / ELF header selection
        -> ET_SCE_EXEC or ET_SCE_RELEXEC validation
        -> PT_LOAD allocation and copy/decompression
        -> PT_SCE_RELA relocation
        -> sce_module_info_raw metadata
        -> load_exports / load_imports
           -> kernel NID tables
           -> import stubs and relocation bindings
  -> interface.cpp:run_app
     -> KernelState::create_thread
        -> ThreadState::init
           -> cpu::init_cpu (currently DynarmicCPU)
        -> ThreadState::start
  -> ThreadState::run_loop
     -> CPU run/step
     -> native SVC import hook
     -> modules::call_import
        -> resolve_import / generated HLE bridge
```

## Existing Vita responsibilities

`vita3k/kernel/src/load_self.cpp` is the authoritative implementation. It
already handles:

- Plain Vita ELF and the SCE/SELF envelope.
- Vita executable types (`ET_SCE_EXEC` and `ET_SCE_RELEXEC`).
- SELF section-info offsets, compression state, and segment encryption checks.
- Fixed and relocatable segment placement through native `MemState`.
- Vita's module-info location encoded in `e_entry` (`e_entry & 0x3fffffff`),
  plus `sce_module_info_raw` start/stop/TLS/metadata fields.
- Vita relocation records in `PT_SCE_RELA`.
- Export and import tables, library NIDs, function/variable stubs, late
  variable binding, and unresolved-import stubs.

`vita3k/modules/module_parent.cpp` owns module-file acquisition and delegates
the actual image load to `load_self`. `interface.cpp` establishes the main
thread from the loaded module's `start_entry`. `modules/module_parent.cpp`
contains the existing HLE dispatch (`resolve_import` and `call_import`), while
`kernel/src/thread.cpp` recognizes the native CPU backend's SVC stop and reads
the NID from the import stub.

## Browser blockers

The native loader cannot currently be added to `browser/CMakeLists.txt` as a
small library:

1. `load_self.cpp` requires native `KernelState`, `MemState`, `Ptr<T>`, and the
   relocation implementation; these are coupled to native virtual memory and
   emulator-wide state.
2. It includes `sce-elf-defs.h` and `self.h` from the Vita toolchain, plus
   `miniz` for SELF segments. The checked-out `external/vita-toolchain` and
   `external/psvpfstools` submodules are empty here.
3. `KernelState::create_thread` creates SDL host threads, and
   `ThreadState::init` always constructs `DynarmicCPU` through `cpu::init_cpu`.
4. The existing HLE bridge expects a native `CPUState`; the browser interpreter
   has a deliberately separate state model and its SVC service 0 is test-only,
   not Vita ABI behavior.
5. No redistributable `.elf`, `.velf`, `.self`, `.suprx`, `.vpk`, or `eboot.bin`
   fixture exists in the repository. `tools/native-tool` is source only and
   expects an installed VitaSDK (`arm-vita-eabi-*`, `vita-elf-create`, and
   related tools).

## Chosen next extraction boundary

Do not expand `browser::ElfImport` into a linker. The next real implementation
should extract or factor a byte-oriented front end from the authoritative
native loader, with native behavior unchanged:

```text
browser Uint8Array
  -> Vita ELF/SELF envelope inspection
  -> Vita ELF metadata + PT_LOAD descriptors
  -> browser memory/process adapter
  -> existing module/import/HLE semantics when their dependencies are adapted
```

That extraction needs a real Vita ELF/VELF input for validation. Until a VitaSDK
fixture is supplied or the VitaSDK submodule is populated, claiming an actual
Vita module load or HLE boundary would be incorrect. The generated ARM ELF and
synthetic guest tests remain permanent generic regression tests, but they are
not substitutes for this missing Vita fixture.
