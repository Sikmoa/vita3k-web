// Narrow WebGPU command consumer for the GXM backend bring-up.
// NOT a GXP translator or a replacement for SceGxm's validation/state machine.
// Input: translated WGSL + snapshotted vertex/index/uniform data. Output: tightly
// packed RGBA8 after GPU completion, suitable for the existing display bridge.
// Supported initially: one interleaved stream, triangle-list, RGBA8, no depth,
// blending, textures or MSAA. Unsupported state must be rejected by the producer.

const formats = Object.freeze({ float32: [4, 4], float32x2: [8, 4],
  float32x3: [12, 4], float32x4: [16, 4], unorm8x4: [4, 1] });
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
    createTarget(width, height) {
      available();
      integer(width, 1, device.limits.maxTextureDimension2D, 'width');
      integer(height, 1, device.limits.maxTextureDimension2D, 'height');
      if (align(width * 4, 256) * height > device.limits.maxBufferSize)
        throw new RangeError('render target readback exceeds maxBufferSize');
      const texture = device.createTexture({ size: [width, height], format: 'rgba8unorm',
        usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
      const id = nextId++;
      targets.set(id, { width, height, texture });
      return id;
    },

    // WGSL must eventually come from real GXP conversion. The smoke test uses
    // explicit test WGSL and deliberately does not claim guest shader execution.
    async createProgram({ vertexWGSL, fragmentWGSL, stride, attributes, uniformSize = 0 }) {
      available();
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
        pipeline = await device.createRenderPipelineAsync({ layout: 'auto',
          vertex: { module: vertex, entryPoint: 'main', buffers: [{ arrayStride: stride, attributes: layout }] },
          fragment: { module: fragment, entryPoint: 'main', targets: [{ format: 'rgba8unorm' }] },
          primitive: { topology: 'triangle-list', cullMode: 'none' },
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
      const id = nextId++;
      programs.set(id, { pipeline, stride, uniformSize });
      return id;
    },

    // Exclusive, completion-safe submission. All input views are copied before
    // the first await: Wasm may grow memory or overwrite its buffers afterwards.
    // The producer must await this Promise before signaling GXM sync objects,
    // running a display-queue callback, or releasing/reusing the target.
    async submit(targetId, draws, { clearColor = [0, 0, 0, 1] } = {}) {
      available();
      const target = lookup(targets, targetId, 'target');
      if (!Array.isArray(clearColor) || clearColor.length !== 4
          || clearColor.some(value => !Number.isFinite(value) || value < 0 || value > 1))
        throw new RangeError('clear color must contain four normalized components');
      const snapshots = draws.map(draw => {
        const program = lookup(programs, draw.program, 'program');
        const vertices = bytes(draw.vertices), indices = bytes(draw.indices);
        const indexSize = draw.indexFormat === 'uint16' ? 2 : draw.indexFormat === 'uint32' ? 4 : 0;
        if (!indexSize) throw new Error('unsupported index format');
        if (!vertices.length || vertices.length % program.stride)
          throw new RangeError('partial or empty vertex stream');
        if (!indices.length || indices.length % (indexSize * 3))
          throw new RangeError('partial or empty triangle list');
        const vertexCount = vertices.length / program.stride;
        const view = new DataView(indices.buffer);
        for (let offset = 0; offset < indices.length; offset += indexSize) {
          const index = indexSize === 2 ? view.getUint16(offset, true) : view.getUint32(offset, true);
          if (index >= vertexCount) throw new RangeError('index exceeds vertex stream');
        }
        const uniforms = draw.uniforms === undefined ? new Uint8Array() : bytes(draw.uniforms);
        if (uniforms.length !== program.uniformSize) throw new RangeError('uniform size mismatch');
        for (const data of [vertices, indices, uniforms])
          if (align(data.length, 4) > device.limits.maxBufferSize)
            throw new RangeError('upload exceeds maxBufferSize');
        return { program, vertices, indices, uniforms, indexFormat: draw.indexFormat,
          indexCount: indices.length / indexSize };
      });
      busy = true;
      device.pushErrorScope('validation');
      const buffers = [];
      let readback, pixels, failure;
      try {
        const upload = (data, usage) => {
          const padded = new Uint8Array(align(data.length, 4));
          padded.set(data);
          const buffer = device.createBuffer({ size: padded.length, usage: usage | GPUBufferUsage.COPY_DST });
          buffers.push(buffer);
          device.queue.writeBuffer(buffer, 0, padded);
          return buffer;
        };
        const encoder = device.createCommandEncoder();
        const pass = encoder.beginRenderPass({ colorAttachments: [{ view: target.texture.createView(),
          loadOp: 'clear', storeOp: 'store', clearValue: clearColor }] });
        for (const draw of snapshots) {
          pass.setPipeline(draw.program.pipeline);
          pass.setVertexBuffer(0, upload(draw.vertices, GPUBufferUsage.VERTEX));
          pass.setIndexBuffer(upload(draw.indices, GPUBufferUsage.INDEX), draw.indexFormat);
          if (draw.uniforms.length) {
            const buffer = upload(draw.uniforms, GPUBufferUsage.UNIFORM);
            pass.setBindGroup(0, device.createBindGroup({ layout: draw.program.pipeline.getBindGroupLayout(0),
              entries: [{ binding: 0, resource: { buffer } }] }));
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
      lookup(targets, id, 'target').texture.destroy();
      targets.delete(id);
    },
    destroyProgram(id) {
      available();
      lookup(programs, id, 'program');
      programs.delete(id);
    },
    dispose() {
      if (busy) throw new Error('renderer operation in flight; await completion');
      for (const target of targets.values()) target.texture.destroy();
      targets.clear(); programs.clear(); disposed = true;
    },
  });
}
