// M4 worker lifecycle shell. The generated Emscripten module is loaded here.
import { createWebStorage } from './storage.js';

// Module directory of each memory model, relative to this Worker. The build
// stages a flavour's modules into its directory by reading this line
// (browser/CMakeLists.txt), so keep it a one-line literal.
const moduleDirectories = { w64: 'wasm64/', w32: '' };

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
// GXM frames presented from the GPU (browser/web/gxm_scene.js). Pixels are
// present only on read-back frames; the canvas (when attached) already shows
// every frame.
globalThis.vita3kWebOnGpuFrame = (generation, width, height, pixels) => {
  if (!pixels) { post({ type: 'vita-present', generation, width, height }); return; }
  const data = pixels.buffer;
  post({ type: 'vita-frame', generation, width, height, pixelFormat: 'A8B8G8R8', data }, [data]);
};
// sceMsgDialog shown to the page (browser/src/msg_dialog_bridge.cpp):
// { id, state: 'open' | 'update', message, buttons, progress, enterButton } or
// { id, state: 'close', buttonId, result }. The page answers with 'dialog-press'.
globalThis.vita3kWebOnDialog = (dialog) => post({ type: 'vita-dialog', dialog });
// SceIme text field shown to the page (browser/src/ime_bridge.cpp):
// { id, state: 'open', text, caret, maxLength, type, option, enterLabel } or
// { id, state: 'close' }. The page reports its field with 'ime-input'.
globalThis.vita3kWebOnIme = (ime) => post({ type: 'vita-ime', ime });
// A canvas transferred by the page ('attach-canvas'); bound once the GXM
// device exists.
let pendingCanvas = null;
globalThis.vita3kGxmReady = (scene) => {
  if (pendingCanvas) { scene.attachCanvas(pendingCanvas); pendingCanvas = null; }
};
// PCM tap (see browser/src/hle_audio_null.cpp): one copied buffer per
// sceAudioOutOutput call. Transfer the copy; the Wasm scratch is reused by
// the next call, so the page must not retain the view.
globalThis.vita3kWebOnAudio = (freq, channels, frames, view) => {
  const data = view.slice().buffer;
  post({ type: 'vita-audio', freq, channels, frames, data }, [data]);
};

const transition = (state) => {
  lifecycle = state;
  post({ type: 'lifecycle', state });
};

try {
  transition('loading');
  const workerParams = new URL(self.location.href).searchParams;
  const jit = workerParams.get('backend') === 'jit';
  const moduleName = jit ? 'vita3k_web_jit' : 'vita3k_web';
  // Memory-model selection: Memory64 is the preferred configuration, wasm32
  // the fallback. ?memory=w64 forces the direct build (fails loudly when
  // unsupported); ?memory=w32 forces the sparse reference; default (auto)
  // probes for Memory64 support, attempts the preferred module first, and
  // falls back to wasm32 on any load/instantiation error (which also covers
  // a failed 8 GiB reservation on constrained devices). See MEMORY64.md.
  const memoryParam = (workerParams.get('memory') || 'auto').toLowerCase();
  const forceW64 = memoryParam === 'w64' || memoryParam === 'wasm64' || memoryParam === 'memory64';
  const forceW32 = memoryParam === 'w32' || memoryParam === 'wasm32';
  const probeMemory64 = () => {
    if (typeof WebAssembly === 'undefined' || typeof WebAssembly.validate !== 'function') return false;
    try {
      return WebAssembly.validate(new Uint8Array([
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x05, 0x03, 0x01, 0x04, 0x00,
      ]));
    } catch {
      return false;
    }
  };
  const attempts = forceW64 ? ['w64'] : forceW32 ? ['w32'] : (probeMemory64() ? ['w64', 'w32'] : ['w32']);
  let memoryFallback = false;
  for (const attempt of attempts) {
    const base = `./${moduleDirectories[attempt]}`;
    const moduleUrl = new URL(`${base}${moduleName}.js`, self.location.href).href;
    try {
      const { default: createModule } = await import(moduleUrl);
      if (typeof createModule !== 'function') throw new TypeError('Emscripten module factory is not callable');
      module = await createModule({
        locateFile: (file) => new URL(`${base}${file}`, self.location.href).href,
        print: (message) => post({ type: 'log', message }),
        printErr: (message) => post({ type: 'log', message }),
        // Matched diagnostic baseline; the native option also disables the
        // kernel table/bookkeeping, not just generated probes.
        VITA3K_JIT_INLINE_MUTEX: workerParams.get('inlineMutex') === '0' ? '0' : '1',
        // Chained-usage LRU stamping: '1' records per-slot usage from the
        // in-Wasm dispatcher for the region-cache eviction policy.
        VITA3K_WASMJIT_STAMP_LRU: workerParams.get('stampLru') === '1' ? '1' : '0',
        // Code-page write observer: default ON (load-bearing for code-page
        // invalidation). '0' only for the isolation diagnostic; see
        // write_observer_enabled().
        VITA3K_WASMJIT_WRITE_OBSERVER: workerParams.get('writeObserver') === '0' ? '0' : '1',
        // Region-cache size: unset keeps the built-in default (currently
        // 4096); a number overrides it for cache-size A/B runs.
        VITA3K_WASMJIT_REGION_CACHE: workerParams.get('regionCache') || undefined,
        // GPU-presented frames read back to the page every N frames (0 = never).
        VITA3K_FRAME_READBACK: workerParams.get('readback') ?? undefined,
        // Per-NID HLE wall time in the progress report (vita_app.cpp).
        VITA3K_HLE_PROFILE: workerParams.get('hleProfile') === '1' ? '1' : undefined,
        // Emulated guest CPU cores (vita_app.cpp); unset keeps the default.
        VITA3K_GUEST_CORES: workerParams.get('cores') ?? undefined,
        // Vita3K's fps-hack: display waits use one vblank (vita_app.cpp).
        VITA3K_FPS_HACK: workerParams.get('fpsHack') === '1' ? '1' : undefined,
        // Internal render resolution multiplier (gxm_webgpu_bridge.cpp); unset = 2.
        VITA3K_RESOLUTION_SCALE: workerParams.get('scale') ?? undefined,
        // Check cached textures and vertex streams against guest memory and
        // report changes the write tracking missed (gxm_webgpu_bridge.cpp).
        VITA3K_TEXTURE_VERIFY: workerParams.get('textureVerify') === '1' ? '1' : undefined,
        // Read rendered surfaces back into guest memory after each scene
        // (gxm_webgpu_bridge.cpp); off by default, it stalls on the GPU.
        VITA3K_SURFACE_SYNC: workerParams.get('surfaceSync') === '1' ? '1' : undefined,
      });
      break;
    } catch (error) {
      if (attempt === attempts[attempts.length - 1]) throw error;
      memoryFallback = true;
      post({ type: 'log', message: `memory model ${attempt} unavailable (${error?.message || error}), falling back` });
    }
  }
  transition('ready');
  post({ type: 'ready', diagnostics: { module: moduleName, backend: jit ? 'jit' : 'interpreter',
    memoryRequested: memoryParam, memoryFallback, inlineMutex: jit && workerParams.get('inlineMutex') !== '0',
    memoryModel: module['vita3kMemoryModel'], hostPointerBits: module['vita3kHostPointerBits'], wasm: true, worker: true } });
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
      if (input.byteLength === 0 || input.byteLength > 0xffffffff)
        throw new RangeError('ELF input size is outside the 32-bit file transport');
      const allocation = module['vita3kHostPointer'](module._vita3k_web_alloc_input(input.byteLength));
      if (!allocation) throw new Error('unable to allocate ELF input buffer');
      try {
        module['vita3kHostBytes'](allocation, input.byteLength).set(input);
        if (vita) {
          // Completion is always reported via the vita3kWebOnExit hook (the
          // call may suspend across ASYNCIFY yields, losing its return value).
          // The input buffer is intentionally not freed here: freeing while
          // the guest run is suspended would mutate the Wasm heap under a
          // suspended stack. One input buffer per worker run is bounded.
          module._vita3k_web_set_trace?.(data.trace ? 1 : 0);
          module._vita3k_web_set_fast_vblank?.(data.fastVblank ? 1 : 0);
          // R2: promoted flags are the production default (Module prop
          // absent). The JIT env hook reads these props first (before
          // process.env), once per process. Set before first run.
          // '' default -> P; 'p' -> P; 'k' -> K; 'pk' -> PK; 'a' -> A.
          module['VITA3K_WASMJIT_PROMOTE_FLAGS'] = (data.promote === 'k' || data.promote === 'a') ? '0' : '1';
          module['VITA3K_WASMJIT_PROMOTE_ACCOUNTING'] = (data.promote === 'k' || data.promote === 'pk') ? '1' : '0';
          module._vita3k_web_run_vita(allocation, input.byteLength);
        } else {
          const exitCode = module._vita3k_web_run_elf_probe(allocation, input.byteLength);
          post({ type: resultType, path, size: input.byteLength, exitCode, ok: exitCode >= 0 });
          module._vita3k_web_free_input(allocation);
        }
      } catch (error) {
        module._vita3k_web_free_input(allocation);
        throw error;
      }
    } catch (error) {
      post({ type: resultType, exitCode: -1, ok: false, message: String(error) });
    }
    break;
  }
  case 'stage-files': {
    // MEMFS staging for the retail-app path. The browser build has no
    // NODERAWFS, so content is uploaded into the Emscripten filesystem before
    // vita3k_web_run_app resolves guest device paths under `<root>`. Every
    // entry is fetched and sized by this side; the caller only names the
    // logical paths and their URLs.
    try {
      const fs = module?.FS;
      if (!fs) throw new Error('filesystem runtime is not exported by this module');
      const root = typeof data.root === 'string' && data.root.startsWith('/') ? data.root : '/vita';
      let files = 0, bytes = 0;
      for (const entry of data.files || []) {
        const path = String(entry?.path ?? '');
        if (!path || path.startsWith('/') || path.split('/').includes('..'))
          throw new RangeError(`unsafe staged path: ${path}`);
        const response = await fetch(entry.url, { credentials: 'same-origin' });
        if (!response.ok) throw new Error(`staged fetch failed (${response.status}): ${path}`);
        const payload = new Uint8Array(await response.arrayBuffer());
        if (Number.isSafeInteger(entry.size) && entry.size >= 0 && payload.byteLength !== entry.size)
          throw new Error(`staged size mismatch for ${path}: ${payload.byteLength} != ${entry.size}`);
        const target = `${root}/${path}`;
        const directory = target.slice(0, target.lastIndexOf('/'));
        if (directory) fs.mkdirTree(directory);
        fs.writeFile(target, payload);
        files += 1; bytes += payload.byteLength;
      }
      post({ type: 'staged', root, files, bytes });
    } catch (error) {
      post({ type: 'error', message: `stage-files failed: ${error}` });
    }
    break;
  }
  case 'input':
    // SCE_CTRL_* button mask and stick axes in [-1, 1] (vita_app.cpp vita3k_web_set_pad).
    module?._vita3k_web_set_pad?.(data.buttons >>> 0, ...(data.axes ?? [0, 0, 0, 0]));
    break;
  case 'dialog-press':
    // SCE_CTRL_CROSS / SCE_CTRL_CIRCLE pressed on dialog `id` with button
    // `selected` highlighted; the runtime applies it at the next HLE call.
    module?._vita3k_web_msg_dialog_press?.(data.id >>> 0, data.button >>> 0, data.selected >>> 0);
    break;
  case 'ime-input':
    // { id, kind: 0 text (whole field + caret) | 1 enter | 2 close, text, caret },
    // taken in order by the runtime after the next HLE call.
    (globalThis.vita3kWebImeInputs ??= []).push(data.input);
    module?._vita3k_web_ime_input_ready?.();
    break;
  case 'attach-canvas': {
    globalThis.vita3kHasCanvas = true;
    const scene = module?.['vita3kGxm'];
    if (scene) scene.attachCanvas(data.canvas); else pendingCanvas = data.canvas;
    break;
  }
  case 'run-app': {
    // Retail-app launch (vita_app.cpp): the guest sees <vitaFs>/ux0/... and
    // the module owns module loading, license setup and the main thread.
    try {
      if (!module?._vita3k_web_set_app_paths || !module?._vita3k_web_run_app)
        throw new Error('retail-app entry points are not exported by this module');
      // Pointers are i64 under Memory64: exported parameter wrappers only
      // accept BigInt there, so route every address through the host-pointer
      // helper (which returns BigInt for Memory64 and a Number otherwise).
      const hostPointer = (value) => typeof module['vita3kHostPointer'] === 'function'
        ? module['vita3kHostPointer'](value) : value;
      // Optional ahead-of-time module for this title (AOT.md). Compiled here,
      // off the guest's critical path; the runtime verifies it against the
      // loaded code and falls back to the lazy JIT when it does not match.
      if (data.aotUrl) {
        const started = performance.now();
        const response = await fetch(data.aotUrl);
        if (!response.ok) throw new Error(`AOT module ${data.aotUrl}: HTTP ${response.status}`);
        module['vita3kAotModule'] = await WebAssembly.compileStreaming(response);
        post({ type: 'log', message: `[vita3k-web] AOT module compiled in ${Math.round(performance.now() - started)} ms` });
      }
      module._vita3k_web_set_trace?.(data.trace ? 1 : 0);
      module._vita3k_web_set_fast_vblank?.(data.fastVblank ? 1 : 0);
      module.ccall('vita3k_web_set_app_paths', null, ['string', 'string', 'string'],
        [data.vitaFs || '/vita', data.title, data.app || data.title]);
      if (data.licenseKey instanceof Uint8Array) {
        if (data.licenseKey.byteLength !== 16) throw new RangeError('license key must be 16 bytes');
        const allocation = module._malloc(16);
        module.HEAPU8.set(data.licenseKey, allocation);
        module._vita3k_web_set_license_key(hostPointer(allocation));
        module._free(hostPointer(allocation));
      } else {
        module._vita3k_web_set_license_key(hostPointer(0));
      }
      module._vita3k_web_run_app();
    } catch (error) {
      post({ type: 'vita-exit', exitCode: -1, ok: false, message: String(error.stack || error) });
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
