import { createGXMRenderer } from './gxm_renderer.js';

// Device acquisition and the narrow command consumer. GXM HLE/shader translation
// must explicitly feed this renderer; the CPU framebuffer path is unchanged.
export function createWebGPUBridge() {
  let device = null;
  let renderer = null;
  return Object.freeze({
    supported: typeof navigator !== 'undefined' && 'gpu' in navigator,
    async requestDevice() {
      if (device) return device;
      if (!this.supported) return null;
      const adapter = await navigator.gpu.requestAdapter();
      if (!adapter) return null;
      device = await adapter.requestDevice();
      renderer = createGXMRenderer(device);
      return device;
    },
    get device() { return device; },
    get renderer() { return renderer; },
  });
}
