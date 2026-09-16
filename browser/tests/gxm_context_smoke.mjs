// End-to-end GXM context/producer test. Real translated GXP shaders from the
// production Vita3K USSE recompiler; no hardcoded WGSL, no mocked GPU.
// Still NOT the guest SceGxm ABI: draw inputs are supplied as typed JS arrays.
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
    `tools/native-tool/src/shaders/${name}.gxp`, `.limbo_work/gxm/${name}.spv`],
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
    if (path === '/') { res.end('<!doctype html><title>GXM context test</title>'); return; }
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
    const { createGXMContextQueue } = await import('./gxm_context.js');
    const bridge = createWebGPUBridge(); const device = await bridge.requestDevice();
    if (!device) throw new Error('WebGPU unavailable');
    const errors = []; device.addEventListener('uncapturederror', e => errors.push(e.error.message));
    let checks = 0;
    const check = (condition, message) => { if (!condition) throw new Error(message); ++checks; };
    const rejects = async (op, message) => { let rejected = false; try { await op(); } catch { rejected = true; } check(rejected, message); };
    const renderer = bridge.renderer;
    const queue = createGXMContextQueue(renderer);
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
    // Ownership: mutating inputs after draw() must not change what is submitted.
    const ctx = queue.createContext();
    queue.beginScene(ctx, target);
    queue.draw(ctx, draw);
    vertices.fill(0); matrix.fill(0);
    let displayFrames = 0;
    const ticket = queue.endScene(ctx, { writeback: async frame => {
      check(frame.pixels.every((v,i) => v === [255,0,0,255][i%4]), 'owned draw inputs survive mutation');
    }, signal: async () => { ++displayFrames; } });
    check(queue.status.completed === 0, 'no premature completion');
    await rejects(() => queue.destroyContext(ctx), 'in-flight context destruction rejected');
    await ticket.completion;
    check(queue.status.completed === 1, 'scene completion tracked');
    // drawGuest adapter: Memory64 ranges snapshotted through the checked path.
    const heap = new Uint8Array(1024);
    vertices.set([-1,-1,0,1, 0,1,0,1, 3,-1,0,1, 0,1,0,1, -1,3,0,1, 0,1,0,1]);
    matrix.set([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]);
    heap.set(new Uint8Array(vertices.buffer), 0);
    heap.set(new Uint8Array(new Uint16Array([0,1,2]).buffer), 512);
    heap.set(new Uint8Array(matrix.buffer), 256);
    heap.set(new Uint8Array(info.buffer), 320);
    const guestCtx = queue.createContext();
    queue.beginScene(guestCtx, target);
    queue.drawGuest(guestCtx, heap, { ...draw,
      vertices: { address: 0n, length: vertices.byteLength },
      indices: { address: 512, length: 6 },
      buffers: { 0: { address: 320, length: 48 }, 2: { address: 256, length: 64 } } });
    const second = queue.endScene(guestCtx);
    heap.fill(0);
    const callbackData = new Uint8Array([7,0,0,0]);
    const displayed = queue.displayQueue(ticket, second, callbackData, async data => {
      check(queue.status.completed === 2, 'display waits for both scenes');
      check(data[0] === 7, 'display callback data owned before await');
      return 42;
    });
    callbackData.fill(99);
    const frame = await second.completion;
    check(frame.pixels.every((v,i) => v === [0,255,0,255][i%4]), 'Memory64 draw ranges survive heap mutation');
    check(await queue.finish() === 2, 'finish reports completed sequence');
    const last = await displayed;
    check(last === 42 && displayFrames >= 1, 'display callback ran after GPU completion');
    await rejects(() => queue.displayQueue(ticket, { sequence: 99, completion: Promise.resolve() }, new Uint8Array(4), async () => {}), 'foreign ticket rejected');
    await rejects(() => queue.draw(ctx, draw), 'draw outside scene rejected');
    queue.beginScene(ctx, target);
    await rejects(() => queue.beginScene(ctx, target), 'nested scene rejected');
    queue.abortScene(ctx);
    queue.destroyContext(ctx); queue.destroyContext(guestCtx);
    await rejects(() => queue.beginScene(ctx, target), 'destroyed context cannot be reused');
    await rejects(() => queue.destroyContext(ctx), 'context cannot be destroyed twice');
    // Failure propagation: validation failure must never signal/display success.
    const failedQueue = createGXMContextQueue(renderer);
    const broken = failedQueue.createContext();
    failedQueue.beginScene(broken, target);
    failedQueue.draw(broken, { ...draw, indices: new Uint16Array([0,1,999]) });
    let falseSignal = false;
    const bad = failedQueue.endScene(broken, { signal: () => { falseSignal = true; } });
    await rejects(() => bad.completion, 'invalid guest draw surfaces validation error');
    await rejects(() => failedQueue.createContext(), 'failed queue rejects new contexts');
    await rejects(() => failedQueue.finish(), 'finish propagates failure');
    check(!falseSignal && failedQueue.status.completed === 0, 'failed sequence not signaled complete');
    renderer.destroyProgram(program); renderer.destroyTarget(target); renderer.dispose();
    check(errors.length === 0, errors.join('; ')); device.destroy();
    return { checks, backend: 'WebGPU', translatedGuestShader: true, guestExecution: false,
      translation: 'Vita3K USSE -> SPIR-V -> Naga WGSL', queue: queue.status };
  }, shaders);
  assert.equal(result.checks, 19); console.log(JSON.stringify(result));
} finally { await browser?.close(); server.close(); }
