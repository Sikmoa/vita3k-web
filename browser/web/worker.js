// M4 worker lifecycle shell. The generated Emscripten module is loaded here.
import { createWebStorage } from './storage.js';

const storage = createWebStorage('./');
const post = (message) => self.postMessage({ ...message, timestamp: performance.now() });
let module = null;
let lifecycle = 'starting';

const transition = (state) => {
  lifecycle = state;
  post({ type: 'lifecycle', state });
};

try {
  transition('loading');
  const moduleUrl = new URL('./vita3k_web.js', self.location.href).href;
  const { default: createModule } = await import(moduleUrl);
  if (typeof createModule !== 'function') throw new TypeError('Emscripten module factory is not callable');
  module = await createModule({
    locateFile: (file) => new URL(`./${file}`, self.location.href).href,
    print: (message) => post({ type: 'log', message }),
    printErr: (message) => post({ type: 'log', message }),
  });
  transition('ready');
  post({ type: 'ready', diagnostics: { module: 'vita3k_web', wasm: true, worker: true } });
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
  case 'run-guest': {
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
        const exitCode = module._vita3k_web_run_elf_probe(allocation, input.byteLength);
        post({ type: 'guest-exit', path, size: input.byteLength, exitCode, ok: exitCode >= 0 });
      } finally {
        module._free(allocation);
      }
    } catch (error) {
      post({ type: 'guest-exit', exitCode: -1, ok: false, message: String(error) });
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
