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

// Guest blend descriptor (GXM units) to WebGPU pipeline state. Pure and
// exported so the packet contract test covers every value without a device.
// GXM has no per-channel "blend disabled": NONE is translated as 'add' by this
// consumer and by the desktop backends (translate_blend_func), and blending is
// enabled when either channel asks for it. Only the inert all-NONE descriptor
// leaves `blend` undefined, which still applies the guest color mask through
// the pipeline write mask.
const blendFuncs = ['add', 'add', 'subtract', 'reverse-subtract', 'min', 'max']; // [NONE, ADD, ...]
const blendFactors = { 0: 'zero', 1: 'one', 2: 'src', 3: 'one-minus-src', 4: 'src-alpha',
  5: 'one-minus-src-alpha', 6: 'dst', 7: 'one-minus-dst', 8: 'dst-alpha', 9: 'one-minus-dst-alpha',
  10: 'src-alpha-saturate' }; // 11 DST_ALPHA_SATURATE has no WebGPU equivalent.
export function gxmBlendState(colorMask, colorFunc, alphaFunc, colorSrc, colorDst, alphaSrc, alphaDst) {
  // GXM color mask bits: A=1, R=2, G=4, B=8. WebGPU write mask: R=1, G=2, B=4, A=8.
  if (!Number.isInteger(colorMask) || colorMask & ~0xf) throw new Error('unsupported blend color mask');
  const writeMask = (colorMask & 2 ? 1 : 0) | (colorMask & 4 ? 2 : 0)
    | (colorMask & 8 ? 4 : 0) | (colorMask & 1 ? 8 : 0);
  const operations = [colorFunc, alphaFunc].map(value => blendFuncs[value]);
  if (operations.some(value => value === undefined)) throw new Error('unsupported blend function');
  const factors = [colorSrc, colorDst, alphaSrc, alphaDst].map(value => blendFactors[value]);
  if (factors.some(value => value === undefined)) throw new Error('unsupported blend factor');
  const [src, dst, alphaSrcFactor, alphaDstFactor] = factors;
  // WebGPU accepts src-alpha-saturate only as the color source factor.
  if ([dst, alphaSrcFactor, alphaDstFactor].includes('src-alpha-saturate'))
    throw new Error('unsupported blend factor position');
  if (colorFunc === 0 && alphaFunc === 0) return { writeMask };
  return { writeMask, blend: { color: { operation: operations[0], srcFactor: src, dstFactor: dst },
    alpha: { operation: operations[1], srcFactor: alphaSrcFactor, dstFactor: alphaDstFactor } } };
}
// Guest depth-stencil descriptor (GXM units) to a WebGPU depth attachment and
// pipeline state. Every accepted format is an exact representation of the
// guest layout; approximations reject. Depth load mode 0 is the only accepted
// value: the native producer rejects force_load/force_store, so the attachment
// is always cleared to the guest background depth and never read back.
const depthFormats = { 0x02444000: { format: 'depth16unorm', stencil: false },
  0x00044000: { format: 'depth32float', stencil: false },
  0x01266000: { format: 'depth24plus-stencil8', stencil: true } };
const depthFuncs = { 0x0000000: 'never', 0x0400000: 'less', 0x0800000: 'equal', 0x0c00000: 'less-equal',
  0x1000000: 'greater', 0x1400000: 'not-equal', 0x1800000: 'greater-equal', 0x1c00000: 'always' };
export function gxmDepthStencilState(format, depthCompare, depthWriteMode, loadMode, clearValue) {
  const spec = depthFormats[format];
  if (!spec) throw new Error('unsupported depth format');
  const compare = depthFuncs[depthCompare];
  if (!compare) throw new Error('unsupported depth compare function');
  if (depthWriteMode !== 0 && depthWriteMode !== 0x00100000) throw new Error('unsupported depth write mode');
  if (loadMode !== 0) throw new Error('unsupported depth load mode');
  if (typeof clearValue !== 'number' || !Number.isFinite(clearValue) || clearValue < 0 || clearValue > 1)
    throw new RangeError('depth clear value must be a normalized finite number');
  return { format: spec.format, stencil: spec.stencil, depthCompare: compare,
    depthWriteEnabled: depthWriteMode === 0, clearValue };
}

// Guest SceGxmAttributeFormat (plus component count) to a WebGPU vertex format.
// Pure and exported so the packet contract test covers the whole table without
// a device. Only shapes a WebGPU vertex format represents exactly are listed:
// the guest formats the shader reads as floats (normalized, F16 and F32). The
// integer formats U8/S8/U16/S16 need a scaled or integer vertex input WebGPU
// has no vertex format for, and UNTYPED needs an integer shader input the
// translator does not declare; all three reject. WebGPU has no 1- or
// 3-component 8/16-bit vertex format either, so those component counts are
// absent here and reject rather than silently widening the fetch.
const vertexFormats = {
  4: { 2: 'unorm8x2', 4: 'unorm8x4' }, // U8N
  5: { 2: 'snorm8x2', 4: 'snorm8x4' }, // S8N
  6: { 2: 'unorm16x2', 4: 'unorm16x4' }, // U16N
  7: { 2: 'snorm16x2', 4: 'snorm16x4' }, // S16N
  8: { 2: 'float16x2', 4: 'float16x4' }, // F16
  9: { 1: 'float32', 2: 'float32x2', 3: 'float32x3', 4: 'float32x4' }, // F32
};
export function gxmVertexFormat(format, components) {
  if (!Number.isInteger(format) || !Number.isInteger(components))
    throw new TypeError('vertex attribute format and component count must be integers');
  const spec = vertexFormats[format];
  const name = spec ? spec[components] : undefined;
  if (!name) throw new Error(`unsupported vertex attribute format ${format}x${components}`);
  return name;
}

// Guest texture formats the C++ producer accepts, always delivered as
// rgba8unorm pixels: LINEAR U8U8U8U8_ABGR passes through byte-for-byte and
// LINEAR U8U8_GRRR is expanded with the SWIZZLE2_GRRR component mapping
// (RGB = first byte, alpha = second). Anything else rejects before the device.
const guestTextureFormats = new Set([0x0c000000, 0x07002000]);
export function gxmFragmentTextureFormat(format) {
  if (!guestTextureFormats.has(format)) throw new Error(`unsupported fragment texture format ${format}`);
  return 'rgba8unorm';
}

// Versioned host-owned packet, not a guest struct containing native pointers.
// Copy every input before awaiting the compiler or device.
export async function drawGuestSurface(packet, initialPixels, width, height) {
  packet = new Uint8Array(packet).slice(); initialPixels = new Uint8Array(initialPixels).slice();
  return enqueueGuestWork(() => drawOwnedSurface(packet, initialPixels, width, height));
}
// Pure decoder exported for lightweight packet contract tests (no GPU/compiler).
// Only GXM5 is accepted: never silently interpret an older native producer.
export function decodeGuestDrawPacket(packet) {
  const view = new DataView(packet.buffer, packet.byteOffset, packet.byteLength);
  let cursor = 0;
  const word = () => {
    if (cursor + 4 > packet.length) throw new Error('truncated GXM draw packet');
    const n = view.getUint32(cursor, true); cursor += 4; return n;
  };
  const float = (what = 'GXM viewport float') => {
    if (cursor + 4 > packet.length) throw new Error('truncated GXM draw packet');
    const n = view.getFloat32(cursor, true); cursor += 4;
    if (!Number.isFinite(n)) throw new Error(`non-finite ${what}`);
    return n;
  };
  const flag = what => {
    const value = word();
    if (value !== 0 && value !== 1) throw new Error(`unsupported ${what}`);
    return value === 1;
  };
  const take = n => {
    if (!Number.isSafeInteger(n) || n < 0 || cursor + n > packet.length) throw new Error('truncated GXM draw packet');
    const data = packet.slice(cursor, cursor + n); cursor += n; return data;
  };
  if (word() !== 0x47584d35) throw new Error('unknown GXM draw packet version');
  const stride = word(), indexSize = word();
  const lengths = Array.from({ length: 6 }, word);
  const attributeCount = word();
  if (![2,4].includes(indexSize) || !attributeCount || attributeCount > 16
      || stride < 4 || stride % 4 || lengths.some(n => n > 16 * 1024 * 1024))
    throw new Error('unsupported draw layout');
  const blendEnabled = flag('blend enable flag');
  const blendWords = Array.from({ length: 7 }, word);
  const { writeMask, blend } = gxmBlendState(...blendWords);
  if (blendEnabled !== (blend !== undefined)) throw new Error('blend flag/state mismatch');
  let depth;
  if (flag('depth enable flag')) {
    const [format, compare, writeMode, loadMode] = Array.from({ length: 4 }, word);
    depth = gxmDepthStencilState(format, compare, writeMode, loadMode, float('GXM depth clear value'));
  }
  const textureCount = word();
  if (textureCount > 1) throw new Error('unsupported fragment texture count');
  let fragmentTexture, textureLength = 0;
  if (textureCount) {
    const width = word(), height = word(), format = word();
    const min = word(), mag = word(), u = word(), v = word();
    textureLength = word();
    // Packet sampler codes are the production enums, not WebGPU constants.
    if (!width || !height || width > 4096 || height > 4096
        || textureLength !== width * height * 4 || textureLength > 16 * 1024 * 1024
        || min > 1 || mag > 1 || u > 2 || v > 2)
      throw new Error('unsupported fragment texture packet');
    const filters = ['nearest', 'linear'], addresses = ['repeat', 'mirror-repeat', 'clamp-to-edge'];
    fragmentTexture = { width, height, format: gxmFragmentTextureFormat(format), sampler: {
      minFilter: filters[min], magFilter: filters[mag], addressModeU: addresses[u], addressModeV: addresses[v],
    } };
  }
  const flat = word();
  if (flat !== 0 && flat !== 1) throw new Error('unsupported viewport flat flag');
  const viewport = { flat: flat === 1, xOffset: float(), yOffset: float(),
    zOffset: float(), xScale: float(), yScale: float(), zScale: float() };
  const info = take(48);
  const attributes = Array.from({ length: attributeCount }, () => {
    const shaderLocation = word(), offset = word(), components = word(), guestFormat = word();
    return { shaderLocation, offset, format: gxmVertexFormat(guestFormat, components) };
  });
  const [indices, vertices, vertexGXP, fragmentGXP, vertexUniforms, fragmentUniforms] = lengths.map(take);
  if (fragmentTexture) fragmentTexture.pixels = take(textureLength);
  if (cursor !== packet.length) throw new Error('trailing GXM draw packet bytes');
  return { stride, indexSize, viewport, info, attributes, indices, vertices, vertexGXP, fragmentGXP,
    vertexUniforms, fragmentUniforms, fragmentTexture, writeMask, blend, depth };
}
async function drawOwnedSurface(packet, initialPixels, width, height) {
  const { stride, indexSize, viewport, info, attributes, indices, vertices, vertexGXP, fragmentGXP,
    vertexUniforms, fragmentUniforms, fragmentTexture, writeMask, blend, depth } = decodeGuestDrawPacket(packet);
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
      fragmentTexture: fragmentTexture !== undefined, writeMask,
      ...(blend ? { blend } : {}),
      ...(depth ? { depthStencil: { format: depth.format, depthCompare: depth.depthCompare,
        depthWriteEnabled: depth.depthWriteEnabled } } : {}),
      vertexWGSL: vertex.wgsl, fragmentWGSL: fragment.wgsl,
      vertexEntryPoint: 'main_vs', fragmentEntryPoint: 'main_fs' });
    // The depth attachment is the same size as the color attachment (one GXM
    // render target); it is per draw and never read back to guest memory.
    target = renderer.createTarget(width, height, depth ? { depthFormat: depth.format } : undefined);
    return (await renderer.submit(target, [{ program, vertices, indices,
      indexFormat: indexSize === 2 ? 'uint16' : 'uint32', buffers,
      viewport: viewportRect,
      ...(fragmentTexture ? { fragmentTexture } : {}) }], { initialPixels,
      // submit() validates the per-pass depth attachment against a strict
      // { format, stencil, clearValue } shape; the per-pipeline depth state
      // (compare/write) already flowed through createProgram above, so drop
      // the decoder-side extras here.
      ...(depth ? { depth: { format: depth.format, stencil: depth.stencil,
        clearValue: depth.clearValue } } : {}) })).pixels;
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
    // WebGPU is exposed only to secure contexts. A page served over plain HTTP
    // from a non-loopback address has no navigator.gpu at all, which is the
    // most common reason a browser is reached but the first draw fails here.
    if (!globalThis.navigator?.gpu)
      throw new Error(globalThis.isSecureContext
        ? 'WebGPU unavailable: navigator.gpu is missing in this browser context (unsupported, or disabled by a flag/preference)'
        : `WebGPU unavailable: ${globalThis.location?.origin ?? 'this origin'} is not a secure context, and WebGPU is exposed only on secure origins (https:// or http://localhost)`);
    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) throw new Error('WebGPU adapter unavailable: requestAdapter() returned null');
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
