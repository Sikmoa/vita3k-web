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

#if PTHREADS
// Each pthread has its own JS module and function tables. Send immutable
// compiled AOT code and JS options before the runtime's load/run messages.
// FIFO delivery also covers newly allocated pool Workers.
if (ENVIRONMENT_IS_PTHREAD) {
  const vita3kHandleMessage = self.onmessage;
  const vita3kMessage = (event) => {
    if (event.data?.vita3kWorkerConfig) {
      Object.assign(Module, event.data.vita3kWorkerConfig);
      return;
    }
    const result = vita3kHandleMessage(event);
    // Loading the runtime restores its own message handler; retain this
    // wrapper for configuration sent after staging and before CMD_RUN.
    self.onmessage = vita3kMessage;
    return result;
  };
  self.onmessage = vita3kMessage;
} else {
  const vita3kWorkerConfig = () => {
    const config = {};
    for (const name of Object.keys(Module)) {
      if (name.startsWith('VITA3K_') || name === 'vita3kAotModule' || name === 'vita3kNullGpu')
        config[name] = Module[name];
    }
    return config;
  };
  if (ENVIRONMENT_IS_NODE && process.env.VITA3K_AOT && !Module['vita3kAotModule']) {
    Module['vita3kAotModule'] = new WebAssembly.Module(require('fs').readFileSync(process.env.VITA3K_AOT));
  }
  const vita3kLoadWorker = PThread.loadWasmModuleToWorker;
  // An AOT image built at launch on a pthread (vita_app.cpp) comes back
  // here for the whole pool. Emscripten ignores messages without a cmd. The
  // initial pool loaded before this file ran, so it is listened to here.
  const vita3kListen = (worker) => worker.addEventListener('message', (event) => {
    if (!(event.data?.vita3kAotModule instanceof WebAssembly.Module)) return;
    Module['vita3kAotModule'] = event.data.vita3kAotModule;
    Module['vita3kConfigureWorkers']();
  });
  PThread.unusedWorkers.forEach(vita3kListen);
  PThread.loadWasmModuleToWorker = (worker) => {
    vita3kListen(worker);
    worker.postMessage({vita3kWorkerConfig: vita3kWorkerConfig()});
    return vita3kLoadWorker(worker);
  };
  // The browser finishes staging after runtime initialization. Call this
  // before starting the application pthread, while every pool Worker is idle.
  // Refill asynchronously; Emscripten still handles exhausted pools normally.
  const vita3kTakeWorker = PThread.getNewWorker;
  let vita3kPoolMisses = 0;
  PThread.getNewWorker = () => {
    if (!PThread.unusedWorkers.length) ++vita3kPoolMisses;
    return vita3kTakeWorker();
  };
  Module['vita3kPoolStats'] = () => ({idle: PThread.unusedWorkers.length,
    busy: Object.keys(PThread.pthreads).length, misses: vita3kPoolMisses});
  Module['vita3kStartPoolRefill'] = () => {
    const timer = setInterval(() => {
      const idle = PThread.unusedWorkers.length;
      const total = idle + Object.keys(PThread.pthreads).length;
      if (idle < 4 && total < 64) {
        for (let i = 0; i < Math.min(4, 64 - total); ++i) {
          PThread.allocateUnusedWorker();
          PThread.loadWasmModuleToWorker(PThread.unusedWorkers.at(-1));
        }
      }
    }, 50);
    return () => clearInterval(timer);
  };
  // Idle Workers and running ones: a running thread's Worker keeps the
  // vita3kMessage wrapper, and the AOT module is only read when a thread
  // first enters guest code (an AOT image built at launch arrives late).
  Module['vita3kConfigureWorkers'] = () => {
    const config = vita3kWorkerConfig();
    for (const worker of [...PThread.unusedWorkers, ...Object.values(PThread.pthreads)])
      worker.postMessage({vita3kWorkerConfig: config});
  };
}
#endif
