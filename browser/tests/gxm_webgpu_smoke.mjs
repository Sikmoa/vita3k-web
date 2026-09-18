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
    await rejects(() => renderer.createProgram({ ...definition, cullMode: 'back' }), 'unsupported pipeline state is not silently defaulted');
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
    // Per-draw viewports map NDC to a target sub-rect; omitted viewports keep
    // the default full-target mapping. Outside-viewport texels are preserved.
    const vpTarget = renderer.createTarget(64, 32);
    const vpProgram = await renderer.createProgram(definition);
    const tri = { program: vpProgram, vertices: new Float32Array([-1,-1, 3,-1, -1,3]),
      indices: new Uint16Array([0,1,2]), uniforms: new Float32Array([1,0,0,1]), indexFormat: 'uint16' };
    const left = await renderer.submit(vpTarget, [{ ...tri, viewport: { x: 0, y: 0, width: 32, height: 32 } }]);
    const vpPixel = (image, x, y) => image.pixels.slice((y * 64 + x) * 4, (y * 64 + x) * 4 + 4).join();
    check(vpPixel(left, 8, 8) === '255,0,0,255' && vpPixel(left, 56, 8) === '0,0,0,255',
      'viewport clips draw to rect');
    const right = await renderer.submit(vpTarget, [{ ...tri, viewport: { x: 32, y: 0, width: 32, height: 32 } }],
      { initialPixels: left.pixels });
    check(right.pixels.every((v, i) => v === [255,0,0,255][i % 4]),
      'outside-viewport preserves initial surface');
    await rejects(() => renderer.submit(vpTarget, [{ ...tri, viewport: { x: 0, y: 0, width: -1, height: 32 } }]),
      'negative viewport extent rejected');
    await rejects(() => renderer.submit(vpTarget, [{ ...tri, viewport: { x: NaN, y: 0, width: 32, height: 32 } }]),
      'non-finite viewport rejected');
    renderer.destroyProgram(vpProgram); renderer.destroyTarget(vpTarget);

    // Fixed-function blend state. Real fragments and real blending: expected
    // values come from the guest factors, checked with a one-step unorm
    // tolerance because the blend unit's rounding mode is unspecified.
    const overlay = new Float32Array([-1,-1, 3,-1, -1,3]);
    const blendTarget = renderer.createTarget(4, 4);
    const opaqueGreen = new Uint8Array(4 * 4 * 4);
    for (let i = 0; i < opaqueGreen.length; i += 4) opaqueGreen.set([0, 255, 0, 255], i);
    const srcAlphaBlend = { color: { operation: 'add', srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha' },
      alpha: { operation: 'add', srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha' } };
    const blendProgram = await renderer.createProgram({ ...definition, writeMask: 0xf, blend: srcAlphaBlend });
    const blendDraw = { program: blendProgram, vertices: overlay, indices,
      uniforms: new Float32Array([1,0,0,0.25]), indexFormat: 'uint16' };
    const blent = await renderer.submit(blendTarget, [blendDraw], { initialPixels: opaqueGreen });
    const at2 = (image, x, y) => image.pixels.slice((y * 4 + x) * 4, (y * 4 + x) * 4 + 4);
    const matches = (actual, expected, tolerance) => expected.every((v, i) => Math.abs(actual[i] - v) <= tolerance);
    // 0.25 * red + 0.75 * green, alpha 0.25 * 0.25 + 1 * 0.75.
    check(matches(at2(blent, 1, 1), [64, 191, 0, 207], 1), `src-alpha blend writes blended pixels: ${at2(blent, 1, 1)}`);
    const blendHits = renderer.pipelineCacheStats().hits;
    const blendReuse = await renderer.createProgram({ ...definition, writeMask: 0xf, blend: srcAlphaBlend });
    check(renderer.pipelineCacheStats().hits === blendHits + 1, 'identical blend state reuses the pipeline');
    const missesBefore = renderer.pipelineCacheStats().misses;
    await renderer.createProgram({ ...definition, writeMask: 0xf, blend: { ...srcAlphaBlend,
      color: { operation: 'max', srcFactor: 'one', dstFactor: 'one' } } });
    check(renderer.pipelineCacheStats().misses === missesBefore + 1, 'blend state participates in the pipeline key');
    const maxProgram = await renderer.createProgram({ ...definition, writeMask: 0xf,
      blend: { color: { operation: 'max', srcFactor: 'one', dstFactor: 'one' },
        alpha: { operation: 'max', srcFactor: 'one', dstFactor: 'one' } } });
    const maxed = await renderer.submit(blendTarget, [{ ...blendDraw, program: maxProgram,
      uniforms: new Float32Array([1,0,0,1]) }], { initialPixels: opaqueGreen });
    check(at2(maxed, 1, 1).join() === '255,255,0,255', 'max blend keeps the larger source channel');
    renderer.destroyProgram(maxProgram); renderer.destroyProgram(blendReuse);
    // The guest color mask is the pipeline write mask, and it applies with
    // blending disabled as well.
    const noneProgram = await renderer.createProgram({ ...definition, writeMask: 0 });
    const unmasked = await renderer.submit(blendTarget, [{ ...blendDraw, program: noneProgram,
      indexFormat: 'uint16' }], { initialPixels: opaqueGreen });
    check(at2(unmasked, 1, 1).join() === '0,255,0,255', 'empty write mask leaves the destination untouched');
    renderer.destroyProgram(noneProgram);
    const blueOnlyProgram = await renderer.createProgram({ ...definition, writeMask: 0x4 });
    const masked = await renderer.submit(blendTarget, [{ ...blendDraw, program: blueOnlyProgram,
      uniforms: new Float32Array([0,0,1,1]) }], { initialPixels: opaqueGreen });
    check(at2(masked, 1, 1).join() === '0,255,255,255', 'write mask leaves unselected channels at their destination value');
    renderer.destroyProgram(blueOnlyProgram);
    const replaced = await renderer.submit(blendTarget, [{ ...blendDraw, program,
      uniforms: new Float32Array([0,0,1,1]) }], { initialPixels: opaqueGreen });
    check(at2(replaced, 1, 1).join() === '0,0,255,255', 'no blend state replaces the destination');
    await rejects(() => renderer.createProgram({ ...definition, blend: { color: srcAlphaBlend.color } }),
      'blend descriptor needs both components');
    await rejects(() => renderer.createProgram({ ...definition, blend: { ...srcAlphaBlend,
      color: { operation: 'multiply', srcFactor: 'one', dstFactor: 'one' } } }), 'unsupported blend operation rejected');
    await rejects(() => renderer.createProgram({ ...definition, blend: { ...srcAlphaBlend,
      color: { operation: 'add', srcFactor: 'one', dstFactor: 'src-alpha-saturate' } } }),
      'saturate factor is invalid as a destination factor');
    await rejects(() => renderer.createProgram({ ...definition, writeMask: 0x10 }), 'write mask out of range rejected');
    renderer.destroyProgram(blendProgram); renderer.destroyTarget(blendTarget);

    // Depth-stencil state. Depth is clear-on-load and never read back, so all
    // checks stay inside one submission (one render pass, one attachment).
    const depthDefinition = { ...definition, stride: 12,
      attributes: [{ shaderLocation: 0, offset: 0, format: 'float32x3' }],
      vertexWGSL: '@vertex fn main(@location(0) p: vec3f) -> @builtin(position) vec4f { return vec4f(p, 1); }' };
    const depthState = (depthCompare, depthWriteEnabled) => ({ format: 'depth24plus-stencil8',
      depthCompare, depthWriteEnabled });
    const lessWrite = await renderer.createProgram({ ...depthDefinition,
      depthStencil: depthState('less-equal', true) });
    const lessNoWrite = await renderer.createProgram({ ...depthDefinition,
      depthStencil: depthState('less-equal', false) });
    const greaterWrite = await renderer.createProgram({ ...depthDefinition,
      depthStencil: depthState('greater', true) });
    const depthTarget = renderer.createTarget(4, 4, { depthFormat: 'depth24plus-stencil8' });
    const depthAttachment = { format: 'depth24plus-stencil8', stencil: true, clearValue: 1 };
    // WebGPU clip space keeps z in [0, 1] (unlike OpenGL's [-1, 1]), so the
    // vertex z is the depth written by the pipeline: 0.25 near, 0.5 mid,
    // 0.75 far, and a negative z would be clipped away entirely.
    const quad = (program, z, color) => ({
      program, vertices: new Float32Array([-1,-1,z, 3,-1,z, -1,3,z]), indices,
      uniforms: new Float32Array(color), indexFormat: 'uint16' });
    const farRed = quad(lessWrite, 0.75, [1,0,0,1]);
    const nearGreen = quad(lessWrite, 0.25, [0,1,0,1]);
    const midBlue = quad(lessWrite, 0.5, [0,0,1,1]);
    const farOnly = await renderer.submit(depthTarget, [farRed], { depth: depthAttachment });
    check(at2(farOnly, 1, 1).join() === '255,0,0,255', 'farther quad passes against a cleared depth buffer');
    const nearWins = await renderer.submit(depthTarget, [farRed, nearGreen], { depth: depthAttachment });
    check(at2(nearWins, 1, 1).join() === '0,255,0,255', 'nearer quad overwrites the farther one');
    const farRejected = await renderer.submit(depthTarget, [nearGreen, farRed], { depth: depthAttachment });
    check(at2(farRejected, 1, 1).join() === '0,255,0,255', 'farther quad fails the depth test');
    // left-to-right through the pass: far writes 0.75, near passes without
    // writing, mid compares against 0.75 and wins. If the middle draw had
    // written depth, mid would be rejected (0.5 <= 0.25 is false).
    const noWrite = await renderer.submit(depthTarget, [farRed, quad(lessNoWrite, 0.25, [0,1,0,1]),
      midBlue], { depth: depthAttachment });
    check(at2(noWrite, 1, 1).join() === '0,0,255,255', 'depthWriteEnabled false keeps the earlier depth value');
    const writeControl = await renderer.submit(depthTarget, [farRed, nearGreen, midBlue], { depth: depthAttachment });
    check(at2(writeControl, 1, 1).join() === '0,255,0,255', 'depthWriteEnabled true rejects the later mid quad');
    // greater + clear 0: 0.5 passes, then 0.25 fails against it.
    const greaterTarget = renderer.createTarget(4, 4, { depthFormat: 'depth24plus-stencil8' });
    const greater = await renderer.submit(greaterTarget, [quad(greaterWrite, 0.5, [0,0,1,1]),
      quad(greaterWrite, 0.25, [0,1,0,1])], { depth: { ...depthAttachment, clearValue: 0 } });
    check(at2(greater, 1, 1).join() === '0,0,255,255', 'greater compare rejects the nearer subsequent quad');
    const greaterControl = await renderer.submit(greaterTarget, [quad(greaterWrite, 0.25, [0,1,0,1])],
      { depth: { ...depthAttachment, clearValue: 0 } });
    check(at2(greaterControl, 1, 1).join() === '0,255,0,255', 'greater compare accepts a depth above the clear value');
    renderer.destroyProgram(greaterWrite); renderer.destroyTarget(greaterTarget);
    // The exact D16 representation is a first-class target format.
    const unormTarget = renderer.createTarget(4, 4, { depthFormat: 'depth16unorm' });
    const unormProgram = await renderer.createProgram({ ...depthDefinition,
      depthStencil: { format: 'depth16unorm', depthCompare: 'less-equal', depthWriteEnabled: true } });
    const unorm = await renderer.submit(unormTarget, [quad(unormProgram, 0.75, [1,0,0,1]),
      quad(unormProgram, 0.25, [0,1,0,1])], { depth: { format: 'depth16unorm', stencil: false, clearValue: 1 } });
    check(at2(unorm, 1, 1).join() === '0,255,0,255', 'depth16unorm target tests and writes depth');
    const depthHits = renderer.pipelineCacheStats().hits;
    const depthReuse = await renderer.createProgram({ ...depthDefinition,
      depthStencil: depthState('less-equal', true) });
    check(renderer.pipelineCacheStats().hits === depthHits + 1, 'identical depth state reuses the pipeline');
    renderer.destroyProgram(depthReuse); renderer.destroyProgram(unormProgram);
    renderer.destroyTarget(unormTarget);
    await rejects(() => renderer.submit(target, [quad(lessWrite, 0.25, [0,1,0,1])], { depth: depthAttachment }),
      'depth pipeline cannot use a target without a depth attachment');
    await rejects(() => renderer.submit(depthTarget, [farRed], {}), 'depth target needs explicit depth state');
    await rejects(() => renderer.submit(depthTarget, [farRed],
      { depth: { ...depthAttachment, format: 'depth32float' } }), 'depth attachment format must match the target');
    await rejects(() => renderer.submit(depthTarget, [farRed],
      { depth: { ...depthAttachment, clearValue: 2 } }), 'depth clear value out of range rejected');
    await rejects(() => renderer.createProgram({ ...depthDefinition,
      depthStencil: depthState('less-or-equal', true) }), 'unsupported depth compare rejected');
    await rejects(() => renderer.createProgram({ ...depthDefinition,
      depthStencil: { format: 'depth16unorm', depthCompare: 'less', depthWriteEnabled: 'yes' } }),
      'non-boolean depth write flag rejected');
    await rejects(() => renderer.createProgram({ ...depthDefinition,
      depthStencil: { ...depthState('less', true), depthBias: 1 } }), 'unknown depth-stencil field rejected');
    renderer.destroyProgram(lessWrite); renderer.destroyProgram(lessNoWrite); renderer.destroyTarget(depthTarget);

    // Vertex attribute formats. Real fragments prove the fetched values, not
    // just that the layout was accepted: every vertex carries the same
    // attribute payload, so the interpolated varying is constant, and a
    // buffered full-target triangle rasterizes it across the whole 4x4 target.
    const formatTarget = renderer.createTarget(4, 4);
    const formatProgram = (stride, offset, format, inputType, attributeExpression) =>
      renderer.createProgram({ stride, attributes: [{ shaderLocation: 0, offset, format }],
        uniformSize: 0, bufferBindings: [],
        vertexWGSL: `struct Out { @builtin(position) position: vec4f, @location(0) color: vec4f };
          @vertex fn main(@builtin(vertex_index) i: u32, @location(0) v: ${inputType}) -> Out {
            var corners = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
            var out: Out; out.position = vec4f(corners[i], 0.0, 1.0);
            out.color = ${attributeExpression}; return out; }`,
        fragmentWGSL: '@fragment fn main(@location(0) v: vec4f) -> @location(0) vec4f { return v; }' });
    const formatPixel = async (formatProgramId, vertex) => at2(await renderer.submit(formatTarget,
      [{ program: formatProgramId, indexFormat: 'uint16', indices,
        vertices: new Uint8Array([...vertex, ...vertex, ...vertex]) }]), 1, 1);
    const tight = await formatProgram(4, 0, 'unorm8x4', 'vec4f', 'v');
    check((await formatPixel(tight, new Uint8Array([255, 128, 0, 64]))).join() === '255,128,0,64',
      'unorm8x4 fetches normalized bytes');
    // Limbo's observed shape: the attribute starts at offset 16 of a 20-byte
    // stride and fills it exactly; the leading 16 bytes must not leak in.
    const stridedVertex = new Uint8Array(20).fill(0x99);
    stridedVertex.set([10, 200, 30, 40], 16);
    const strided = await formatProgram(20, 16, 'unorm8x4', 'vec4f', 'v');
    check((await formatPixel(strided, stridedVertex)).join() === '10,200,30,40',
      'unorm8x4 reads only its own bytes inside a 20-byte stride');
    // 16-bit normalized components at offset 4 of an 8-byte stride. 0x8080 and
    // 0x4040 are exactly 128 and 64 in an 8-bit target.
    const wide = await formatProgram(8, 4, 'unorm16x2', 'vec2f', 'vec4f(v, 0.0, 1.0)');
    check((await formatPixel(wide, new Uint8Array([9,9,9,9, 0x80,0x80, 0x40,0x40]))).join() === '128,64,0,255',
      'unorm16x2 fetches 16-bit normalized components');
    // Half-float components land in an f32 shader input with no conversion.
    const halves = await formatProgram(8, 0, 'float16x4', 'vec4f', 'v');
    check(matches(await formatPixel(halves,
      new Uint8Array([0,0x3c, 0,0x38, 0,0x34, 0,0x3a])), [255, 128, 64, 191], 1),
      'float16x4 feeds a vec4<f32> input');
    // Signed normalized bytes are only visible through a remap: 127 -> +1,
    // 0 -> 0, -128 -> -1 mapped back into [0, 1].
    const signed = await formatProgram(4, 0, 'snorm8x4', 'vec4f', 'v * vec4f(0.5) + vec4f(0.5)');
    check(matches(await formatPixel(signed, new Uint8Array([127, 0, 128, 0])), [255, 128, 0, 128], 1),
      'snorm8x4 decodes signed normalized bytes');
    const formatHits = renderer.pipelineCacheStats().hits;
    await formatProgram(20, 16, 'unorm8x4', 'vec4f', 'v');
    check(renderer.pipelineCacheStats().hits === formatHits + 1, 'identical attribute layout reuses the pipeline');
    // Every part of the layout that WebGPU cannot express exactly must reject
    // instead of reinterpreting or truncating the guest bytes.
    await rejects(() => formatProgram(4, 0, 'unorm8x1', 'vec4f', 'v'), 'no 1-component 8-bit vertex format');
    await rejects(() => formatProgram(4, 0, 'unorm8x3', 'vec4f', 'v'), 'no 3-component 8-bit vertex format');
    await rejects(() => formatProgram(6, 2, 'unorm8x4', 'vec4f', 'v'), 'unorm8x4 offset must be 4-aligned');
    await rejects(() => formatProgram(4, 1, 'unorm8x2', 'vec4f', 'v'), 'unorm8x2 offset must be 2-aligned');
    await rejects(() => formatProgram(4, 4, 'unorm16x2', 'vec2f', 'vec4f(v, 0.0, 1.0)'),
      'attribute element must fit inside the stride');
    // A format with fewer components than the shader input is legal: WebGPU
    // fills the missing components with (0, 0, 0, 1) instead of reading the
    // next attribute's bytes, which is why only x2/x4 8- and 16-bit formats
    // are needed to represent a guest x2/x4 fetch exactly.
    const filled = await formatProgram(8, 0, 'unorm16x2', 'vec4f', 'v');
    check((await formatPixel(filled, new Uint8Array([0x80,0x80, 0x40,0x40, 0,0,0,0]))).join()
      === '128,64,0,255', 'missing vertex components default to (0, 0, 0, 1)');
    await rejects(() => renderer.createProgram({ stride: 16, uniformSize: 0, bufferBindings: [],
      attributes: [{ shaderLocation: 0, offset: 0, format: 'float32x2' },
        { shaderLocation: 0, offset: 8, format: 'float32x2' }],
      vertexWGSL: 'invalid', fragmentWGSL: 'invalid' }), 'duplicate shader location rejected');
    for (const program of [tight, strided, wide, halves, signed]) renderer.destroyProgram(program);
    renderer.destroyTarget(formatTarget);

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
  assert.equal(result.checks, 81);
  console.log(JSON.stringify(result));
} finally {
  clearTimeout(timeout);
  await browser?.close();
  server.close();
}
