// Ahead-of-time image build and load, shared by the two launch paths: the
// retail-app launch (vita_app.cpp) and the single-homebrew launch
// (vita_runtime.cpp, which display.html drives via run-vita).
//
// Both paths need both halves. An AOT module records a hash of every code
// range it was built from, and WasmJitCPU::load_aot refuses the module when the
// guest code it actually loaded differs. The two paths load different module
// sets -- run-app loads the title plus the firmware modules it links, run-vita
// loads one eboot and nothing else -- so a module built by one is rejected by
// the other. Keeping the builder and the load side here is what lets each path
// build from, and be served, the code it will actually execute.
//
// Format and policy: vita3k/cpu/src/wasmjit/AOT.md.
#pragma once

#include <emuenv/state.h>

// Build one AOT module from the modules env has loaded. Offline: writes
// out_path and returns 0, or a negative value on failure.
int build_aot_image(EmuEnvState &env, const char *out_path);

// Hand the module the host supplied (a page fetches and compiles it, then sets
// Module.vita3kAotModule before calling the launch entry point) to the runtime
// and report the outcome on stdout as the run-app path always has:
// "AOT on: ...", "AOT off: ..." or "AOT REJECTED: ...".
// Returns 1 on, 0 off, negative when rejected.
int load_aot_image(EmuEnvState &env);
