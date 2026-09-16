// Cooperative producer boundary for GXM-style immediate contexts. This is not
// the SceGxm guest ABI: the HLE must validate/translate its structs and addresses.
// Draw buffers are owned at draw() time; queued scenes never retain Wasm views.
import { snapshotGuestBytes } from './gxm_renderer.js';

function owned(view) {
  if (view instanceof ArrayBuffer) return new Uint8Array(view).slice();
  if (ArrayBuffer.isView(view)) return new Uint8Array(view.buffer, view.byteOffset, view.byteLength).slice();
  throw new TypeError('expected draw buffer');
}
function snapshotDraw(draw) {
  const copy = { ...draw, vertices: owned(draw.vertices), indices: owned(draw.indices) };
  if (draw.uniforms !== undefined) copy.uniforms = owned(draw.uniforms);
  if (draw.buffers !== undefined)
    copy.buffers = Object.fromEntries(Object.entries(draw.buffers).map(([key, value]) => [key, owned(value)]));
  return copy;
}

export function createGXMContextQueue(renderer, { maxPendingScenes = 2, maxSceneBytes = 16 * 1024 * 1024 } = {}) {
  if (!Number.isSafeInteger(maxPendingScenes) || maxPendingScenes < 1 || maxPendingScenes > 8)
    throw new RangeError('invalid scene queue capacity');
  if (!Number.isSafeInteger(maxSceneBytes) || maxSceneBytes < 1)
    throw new RangeError('invalid scene byte budget');
  let tail = Promise.resolve(), pending = 0, serial = 0, completed = 0, failure = null;
  const contexts = new WeakMap();
  const tickets = new WeakSet();
  function healthy() { if (failure) throw new Error(`GXM queue failed: ${failure.message}`); }
  function context(id) {
    healthy();
    if (!contexts.has(id)) throw new Error('unknown GXM context');
    return contexts.get(id);
  }
  return Object.freeze({
    createContext() {
      healthy(); const id = Object.freeze({}); contexts.set(id, { scene: null, queued: 0 }); return id;
    },
    destroyContext(id) {
      const ctx = context(id);
      if (ctx.scene || ctx.queued) throw new Error('GXM context still in use');
      contexts.delete(id);
    },
    beginScene(id, target, { clearColor = [0, 0, 0, 1] } = {}) {
      const ctx = context(id);
      if (ctx.scene) throw new Error('GXM scene already active');
      if (pending >= maxPendingScenes) throw new Error('GXM queue full; await finish before beginning scene');
      ctx.scene = { target, clearColor: [...clearColor], draws: [], byteLength: 0 };
    },
    draw(id, draw) {
      const ctx = context(id);
      if (!ctx.scene) throw new Error('GXM draw outside scene');
      const byteLength = [draw.vertices, draw.indices, draw.uniforms, ...Object.values(draw.buffers ?? {})]
        .filter(b => b !== undefined).reduce((sum, b) => {
          if (!(b instanceof ArrayBuffer) && !ArrayBuffer.isView(b)) throw new TypeError('expected draw buffer');
          return sum + b.byteLength;
        }, 0);
      if (ctx.scene.byteLength + byteLength > maxSceneBytes) throw new RangeError('GXM scene upload budget exceeded');
      ctx.scene.draws.push(snapshotDraw(draw)); ctx.scene.byteLength += byteLength;
    },
    // Memory64 host-address adapter, not 32-bit guest-address translation.
    // Pass a fresh HEAPU8 after growth. Copy all ranges before any async work.
    drawGuest(id, heap, { vertices, indices, buffers = {}, ...draw }) {
      const read = range => snapshotGuestBytes(heap, range.address, range.length);
      this.draw(id, { ...draw, vertices: read(vertices), indices: read(indices),
        buffers: Object.fromEntries(Object.entries(buffers).map(([key, range]) => [key, read(range)])) });
    },
    abortScene(id) { context(id).scene = null; },
    endScene(id, { writeback, signal } = {}) {
      const ctx = context(id);
      if (!ctx.scene) throw new Error('GXM end outside scene');
      if (pending >= maxPendingScenes) throw new Error('GXM queue full; scene remains active');
      if (writeback !== undefined && typeof writeback !== 'function') throw new TypeError('invalid writeback hook');
      if (signal !== undefined && typeof signal !== 'function') throw new TypeError('invalid signal hook');
      const scene = ctx.scene; ctx.scene = null; ++pending; ++ctx.queued;
      const sequence = ++serial;
      const completion = tail.then(async () => {
        healthy();
        const frame = await renderer.submit(scene.target, scene.draws, { clearColor: scene.clearColor });
        // CPU surface writeback must finish before waking guest waiters.
        if (writeback) await writeback(frame);
        if (signal) await signal(sequence);
        completed = sequence;
        return frame;
      }).catch(error => { failure = error; throw error; }).finally(() => { --pending; --ctx.queued; });
      // Handle the internal rejection without concealing it from ticket/finish.
      tail = completion.then(() => undefined, () => undefined);
      const ticket = Object.freeze({ sequence, completion }); tickets.add(ticket); return ticket;
    },
    // Wait for BOTH old and new render completions, then yield cooperatively.
    // Callback is supplied by the HLE scheduler; never call guest code on a
    // browser message callback while an Asyncify stack is suspended.
    async displayQueue(oldTicket, newTicket, callbackData, callback) {
      if ((oldTicket && !tickets.has(oldTicket)) || !tickets.has(newTicket))
        throw new Error('foreign GXM completion ticket');
      if (typeof callback !== 'function') throw new TypeError('invalid display callback');
      const data = owned(callbackData);
      await Promise.all([oldTicket?.completion, newTicket.completion]);
      healthy();
      await new Promise(resolve => setTimeout(resolve, 0));
      return callback(data, newTicket.sequence);
    },
    async finish() { await tail; healthy(); return completed; },
    get status() { return Object.freeze({ pending, submitted: serial, completed, failed: !!failure }); },
  });
}
