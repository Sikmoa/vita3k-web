// Real WebGPU pixel regression. Exercises the production consumer/queue under
// saturation, including persistent partial updates to alternating targets.
// Shader translation is stubbed: these streams only use the built-in blits.
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';

const server = createServer(async (req, res) => {
  const path = new URL(req.url, 'http://localhost').pathname;
  res.setHeader('Content-Type', path === '/' ? 'text/html' : 'text/javascript');
  if (path === '/') return res.end('<!doctype html><canvas id="screen"></canvas>');
  if (path === '/gxp_shader_adapter.js')
    return res.end('export async function createGXPShaderAdapter() { return {}; }');
  if (!['/gxm_scene.js', '/gpu_queue.js'].includes(path)) { res.writeHead(404); return res.end(); }
  try { res.end(await readFile(new URL('../web' + path, import.meta.url))); }
  catch { res.writeHead(404); res.end(); }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE_URL || 'playwright');
let browser;
try {
  browser = await chromium.launch({ headless: true,
    args: ['--no-sandbox', '--enable-unsafe-webgpu', '--use-angle=swiftshader', '--enable-features=Vulkan', '--disable-vulkan-surface'],
    ...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE ? { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE } : {}),
  });
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  await page.goto(`http://127.0.0.1:${server.address().port}/?maxInFlight=1`);
  const result = await page.evaluate(async () => {
    const scene = await import('./gxm_scene.js');
    const logs = [];
    await scene.init({ submissionProtocol: 1, logger: m => logs.push(m) });
    scene.attachCanvas(document.querySelector('#screen'));
    const MAGIC = 0x31535847;
    const empty = new Uint8Array();
    function target(address) {
      return new Uint32Array([MAGIC, 1, address, 0, 2, 1, 0, 0, 0, 0, 0x3f800000, 0, 0, 0, 0, 1, 1, 4]);
    }
    function texel(address, x) {
      return new Uint32Array([MAGIC, 7, address, x, 0, 1, 1, 0, 0xffffffff]);
    }
    async function submit(words, bytes) {
      while (!scene.trySubmitScene(words, bytes)) await scene.waitForCapacity();
    }
    const A = 0x1000, B = 0x2000;
    // Fill the only slot, then try again in the SAME JS task: the second
    // stream must be refused without side effects, retained and retried.
    const first = scene.trySubmitScene(target(A), empty);
    const busy = scene.trySubmitScene(target(B), empty);
    await scene.waitForCapacity();
    await submit(target(B), empty);
    await submit(texel(A, 0), new Uint8Array([255, 0, 0, 255]));
    await submit(texel(B, 0), new Uint8Array([0, 0, 255, 255]));
    await submit(texel(A, 1), new Uint8Array([0, 255, 0, 255]));
    await submit(texel(B, 1), new Uint8Array([255, 255, 0, 255]));
    const frames = [];
    async function present(address) {
      let ready;
      const received = new Promise(resolve => { ready = resolve; });
      while (scene.presentTarget(address, (generation, width, height, pixels) => {
        frames.push({ generation, width, height, pixels: Array.from(pixels) }); ready();
      }, 1) === null) await scene.waitForCapacity();
      await received;
    }
    await present(A);
    await present(B);
    await present(A);
    let surface;
    await scene.readTarget(A, 2, 1, 4, pixels => { surface = Array.from(pixels.slice(0, 8)); });
    await scene.waitForCapacity();
    return { first, busy, frames, surface, stats: scene.sceneStats(), logs };
  });
  assert.deepEqual(errors, []);
  assert.equal(result.first, true);
  assert.equal(result.busy, false);
  const A = [255, 0, 0, 255, 0, 255, 0, 255], B = [0, 0, 255, 255, 255, 255, 0, 255];
  assert.deepEqual(result.frames.map(f => f.pixels), [A, B, A], 'both partial updates persist across alternating presents');
  assert.deepEqual(result.surface, A);
  assert.equal(result.stats.scenes, 6);
  assert.equal(result.stats.presents, 3);
  assert.equal(result.stats.submitSerial, 10, 'six scenes + three combined blit/readbacks + one surface read');
  assert.equal(result.stats.peakInFlight, 1);
  assert.equal(result.stats.droppedScenes, 0);
  assert.equal(result.stats.presentFailures, 0);
  assert(!result.logs.some(line => /validation|failed|queue error/i.test(line)), result.logs.join('\n'));
  console.log('PASS: bounded WebGPU, persistent partial updates, alternating targets, presentation and surface readback');
  console.log(JSON.stringify(result.stats));
} finally {
  await browser?.close();
  server.close();
}
