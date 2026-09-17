// Real GPU validation, not a mock. Test WGSL is NOT translated guest GXP.
// PLAYWRIGHT_MODULE_URL=file://$PWD/build/playwright/node_modules/playwright/index.mjs \
//   node browser/tests/gxm_webgpu_smoke.mjs
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { resolve, sep } from 'node:path';
import assert from 'node:assert/strict';
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE_URL || 'playwright');
const root = resolve('browser/web');
const server = createServer(async (req, res) => {
  try {
    const path = new URL(req.url, 'http://localhost').pathname;
    if (path === '/') { res.end('<!doctype html><title>GXM WebGPU test</title>'); return; }
    const file = resolve(root, `.${path}`);
    if (!file.startsWith(root + sep)) throw new Error('bad path');
    res.setHeader('Content-Type', 'text/javascript');
    res.end(await readFile(file));
  } catch { res.writeHead(404); res.end(); }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
let browser;
const timeout = setTimeout(() => { console.error('WebGPU test timed out'); process.exit(1); }, 45000);
try {
  browser = await chromium.launch({ headless: true,
    args: ['--enable-unsafe-webgpu', '--use-angle=swiftshader', '--enable-features=Vulkan', '--disable-vulkan-surface'],
    ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}),
  });
  const page = await browser.newPage();
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
  const result = await page.evaluate(async () => {
    const { createWebGPUBridge } = await import('./webgpu.js');
    const { snapshotGuestBytes } = await import('./gxm_renderer.js');
    const bridge = createWebGPUBridge();
    const device = await bridge.requestDevice();
    if (!device) throw new Error('WebGPU unavailable (not a passing skip)');
    const errors = [];
    device.addEventListener('uncapturederror', event => errors.push(event.error.message));
    let checks = 0;
    const check = (condition, message) => { if (!condition) throw new Error(message); ++checks; };
    const rejects = async (operation, message) => {
      let rejected = false;
      try { await operation(); } catch { rejected = true; }
      check(rejected, message);
    };
    const heap = new Uint8Array([1, 2, 3, 4]);
    const copy = snapshotGuestBytes(heap, 1n, 2);
    heap[1] = 99;
    check(copy.join() === '2,3', 'Memory64 upload must be an owned snapshot');
    await rejects(() => snapshotGuestBytes(heap, 2n ** 60n, 1), 'unsafe host pointer rejected');
    await rejects(() => snapshotGuestBytes(heap, 3, 2), 'out of bounds upload rejected');

    // Independent 2x2 upload/copy/map sanity check; catches row alignment mistakes.
    const texture = device.createTexture({ size: [2, 2], format: 'rgba8unorm',
      usage: GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST });
    const expected = new Uint8Array([255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255]);
    device.queue.writeTexture({ texture }, expected, { bytesPerRow: 8 }, [2,2]);
    const rb = device.createBuffer({ size: 512, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const encoder = device.createCommandEncoder();
    encoder.copyTextureToBuffer({ texture }, { buffer: rb, bytesPerRow: 256 }, [2,2]);
    device.queue.submit([encoder.finish()]);
    await rb.mapAsync(GPUMapMode.READ);
    const mapped = new Uint8Array(rb.getMappedRange());
    check(mapped.slice(0,8).join() === expected.slice(0,8).join(), 'upload row zero');
    check(mapped.slice(256,264).join() === expected.slice(8).join(), 'upload row one');
    rb.unmap(); rb.destroy(); texture.destroy();

    const renderer = bridge.renderer;
    const target = renderer.createTarget(65, 33); // deliberately not 256-byte aligned
    const definition = { stride: 8,
      attributes: [{ shaderLocation: 0, offset: 0, format: 'float32x2' }], uniformSize: 16,
      vertexWGSL: '@vertex fn main(@location(0) p: vec2f) -> @builtin(position) vec4f { return vec4f(p, 0, 1); }',
      fragmentWGSL: '@group(0) @binding(0) var<uniform> color: vec4f; @fragment fn main() -> @location(0) vec4f { return color; }',
    };
    const program = await renderer.createProgram(definition);
    const reused = await renderer.createProgram({ ...definition, attributes: definition.attributes.map(a => ({ ...a })) });
    check(reused !== program && renderer.pipelineCacheStats().hits === 1
      && renderer.pipelineCacheStats().entries === 1, 'equivalent state shares pipeline but not program handle');
    const vertices = new Float32Array([-1,-1, 3,-1, -1,3]);
    const indices = new Uint16Array([0,1,2]); // six bytes: writeBuffer must pad to four
    const uniforms = new Float32Array([1,0,0,1]);
    const draw = { program, vertices, indices, uniforms, indexFormat: 'uint16' };
    const pending = renderer.submit(target, [draw]);
    uniforms[0] = 0; uniforms[1] = 1; vertices.fill(0);
    await rejects(() => renderer.destroyTarget(target), 'in-flight target cannot be destroyed');
    const red = await pending;
    check(red.pixels.length === 65 * 33 * 4, 'readback is tightly packed');
    check(red.pixels.every((v,i) => v === [255,0,0,255][i%4]), 'indexed triangle uses snapshotted data');
    vertices.set([-1,-1, 3,-1, -1,3]);
    const green = await renderer.submit(target, [draw]);
    check(green.pixels.every((v,i) => v === [0,255,0,255][i%4]), 'uniform changes affect pixels');
    const green32 = await renderer.submit(target, [{ ...draw, indices: new Uint32Array([0,1,2]), indexFormat: 'uint32' }]);
    check(green32.pixels.every((v,i) => v === [0,255,0,255][i%4]), '32-bit index draw');
    renderer.destroyProgram(reused);
    const retained = await renderer.submit(target, [draw]);
    check(retained.pixels.every((v,i) => v === [0,255,0,255][i%4]), 'destroying shared handle preserves original program');
    const blueProgram = await renderer.createProgram({ ...definition,
      fragmentWGSL: '@fragment fn main() -> @location(0) vec4f { return vec4f(0, 0, 1, 1); }' });
    const blue = await renderer.submit(target, [{ ...draw, program: blueProgram }]);
    check(blue.pixels.every((v,i) => v === [0,0,255,255][i%4])
      && renderer.pipelineCacheStats().misses === 2, 'fragment shader change misses cache and changes pixels');
    renderer.destroyProgram(blueProgram);
    const paddedProgram = await renderer.createProgram({ ...definition, stride: 12,
      attributes: [{ shaderLocation: 0, offset: 4, format: 'float32x2' }] });
    const paddedDraw = { ...draw, program: paddedProgram,
      vertices: new Float32Array([99,-1,-1, 99,3,-1, 99,-1,3]) };
    const padded = await renderer.submit(target, [paddedDraw]);
    check(padded.pixels.every((v,i) => v === [0,255,0,255][i%4])
      && renderer.pipelineCacheStats().misses === 3, 'changed vertex stride and offsets miss cache');
    renderer.destroyProgram(paddedProgram);
    const explicitProgram = await renderer.createProgram({ ...definition, uniformSize: 0,
      bufferBindings: [{ binding: 0, size: 32, type: 'uniform', visibility: GPUShaderStage.FRAGMENT }] });
    const explicit = await renderer.submit(target, [{ ...draw, program: explicitProgram, uniforms: undefined,
      buffers: { 0: new Float32Array([1,0,0,1, 0,0,0,0]) } }]);
    check(explicit.pixels.every((v,i) => v === [255,0,0,255][i%4])
      && renderer.pipelineCacheStats().misses === 4, 'binding size and visibility miss cache');
    renderer.destroyProgram(explicitProgram);
    await rejects(() => renderer.createProgram({ ...definition, blend: {} }), 'unsupported pipeline state is not silently defaulted');
    vertices.fill(0);
    const black = await renderer.submit(target, [draw]);
    check(black.pixels.every((v,i) => v === [0,0,0,255][i%4]), 'vertex changes affect coverage');
    await rejects(() => renderer.submit(target, [{ ...draw, indices: new Uint16Array([0,1,3]) }]), 'invalid index rejected');
    await rejects(() => renderer.submit(target, [{ ...draw, indexFormat: 'uint8' }]), 'unsupported index format rejected');
    await rejects(() => renderer.submit(target, [{ ...draw, uniforms: new Uint8Array(4) }]), 'wrong uniform size rejected');
    await rejects(() => renderer.createProgram({ stride: 8, attributes: [{ shaderLocation: 0, offset: 0, format: 'float32x2' }],
      vertexWGSL: 'invalid wgsl', fragmentWGSL: 'invalid wgsl' }), 'bad shader rejected');
    check(renderer.pipelineCacheStats().entries === 4, 'invalid shader never populates pipeline cache');
    const retry = await renderer.createProgram(definition);
    check(renderer.pipelineCacheStats().hits === 2, 'successful pipeline survives unrelated failed creation');
    renderer.destroyProgram(retry);

    // Fragment texture unit zero uses the real translator's group-3 ABI.
    // Test WGSL isolates the consumer; guest texture packet wiring is separate.
    const texturedDefinition = { ...definition, uniformSize: 0, fragmentTexture: true,
      fragmentWGSL: `@group(3) @binding(0) var image: texture_2d<f32>;
        @group(3) @binding(1) var imageSampler: sampler;
        @fragment fn main(@builtin(position) p: vec4f) -> @location(0) vec4f {
          return textureSample(image, imageSampler, p.xy / vec2f(65.0, 33.0));
        }` };
    const textured = await renderer.createProgram(texturedDefinition);
    const texels = new Uint8Array([255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255]);
    const textureDraw = { program: textured, vertices: new Float32Array([-1,-1, 3,-1, -1,3]),
      indices, indexFormat: 'uint16', fragmentTexture: { width: 2, height: 2, pixels: texels } };
    const texturePending = renderer.submit(target, [textureDraw]);
    texels.fill(0);
    const texturedResult = await texturePending;
    const pixel = (image, x, y) => image.pixels.slice((y * 65 + x) * 4, (y * 65 + x) * 4 + 4).join();
    check(pixel(texturedResult, 8, 4) === '255,0,0,255'
      && pixel(texturedResult, 56, 4) === '0,255,0,255'
      && pixel(texturedResult, 8, 28) === '0,0,255,255'
      && pixel(texturedResult, 56, 28) === '255,255,255,255', 'sampled quadrants use owned texture snapshot');
    const changedTexture = await renderer.submit(target, [textureDraw]);
    check(changedTexture.pixels.every(v => v === 0), 'later draw observes modified texels');
    const textureReuse = await renderer.createProgram(texturedDefinition);
    check(renderer.pipelineCacheStats().hits === 3, 'texture pipeline reused independently of texel bytes');
    renderer.destroyProgram(textureReuse);
    await rejects(() => renderer.submit(target, [{ ...textureDraw, fragmentTexture: undefined }]), 'missing texture rejected');
    await rejects(() => renderer.submit(target, [{ ...textureDraw, fragmentTexture: {
      ...textureDraw.fragmentTexture, pixels: new Uint8Array(4) } }]), 'partial texture rejected');
    await rejects(() => renderer.submit(target, [{ ...textureDraw, fragmentTexture: {
      ...textureDraw.fragmentTexture, sampler: { compare: 'less' } } }]), 'unsupported sampler rejected');
    await rejects(() => renderer.submit(target, [{ ...draw, fragmentTexture: textureDraw.fragmentTexture }]),
      'texture cannot be silently ignored by untextured program');
    // The resource layout must participate in the pipeline cache key even if
    // this particular shader does not use its additional texture bindings.
    const layoutMisses = renderer.pipelineCacheStats().misses;
    const unusedTexture = await renderer.createProgram({ ...definition, fragmentTexture: true });
    check(renderer.pipelineCacheStats().misses === layoutMisses + 1, 'texture layout changes pipeline key');
    renderer.destroyProgram(unusedTexture);
    renderer.destroyProgram(textured);
    renderer.destroyProgram(program); renderer.destroyTarget(target);
    await rejects(() => renderer.submit(target, []), 'destroyed target rejected');
    renderer.dispose();
    check(renderer.pipelineCacheStats().entries === 0, 'dispose releases pipeline cache');
    await rejects(() => renderer.createTarget(1,1), 'disposed renderer rejected');
    check(errors.length === 0, `uncaptured GPU errors: ${errors.join('; ')}`);
    // This queue test needs no shader compiler/assets: decoding a malformed
    // packet fails before device/compiler acquisition. Later operations must
    // inherit that failure instead of reporting a completed fence or fill.
    const guest = await import('./gxm_hle_bridge.js');
    const badDraw = guest.drawGuestSurface(new Uint8Array(4), new Uint8Array(4), 1, 1);
    const laterFence = guest.finishGuestQueue();
    const laterFill = guest.fillGuestSurface(0xff000000, 1, 1);
    const failedQueue = await Promise.allSettled([badDraw, laterFence, laterFill]);
    check(failedQueue.every(result => result.status === 'rejected'), 'rejected draw poisons queued fence and fill');
    check(failedQueue[0].reason === failedQueue[1].reason && failedQueue[1].reason === failedQueue[2].reason,
      'queue preserves original failure diagnostic');
    device.destroy();
    return { checks, backend: 'WebGPU', translatedGuestShader: false, pipelineCache: true };
  });
  assert.equal(result.checks, 37);
  console.log(JSON.stringify(result));
} finally {
  clearTimeout(timeout);
  await browser?.close();
  server.close();
}
