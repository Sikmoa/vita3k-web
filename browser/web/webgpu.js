// M6 WebGPU presentation seam; rendering remains outside this milestone.
export function createWebGPUBridge() {
  let device = null;
  return Object.freeze({
    supported: typeof navigator !== 'undefined' && 'gpu' in navigator,
    async requestDevice() {
      if (!this.supported) return null;
      const adapter = await navigator.gpu.requestAdapter();
      if (!adapter) return null;
      device = await adapter.requestDevice();
      return device;
    },
    get device() { return device; },
  });
}
