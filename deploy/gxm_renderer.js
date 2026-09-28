// Narrow WebGPU command consumer for the GXM backend bring-up.
// NOT a GXP translator or a replacement for SceGxm's validation/state machine.
// Input: translated WGSL + snapshotted vertex/index/uniform data. Output: tightly
// packed RGBA8 after GPU completion, suitable for the existing display bridge.
// Supported initially: one interleaved stream, triangle-list, RGBA8, no MSAA.
// Per-draw viewports map NDC to a target sub-rect (depth is always [0, 1]);
// omitted viewports cover the full target. Optional fragment unit zero: linear
// RGBA8, one mip, explicit sampler, group 3 bindings 0/1. No guest texture
// state is inferred. Optional fixed-function state: per-target write mask and
// color/alpha blending, and a per-target depth-stencil attachment with the
// depth compare/write mode from the GXM record state. The guest depth-stencil
// descriptor is translated by the caller (gxm_hle_bridge.js); this consumer
// accepts WebGPU values and rejects anything it cannot express.

// Vertex formats: [byte size, minimum attribute offset alignment]. WebGPU
// requires the offset to be a multiple of min(4, byte size of the format), so
// only the two-component 8-bit formats align to 2; every wider format aligns
// to 4 (which is also what a Metal-backed implementation demands).
const formats = Object.freeze({
  float32: [4, 4], float32x2: [8, 4], float32x3: [12, 4], float32x4: [16, 4],
  unorm8x2: [2, 2], unorm8x4: [4, 4], snorm8x2: [2, 2], snorm8x4: [4, 4],
  unorm16x2: [4, 4], unorm16x4: [8, 4], snorm16x2: [4, 4], snorm16x4: [8, 4],
  float16x2: [4, 4], float16x4: [8, 4],
});
const align = (value, alignment) => Math.ceil(value / alignment) * alignment;
function integer(value, min, max, name) {
  if (!Number.isSafeInteger(value) || value < min || value > max)
    throw new RangeError(`${name} out of range`);
  return value;
}
function bytes(value) {
  if (value instanceof ArrayBuffer) return new Uint8Array(value).slice();
  if (ArrayBuffer.isView(value))
    return new Uint8Array(value.buffer, value.byteOffset, value.byteLength).slice();
  throw new TypeError('expected a buffer or typed array');
}
// Non-copying view of the same inputs. Only for data the caller guarantees
// stable until the synchronous prefix of submit() has staged it into padded
// upload buffers (the bridge passes owned per-draw buffers, never Wasm views).
// Public snapshot semantics are unchanged: every staged copy still happens
// before the first await, so mutating the input after submit() returns can
// never affect the draw. Use viewBytes() for staging, bytes() for retained
// copies (initial surface, texture snapshots consumed across awaits).
function viewBytes(value) {
  if (value instanceof ArrayBuffer) return new Uint8Array(value);
  if (ArrayBuffer.isView(value))
    return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  throw new TypeError('expected a buffer or typed array');
}

// Memory64 host pointers may be BigInt. Never truncate them with >>>0 or Number
// before checking representability. Pass a fresh heap view after memory.grow.
export function snapshotGuestBytes(heap, address, length) {
  if (!(heap instanceof Uint8Array)) throw new TypeError('expected a byte heap');
  if (typeof address === 'bigint') {
    if (address < 0n || address > BigInt(Number.MAX_SAFE_INTEGER))
      throw new RangeError('guest upload address out of range');
    address = Number(address);
  }
  integer(address, 0, heap.byteLength, 'guest upload address');
  integer(length, 0, heap.byteLength - address, 'guest upload length');
  return heap.slice(address, address + length);
}

export function createGXMRenderer(device) {
  if (!device) throw new TypeError('WebGPU device required');
  const targets = new Map(), programs = new Map();
  // Device-local LRU. Program handles and pipeline-cache ownership are separate:
  // destroying a handle never invalidates another handle using the pipeline.
  const pipelines = new Map(), pipelineLimit = 64;
  let pipelineHits = 0, pipelineMisses = 0;
  // Resident GPU objects across submits. Samplers are immutable so they are
  // shared by descriptor; the fragment texture is reused when dimensions
  // match and re-uploaded every submit (queue order keeps the write after
  // prior reads). Both are bounded (4 sampler shapes max, one texture) and
  // cleared on dispose. Targets/programs stay caller-owned; the bridge
  // caches those handles one-deep instead.
  const samplers = new Map();
  let residentTexture = null;
  function samplerFor(spec) {
    const key = spec.minFilter + '|' + spec.magFilter + '|' + spec.addressModeU + '|' + spec.addressModeV;
    let sampler = samplers.get(key);
    if (!sampler) {
      sampler = device.createSampler({ ...spec, mipmapFilter: 'nearest', lodMinClamp: 0, lodMaxClamp: 0 });
      samplers.set(key, sampler);
    }
    return sampler;
  }
  let nextId = 1, busy = false, disposed = false, lost = null;
  device.lost.then(info => { lost = `WebGPU device lost: ${info.message}`; });
  function available() {
    if (disposed) throw new Error('renderer disposed');
    if (lost) throw new Error(lost);
    if (busy) throw new Error('renderer operation in flight; await completion');
  }
  function lookup(map, id, kind) {
    const resource = map.get(id);
    if (!resource) throw new Error(`unknown ${kind}: ${id}`);
    return resource;
  }

  return Object.freeze({
    // depthFormat adds a depth-stencil attachment. Only formats WebGPU renders
    // directly are accepted; there is no depth readback, so the guest's depth
    // memory is never written (the native producer rejects the force-store
    // state that would require it).
    createTarget(width, height, { depthFormat } = {}) {
      available();
      integer(width, 1, device.limits.maxTextureDimension2D, 'width');
      integer(height, 1, device.limits.maxTextureDimension2D, 'height');
      if (align(width * 4, 256) * height > device.limits.maxBufferSize)
        throw new RangeError('render target readback exceeds maxBufferSize');
      const texture = device.createTexture({ size: [width, height], format: 'rgba8unorm',
        usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST });
      let depthTexture;
      if (depthFormat !== undefined) {
        if (!['depth16unorm', 'depth32float', 'depth24plus-stencil8'].includes(depthFormat))
          throw new Error(`unsupported depth format: ${depthFormat}`);
        depthTexture = device.createTexture({ size: [width, height], format: depthFormat,
          usage: GPUTextureUsage.RENDER_ATTACHMENT });
      }
      const id = nextId++;
      targets.set(id, { width, height, texture, depthTexture, depthFormat });
      return id;
    },

    // WGSL must eventually come from real GXP conversion. The smoke test uses
    // explicit test WGSL and deliberately does not claim guest shader execution.
    async createProgram({ vertexWGSL, fragmentWGSL, stride, attributes, uniformSize = 0,
      vertexEntryPoint = 'main', fragmentEntryPoint = 'main', bufferBindings,
      fragmentTexture = false, writeMask = 0xf, blend, depthStencil, ...unsupportedState }) {
      available();
      if (typeof fragmentTexture !== 'boolean') throw new TypeError('fragmentTexture must be boolean');
      if (Object.keys(unsupportedState).length)
        throw new Error(`unsupported pipeline state: ${Object.keys(unsupportedState).join(', ')}`);
      integer(writeMask, 0, 0xf, 'color write mask');
      const blendOperations = ['add', 'subtract', 'reverse-subtract', 'min', 'max'];
      const blendFactors = ['zero', 'one', 'src', 'one-minus-src', 'src-alpha', 'one-minus-src-alpha',
        'dst', 'one-minus-dst', 'dst-alpha', 'one-minus-dst-alpha', 'src-alpha-saturate'];
      // WebGPU accepts src-alpha-saturate as the color source factor only.
      const blendComponent = (component, name, color) => {
        if (!component || typeof component !== 'object') throw new TypeError(`${name} blend component required`);
        const { operation, srcFactor, dstFactor, ...extra } = component;
        if (Object.keys(extra).length) throw new Error(`unsupported ${name} blend state`);
        if (!blendOperations.includes(operation) || !blendFactors.includes(srcFactor)
            || !blendFactors.includes(dstFactor) || (!color && srcFactor === 'src-alpha-saturate'))
          throw new Error(`unsupported ${name} blend state`);
        return { operation, srcFactor, dstFactor };
      };
      let fragmentBlend;
      if (blend !== undefined) {
        if (!blend || typeof blend !== 'object' || Object.keys(blend).some(key => !['color', 'alpha'].includes(key)))
          throw new TypeError('unsupported blend descriptor');
        fragmentBlend = { color: blendComponent(blend.color, 'color', true),
          alpha: blendComponent(blend.alpha, 'alpha', false) };
      }
      let pipelineDepth;
      if (depthStencil !== undefined) {
        if (!depthStencil || typeof depthStencil !== 'object') throw new TypeError('depth-stencil state required');
        const { format, depthCompare, depthWriteEnabled, ...extraDepth } = depthStencil;
        if (Object.keys(extraDepth).length || !['depth16unorm', 'depth32float', 'depth24plus-stencil8'].includes(format)
            || !['never', 'less', 'equal', 'less-equal', 'greater', 'not-equal', 'greater-equal', 'always'].includes(depthCompare)
            || typeof depthWriteEnabled !== 'boolean')
          throw new Error('unsupported depth-stencil state');
        pipelineDepth = { format, depthWriteEnabled, depthCompare };
      }
      if (typeof vertexWGSL !== 'string' || typeof fragmentWGSL !== 'string'
          || typeof vertexEntryPoint !== 'string' || typeof fragmentEntryPoint !== 'string')
        throw new TypeError('shader sources and entry points must be strings');
      integer(stride, 4, device.limits.maxVertexBufferArrayStride, 'vertex stride');
      if (stride % 4) throw new RangeError('vertex stride must be a multiple of four');
      integer(uniformSize, 0, device.limits.maxUniformBufferBindingSize, 'uniform size');
      if (uniformSize % 16) throw new RangeError('uniform size must be a multiple of sixteen');
      if (!Array.isArray(attributes) || !attributes.length
          || attributes.length > device.limits.maxVertexAttributes)
        throw new RangeError('invalid vertex attribute count');
      const locations = new Set();
      const layout = attributes.map(({ shaderLocation, offset, format }) => {
        const spec = formats[format];
        if (!spec) throw new Error(`unsupported vertex format: ${format}`);
        integer(shaderLocation, 0, device.limits.maxVertexAttributes - 1, 'shader location');
        integer(offset, 0, stride - spec[0], 'attribute offset');
        if (offset % spec[1] || locations.has(shaderLocation))
          throw new Error('misaligned or duplicate vertex attribute');
        locations.add(shaderLocation);
        return { shaderLocation, offset, format };
      });
      // Explicit binding ABI from the translator/producer. Native Vita3K uses
      // group 0: render-info UBOs at 0/1, guest uniform data SSBOs at 2/3.
      // Keep the original single-UBO API for non-GXP consumers.
      const bindings = bufferBindings ?? (uniformSize ? [{ binding: 0, size: uniformSize,
        type: 'uniform', visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT }] : []);
      const seen = new Set();
      const bindingLayout = bindings.map(({ binding, size, type, visibility }) => {
        integer(binding, 0, device.limits.maxBindingsPerBindGroup - 1, 'buffer binding');
        if (seen.has(binding)) throw new Error('duplicate buffer binding');
        seen.add(binding);
        if (!['uniform', 'read-only-storage'].includes(type)) throw new Error('unsupported buffer binding type');
        integer(size, 4, type === 'uniform' ? device.limits.maxUniformBufferBindingSize
          : device.limits.maxStorageBufferBindingSize, 'binding size');
        if (size % 4) throw new RangeError('binding size must be a multiple of four');
        integer(visibility, 1, GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT, 'binding visibility');
        return { binding, size, type, visibility };
      });
      // Normalize only ordering, not meaning. The key includes every field in
      // the descriptor below and explicit fixed render state. No hash collision
      // can alias guest shaders; dynamic vertices/uniform bytes are NOT state.
      layout.sort((a, b) => a.shaderLocation - b.shaderLocation);
      bindingLayout.sort((a, b) => a.binding - b.binding);
      const key = JSON.stringify([vertexWGSL, fragmentWGSL, vertexEntryPoint, fragmentEntryPoint,
        stride, layout, bindingLayout, fragmentTexture, 'rgba8unorm', 'triangle-list', 'none',
        'no-depth-bias', pipelineDepth ?? 'no-depth-stencil', fragmentBlend ?? 'no-blend', writeMask, 1]);
      const publish = pipeline => {
        const id = nextId++;
        programs.set(id, { pipeline, stride, uniformSize, bindingLayout, fragmentTexture,
          explicitBindings: bufferBindings !== undefined, depthStencil: pipelineDepth, writeMask });
        return id;
      };
      if (pipelines.has(key)) {
        const pipeline = pipelines.get(key);
        pipelines.delete(key); pipelines.set(key, pipeline);
        ++pipelineHits;
        return publish(pipeline);
      }
      ++pipelineMisses;
      busy = true;
      device.pushErrorScope('validation');
      let pipeline, failure;
      try {
        const vertex = device.createShaderModule({ code: vertexWGSL });
        const fragment = device.createShaderModule({ code: fragmentWGSL });
        for (const module of [vertex, fragment]) {
          const info = await module.getCompilationInfo();
          const errors = info.messages.filter(message => message.type === 'error');
          if (errors.length) throw new Error(errors.map(message => message.message).join('\n'));
        }
        const bindGroupLayout = device.createBindGroupLayout({ entries: bindingLayout.map(b => ({
          binding: b.binding, visibility: b.visibility, buffer: { type: b.type, minBindingSize: b.size },
        })) });
        const bindGroupLayouts = [bindGroupLayout];
        if (fragmentTexture) {
          // Match webgpu_spirv.h: fragment texture unit zero is group 3,
          // texture binding 0 / sampler binding 1. Groups 1 and 2 are empty.
          bindGroupLayouts.push(device.createBindGroupLayout({ entries: [] }),
            device.createBindGroupLayout({ entries: [] }),
            device.createBindGroupLayout({ entries: [
              { binding: 0, visibility: GPUShaderStage.FRAGMENT,
                texture: { sampleType: 'float', viewDimension: '2d', multisampled: false } },
              { binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } },
            ] }));
        }
        // Depth and blend state are fixed pipeline state: both are part of the
        // key above and both are materialized here. A program with depth state
        // can only be submitted to a target created with the same format.
        pipeline = await device.createRenderPipelineAsync({ layout: device.createPipelineLayout({ bindGroupLayouts }),
          vertex: { module: vertex, entryPoint: vertexEntryPoint, buffers: [{ arrayStride: stride, attributes: layout }] },
          fragment: { module: fragment, entryPoint: fragmentEntryPoint,
            targets: [{ format: 'rgba8unorm', writeMask, ...(fragmentBlend ? { blend: fragmentBlend } : {}) }] },
          primitive: { topology: 'triangle-list', cullMode: 'none' },
          ...(pipelineDepth ? { depthStencil: pipelineDepth } : {}),
        });
      } catch (error) { failure = error; }
      finally {
        try {
          const error = await device.popErrorScope();
          if (error && !failure) failure = new Error(error.message);
        } finally { busy = false; }
      }
      if (failure) throw failure;
      if (lost) throw new Error(lost);
      // Failed compilation/validation never populates the cache. Eviction drops
      // only this reference; live program handles retain their own pipeline.
      pipelines.set(key, pipeline);
      if (pipelines.size > pipelineLimit) pipelines.delete(pipelines.keys().next().value);
      return publish(pipeline);
    },

    // Exclusive, completion-safe submission. All input views are copied before
    // the first await: Wasm may grow memory or overwrite its buffers afterwards.
    // The producer must await this Promise before signaling GXM sync objects,
    // running a display-queue callback, or releasing/reusing the target.
    async submit(targetId, draws, { clearColor = [0, 0, 0, 1], initialPixels, depth } = {}) {
      available();
      const target = lookup(targets, targetId, 'target');
      if (!Array.isArray(clearColor) || clearColor.length !== 4
          || clearColor.some(value => !Number.isFinite(value) || value < 0 || value > 1))
        throw new RangeError('clear color must contain four normalized components');
      const initial = initialPixels === undefined ? null : bytes(initialPixels);
      if (initial && initial.length !== target.width * target.height * 4)
        throw new RangeError('initial surface size mismatch');
      // The depth attachment is per render pass, so its state is validated once
      // here; the load op is always clear (the native producer rejects the
      // force-load/force-store states that would need guest depth contents).
      let depthAttachment = null;
      if (depth === undefined) {
        if (target.depthFormat !== undefined)
          throw new Error('depth-stencil target submitted without depth state');
      } else {
        if (!depth || typeof depth !== 'object' || target.depthFormat === undefined)
          throw new TypeError('depth state requires a depth-stencil render target');
        const { format, stencil, clearValue, ...extraDepth } = depth;
        if (Object.keys(extraDepth).length || format !== target.depthFormat
            || typeof stencil !== 'boolean' || stencil !== (target.depthFormat === 'depth24plus-stencil8')
            || typeof clearValue !== 'number' || !Number.isFinite(clearValue) || clearValue < 0 || clearValue > 1)
          throw new Error('unsupported depth attachment state');
        depthAttachment = { format, stencil, clearValue };
      }
      const snapshots = draws.map(draw => {
        const program = lookup(programs, draw.program, 'program');
        // A pipeline with depth state must be submitted to a target created
        // with the same depth format, and a pass with a depth attachment must
        // use a pipeline that names it; otherwise depth would be ignored.
        if ((program.depthStencil?.format ?? null) !== (target.depthFormat ?? null))
          throw new Error('depth-stencil state does not match render target');
        // Views, not copies: staging into padded upload buffers below happens
        // synchronously before the first await, so post-submit mutation still
        // cannot affect the draw (covered by the smoke test).
        const vertices = viewBytes(draw.vertices), indices = viewBytes(draw.indices);
        const indexSize = draw.indexFormat === 'uint16' ? 2 : draw.indexFormat === 'uint32' ? 4 : 0;
        if (!indexSize) throw new Error('unsupported index format');
        if (!vertices.length || vertices.length % program.stride)
          throw new RangeError('partial or empty vertex stream');
        if (!indices.length || indices.length % (indexSize * 3))
          throw new RangeError('partial or empty triangle list');
        const vertexCount = vertices.length / program.stride;
        const indexView = new DataView(indices.buffer, indices.byteOffset, indices.byteLength);
        for (let offset = 0; offset < indices.length; offset += indexSize) {
          const index = indexSize === 2 ? indexView.getUint16(offset, true) : indexView.getUint32(offset, true);
          if (index >= vertexCount) throw new RangeError('index exceeds vertex stream');
        }
        // Viewport rects are dynamic draw state like vertices, never pipeline
        // key material. Normalized (non-negative extent, top-left origin) by
        // the producer; negative extents reject here instead of misrendering.
        let viewportRect = null;
        if (draw.viewport !== undefined) {
          const viewport = draw.viewport;
          if (!viewport || typeof viewport !== 'object') throw new TypeError('viewport must be an object');
          const { x, y, width, height } = viewport;
          for (const [name, value] of [['viewport x', x], ['viewport y', y],
              ['viewport width', width], ['viewport height', height]])
            if (typeof value !== 'number' || !Number.isFinite(value))
              throw new RangeError(`${name} must be finite`);
          if (width < 0 || height < 0) throw new RangeError('viewport extent must be non-negative');
          viewportRect = { x, y, width, height };
        }
        const uniforms = draw.uniforms === undefined ? new Uint8Array() : viewBytes(draw.uniforms);
        if (uniforms.length !== program.uniformSize) throw new RangeError('uniform size mismatch');
        const supplied = program.explicitBindings ? draw.buffers ?? {} : { 0: uniforms };
        if (program.explicitBindings && Object.keys(supplied).length !== program.bindingLayout.length)
          throw new RangeError('buffer binding count mismatch');
        const boundBuffers = program.bindingLayout.map(binding => {
          const data = viewBytes(supplied[binding.binding]);
          if (data.length !== binding.size) throw new RangeError('buffer binding size mismatch');
          return { ...binding, data };
        });
        for (const data of [vertices, indices, ...boundBuffers.map(b => b.data)])
          if (align(data.length, 4) > device.limits.maxBufferSize)
            throw new RangeError('upload exceeds maxBufferSize');
        let sampledTexture = null;
        if (program.fragmentTexture) {
          const source = draw.fragmentTexture;
          if (!source || typeof source !== 'object') throw new TypeError('fragment texture required');
          const { width, height, pixels, format = 'rgba8unorm', sampler = {}, ...extra } = source;
          if (Object.keys(extra).length || format !== 'rgba8unorm')
            throw new Error('unsupported fragment texture state');
          integer(width, 1, device.limits.maxTextureDimension2D, 'texture width');
          integer(height, 1, device.limits.maxTextureDimension2D, 'texture height');
          if (width * height * 4 > device.limits.maxBufferSize)
            throw new RangeError('texture upload exceeds maxBufferSize');
          const data = bytes(pixels);
          if (data.length !== width * height * 4) throw new RangeError('texture byte size mismatch');
          const { minFilter = 'nearest', magFilter = 'nearest', addressModeU = 'clamp-to-edge',
            addressModeV = 'clamp-to-edge', ...extraSampler } = sampler;
          if (Object.keys(extraSampler).length
              || !['nearest', 'linear'].includes(minFilter) || !['nearest', 'linear'].includes(magFilter)
              || !['clamp-to-edge', 'repeat', 'mirror-repeat'].includes(addressModeU)
              || !['clamp-to-edge', 'repeat', 'mirror-repeat'].includes(addressModeV))
            throw new Error('unsupported fragment sampler state');
          // Retained copy: the texture upload below runs before the first
          // await, but the sampler/texture objects persist across submits,
          // so texel bytes must stay owned here, not view the caller buffer.
          sampledTexture = { width, height, data,
            sampler: { minFilter, magFilter, addressModeU, addressModeV } };
        } else if (draw.fragmentTexture !== undefined) {
          throw new Error('fragment texture supplied to untextured program');
        }
        return { program, vertices, indices, boundBuffers, sampledTexture, viewportRect, indexFormat: draw.indexFormat,
          indexCount: indices.length / indexSize };
      });
      busy = true;
      device.pushErrorScope('validation');
      const buffers = [];
      let readback, pixels, failure;
      try {
        // Single staging copy per buffer: the padded upload is filled
        // synchronously here (still before the first await), fusing the old
        // snapshot-then-pad double copy with identical post-submit-mutation
        // semantics.
        const upload = (data, usage) => {
          const padded = new Uint8Array(align(data.length, 4));
          padded.set(data);
          const buffer = device.createBuffer({ size: padded.length, usage: usage | GPUBufferUsage.COPY_DST });
          buffers.push(buffer);
          device.queue.writeBuffer(buffer, 0, padded);
          return buffer;
        };
        if (initial) device.queue.writeTexture({ texture: target.texture }, initial,
          { bytesPerRow: target.width * 4 }, [target.width, target.height]);
        const encoder = device.createCommandEncoder();
        const pass = encoder.beginRenderPass({ colorAttachments: [{ view: target.texture.createView(),
          loadOp: initial ? 'load' : 'clear', storeOp: 'store', clearValue: clearColor }],
          ...(depthAttachment ? { depthStencilAttachment: {
            view: target.depthTexture.createView(), depthLoadOp: 'clear',
            depthClearValue: depthAttachment.clearValue, depthStoreOp: 'discard',
            ...(depthAttachment.stencil ? { stencilLoadOp: 'clear', stencilStoreOp: 'discard',
              stencilClearValue: 0 } : {}) } } : {}) });
        for (const draw of snapshots) {
          pass.setPipeline(draw.program.pipeline);
          // Explicit every draw: identical to the default full-target viewport
          // when omitted, and required before drawIndexed when provided.
          const viewport = draw.viewportRect ?? { x: 0, y: 0, width: target.width, height: target.height };
          pass.setViewport(viewport.x, viewport.y, viewport.width, viewport.height, 0, 1);
          pass.setVertexBuffer(0, upload(draw.vertices, GPUBufferUsage.VERTEX));
          pass.setIndexBuffer(upload(draw.indices, GPUBufferUsage.INDEX), draw.indexFormat);
          // Even an empty explicit group must be set with an explicit layout.
          pass.setBindGroup(0, device.createBindGroup({ layout: draw.program.pipeline.getBindGroupLayout(0),
            entries: draw.boundBuffers.map(b => ({ binding: b.binding, resource: {
              buffer: upload(b.data, b.type === 'uniform' ? GPUBufferUsage.UNIFORM : GPUBufferUsage.STORAGE),
            } })) }));
          if (draw.sampledTexture) {
            const source = draw.sampledTexture;
            // Resident texture: same dimensions reuse the GPU object with a
            // fresh upload every submit; a size change replaces it. The queue
            // is ordered, so this write stays after prior passes that read it.
            let texture;
            if (residentTexture && residentTexture.width === source.width
                && residentTexture.height === source.height) {
              texture = residentTexture.texture;
            } else {
              texture = device.createTexture({ size: [source.width, source.height], format: 'rgba8unorm',
                usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
              if (residentTexture) { try { residentTexture.texture.destroy(); } catch {} }
              residentTexture = { width: source.width, height: source.height, texture };
            }
            device.queue.writeTexture({ texture }, source.data,
              { bytesPerRow: source.width * 4 }, [source.width, source.height]);
            for (const group of [1, 2]) pass.setBindGroup(group, device.createBindGroup({
              layout: draw.program.pipeline.getBindGroupLayout(group), entries: [] }));
            pass.setBindGroup(3, device.createBindGroup({ layout: draw.program.pipeline.getBindGroupLayout(3),
              entries: [{ binding: 0, resource: texture.createView() },
                { binding: 1, resource: samplerFor(source.sampler) }] }));
          }
          pass.drawIndexed(draw.indexCount);
        }
        pass.end();
        const bytesPerRow = align(target.width * 4, 256);
        readback = device.createBuffer({ size: bytesPerRow * target.height,
          usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
        buffers.push(readback);
        encoder.copyTextureToBuffer({ texture: target.texture }, { buffer: readback, bytesPerRow },
          [target.width, target.height]);
        device.queue.submit([encoder.finish()]);
        await readback.mapAsync(GPUMapMode.READ);
        if (lost) throw new Error(lost);
        const mapped = new Uint8Array(readback.getMappedRange());
        pixels = new Uint8Array(target.width * target.height * 4);
        for (let y = 0; y < target.height; ++y)
          pixels.set(mapped.subarray(y * bytesPerRow, y * bytesPerRow + target.width * 4), y * target.width * 4);
      } catch (error) { failure = error; }
      finally {
        if (readback?.mapState === 'mapped') readback.unmap();
        for (const buffer of buffers) buffer.destroy();
        // Resident texture/samplers persist across submits by design; only
        // per-submit upload buffers and the readback are destroyed here.
        try {
          const error = await device.popErrorScope();
          if (error && !failure) failure = new Error(error.message);
        } finally { busy = false; }
      }
      if (failure) throw failure;
      return { width: target.width, height: target.height, pixels };
    },
    destroyTarget(id) {
      available();
      const target = lookup(targets, id, 'target');
      target.texture.destroy();
      target.depthTexture?.destroy();
      targets.delete(id);
    },
    destroyProgram(id) {
      available();
      lookup(programs, id, 'program');
      programs.delete(id);
    },
    pipelineCacheStats() {
      return Object.freeze({ hits: pipelineHits, misses: pipelineMisses,
        entries: pipelines.size, limit: pipelineLimit });
    },
    dispose() {
      if (busy) throw new Error('renderer operation in flight; await completion');
      for (const target of targets.values()) {
        target.texture.destroy();
        target.depthTexture?.destroy();
      }
      targets.clear(); programs.clear(); pipelines.clear();
      if (residentTexture) { try { residentTexture.texture.destroy(); } catch {} residentTexture = null; }
      samplers.clear(); disposed = true;
    },
  });
}
