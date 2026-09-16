// Real GXP -> production USSE/SPIR-V -> validated Naga WGSL -> Chromium pixels.
// No hardcoded/replacement shader. This is NOT yet an ARM/SceGxm ABI test.
import { execFileSync } from 'node:child_process';
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { resolve, sep } from 'node:path';
import assert from 'node:assert/strict';
const work = resolve('.limbo_work/gxm');
const naga = resolve(process.env.NAGA_CLI || `${work}/node_modules/naga-wasi-cli/bin/naga.mjs`);
const shaders = {};
for (const name of ['color_v', 'color_f']) {
  execFileSync('timeout', ['-s', 'KILL', '10s', `${work}/gxp_compile`,
    `tools/native-tool/src/shaders/${name}.gxp`, `${work}/${name}.spv`],
    { env: { ...process.env, TRACY_NO_INVARIANT_CHECK: '1' }, stdio: 'inherit' });
  execFileSync('timeout', ['-s', 'KILL', '10s', process.execPath, naga,
    '--keep-coordinate-space', `.limbo_work/gxm/${name}.spv`, `.limbo_work/gxm/${name}.wgsl`], { stdio: 'inherit' });
  shaders[name] = await readFile(`${work}/${name}.wgsl`, 'utf8');
}
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE_URL || 'playwright');
const root = resolve('browser/web');
const server = createServer(async (req, res) => {
  try {
    const path = new URL(req.url, 'http://localhost').pathname;
    if (path === '/') { res.end('<!doctype html><title>Translated GXP test</title>'); return; }
    const file = resolve(root, `.${path}`);
    if (!file.startsWith(root + sep)) throw new Error('bad path');
    res.setHeader('Content-Type', 'text/javascript'); res.end(await readFile(file));
  } catch { res.writeHead(404); res.end(); }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
let browser;
try {
  browser = await chromium.launch({ headless: true,
    args: ['--enable-unsafe-webgpu', '--use-angle=swiftshader', '--enable-features=Vulkan', '--disable-vulkan-surface'],
    ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}),
  });
  const page = await browser.newPage();
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
  const result = await page.evaluate(async shaders => {
    const { createWebGPUBridge } = await import('./webgpu.js');
    const bridge = createWebGPUBridge(); const device = await bridge.requestDevice();
    if (!device) throw new Error('WebGPU unavailable');
    const errors = []; device.addEventListener('uncapturederror', e => errors.push(e.error.message));
    let checks = 0;
    const check = (condition, message) => { if (!condition) throw new Error(message); ++checks; };
    const rejects = async (op, message) => { let rejected = false; try { await op(); } catch { rejected = true; } check(rejected, message); };
    const renderer = bridge.renderer;
    const target = renderer.createTarget(65, 33);
    const program = await renderer.createProgram({ stride: 32,
      attributes: [{ shaderLocation: 0, offset: 0, format: 'float32x4' }, { shaderLocation: 1, offset: 16, format: 'float32x4' }],
      vertexWGSL: shaders.color_v, fragmentWGSL: shaders.color_f,
      vertexEntryPoint: 'main_vs', fragmentEntryPoint: 'main_fs',
      bufferBindings: [
        { binding: 0, size: 48, type: 'uniform', visibility: GPUShaderStage.VERTEX },
        { binding: 2, size: 64, type: 'read-only-storage', visibility: GPUShaderStage.VERTEX },
      ],
    });
    const vertices = new Float32Array([-1,-1,0,1, 1,0,0,1, 3,-1,0,1, 1,0,0,1, -1,3,0,1, 1,0,0,1]);
    const matrix = new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]);
    const info = new Float32Array([1,1,1,1, 1,65,33,0, 1,0,0,0]);
    const draw = { program, vertices, indices: new Uint16Array([0,1,2]), indexFormat: 'uint16', buffers: { 0: info, 2: matrix } };
    const pending = renderer.submit(target, [draw]);
    matrix[0] = 0; vertices.fill(0);
    const red = await pending;
    check(red.pixels.every((v,i) => v === [255,0,0,255][i%4]), 'translated USSE draws red indexed triangle from owned snapshots');
    vertices.set([-1,-1,0,1, 0,1,0,1, 3,-1,0,1, 0,1,0,1, -1,3,0,1, 0,1,0,1]);
    matrix[0] = 1;
    const green = await renderer.submit(target, [draw]);
    check(green.pixels.every((v,i) => v === [0,255,0,255][i%4]), 'guest color attribute changes translated fragment output');
    matrix[12] = 4;
    const shifted = await renderer.submit(target, [draw]);
    check(shifted.pixels.every((v,i) => v === [0,0,0,255][i%4]), 'guest WVP uniform changes translated vertex coverage');
    matrix[12] = 0; matrix[0] = 0.25; matrix[5] = 0.25;
    const small = await renderer.submit(target, [draw]);
    const pixel = (x,y) => small.pixels.slice((y*65+x)*4,(y*65+x)*4+4).join();
    check(pixel(32,16) === '0,255,0,255', 'uniform-scaled triangle center');
    check(pixel(0,0) === '0,0,0,255' && pixel(64,32) === '0,0,0,255', 'uniform-scaled triangle exterior');
    await rejects(() => renderer.submit(target, [{ ...draw, buffers: { 0: info } }]), 'missing guest SSBO rejected');
    await rejects(() => renderer.submit(target, [{ ...draw, buffers: { 0: info, 2: new Uint8Array(16) } }]), 'short guest SSBO rejected');
    renderer.destroyProgram(program); renderer.destroyTarget(target); renderer.dispose();
    check(errors.length === 0, errors.join('; ')); device.destroy();
    return { checks, backend: 'WebGPU', translatedGuestShader: true, guestExecution: false, translation: 'Vita3K USSE -> SPIR-V -> Naga WGSL' };
  }, shaders);
  assert.ok(result.checks >= 8); console.log(JSON.stringify(result));
} finally { await browser?.close(); server.close(); }
