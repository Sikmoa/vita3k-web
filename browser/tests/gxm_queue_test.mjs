// node --experimental-vm-modules browser/tests/gxm_queue_test.mjs
// Deterministic queue/completion tests and the real scene consumer with a
// controllable WebGPU device. No game files, compiler or Wasm build required.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { test } from 'node:test';
import vm from 'node:vm';
import { GpuQueue } from '../web/gpu_queue.js';

const turn = () => new Promise(resolve => setImmediate(resolve));
function fakeQueue() {
  const submissions = [], probes = [], writes = [];
  return {
    submissions, probes, writes,
    submit(commands) { submissions.push(commands); },
    writeBuffer(buffer, offset, data) { writes.push(['buffer', [...data]]); },
    writeTexture(target, data) { writes.push(['texture', [...data]]); },
    onSubmittedWorkDone() {
      return new Promise((resolve, reject) => probes.push({ through: submissions.length, resolve, reject }));
    },
    async complete() {
      assert(probes.length, 'a completion probe must be registered');
      probes.shift().resolve();
      await turn();
    },
    async drain() { while (probes.length) await this.complete(); },
  };
}

test('bounded window, exact completion watermark, eager tail probes and ordered retry', async () => {
  const deviceQueue = fakeQueue();
  let clock = 0;
  const queue = new GpuQueue(deviceQueue, { maxInFlight: 2, now: () => clock });
  queue.submit(['A']); queue.submit(['B']);
  assert.equal(deviceQueue.probes.length, 1);
  assert.equal(queue.hasCapacity(), false);
  assert.throws(() => queue.submit(['C']), /capacity/);
  let resumed = false;
  const waiter = queue.waitForCapacity().then(() => { resumed = true; queue.submit(['C']); });
  await turn();
  assert.equal(resumed, false);
  clock = 12;
  await deviceQueue.complete();
  await waiter;
  assert.equal(queue.snapshot().completedSerial, 1, 'first probe covers only A');
  assert.equal(deviceQueue.probes[0].through, 2, 'probe covers B before C is retried');
  assert.deepEqual(deviceQueue.submissions, [['A'], ['B'], ['C']]);
  clock = 20;
  await deviceQueue.complete();
  assert.equal(queue.snapshot().completedSerial, 2);
  assert.equal(deviceQueue.probes[0].through, 3, 'tail advances without another submit');
  clock = 24;
  await deviceQueue.drain();
  const stats = queue.snapshot();
  assert.equal(stats.inFlight, 0);
  assert.equal(stats.peakInFlight, 2);
  assert.equal(stats.queueWaitMs, 12);
  assert.equal(stats.completionAvgMs, 8);
});

test('loss, completion rejection and missing completion release waiters without allowing more work', async () => {
  for (const kind of ['lost', 'rejected', 'timeout', 'synchronous-error']) {
    const deviceQueue = fakeQueue();
    if (kind === 'synchronous-error') deviceQueue.onSubmittedWorkDone = () => { throw new Error(kind); };
    const queue = new GpuQueue(deviceQueue, { maxInFlight: 1, timeoutMs: kind === 'timeout' ? 10 : 15000 });
    queue.submit(['A']);
    const waiter = assert.rejects(queue.waitForCapacity(), /lost|rejected|progress|synchronous-error/);
    if (kind === 'lost') queue.fail(new Error('lost'));
    if (kind === 'rejected') deviceQueue.probes.shift().reject(new Error('rejected'));
    await waiter;
    assert.throws(() => queue.submit(['B']));
    await deviceQueue.drain();
    assert.equal(queue.snapshot().completedSerial, 0, 'late completions cannot revive a failed queue');
  }
});

test('zero explicitly disables capacity limiting, but retains completion accounting', async () => {
  const deviceQueue = fakeQueue();
  const queue = new GpuQueue(deviceQueue, { maxInFlight: 0 });
  for (let i = 0; i < 20; ++i) queue.submit([i]);
  assert.equal(queue.hasCapacity(), true);
  await deviceQueue.drain();
  assert.equal(queue.snapshot().inFlight, 0);
});

async function sceneFixture() {
  const queue = fakeQueue();
  let lose;
  const buffers = [], textures = [];
  const device = {
    queue,
    lost: new Promise(resolve => { lose = resolve; }),
    addEventListener() {},
    createBindGroupLayout: x => x,
    createPipelineLayout: x => x,
    createBindGroup: x => x,
    createSampler: x => x,
    createShaderModule: x => x,
    createRenderPipeline: () => ({ getBindGroupLayout: () => ({}) }),
    createBuffer({ size }) {
      const buffer = { bytes: new ArrayBuffer(size), destroyed: false, destroy() { this.destroyed = true; },
        unmap() {}, mapAsync: async () => {}, getMappedRange() { return this.bytes; } };
      buffers.push(buffer);
      return buffer;
    },
    createTexture(descriptor) {
      const texture = { descriptor, destroyed: false, destroy() { this.destroyed = true; },
        createView() { return { texture: this }; } };
      textures.push(texture);
      return texture;
    },
    createCommandEncoder() {
      const commands = [];
      return {
        beginRenderPass(descriptor) {
          commands.push(['pass', descriptor]);
          return { setPipeline() {}, setBindGroup() {}, draw() {}, end() {} };
        },
        copyTextureToBuffer() { commands.push(['readback']); },
        finish: () => commands,
      };
    },
  };
  const notices = [];
  const context = vm.createContext({ URL, performance, setTimeout, clearTimeout, console,
    self: { location: { href: 'https://test/worker.js?maxInFlight=2' } },
    navigator: { gpu: { requestAdapter: async () => ({ info: {}, requestDevice: async () => device }),
      getPreferredCanvasFormat: () => 'bgra8unorm' } },
    GPUShaderStage: { VERTEX: 1, FRAGMENT: 2 },
    GPUTextureUsage: { TEXTURE_BINDING: 1, COPY_DST: 2, COPY_SRC: 4, RENDER_ATTACHMENT: 8 },
    GPUBufferUsage: { VERTEX: 1, INDEX: 2, UNIFORM: 4, STORAGE: 8, COPY_DST: 16, MAP_READ: 32 },
    GPUMapMode: { READ: 1 },
    vita3kWebOnGxmDevice: info => notices.push(info),
  });
  const source = await readFile(new URL('../web/gxm_scene.js', import.meta.url), 'utf8');
  const module = new vm.SourceTextModule(source, { context });
  await module.link(async specifier => {
    if (specifier === './gxp_shader_adapter.js')
      return new vm.SourceTextModule('export async function createGXPShaderAdapter() { return {}; }', { context });
    assert.equal(specifier, './gpu_queue.js');
    return new vm.SourceTextModule(await readFile(new URL('../web/gpu_queue.js', import.meta.url), 'utf8'), { context });
  });
  await module.evaluate();
  const scene = module.namespace;
  await assert.rejects(scene.init({}), /rebuild vita3k_web_dist/, 'stale Wasm must fail before drawing');
  await scene.init({ submissionProtocol: 1, logger() {} });
  let canvasAcquisitions = 0;
  scene.attachCanvas({ width: 1, height: 1, getContext: () => ({ configure() {},
    getCurrentTexture() { ++canvasAcquisitions; return device.createTexture({ canvas: true }); } }) });
  // Typed arrays must belong to the consumer's realm (as they do in a Worker).
  function stream(address, textureId) {
    const words = [0x31535847,
      3, textureId, 1, 1, 1, 0, 4, // texture upload preceding BEGIN_PASS
      1, address, 0, 1, 1, 0, 0, 0, 0, 0x3f800000, 0, 0, 0, 0, 1, 1, 4];
    context.fixtureWords = words;
    return vm.runInContext('new Uint32Array(fixtureWords)', context);
  }
  return { scene, queue, notices, buffers, textures, lose, stream, canvasAcquisitions: () => canvasAcquisitions,
    bytes: vm.runInContext('new Uint8Array([10, 20, 30, 255])', context) };
}

test('scene and presentation retries preserve every stream/upload, including alternating targets', async () => {
  const f = await sceneFixture();
  const { scene, queue, stream, bytes } = f;
  const A = 0x1000, B = 0x2000;
  assert.equal(scene.trySubmitScene(stream(A, 1), bytes), true);
  assert.equal(scene.trySubmitScene(stream(B, 2), bytes), true);
  const writesBefore = queue.writes.length;
  assert.equal(scene.trySubmitScene(stream(A, 3), bytes), false);
  const frames = [];
  const frame = (...args) => frames.push(args);
  assert.equal(scene.presentTarget(A, frame, 0), null, 'busy is distinct from missing GPU target');
  assert.equal(f.canvasAcquisitions(), 0, 'busy present does not acquire/overwrite canvas');
  assert.equal(frames.length, 0);
  assert.equal(queue.writes.length, writesBefore, 'busy scene does not mutate GPU resources');
  await queue.complete();
  assert.equal(scene.trySubmitScene(stream(A, 3), bytes), true);
  assert.equal(queue.writes.filter(([kind]) => kind === 'texture').length, 3, 'retry uploads the texture exactly once');
  await queue.drain();
  assert.equal(scene.presentTarget(A, frame, 0), true);
  assert.equal(scene.presentTarget(B, frame, 0), true);
  assert.equal(scene.presentTarget(A, frame, 0), null, 'presentation also consumes queue capacity');
  assert.deepEqual(frames.map(f => f[0]), [1, 2], 'busy presents do not increment the FPS/generation');
  await queue.drain();
  assert.equal(scene.presentTarget(A, frame, 0), true);
  await queue.drain();
  const stats = scene.sceneStats();
  assert.equal(stats.scenes, 3);
  assert.equal(stats.presents, 3);
  assert.equal(stats.submitSerial, 6, 'scenes AND blits are accounted for');
  assert.equal(stats.peakInFlight, 2);
  assert.equal(stats.droppedScenes, 0);
});

test('canvas + pixel readback share one bounded submission; surface sync waits too', async () => {
  const f = await sceneFixture();
  const { scene, queue, stream, bytes } = f;
  scene.trySubmitScene(stream(0x1000, 1), bytes);
  await queue.drain();
  const frames = [];
  const before = queue.submissions.length;
  assert.equal(scene.presentTarget(0x1000, (...args) => frames.push(args), 1), true);
  assert.equal(queue.submissions.length, before + 1);
  await turn();
  assert.equal(frames[0][3].length, 4);
  assert(f.buffers.at(-1).destroyed, 'presentation readback buffer released');
  assert(f.textures.at(-1).destroyed, 'presentation readback texture released');
  scene.presentTarget(0x1000, () => {}, 0); // window full
  const full = queue.submissions.length;
  let mapped = false;
  const read = scene.readTarget(0x1000, 1, 1, 4, () => { mapped = true; });
  await turn();
  assert.equal(queue.submissions.length, full);
  assert.equal(mapped, false);
  await queue.complete();
  await read;
  assert.equal(queue.submissions.length, full + 1);
  await queue.drain();
  assert.equal(scene.sceneStats().surfaceSyncs, 1);
  assert.equal(scene.sceneStats().peakInFlight, 2);
});

test('device loss unblocks a renderer wait and forbids subsequent uploads', async () => {
  const f = await sceneFixture();
  f.scene.trySubmitScene(f.stream(0x1000, 1), f.bytes);
  f.scene.trySubmitScene(f.stream(0x2000, 2), f.bytes);
  const wait = assert.rejects(f.scene.waitForCapacity(), /device lost/);
  f.lose({ reason: 'destroyed', message: 'test loss' });
  await wait;
  assert.throws(() => f.scene.trySubmitScene(f.stream(0x1000, 3), f.bytes), /device lost/);
  assert.equal(f.notices.length, 1);
  await f.queue.drain();
});
