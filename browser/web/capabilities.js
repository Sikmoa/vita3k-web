export function detectBrowserCapabilities() {
  return {
    wasm: typeof WebAssembly !== 'undefined',
    workers: typeof Worker !== 'undefined',
    webgpu: typeof navigator !== 'undefined' && 'gpu' in navigator,
    opfs: typeof navigator !== 'undefined' && !!navigator.storage?.getDirectory,
    audioWorklet: typeof AudioWorkletNode !== 'undefined',
    gamepad: typeof navigator !== 'undefined' && 'getGamepads' in navigator,
    sharedArrayBuffer: typeof SharedArrayBuffer !== 'undefined',
    crossOriginIsolated: globalThis.crossOriginIsolated === true,
  };
}

export function requiredCapabilities(capabilities) {
  return ['wasm', 'workers'];
}

export function formatCapabilities(capabilities) {
  return Object.entries(capabilities)
    .map(([name, available]) => `${name}: ${available ? 'yes' : 'no'}`)
    .join(', ');
}
