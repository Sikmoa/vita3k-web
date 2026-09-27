// SPDX-License-Identifier: GPL-2.0-or-later
// GXM scene consumer (GXS1 stream, produced by browser/src/gxm_webgpu_bridge.cpp).
//
// Render targets stay on the GPU for their whole life, keyed by the guest color
// surface (address, format, size). A scene is one synchronous call: every draw
// is encoded into one render pass and submitted without waiting; guest memory
// never receives rendered pixels. Textures that alias a render target sample it
// directly; others arrive as RGBA8 mip chains the producer decoded and caches by
// content. Presentation draws the displayed target into an OffscreenCanvas when
// the page attached one, and posts sampled frames for headless observers.
//
// Everything asynchronous (device, GXP -> WGSL translation) happens before the
// guest can draw: init() at sceGxmInitialize, registerProgram() when the guest
// creates a shader program.
import { createGXPShaderAdapter } from './gxp_shader_adapter.js';

let device, compiler;
const programs = new Map();  // id -> { wgsl, fragment, module }
const targets = new Map();   // guest color address -> target
const textures = new Map();  // producer texture id -> { texture, view }
const pipelines = new Map(); // pipeline key -> GPURenderPipeline
const samplers = new Map();
const bindGroups = new Map();
const depthScratch = new Map(); // `${w}x${h}` -> transient on-chip depth texture
const depthSurfaces = new Map(); // `${depth}:${stencil}` guest addresses -> kept depth/stencil texture
let layouts, sceneBuffer, sceneBufferSize = 0, bufferGeneration = 0;
let canvas, canvasContext, canvasFormat, blitPipeline, blitSampler;
let presentGeneration = 0;
const stagingBuffers = []; // per-submission fill sources, destroyed after submit
const stats = { scenes: 0, draws: 0, pipelines: 0, textureUploads: 0, presents: 0, bindGroups: 0, submitMs: 0, sceneBytes: 0 };
let log = message => console.warn(message);
let statsReportedAt = 0;
const warned = new Set();
const warnOnce = message => {
  if (warned.has(message)) return;
  warned.add(message);
  log(`[gxm-scene] ${message}`);
};

export async function init({ compilerURL, nagaURL, wasiShimURL, logger }) {
  if (logger) log = logger;
  if (device) return;
  if (!globalThis.navigator?.gpu)
    throw new Error(globalThis.isSecureContext
      ? 'WebGPU unavailable: navigator.gpu is missing in this browser context'
      : 'WebGPU unavailable: this origin is not a secure context (use https:// or http://localhost)');
  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) throw new Error('WebGPU adapter unavailable');
  const info = adapter.info ?? {};
  log(`[gxm-scene] adapter: ${info.vendor} ${info.architecture} ${info.device} ${info.description}`);
  device = await adapter.requestDevice();
  device.addEventListener('uncapturederror', event =>
    warnOnce(`WebGPU validation: ${event.error?.message ?? event.error}`));
  compiler = await createGXPShaderAdapter({ compilerURL, nagaURL, wasiShimURL });
  const bufferEntry = (binding, visibility, type) =>
    ({ binding, visibility, buffer: { type, hasDynamicOffset: true } });
  const textureGroup = visibility => device.createBindGroupLayout({ entries:
    Array.from({ length: 32 }, (_, i) => i % 2 === 0
      ? { binding: i, visibility, texture: { sampleType: 'float' } }
      : { binding: i, visibility, sampler: { type: 'filtering' } }) });
  const empty = device.createBindGroupLayout({ entries: [] });
  layouts = {
    buffers: device.createBindGroupLayout({ entries: [
      bufferEntry(0, GPUShaderStage.VERTEX, 'uniform'),
      bufferEntry(1, GPUShaderStage.FRAGMENT, 'uniform'),
      bufferEntry(2, GPUShaderStage.VERTEX, 'read-only-storage'),
      bufferEntry(3, GPUShaderStage.FRAGMENT, 'read-only-storage'),
    ] }),
    empty,
    vertexTextures: textureGroup(GPUShaderStage.VERTEX),
    fragmentTextures: textureGroup(GPUShaderStage.FRAGMENT),
  };
  layouts.pipeline = device.createPipelineLayout({ bindGroupLayouts:
    [layouts.buffers, empty, layouts.vertexTextures, layouts.fragmentTextures] });
  // Unbound texture slots of a 16-unit group still need valid resources.
  layouts.dummyView = device.createTexture({ size: [1, 1], format: 'rgba8unorm',
    usage: GPUTextureUsage.TEXTURE_BINDING }).createView();
}

// Translation of one GXP (vertex or fragment) into WGSL. `id` is the
// producer's program id; the same GXP bytes always get the same id.
export async function registerProgram(id, gxp, fragment) {
  if (programs.has(id)) return;
  const textureFormats = new Uint32Array(32).fill(0x0c000000); // RGBA8 after producer decode
  const { wgsl } = await compiler.translate(gxp, { textureFormats });
  const module = device.createShaderModule({ code: wgsl });
  const info = await module.getCompilationInfo();
  const errors = info.messages.filter(message => message.type === 'error');
  if (errors.length) throw new Error(`GXP ${id} WGSL: ${errors.map(m => m.message).join('\n')}`);
  programs.set(id, { module, fragment });
}

export function attachCanvas(offscreen) {
  canvas = offscreen;
  canvasContext = canvas.getContext('webgpu');
  canvasFormat = navigator.gpu.getPreferredCanvasFormat();
  canvasContext.configure({ device, format: canvasFormat, alphaMode: 'opaque' });
}

// GXM color formats the consumer renders to (base format | swizzle).
const colorFormats = new Map([
  [0x00000000, 'rgba8unorm'],  // U8U8U8U8_ABGR
  [0x10000000, 'rgba8unorm'],  // U8U8U8U8 variants (swizzle handled by the shader)
  [0x60800000, 'rgb10a2unorm'], // U2U10U10U10_ABGR
  [0x61800000, 'rgb10a2unorm'],
  [0x01000000, 'rgba16float'], // F16F16F16F16
]);
// One texel of `format` holding an RGBA8 color (partial target fills).
function packColor(format, [r, g, b, a]) {
  switch (format) {
  case 'rgba8unorm': return new Uint8Array([r, g, b, a]);
  case 'rgb10a2unorm': {
    const scale = v => Math.round(v * 1023 / 255);
    return new Uint8Array(new Uint32Array([(scale(r) | scale(g) << 10 | scale(b) << 20 | Math.round(a * 3 / 255) << 30) >>> 0]).buffer);
  }
  default: throw new Error(`partial fill of a ${format} render target`);
  }
}
function colorFormat(guest) {
  const format = colorFormats.get(guest >>> 0) ?? colorFormats.get((guest & 0xf1800000) >>> 0);
  if (!format) throw new Error(`unsupported GXM color format ${(guest >>> 0).toString(16)}`);
  return format;
}

function targetFor(address, format, width, height) {
  let target = targets.get(address);
  if (target && target.width === width && target.height === height && target.guestFormat === format)
    return target;
  target?.texture.destroy();
  target?.snapshot?.texture.destroy();
  const gpuFormat = colorFormat(format);
  const texture = device.createTexture({ size: [width, height], format: gpuFormat,
    usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING
      | GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST });
  target = { address, width, height, guestFormat: format, gpuFormat, texture, view: texture.createView(),
    depth: null, fresh: true };
  targets.set(address, target);
  return target;
}

const depthFormats = new Map([
  [0x02444000, 'depth16unorm'], [0x00044000, 'depth32float'],
  [0x01266000, 'depth24plus-stencil8'], [0x00000000, 'depth24plus-stencil8'],
]);
// mode 1: on-chip only (no guest surface), a transient buffer shared by
// same-sized scenes; mode 2: a guest depth/stencil surface, kept on the GPU
// under its own data addresses so any color target can load it later.
function depthAttachmentFor(target, mode, format, depthAddress, stencilAddress) {
  const gpuFormat = depthFormats.get(format >>> 0) ?? 'depth24plus-stencil8';
  if (mode === 1) {
    const key = `${target.width}x${target.height}:${gpuFormat}`;
    let texture = depthScratch.get(key);
    if (!texture) {
      texture = device.createTexture({ size: [target.width, target.height], format: gpuFormat,
        usage: GPUTextureUsage.RENDER_ATTACHMENT });
      depthScratch.set(key, texture);
    }
    return { view: texture.createView(), format: gpuFormat, fresh: true };
  }
  const key = `${depthAddress >>> 0}:${stencilAddress >>> 0}`;
  let surface = depthSurfaces.get(key);
  if (!surface || surface.format !== gpuFormat || surface.width !== target.width || surface.height !== target.height) {
    surface?.texture.destroy();
    const texture = device.createTexture({ size: [target.width, target.height], format: gpuFormat,
      usage: GPUTextureUsage.RENDER_ATTACHMENT });
    surface = { texture, view: texture.createView(), format: gpuFormat, width: target.width, height: target.height, fresh: true };
    depthSurfaces.set(key, surface);
  }
  return surface;
}

// --- GXM state translation --------------------------------------------------
const blendOps = ['add', 'add', 'subtract', 'reverse-subtract', 'min', 'max'];
const blendFactors = ['zero', 'one', 'src', 'one-minus-src', 'src-alpha', 'one-minus-src-alpha',
  'dst', 'one-minus-dst', 'dst-alpha', 'one-minus-dst-alpha', 'src-alpha-saturate', 'dst-alpha'];
const compareFuncs = ['never', 'less', 'equal', 'less-equal', 'greater', 'not-equal', 'greater-equal', 'always'];
const stencilOps = ['keep', 'zero', 'replace', 'increment-clamp', 'decrement-clamp', 'invert',
  'increment-wrap', 'decrement-wrap'];
// SceGxmAttributeFormat x components -> WebGPU vertex format
const vertexFormats = {
  0: { 2: 'uint8x2', 4: 'uint8x4' }, 1: { 2: 'sint8x2', 4: 'sint8x4' },
  2: { 2: 'uint16x2', 4: 'uint16x4' }, 3: { 2: 'sint16x2', 4: 'sint16x4' },
  4: { 2: 'unorm8x2', 4: 'unorm8x4' }, 5: { 2: 'snorm8x2', 4: 'snorm8x4' },
  6: { 2: 'unorm16x2', 4: 'unorm16x4' }, 7: { 2: 'snorm16x2', 4: 'snorm16x4' },
  8: { 2: 'float16x2', 4: 'float16x4' },
  9: { 1: 'float32', 2: 'float32x2', 3: 'float32x3', 4: 'float32x4' },
};
const filters = ['nearest', 'linear'];
const addressModes = ['repeat', 'mirror-repeat', 'clamp-to-edge', 'clamp-to-edge', 'clamp-to-edge',
  'clamp-to-edge', 'repeat', 'clamp-to-edge'];
function samplerFor(min, mag, mip, u, v, lodMax) {
  const key = `${min}|${mag}|${mip}|${u}|${v}|${lodMax}`;
  let sampler = samplers.get(key);
  if (!sampler) {
    sampler = device.createSampler({ minFilter: filters[min & 1], magFilter: filters[mag & 1],
      mipmapFilter: filters[mip & 1], addressModeU: addressModes[u & 7], addressModeV: addressModes[v & 7],
      lodMinClamp: 0, lodMaxClamp: lodMax });
    samplers.set(key, sampler);
  }
  return sampler;
}

function pipelineFor(d, target, depth) {
  const key = d.pipelineKey + '|' + target.gpuFormat + '|' + (depth ? depth.format : '-');
  let pipeline = pipelines.get(key);
  if (pipeline) return pipeline;
  const vertex = programs.get(d.vs), fragment = programs.get(d.fs);
  if (!vertex || !fragment) throw new Error(`draw with unregistered program ${d.vs}/${d.fs}`);
  const buffers = d.streams.map(stream => ({ arrayStride: stream.stride, stepMode: 'vertex', attributes: [] }));
  for (const a of d.attributes) {
    const format = vertexFormats[a.format]?.[a.components];
    if (!format) throw new Error(`unsupported vertex attribute ${a.format}x${a.components}`);
    buffers[a.stream].attributes.push({ shaderLocation: a.location, offset: a.offset, format });
  }
  const colorMask = d.blend[0];
  const writeMask = (colorMask & 2 ? 1 : 0) | (colorMask & 4 ? 2 : 0) | (colorMask & 8 ? 4 : 0) | (colorMask & 1 ? 8 : 0);
  const [, colorFunc, alphaFunc, colorSrc, colorDst, alphaSrc, alphaDst] = d.blend;
  const blend = colorFunc || alphaFunc ? {
    color: { operation: blendOps[colorFunc], srcFactor: blendFactors[colorSrc], dstFactor: blendFactors[colorDst] },
    alpha: { operation: blendOps[alphaFunc], srcFactor: blendFactors[alphaSrc], dstFactor: blendFactors[alphaDst] },
  } : undefined;
  const face = s => ({ compare: compareFuncs[s[0]], failOp: stencilOps[s[1]],
    depthFailOp: stencilOps[s[2]], passOp: stencilOps[s[3]] });
  const hasStencil = depth && depth.format === 'depth24plus-stencil8';
  const descriptor = {
    layout: layouts.pipeline,
    vertex: { module: vertex.module, entryPoint: 'main_vs', buffers },
    fragment: { module: fragment.module, entryPoint: 'main_fs',
      targets: [{ format: target.gpuFormat, writeMask: d.fragmentDisabled ? 0 : writeMask, ...(blend ? { blend } : {}) }] },
    primitive: { topology: d.topology, cullMode: ['none', 'back', 'front'][d.cull] ?? 'none',
      frontFace: 'ccw', ...(d.topology.endsWith('strip') ? { stripIndexFormat: d.indexSize === 2 ? 'uint16' : 'uint32' } : {}) },
    ...(depth ? { depthStencil: { format: depth.format, depthWriteEnabled: d.depthWrite,
      depthCompare: compareFuncs[d.depthFunc],
      ...(hasStencil ? { stencilFront: face(d.stencilFront), stencilBack: face(d.stencilBack),
        stencilReadMask: d.stencilReadMask, stencilWriteMask: d.stencilWriteMask } : {}) } } : {}),
  };
  pipeline = device.createRenderPipeline(descriptor);
  pipelines.set(key, pipeline);
  ++stats.pipelines;
  return pipeline;
}

function ensureSceneBuffer(size) {
  if (size <= sceneBufferSize) return;
  sceneBuffer?.destroy();
  sceneBufferSize = Math.max(1 << 20, 2 ** Math.ceil(Math.log2(size)));
  sceneBuffer = device.createBuffer({ size: sceneBufferSize, usage: GPUBufferUsage.VERTEX
    | GPUBufferUsage.INDEX | GPUBufferUsage.UNIFORM | GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
  ++bufferGeneration;
  bindGroups.clear();
}

function bufferGroupFor(vsSize, fsSize) {
  const key = `${bufferGeneration}|${vsSize}|${fsSize}`;
  let group = bindGroups.get(key);
  if (!group) {
    ++stats.bindGroups;
    group = device.createBindGroup({ layout: layouts.buffers, entries: [
      { binding: 0, resource: { buffer: sceneBuffer, size: 48 } },
      { binding: 1, resource: { buffer: sceneBuffer, size: 32 } },
      { binding: 2, resource: { buffer: sceneBuffer, size: Math.max(16, vsSize) } },
      { binding: 3, resource: { buffer: sceneBuffer, size: Math.max(16, fsSize) } },
    ] });
    bindGroups.set(key, group);
  }
  return group;
}

function textureView(id) {
  if (id & 0x80000000) {
    // Render target sampled as a texture: the id carries the guest address;
    // bit 30 selects the snapshot taken when its own pass began.
    const target = targets.get((id & 0x3fffffff) * 4);
    if (!target) { warnOnce('texture aliases a render target that was never rendered'); return layouts.dummyView; }
    if (id & 0x40000000) return target.snapshot.view;
    return target.view;
  }
  return textures.get(id)?.view ?? layouts.dummyView;
}

function textureGroupFor(layout, units) {
  const key = (layout === layouts.fragmentTextures ? 'f' : 'v') + units.map(t => t.key).join(';');
  let group = bindGroups.get(key);
  if (!group) {
    const entries = [];
    for (let unit = 0; unit < 16; ++unit) {
      const t = units.find(u => u.unit === unit);
      entries.push({ binding: unit * 2, resource: t ? textureView(t.id) : layouts.dummyView });
      entries.push({ binding: unit * 2 + 1, resource: t ? t.sampler : samplerFor(0, 0, 0, 2, 2, 0) });
    }
    ++stats.bindGroups;
    group = device.createBindGroup({ layout, entries });
    // Render-target aliases can be recreated; do not keep their groups.
    if (!units.some(t => t.id & 0x80000000)) bindGroups.set(key, group);
  }
  return group;
}
const emptyGroupCache = {};
function emptyGroup() {
  return emptyGroupCache.group ??= device.createBindGroup({ layout: layouts.empty, entries: [] });
}

// words: Uint32Array view of the command stream; data: Uint8Array payload.
export function submitScene(words, data) {
  const started = performance.now();
  try {
    encodeScene(words, data);
  } finally {
    const now = performance.now();
    stats.submitMs += now - started;
    if (now - statsReportedAt >= 5000) {
      statsReportedAt = now;
      log(`[gxm-scene] stats ${JSON.stringify({ ...stats, submitMs: Math.round(stats.submitMs),
        targets: targets.size, textures: textures.size, pipelinesCached: pipelines.size, groupsCached: bindGroups.size })}`);
    }
  }
}

function encodeScene(words, data) {
  ensureSceneBuffer(data.byteLength + 4096);
  device.queue.writeBuffer(sceneBuffer, 0, data);
  stats.sceneBytes += data.byteLength;
  const floats = new Float32Array(words.buffer, words.byteOffset, words.length);
  let cursor = 0;
  const word = () => words[cursor++];
  if (word() !== 0x31535847) throw new Error('unknown GXM scene stream');
  const encoder = device.createCommandEncoder();
  let pass = null, target = null, depth = null;
  while (cursor < words.length) {
    const command = word();
    switch (command) {
    case 1: { // BEGIN_PASS
      const address = word(), format = word(), width = word(), height = word();
      const depthMode = word(), depthFormat = word(), depthLoad = word(), depthStore = word();
      const clearDepth = floats[cursor++], clearStencil = word();
      const depthAddress = word(), stencilAddress = word(), snapshot = word();
      target = targetFor(address, format, width, height);
      if (snapshot) {
        // A draw samples this target: give it the contents from before the pass.
        if (!target.snapshot) {
          const texture = device.createTexture({ size: [target.width, target.height], format: target.gpuFormat,
            usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
          target.snapshot = { texture, view: texture.createView() };
        }
        encoder.copyTextureToTexture({ texture: target.texture }, { texture: target.snapshot.texture },
          [target.width, target.height]);
      }
      depth = depthMode ? depthAttachmentFor(target, depthMode, depthFormat, depthAddress, stencilAddress) : null;
      // Guest depth contents exist only once this surface was stored.
      const depthKept = depth && depthMode === 2 && !depth.fresh;
      const stencil = depth && depth.format === 'depth24plus-stencil8';
      pass = encoder.beginRenderPass({
        colorAttachments: [{ view: target.view, loadOp: target.fresh ? 'clear' : 'load', storeOp: 'store',
          clearValue: [0, 0, 0, 1] }],
        ...(depth ? { depthStencilAttachment: { view: depth.view,
          depthLoadOp: depthLoad && depthKept ? 'load' : 'clear',
          depthClearValue: clearDepth, depthStoreOp: depthStore || depthMode === 2 ? 'store' : 'discard',
          ...(stencil ? { stencilLoadOp: depthLoad && depthKept ? 'load' : 'clear',
            stencilClearValue: clearStencil, stencilStoreOp: depthStore || depthMode === 2 ? 'store' : 'discard' } : {}) } } : {}),
      });
      target.fresh = false;
      if (depth && depthMode === 2) depth.fresh = false; // mode 2 always stores
      break;
    }
    case 2: { // DRAW
      const d = { vs: word(), fs: word(), cull: word(), topology: ['triangle-list', 'triangle-strip', 'line-list', 'line-strip', 'point-list'][word()],
        blend: [word(), word(), word(), word(), word(), word(), word()], fragmentDisabled: word() !== 0,
        depthFunc: word(), depthWrite: word() !== 0,
        stencilFront: [word(), word(), word(), word()], stencilBack: [word(), word(), word(), word()],
        stencilReadMask: word(), stencilWriteMask: word(), stencilRef: word() };
      const streamCount = word();
      d.streams = [];
      for (let i = 0; i < streamCount; ++i) d.streams.push({ stride: word(), offset: word(), size: word() });
      const attributeCount = word();
      d.attributes = [];
      for (let i = 0; i < attributeCount; ++i)
        d.attributes.push({ location: word(), stream: word(), offset: word(), format: word(), components: word() });
      d.indexSize = word(); const indexCount = word(), indexOffset = word();
      const viewport = [floats[cursor++], floats[cursor++], floats[cursor++], floats[cursor++]];
      const scissor = [word(), word(), word(), word()];
      const vsInfo = word(), fsInfo = word(), vsUniforms = word(), vsUniformSize = word(),
        fsUniforms = word(), fsUniformSize = word();
      const textureCount = word();
      const fragmentUnits = [], vertexUnits = [];
      for (let i = 0; i < textureCount; ++i) {
        const unit = word(), id = word(), min = word(), mag = word(), mip = word(), u = word(), v = word(), lodMax = word();
        const sampler = samplerFor(min, mag, mip, u, v, lodMax);
        const entry = { unit: unit & 15, id, sampler, key: `${unit}:${id}:${min}${mag}${mip}${u}${v}${lodMax}` };
        (unit & 16 ? vertexUnits : fragmentUnits).push(entry);
      }
      d.pipelineKey = `${d.vs}|${d.fs}|${d.cull}|${d.topology}|${d.blend}|${d.fragmentDisabled}|${d.depthFunc}|${d.depthWrite}|`
        + `${d.stencilFront}|${d.stencilBack}|${d.stencilReadMask}|${d.stencilWriteMask}|${d.indexSize}|`
        + d.streams.map(s => s.stride).join(',') + '|' + d.attributes.map(a => `${a.location}:${a.stream}:${a.offset}:${a.format}:${a.components}`).join(',');
      if (!pass) throw new Error('draw outside a pass');
      pass.setPipeline(pipelineFor(d, target, depth));
      pass.setBindGroup(0, bufferGroupFor(vsUniformSize, fsUniformSize), [vsInfo, fsInfo, vsUniforms, fsUniforms]);
      pass.setBindGroup(1, emptyGroup());
      pass.setBindGroup(2, textureGroupFor(layouts.vertexTextures, vertexUnits));
      pass.setBindGroup(3, textureGroupFor(layouts.fragmentTextures, fragmentUnits));
      d.streams.forEach((stream, i) => pass.setVertexBuffer(i, sceneBuffer, stream.offset, stream.size));
      pass.setIndexBuffer(sceneBuffer, d.indexSize === 2 ? 'uint16' : 'uint32', indexOffset, indexCount * d.indexSize);
      pass.setViewport(viewport[0], viewport[1], Math.max(viewport[2], 0), Math.max(viewport[3], 0), 0, 1);
      pass.setScissorRect(...scissor);
      pass.setStencilReference(d.stencilRef);
      pass.drawIndexed(indexCount);
      ++stats.draws;
      break;
    }
    case 3: { // TEXTURE: id, width, height, levels, then per level offset,size
      const id = word(), width = word(), height = word(), levels = word();
      let entry = textures.get(id);
      if (!entry || entry.width !== width || entry.height !== height || entry.levels !== levels) {
        entry?.texture.destroy();
        const texture = device.createTexture({ size: [width, height], format: 'rgba8unorm', mipLevelCount: levels,
          usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
        entry = { texture, view: texture.createView(), width, height, levels };
        textures.set(id, entry);
        // Texture groups referencing a destroyed texture must be rebuilt.
        for (const key of bindGroups.keys()) if (!key.includes('|')) bindGroups.delete(key);
      }
      for (let level = 0; level < levels; ++level) {
        const offset = word(), size = word();
        const w = Math.max(1, width >> level), h = Math.max(1, height >> level);
        device.queue.writeTexture({ texture: entry.texture, mipLevel: level },
          data.subarray(offset, offset + size), { bytesPerRow: w * 4 }, [w, h]);
      }
      ++stats.textureUploads;
      break;
    }
    case 6: { // REGION: id, target address, x, y, width, height (outside any pass)
      const id = word(), address = word(), x = word(), y = word(), width = word(), height = word();
      const source = targets.get(address);
      if (!source) { warnOnce('region of a render target that was never rendered'); break; }
      let entry = textures.get(id);
      if (!entry || entry.width !== width || entry.height !== height || entry.format !== source.gpuFormat) {
        entry?.texture.destroy();
        const texture = device.createTexture({ size: [width, height], format: source.gpuFormat,
          usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
        entry = { texture, view: texture.createView(), width, height, levels: 1, format: source.gpuFormat };
        textures.set(id, entry);
        for (const key of bindGroups.keys()) if (!key.includes('|')) bindGroups.delete(key);
      }
      encoder.copyTextureToTexture({ texture: source.texture, origin: { x, y } }, { texture: entry.texture }, [width, height]);
      break;
    }
    case 4: // END_PASS
      pass.end(); pass = null;
      break;
    case 5: { // CLEAR_TARGET: address, x, y, width, height, A8B8G8R8 color
      const address = word(), x = word(), y = word(), width = word(), height = word(), color = word();
      const cleared = targets.get(address);
      if (!cleared) break; // guest memory already holds the fill
      const rgba = [color & 255, (color >>> 8) & 255, (color >>> 16) & 255, color >>> 24];
      if (x === 0 && y === 0 && width >= cleared.width && height >= cleared.height) {
        encoder.beginRenderPass({ colorAttachments: [{ view: cleared.view, loadOp: 'clear', storeOp: 'store',
          clearValue: rgba.map(v => v / 255) }] }).end();
      } else {
        const w = Math.min(width, cleared.width - Math.min(x, cleared.width));
        const h = Math.min(height, cleared.height - Math.min(y, cleared.height));
        if (w > 0 && h > 0) {
          // Through the encoder, so the fill stays ordered with this stream's passes.
          const texel = packColor(cleared.gpuFormat, rgba);
          const bytesPerRow = Math.ceil(w * texel.length / 256) * 256;
          const staging = device.createBuffer({ size: bytesPerRow * h, usage: GPUBufferUsage.COPY_SRC, mappedAtCreation: true });
          const bytes = new Uint8Array(staging.getMappedRange());
          for (let row = 0; row < h; ++row)
            for (let i = 0; i < w; ++i) bytes.set(texel, row * bytesPerRow + i * texel.length);
          staging.unmap();
          encoder.copyBufferToTexture({ buffer: staging, bytesPerRow }, { texture: cleared.texture, origin: { x, y } }, [w, h]);
          stagingBuffers.push(staging);
        }
      }
      cleared.fresh = false;
      break;
    }
    default:
      throw new Error(`unknown GXM scene command ${command}`);
    }
  }
  if (pass) pass.end();
  device.queue.submit([encoder.finish()]);
  for (const buffer of stagingBuffers.splice(0)) buffer.destroy();
  ++stats.scenes;
}

export function hasTarget(address) { return targets.has(address); }

function blit(texture, viewTarget, format) {
  if (!blitPipeline || blitPipeline.format !== format) {
    const module = device.createShaderModule({ code: `
      @group(0) @binding(0) var image: texture_2d<f32>;
      @group(0) @binding(1) var linearSampler: sampler;
      struct Out { @builtin(position) position: vec4f, @location(0) uv: vec2f };
      @vertex fn vs(@builtin(vertex_index) i: u32) -> Out {
        let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
        return Out(vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0), uv);
      }
      @fragment fn fs(input: Out) -> @location(0) vec4f {
        return vec4f(textureSample(image, linearSampler, input.uv).rgb, 1.0);
      }` });
    blitPipeline = device.createRenderPipeline({ layout: 'auto',
      vertex: { module, entryPoint: 'vs' }, fragment: { module, entryPoint: 'fs', targets: [{ format }] },
      primitive: { topology: 'triangle-list' } });
    blitPipeline.format = format;
    blitSampler = device.createSampler({ minFilter: 'linear', magFilter: 'linear' });
  }
  const encoder = device.createCommandEncoder();
  const pass = encoder.beginRenderPass({ colorAttachments: [{ view: viewTarget, loadOp: 'clear', storeOp: 'store' }] });
  pass.setPipeline(blitPipeline);
  pass.setBindGroup(0, device.createBindGroup({ layout: blitPipeline.getBindGroupLayout(0),
    entries: [{ binding: 0, resource: texture.createView() }, { binding: 1, resource: blitSampler }] }));
  pass.draw(3);
  pass.end();
  device.queue.submit([encoder.finish()]);
}

// Present the render target at `address`. Returns false when no GPU target
// exists there (the caller then presents guest memory instead).
// `onFrame(generation, width, height, pixels|null)` receives every presented
// frame; pixels are read back only every `readbackEvery` frames.
export function presentTarget(address, onFrame, readbackEvery) {
  const target = targets.get(address);
  if (!target) return false;
  const generation = ++presentGeneration;
  ++stats.presents;
  if (canvasContext) {
    if (canvas.width !== target.width || canvas.height !== target.height) {
      canvas.width = target.width; canvas.height = target.height;
    }
    blit(target.texture, canvasContext.getCurrentTexture().createView(), canvasFormat);
  }
  if (readbackEvery > 0 && generation % readbackEvery === 1 % readbackEvery) {
    // RGBA8 copy of the displayed target, asynchronously; never blocks the guest.
    const copy = device.createTexture({ size: [target.width, target.height], format: 'rgba8unorm',
      usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
    blit(target.texture, copy.createView(), 'rgba8unorm');
    const bytesPerRow = Math.ceil(target.width * 4 / 256) * 256;
    const buffer = device.createBuffer({ size: bytesPerRow * target.height,
      usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const encoder = device.createCommandEncoder();
    encoder.copyTextureToBuffer({ texture: copy }, { buffer, bytesPerRow }, [target.width, target.height]);
    device.queue.submit([encoder.finish()]);
    const { width, height } = target;
    buffer.mapAsync(GPUMapMode.READ).then(() => {
      const mapped = new Uint8Array(buffer.getMappedRange());
      const pixels = new Uint8Array(width * height * 4);
      for (let y = 0; y < height; ++y)
        pixels.set(mapped.subarray(y * bytesPerRow, y * bytesPerRow + width * 4), y * width * 4);
      buffer.destroy(); copy.destroy();
      onFrame(generation, width, height, pixels);
    }, error => warnOnce(`frame readback failed: ${error}`));
  } else {
    onFrame(generation, target.width, target.height, null);
  }
  return true;
}

export function sceneStats() { return { ...stats, targets: targets.size, textures: textures.size, pipelines: pipelines.size }; }
