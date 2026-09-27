#preprocess
#if ASYNCIFY == 1
// Asyncify wraps every Wasm export so that an unwind stops at the outermost
// one: each call pushes the export name, copies its arguments (Memory64
// rewind) and checks the unwind state. The exports below only ever run nested
// inside another export: dynCall_* from the invoke_* exception wrappers and the
// stack/setThrew helpers those wrappers call. An unwind through them is stopped
// by the outer export and replayed through the import call on rewind, so the
// wrapper adds nothing there but garbage on every C++ call that has a landing
// pad. dynCall_vj stays wrapped: the fiber trampoline calls it as the outermost
// entry of a new fiber. Needs unminified export names
// (-lexports.js disables their minification).
{
  const instrument = Asyncify.instrumentWasmExports;
  const nested = /^(dynCall_(?!vj$)\w+|emscripten_stack_get_current|_emscripten_stack_restore|setThrew)$/;
  Asyncify.instrumentWasmExports = (exports) => {
    const wrapped = instrument(exports);
    let kept = 0;
    for (const [name, original] of Object.entries(exports)) {
      if (typeof original === 'function' && nested.test(name)) {
        wrapped[name] = original;
        ++kept;
      }
    }
    if (!kept) throw new Error('asyncify_post.js: no nested exports found (are export names minified?)');
    return wrapped;
  };
}
#endif
