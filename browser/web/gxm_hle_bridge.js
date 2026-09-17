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

// GXM viewport (pass-through floats) to a WebGPU viewport rect in top-left
// device pixels. Mirrors vulkan sync_viewport_real with res_multiplier 1:
// x = xOffset - |xScale|, y = yOffset - yScale, w = |2*xScale|, h = 2*yScale.
// A negative height (the normal negative-yScale GXM convention, including the
// default viewport) is normalized to the same framebuffer rect it describes
// under Vulkan's negative-height flip. Flat viewports cover the full target.
// Pure: unit-tested without a compiler or device. Depth is always [0, 1]: the
// translated shader applies z offset/scale from render info itself.
export function gxmViewportRect(viewport, width, height) {
  // Local bounds (gxm_renderer.js owns the shared integer helper and does not
  // export it): the device-limit check in createTarget stays authoritative.
  for (const [name, value] of [['surface width', width], ['surface height', height]])
    if (!Number.isSafeInteger(value) || value < 1 || value > 16384)
      throw new RangeError(`${name} out of range`);
  if (!viewport || typeof viewport !== 'object') throw new TypeError('viewport required');
  const { flat, xOffset, yOffset, xScale, yScale } = viewport;
  if (typeof flat !== 'boolean') throw new TypeError('viewport flat flag required');
  if (flat) return { x: 0, y: 0, width, height };
  for (const [name, value] of [['xOffset', xOffset], ['yOffset', yOffset],
      ['xScale', xScale], ['yScale', yScale]])
    if (typeof value !== 'number' || !Number.isFinite(value))
      throw new RangeError(`${name} must be finite`);
  // Negative xScale is a game bug that both production backends render with
  // abs(); reject nothing, match them. Negative height is the norm, not an error.
  const w = Math.abs(2 * xScale);
  let h = 2 * yScale, y = yOffset - yScale;
  if (h < 0) { y += h; h = -h; }
  const x = xOffset - Math.abs(xScale);
  if (![x, y, w, h].every(Number.isFinite)) throw new RangeError('viewport rect overflow');
  return { x, y, width: w, height: h };
}

// Versioned host-owned packet, not a guest struct containing native pointers.
// Copy every input before awaiting the compiler or device.
export async function drawGuestSurface(packet, initialPixels, width, height) {
  packet = new Uint8Array(packet).slice(); initialPixels = new Uint8Array(initialPixels).slice();
  return enqueueGuestWork(() => drawOwnedSurface(packet, initialPixels, width, height));
}
// Pure decoder exported for lightweight packet contract tests (no GPU/compiler).
// Only GXM3 is accepted: never silently interpret an old native producer.
export function decodeGuestDrawPacket(packet) {
  const view = new DataView(packet.buffer, packet.byteOffset, packet.byteLength);
  let cursor = 0;
  const word = () => {
    if (cursor + 4 > packet.length) throw new Error('truncated GXM draw packet');
    const n = view.getUint32(cursor, true); cursor += 4; return n;
  };
  const float = () => {
    if (cursor + 4 > packet.length) throw new Error('truncated GXM draw packet');
    const n = view.getFloat32(cursor, true); cursor += 4;
    if (!Number.isFinite(n)) throw new Error('non-finite GXM viewport float');
    return n;
  };
  const take = n => {
    if (!Number.isSafeInteger(n) || n < 0 || cursor + n > packet.length) throw new Error('truncated GXM draw packet');
    const data = packet.slice(cursor, cursor + n); cursor += n; return data;
  };
  if (word() !== 0x47584d33) throw new Error('unknown GXM draw packet version');
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
  const flat = word();
  if (flat !== 0 && flat !== 1) throw new Error('unsupported viewport flat flag');
  const viewport = { flat: flat === 1, xOffset: float(), yOffset: float(),
    zOffset: float(), xScale: float(), yScale: float(), zScale: float() };
  const info = take(48);
  const attributes = Array.from({ length: attributeCount }, () => {
    const shaderLocation = word(), offset = word(), components = word();
    if (components < 1 || components > 4) throw new Error('unsupported attribute components');
    return { shaderLocation, offset, format: components === 1 ? 'float32' : `float32x${components}` };
  });
  const [indices, vertices, vertexGXP, fragmentGXP, vertexUniforms, fragmentUniforms] = lengths.map(take);
  if (fragmentTexture) fragmentTexture.pixels = take(textureLength);
  if (cursor !== packet.length) throw new Error('trailing GXM draw packet bytes');
  return { stride, indexSize, viewport, info, attributes, indices, vertices, vertexGXP, fragmentGXP,
    vertexUniforms, fragmentUniforms, fragmentTexture };
}
async function drawOwnedSurface(packet, initialPixels, width, height) {
  const { stride, indexSize, viewport, info, attributes, indices, vertices, vertexGXP, fragmentGXP,
    vertexUniforms, fragmentUniforms, fragmentTexture } = decodeGuestDrawPacket(packet);
  // Viewport rect is computed before any await from owned decode output, so
  // later device work cannot observe mutated state.
  const viewportRect = gxmViewportRect(viewport, width, height);
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
      viewport: viewportRect,
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
