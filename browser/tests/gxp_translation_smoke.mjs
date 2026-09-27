// Translation runs IN Chromium, from public repo GXP bytes. No native compiler.
// The shader assets come from the built dist (GXM_RUNTIME_DIST, default
// build/web64/dist), served under /dist/.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { resolve, sep } from 'node:path';
import assert from 'node:assert/strict';
const root = resolve('.');
const dist = resolve(process.env.GXM_RUNTIME_DIST || 'build/web64/dist');
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE_URL || '../../build/playwright/node_modules/playwright/index.mjs');
const server = createServer(async (req, res) => {
  try {
    const path = new URL(req.url, 'http://localhost').pathname;
    if (path === '/') { res.end('<!doctype html><title>Browser GXP translation</title>'); return; }
    const base = path.startsWith('/dist/') ? dist : root;
    const file = resolve(base, `.${path.replace(/^\/dist\//, '/')}`);
    if (!file.startsWith(base+sep)) throw new Error('bad path');
    res.setHeader('Content-Type', file.endsWith('.wasm') ? 'application/wasm' : file.endsWith('.gxp') ? 'application/octet-stream' : 'text/javascript');
    res.end(await readFile(file));
  } catch { res.writeHead(404); res.end(); }
});
await new Promise(r => server.listen(0, '127.0.0.1', r));
let browser;
try {
  browser = await chromium.launch({ headless: true,
    // Keep Chromium shared memory in /dev/shm, not AppArmor-blocked repo TMPDIR.
    ignoreDefaultArgs: ['--disable-dev-shm-usage'],
    args: ['--enable-unsafe-webgpu', '--use-angle=swiftshader', '--enable-features=Vulkan', '--disable-vulkan-surface'],
    ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}),
  });
  const page = await browser.newPage();
  page.on('console', m => console.log('browser:', m.text()));
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
  const result = await page.evaluate(async () => {
    const { createGXPShaderAdapter } = await import('/browser/web/gxp_shader_adapter.js');
    const adapter = await createGXPShaderAdapter({
      compilerURL: '/dist/shaders/gxp_compiler.mjs',
      nagaURL: '/dist/shaders/naga.wasm',
      wasiShimURL: '/dist/shaders/wasi/index.js',
    });
    let checks = 0;
    const check = (ok, text) => { if (!ok) throw new Error(text); ++checks; };
    const shaders = {};
    const gpu = await navigator.gpu.requestAdapter();
    const device = await gpu.requestDevice();
    const errors = []; device.addEventListener('uncapturederror', e => errors.push(e.error.message));
    for (const name of ['color_v', 'color_f', 'texture_v', 'texture_f', 'texture_tint_f', 'clear_v', 'clear_f']) {
      const bytes = new Uint8Array(await (await fetch(`/tools/native-tool/src/shaders/${name}.gxp`)).arrayBuffer());
      const translated = await adapter.translate(bytes);
      check(new DataView(translated.spirv.buffer).getUint32(0,true) === 0x07230203, `${name} real SPIR-V`);
      const module = device.createShaderModule({ code: translated.wgsl });
      const info = await module.getCompilationInfo();
      check(!info.messages.some(m => m.type === 'error'), `${name}: ${info.messages.map(m=>m.message).join(';')}`);
      shaders[name] = module;
    }
    const invalid = new Uint8Array(156);
    let rejected = false;
    try { await adapter.translate(invalid); } catch { rejected = true; }
    check(rejected, 'bad GXP header rejected');

    // Real texture_v/texture_f pipeline, no substitute WGSL. Sample distinct
    // texels/alpha via guest UV attributes and change actual bound texture data.
    device.pushErrorScope('validation');
    const pipeline = await device.createRenderPipelineAsync({ layout: 'auto',
      vertex: { module: shaders.texture_v, entryPoint: 'main_vs', buffers: [{ arrayStride: 24,
        attributes: [{ shaderLocation: 0, offset: 0, format: 'float32x4' }, { shaderLocation: 1, offset: 16, format: 'float32x2' }] }] },
      fragment: { module: shaders.texture_f, entryPoint: 'main_fs', targets: [{ format: 'rgba8unorm' }] },
      primitive: { topology: 'triangle-list' },
    });
    check(!await device.popErrorScope(), 'real textured pipeline validates');
    const buffer = (data, usage) => {
      const b = device.createBuffer({ size: data.byteLength, usage: usage | GPUBufferUsage.COPY_DST });
      device.queue.writeBuffer(b, 0, data); return b;
    };
    const info = buffer(new Float32Array([1,1,1,1, 1,1,1,0, 1,0,0,0]), GPUBufferUsage.UNIFORM);
    const matrix = buffer(new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]), GPUBufferUsage.STORAGE);
    const group0 = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [
      { binding: 0, resource: { buffer: info } }, { binding: 2, resource: { buffer: matrix } },
    ] });
    const texture = device.createTexture({ size: [2,2], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
    const group3 = device.createBindGroup({ layout: pipeline.getBindGroupLayout(3), entries: [
      { binding: 0, resource: texture.createView() }, { binding: 1, resource: device.createSampler({ minFilter: 'nearest', magFilter: 'nearest' }) },
    ] });
    const empty = [1,2].map(n => device.createBindGroup({ layout: pipeline.getBindGroupLayout(n), entries: [] }));
    const target = device.createTexture({ size: [1,1], format: 'rgba8unorm', usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
    const readback = device.createBuffer({ size: 256, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const vertices = buffer(new Float32Array(18), GPUBufferUsage.VERTEX);
    const indices = buffer(new Uint16Array([0,1,2,0]), GPUBufferUsage.INDEX);
    const texels = new Uint8Array([255,0,0,255, 0,255,0,128, 0,0,255,64, 255,255,0,0]);
    device.queue.writeTexture({ texture }, texels, { bytesPerRow: 8 }, [2,2]);
    async function draw(u,v) {
      device.queue.writeBuffer(vertices, 0, new Float32Array([-1,-1,0,1,u,v, 3,-1,0,1,u,v, -1,3,0,1,u,v]));
      const encoder = device.createCommandEncoder();
      const pass = encoder.beginRenderPass({ colorAttachments: [{ view: target.createView(), loadOp: 'clear', storeOp: 'store', clearValue: [0,0,0,0] }] });
      pass.setPipeline(pipeline); pass.setBindGroup(0,group0); pass.setBindGroup(1,empty[0]); pass.setBindGroup(2,empty[1]); pass.setBindGroup(3,group3);
      pass.setVertexBuffer(0,vertices); pass.setIndexBuffer(indices,'uint16'); pass.drawIndexed(3); pass.end();
      encoder.copyTextureToBuffer({ texture: target }, { buffer: readback, bytesPerRow: 256 }, [1,1]);
      device.queue.submit([encoder.finish()]); await readback.mapAsync(GPUMapMode.READ);
      const pixel = new Uint8Array(readback.getMappedRange()).slice(0,4); readback.unmap(); return pixel.join();
    }
    for (const [u,v,pixel] of [[.25,.25,'255,0,0,255'], [.75,.25,'0,255,0,128'], [.25,.75,'0,0,255,64'], [.75,.75,'255,255,0,0']])
      check(await draw(u,v) === pixel, `texture UV ${u},${v} reads ${pixel}`);
    device.queue.writeTexture({ texture }, new Uint8Array([255,0,255,255, ...texels.slice(4)]), { bytesPerRow: 8 }, [2,2]);
    check(await draw(.25,.25) === '255,0,255,255', 'texture upload changes translated fragment output');
    check(errors.length === 0, errors.join(';'));
    device.destroy();
    return { checks, browserTranslation: true, texturedPixels: true, guestExecution: false, shaders: Object.keys(shaders) };
  });
  assert.ok(result.checks >= 22); console.log(JSON.stringify(result));
} finally { await browser?.close(); server.close(); }
