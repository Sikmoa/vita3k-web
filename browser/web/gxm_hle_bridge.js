// SPDX-License-Identifier: GPL-2.0-or-later
// Guest HLE bridge, deliberately separate from the JS-only GXM context API.
// Queue completion, ABGR8 transfer fill and a deliberately bounded GXP draw path.
import { createGXMRenderer } from './gxm_renderer.js';
import { createGXPShaderAdapter } from './gxp_shader_adapter.js';
let shaderPromise;
export function configureGuestShaders(urls) {
  if (shaderPromise) throw new Error('guest shader compiler already initialized');
  shaderPromise = createGXPShaderAdapter(urls);
}

// Versioned host-owned packet, not a guest struct containing native pointers.
// Copy every input before awaiting the compiler or device.
export async function drawGuestSurface(packet, initialPixels, width, height) {
  packet = new Uint8Array(packet); initialPixels = new Uint8Array(initialPixels);
  const view = new DataView(packet.buffer, packet.byteOffset, packet.byteLength);
  let cursor = 0;
  const word = () => { const n = view.getUint32(cursor, true); cursor += 4; return n; };
  const take = n => {
    if (!Number.isSafeInteger(n) || n < 0 || cursor + n > packet.length) throw new Error('truncated GXM draw packet');
    const data = packet.slice(cursor, cursor + n); cursor += n; return data;
  };
  if (word() !== 0x47584d31) throw new Error('unknown GXM draw packet version');
  const stride = word(), indexSize = word();
  const lengths = Array.from({ length: 6 }, word);
  const attributeCount = word();
  if (![2,4].includes(indexSize) || attributeCount > 16) throw new Error('unsupported draw layout');
  const info = take(48);
  const attributes = Array.from({ length: attributeCount }, () => {
    const shaderLocation = word(), offset = word(), components = word();
    if (components < 1 || components > 4) throw new Error('unsupported attribute components');
    return { shaderLocation, offset, format: components === 1 ? 'float32' : `float32x${components}` };
  });
  const [indices, vertices, vertexGXP, fragmentGXP, vertexUniforms, fragmentUniforms] = lengths.map(take);
  if (cursor !== packet.length) throw new Error('trailing GXM draw packet bytes');
  if (!shaderPromise) configureGuestShaders({
    compilerURL: new URL('./shaders/gxp_compiler.mjs', import.meta.url).href,
    nagaURL: new URL('./shaders/naga.wasm', import.meta.url).href,
    wasiShimURL: new URL('./shaders/wasi/index.js', import.meta.url).href,
  });
  const compiler = await shaderPromise;
  const vertex = await compiler.translate(vertexGXP);
  const fragment = await compiler.translate(fragmentGXP);
  const renderer = createGXMRenderer(await initializeGuestDevice());
  let target, program;
  try {
    // Unused explicit bindings are legal. Nonempty SSBOs use the production
    // uniform packing, never a shader-name-dependent hardcoded matrix.
    const buffers = { 0: info, 1: new Float32Array([0,0,0,0,1]) };
    const bufferBindings = [
      { binding: 0, size: 48, type: 'uniform', visibility: GPUShaderStage.VERTEX },
      { binding: 1, size: 20, type: 'uniform', visibility: GPUShaderStage.FRAGMENT },
    ];
    for (const [binding, data, visibility] of [[2, vertexUniforms, GPUShaderStage.VERTEX],
      [3, fragmentUniforms, GPUShaderStage.FRAGMENT]]) if (data.length) {
      buffers[binding] = data;
      bufferBindings.push({ binding, size: data.length, type: 'read-only-storage', visibility });
    }
    program = await renderer.createProgram({ stride, attributes, bufferBindings,
      vertexWGSL: vertex.wgsl, fragmentWGSL: fragment.wgsl,
      vertexEntryPoint: 'main_vs', fragmentEntryPoint: 'main_fs' });
    target = renderer.createTarget(width, height);
    return (await renderer.submit(target, [{ program, vertices, indices,
      indexFormat: indexSize === 2 ? 'uint16' : 'uint32', buffers }], { initialPixels })).pixels;
  } finally { renderer.dispose(); }
}
let devicePromise;
export async function initializeGuestDevice() {
  if (!devicePromise) devicePromise = (async () => {
    if (!globalThis.navigator?.gpu) throw new Error('WebGPU unavailable');
    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) throw new Error('WebGPU adapter unavailable');
    return adapter.requestDevice();
  })();
  return devicePromise;
}
export async function fillGuestSurface(color, width, height) {
  const gpu = await initializeGuestDevice();
  const pitch = Math.ceil(width * 4 / 256) * 256;
  let texture, readback;
  gpu.pushErrorScope('validation');
  try {
    texture = gpu.createTexture({ size: [width, height], format: 'rgba8unorm',
      usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
    readback = gpu.createBuffer({ size: pitch * height,
      usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const encoder = gpu.createCommandEncoder({ label: 'guest sceGxmTransferFill' });
    const pass = encoder.beginRenderPass({ colorAttachments: [{ view: texture.createView(),
      loadOp: 'clear', storeOp: 'store', clearValue: [
        (color & 255) / 255, ((color >>> 8) & 255) / 255,
        ((color >>> 16) & 255) / 255, (color >>> 24) / 255
      ] }] });
    pass.end();
    encoder.copyTextureToBuffer({ texture }, { buffer: readback, bytesPerRow: pitch }, [width, height]);
    gpu.queue.submit([encoder.finish()]);
    await readback.mapAsync(GPUMapMode.READ);
    const mapped = new Uint8Array(readback.getMappedRange());
    const pixels = new Uint8Array(width * height * 4);
    for (let y = 0; y < height; ++y)
      pixels.set(mapped.subarray(y * pitch, y * pitch + width * 4), y * width * 4);
    return pixels;
  } finally {
    readback?.destroy();
    texture?.destroy();
    const error = await gpu.popErrorScope();
    if (error) throw new Error(error.message);
  }
}

export async function finishGuestQueue() {
  const gpu = await initializeGuestDevice();
  const encoder = gpu.createCommandEncoder({ label: 'guest sceGxmFinish fence' });
  gpu.queue.submit([encoder.finish()]);
  await gpu.queue.onSubmittedWorkDone();
}
