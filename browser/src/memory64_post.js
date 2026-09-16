#preprocess
// Emscripten 3.1.69 can emit a wasm64 table-index conversion probe that
// chooses a Number on newer Node releases. Native wasm64 table/memory APIs
// require BigInt indices. This post-js shim is deliberately limited to the
// Memory64 build; the wasm32 runtime keeps its original conversion function.
#if MEMORY64
if (typeof toIndexType === 'function') {
  toIndexType = BigInt;
}
#endif
