// M4 worker lifecycle shell. The generated Emscripten module is loaded here.
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

self.onmessage = ({ data }) => {
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
    const exitCode = module?._vita3k_web_run_guest_probe?.() ?? -1;
    post({ type: 'guest-exit', exitCode, ok: exitCode >= 0 });
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
