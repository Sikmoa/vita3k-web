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
    const program = await renderer.createProgram({ stride: 8,
      attributes: [{ shaderLocation: 0, offset: 0, format: 'float32x2' }], uniformSize: 16,
      vertexWGSL: '@vertex fn main(@location(0) p: vec2f) -> @builtin(position) vec4f { return vec4f(p, 0, 1); }',
      fragmentWGSL: '@group(0) @binding(0) var<uniform> color: vec4f; @fragment fn main() -> @location(0) vec4f { return color; }',
    });
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
    vertices.fill(0);
    const black = await renderer.submit(target, [draw]);
    check(black.pixels.every((v,i) => v === [0,0,0,255][i%4]), 'vertex changes affect coverage');
    await rejects(() => renderer.submit(target, [{ ...draw, indices: new Uint16Array([0,1,3]) }]), 'invalid index rejected');
    await rejects(() => renderer.submit(target, [{ ...draw, indexFormat: 'uint8' }]), 'unsupported index format rejected');
    await rejects(() => renderer.submit(target, [{ ...draw, uniforms: new Uint8Array(4) }]), 'wrong uniform size rejected');
    await rejects(() => renderer.createProgram({ stride: 8, attributes: [{ shaderLocation: 0, offset: 0, format: 'float32x2' }],
      vertexWGSL: 'invalid wgsl', fragmentWGSL: 'invalid wgsl' }), 'bad shader rejected');
    renderer.destroyProgram(program); renderer.destroyTarget(target);
    await rejects(() => renderer.submit(target, []), 'destroyed target rejected');
    renderer.dispose();
    await rejects(() => renderer.createTarget(1,1), 'disposed renderer rejected');
    check(errors.length === 0, `uncaptured GPU errors: ${errors.join('; ')}`);
    device.destroy();
    return { checks, backend: 'WebGPU', translatedGuestShader: false };
  });
  assert.ok(result.checks >= 18);
  console.log(JSON.stringify(result));
} finally {
  clearTimeout(timeout);
  await browser?.close();
  server.close();
}
