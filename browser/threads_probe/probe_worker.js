// Runs the probe module the way the threaded emulator runs: its main thread is
// this dedicated Worker (the coordinator), its pthreads are nested Workers.
const post = (line) => postMessage({ line });
const started = performance.now();
globalThis.probeDone = () => postMessage({ done: true });
try {
  const { default: createProbe } = await import('./vita3k_threads_probe.js');
  await createProbe({
    print: post,
    printErr: (line) => post('[stderr] ' + line),
    onRuntimeInitialized() {
      post(`[probe] module and Worker pool ready after ${(performance.now() - started).toFixed(0)} ms`);
    },
  });
} catch (error) {
  post('[probe] FAIL: ' + (error?.stack || error));
  postMessage({ done: true });
}
