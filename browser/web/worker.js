// M4 worker lifecycle shell. The generated Emscripten module is loaded here.
import { createWebStorage } from './storage.js';

const storage = createWebStorage('./');
const post = (message, transfer) => self.postMessage({ ...message, timestamp: performance.now() }, transfer || []);
let module = null;
let lifecycle = 'starting';

// Host hooks called from Wasm (see browser/src/vita_runtime.cpp). With
// ASYNCIFY the run call may suspend; the final exit code therefore arrives
// through vita3kWebOnExit instead of the call's return value.
globalThis.vita3kWebOnExit = (code) => {
  post({ type: 'vita-exit', exitCode: code, ok: code >= 0 });
};
// The view is only valid synchronously; copy it before yielding. The data is
// TIGHT RGBA produced in Wasm from the real sceDisplaySetFrameBuf state.
globalThis.vita3kWebOnFrame = (generation, width, height, view) => {
  const data = view.slice().buffer;
  post({ type: 'vita-frame', generation, width, height, pixelFormat: 'A8B8G8R8', data }, [data]);
};

const transition = (state) => {
  lifecycle = state;
  post({ type: 'lifecycle', state });
};

try {
  transition('loading');
  const jit = new URL(self.location.href).searchParams.get('backend') === 'jit';
  const moduleName = jit ? 'vita3k_web_jit' : 'vita3k_web';
  const moduleUrl = new URL(`./${moduleName}.js`, self.location.href).href;
  const { default: createModule } = await import(moduleUrl);
  if (typeof createModule !== 'function') throw new TypeError('Emscripten module factory is not callable');
  module = await createModule({
    locateFile: (file) => new URL(`./${file}`, self.location.href).href,
    print: (message) => post({ type: 'log', message }),
    printErr: (message) => post({ type: 'log', message }),
  });
  transition('ready');
  post({ type: 'ready', diagnostics: { module: moduleName, backend: jit ? 'jit' : 'interpreter', wasm: true, worker: true } });
} catch (error) {
  lifecycle = 'error';
  post({ type: 'error', state: lifecycle, message: String(error) });
}

self.onmessage = async ({ data }) => {
  if (!data || lifecycle === 'error') return;
  switch (data.type) {
  case 'status':
    post({ type: 'status', state: lifecycle });
    break;
  case 'pause':
    if (lifecycle === 'ready') transition('paused');
    break;
  case 'resume':
    if (lifecycle === 'paused') transition('ready');
    break;
  case 'run-vita':
  case 'run-guest': {
    const vita = data.type === 'run-vita';
    const resultType = vita ? 'vita-exit' : 'guest-exit';
    try {
      const path = data.path || 'guest.elf';
      let input;
      if (data.bytes instanceof Uint8Array) {
        input = data.bytes;
      } else if (data.bytes instanceof ArrayBuffer) {
        input = new Uint8Array(data.bytes);
      } else if (data.file instanceof Blob) {
        input = new Uint8Array(await data.file.arrayBuffer());
      } else {
        input = await storage.read(path);
      }
      const allocation = module._malloc(input.byteLength);
      if (!allocation) throw new Error('unable to allocate ELF input buffer');
      try {
        module.HEAPU8.set(input, allocation);
        if (vita) {
          // Completion is always reported via the vita3kWebOnExit hook (the
          // call may suspend across ASYNCIFY yields, losing its return value).
          // The input buffer is intentionally not freed here: freeing while
          // the guest run is suspended would mutate the Wasm heap under a
          // suspended stack. One input buffer per worker run is bounded.
          module._vita3k_web_set_trace?.(data.trace ? 1 : 0);
          module._vita3k_web_set_fast_vblank?.(data.fastVblank ? 1 : 0);
          module._vita3k_web_run_vita(allocation, input.byteLength);
        } else {
          const exitCode = module._vita3k_web_run_elf_probe(allocation, input.byteLength);
          post({ type: resultType, path, size: input.byteLength, exitCode, ok: exitCode >= 0 });
          module._free(allocation);
        }
      } catch (error) {
        module._free(allocation);
        throw error;
      }
    } catch (error) {
      post({ type: resultType, exitCode: -1, ok: false, message: String(error) });
    }
    break;
  }
  case 'shutdown':
    if (module?._vita3k_web_shutdown) module._vita3k_web_shutdown();
    transition('stopped');
    self.close();
    break;
  default:
    post({ type: 'error', message: `Unknown worker command: ${data.type}` });
  }
};
