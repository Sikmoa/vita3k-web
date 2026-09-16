#preprocess
// Emscripten preprocesses this file when linking (--pre-js). -m64 determines
// MEMORY64; this is not a separate runtime toggle or a second Wasm memory.
Module['vita3kMemory64'] = {{{ MEMORY64 ? 'true' : 'false' }}};
Module['vita3kMemoryModel'] = Module['vita3kMemory64'] ? 'wasm64-direct' : 'wasm32-sparse';
Module['vita3kHostPointerBits'] = {{{ POINTER_BITS }}};

// Typed-array offsets are Numbers; raw Wasm i64 arguments are BigInts. Convert
// only after an exact range check. No bitwise coercion of native pointers.
Module['vita3kHostOffset'] = (pointer, length = 0) => {
  // Raw wasm32 i32 pointer exports/imports may arrive as signed Numbers.
  // Reinterpret only that known 32-bit ABI; never coerce a wasm64 pointer.
  if (!Module['vita3kMemory64'] && typeof pointer === 'number' &&
      Number.isInteger(pointer) && pointer < 0 && pointer >= -0x80000000)
    pointer += 0x100000000;
  if ((typeof pointer !== 'number' && typeof pointer !== 'bigint') ||
      (typeof pointer === 'number' && !Number.isSafeInteger(pointer)) ||
      !Number.isSafeInteger(length) || length < 0)
    throw new RangeError('invalid host memory range');
  const start = BigInt(pointer);
  const end = start + BigInt(length);
  if (start < 0n || start >= 0x100000000n || end > 0x100000000n ||
      end > BigInt(wasmMemory.buffer.byteLength))
    throw new RangeError('host memory range is outside the runtime area');
  return Number(start);
};
Module['vita3kHostPointer'] = (pointer) => {
  const offset = Module['vita3kHostOffset'](pointer);
  return Module['vita3kMemory64'] ? BigInt(offset) : offset;
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
