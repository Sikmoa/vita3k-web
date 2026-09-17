// SPDX-License-Identifier: GPL-2.0-or-later
// Guest HLE bridge, deliberately separate from the JS-only GXM context API.
// Queue completion, ABGR8 transfer fill and a deliberately bounded GXP draw path.
import { createGXMRenderer } from './gxm_renderer.js';
import { createGXPShaderAdapter } from './gxp_shader_adapter.js';
// One ordered device stream, including fills and fences. A failed operation
// poisons later completion requests: none may acknowledge a rejected draw.
let queueTail = Promise.resolve(), queueFailure, queueFailed = false, guestRenderer;
function enqueueGuestWork(work) {
  const result = queueTail.then(() => {
    if (queueFailed) throw queueFailure;
    return work();
  });
  queueTail = result.catch(error => { queueFailed = true; queueFailure = error; });
  return result;
}
let shaderPromise;
export function configureGuestShaders(urls) {
  if (shaderPromise) throw new Error('guest shader compiler already initialized');
  shaderPromise = createGXPShaderAdapter(urls);
}

// Versioned host-owned packet, not a guest struct containing native pointers.
// Copy every input before awaiting the compiler or device.
export async function drawGuestSurface(packet, initialPixels, width, height) {
  packet = new Uint8Array(packet).slice(); initialPixels = new Uint8Array(initialPixels).slice();
  return enqueueGuestWork(() => drawOwnedSurface(packet, initialPixels, width, height));
}
// Pure decoder exported for lightweight packet contract tests (no GPU/compiler).
// Only GXM2 is accepted: never silently interpret an old native producer.
export function decodeGuestDrawPacket(packet) {
  const view = new DataView(packet.buffer, packet.byteOffset, packet.byteLength);
  let cursor = 0;
  const word = () => {
    if (cursor + 4 > packet.length) throw new Error('truncated GXM draw packet');
    const n = view.getUint32(cursor, true); cursor += 4; return n;
  };
  const take = n => {
    if (!Number.isSafeInteger(n) || n < 0 || cursor + n > packet.length) throw new Error('truncated GXM draw packet');
    const data = packet.slice(cursor, cursor + n); cursor += n; return data;
  };
  if (word() !== 0x47584d32) throw new Error('unknown GXM draw packet version');
  const stride = word(), indexSize = word();
  const lengths = Array.from({ length: 6 }, word);
  const attributeCount = word();
  if (![2,4].includes(indexSize) || !attributeCount || attributeCount > 16
      || stride < 4 || stride % 4 || lengths.some(n => n > 16 * 1024 * 1024))
    throw new Error('unsupported draw layout');
  const textureCount = word();
  if (textureCount > 1) throw new Error('unsupported fragment texture count');
  let fragmentTexture, textureLength = 0;
  if (textureCount) {
    const width = word(), height = word(), format = word();
    const min = word(), mag = word(), u = word(), v = word();
    textureLength = word();
    // GXM LINEAR ABGR8 is little-endian RGBA bytes, already depadded by C++.
    // Packet sampler codes are the production enums, not WebGPU constants.
    if (!width || !height || width > 4096 || height > 4096 || format !== 0x0c000000
        || textureLength !== width * height * 4 || textureLength > 16 * 1024 * 1024
        || min > 1 || mag > 1 || u > 2 || v > 2)
      throw new Error('unsupported fragment texture packet');
    const filters = ['nearest', 'linear'], addresses = ['repeat', 'mirror-repeat', 'clamp-to-edge'];
    fragmentTexture = { width, height, format: 'rgba8unorm', sampler: {
      minFilter: filters[min], magFilter: filters[mag], addressModeU: addresses[u], addressModeV: addresses[v],
    } };
  }
  const info = take(48);
  const attributes = Array.from({ length: attributeCount }, () => {
    const shaderLocation = word(), offset = word(), components = word();
    if (components < 1 || components > 4) throw new Error('unsupported attribute components');
    return { shaderLocation, offset, format: components === 1 ? 'float32' : `float32x${components}` };
  });
  const [indices, vertices, vertexGXP, fragmentGXP, vertexUniforms, fragmentUniforms] = lengths.map(take);
  if (fragmentTexture) fragmentTexture.pixels = take(textureLength);
  if (cursor !== packet.length) throw new Error('trailing GXM draw packet bytes');
  return { stride, indexSize, info, attributes, indices, vertices, vertexGXP, fragmentGXP,
    vertexUniforms, fragmentUniforms, fragmentTexture };
}
async function drawOwnedSurface(packet, initialPixels, width, height) {
  const { stride, indexSize, info, attributes, indices, vertices, vertexGXP, fragmentGXP,
    vertexUniforms, fragmentUniforms, fragmentTexture } = decodeGuestDrawPacket(packet);
  if (!shaderPromise) configureGuestShaders({
    compilerURL: new URL('./shaders/gxp_compiler.mjs', import.meta.url).href,
    nagaURL: new URL('./shaders/naga.wasm', import.meta.url).href,
    wasiShimURL: new URL('./shaders/wasi/index.js', import.meta.url).href,
  });
  const compiler = await shaderPromise;
  // Explicit format hints match the only accepted guest texture format. The
  // translator retains descriptor sets 2/3 and splits unit n into 2n / 2n+1.
  const textureFormats = new Uint32Array(32).fill(0x0c000000);
  const vertex = await compiler.translate(vertexGXP, { textureFormats });
  const fragment = await compiler.translate(fragmentGXP, { textureFormats });
  if (!guestRenderer) guestRenderer = createGXMRenderer(await initializeGuestDevice());
  const renderer = guestRenderer;
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
      fragmentTexture: fragmentTexture !== undefined,
      vertexWGSL: vertex.wgsl, fragmentWGSL: fragment.wgsl,
      vertexEntryPoint: 'main_vs', fragmentEntryPoint: 'main_fs' });
    target = renderer.createTarget(width, height);
    return (await renderer.submit(target, [{ program, vertices, indices,
      indexFormat: indexSize === 2 ? 'uint16' : 'uint32', buffers,
      ...(fragmentTexture ? { fragmentTexture } : {}) }], { initialPixels })).pixels;
  } finally {
    // submit has settled (including GPU readback) before either resource can
    // be released. Retain only the bounded device-local pipeline cache.
    try {
      if (target !== undefined) renderer.destroyTarget(target);
      if (program !== undefined) renderer.destroyProgram(program);
    } catch (error) {
      // Device loss makes normal destroy methods unavailable. dispose still
      // releases resources and ensures a poisoned renderer is never reused.
      renderer.dispose(); guestRenderer = undefined;
      throw error;
    }
  }
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
export function fillGuestSurface(color, width, height) {
  return enqueueGuestWork(() => fillOrderedSurface(color, width, height));
}
async function fillOrderedSurface(color, width, height) {
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

export function finishGuestQueue() {
  return enqueueGuestWork(async () => {
    const gpu = await initializeGuestDevice();
    const encoder = gpu.createCommandEncoder({ label: 'guest sceGxmFinish fence' });
    gpu.queue.submit([encoder.finish()]);
    await gpu.queue.onSubmittedWorkDone();
  });
}
