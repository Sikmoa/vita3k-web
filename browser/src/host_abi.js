#preprocess
// Emscripten preprocesses this file when linking (--pre-js). The build's
// MEMORY64 setting determines the mode; this is not a runtime toggle or a
// second Wasm memory.
Module['vita3kMemory64'] = {{{ MEMORY64 ? 'true' : 'false' }}};
Module['vita3kMemoryModel'] = Module['vita3kMemory64'] ? 'wasm64-direct' : 'wasm32-sparse';
Module['vita3kHostPointerBits'] = {{{ POINTER_BITS }}};

// Emscripten's libc environment does not inherit Node's process.env. Forward
// only these opt-in diagnostics, also accepting browser Module options.
Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(() => {
  for (const name of ['VITA3K_TRACE_MODULE_IMPORTS', 'VITA3K_TRACE_HLE',
      'VITA3K_WASMJIT_FAULT_TRACE', 'VITA3K_WASMJIT_REJECT_TRACE',
      'VITA3K_BENCH_PC_SAMPLE', 'VITA3K_HLE_PROFILE', 'VITA3K_BENCH_SECONDS', 'VITA3K_AOT_SEEDS_OUT',
      'VITA3K_AOT_BUILD', 'VITA3K_AOT_SEEDS', 'VITA3K_AOT_LIMIT', 'VITA3K_AOT_TRACE', 'VITA3K_AOT_CHECKED_MEMORY', 'VITA3K_AOT_EXACT_FP', 'VITA3K_AOT_TRACE_MISSES', 'VITA3K_AOT_DIFF', 'VITA3K_AOT_CANARY',
      'VITA3K_AOT_EXCLUDE_THREADS', 'VITA3K_GUEST_CORES', 'VITA3K_AOT_UNTIL', 'VITA3K_AOT_DIFF_THREAD', 'VITA3K_AOT_DIFF_AFTER', 'VITA3K_TEXTURE_VERIFY', 'VITA3K_JIT_TIMING', 'VITA3K_FPS_HACK', 'VITA3K_BENCH_INPUT', 'VITA3K_RESOLUTION_SCALE', 'VITA3K_SURFACE_SYNC']) {
    const value = Module[name] ??
      (typeof process !== 'undefined' ? process.env?.[name] : undefined);
    if (typeof value === 'string') ENV[name] = value;
  }
});

// Benchmark-only: skip WebGPU draws so the CPU path can run under Node.
Module['vita3kNullGpu'] = Module['VITA3K_NULL_GPU'] === '1' ||
  (typeof process !== 'undefined' && process.env?.VITA3K_NULL_GPU === '1');

// Typed-array offsets are Numbers; raw Wasm i64 arguments are BigInts. Convert
// only after an exact range check. No bitwise coercion of native pointers.
// These run on every JIT dispatch and GXM submission, so the checks use
// Numbers (exact below 2^53, far above any memory size) and allocate nothing:
// BigInt arithmetic and an 8 GiB byteLength are heap values each time.
let hostLimit = 0;
Module['vita3kHostOffset'] = (pointer, length = 0) => {
  // Raw wasm32 i32 pointer exports/imports may arrive as signed Numbers.
  // Reinterpret only that known 32-bit ABI; never coerce a wasm64 pointer.
  if (!Module['vita3kMemory64'] && typeof pointer === 'number' &&
      Number.isInteger(pointer) && pointer < 0 && pointer >= -0x80000000)
    pointer += 0x100000000;
  // A BigInt at or above 2^53 converts to at least 2^53 and fails the limit.
  const start = typeof pointer === 'bigint' ? Number(pointer) : pointer;
  if (typeof start !== 'number' || !Number.isSafeInteger(start) || start < 0 ||
      !Number.isSafeInteger(length) || length < 0)
    throw new RangeError('invalid host memory range');
  // The memory can only grow: re-read its size only when a range exceeds it.
  if (start + length > hostLimit) hostLimit = wasmMemory.buffer.byteLength;
  if (start + length > hostLimit)
    throw new RangeError('host memory range is outside the runtime area');
  return start;
};
Module['vita3kHostPointer'] = (pointer) => {
  const offset = Module['vita3kHostOffset'](pointer);
  if (!Module['vita3kMemory64']) return offset;
  return typeof pointer === 'bigint' ? pointer : BigInt(offset);
};
Module['vita3kHostBytes'] = (pointer, length) =>
  new Uint8Array(wasmMemory.buffer, Module['vita3kHostOffset'](pointer, length), length);

// Emscripten owns its native function table and any wasm64 table adaptation.
// Our separately created region_table always uses ordinary i32 slot numbers.
Module['vita3kNativeFunction'] = (pointer) => {
  const index = Number(pointer);
  if (!Number.isSafeInteger(index) || index < 0 || index > 0xffffffff ||
      BigInt(index) !== BigInt(pointer))
    throw new RangeError('invalid native function table index');
  return getWasmTableEntry(index);
};
