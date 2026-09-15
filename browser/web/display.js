// Vita3K Web — M9 framebuffer presentation.
// Receives real sceDisplaySetFrameBuf frames (tight RGBA8) from the Worker
// and paints them on a canvas. No WebGPU/GL: this is the CPU-rendered Vita
// framebuffer path, blitted with Canvas2D ImageData.
const status = document.querySelector('#status');
const canvas = document.querySelector('#vita-canvas');
const ctx = canvas.getContext('2d', { alpha: false });

const frames = { received: 0, lastGeneration: -1, lastChecksum: 0, checksums: new Set() };
const fpsEl = document.querySelector('#fps');
const fps = { start: 0, windowStart: 0, windowCount: 0 };

function tickFps() {
  const now = performance.now();
  if (!fps.start) { fps.start = now; fps.windowStart = now; }
  fps.windowCount++;
  if (now - fps.windowStart >= 1000) {
    const windowFps = (fps.windowCount * 1000) / (now - fps.windowStart);
    const avgFps = (frames.received * 1000) / (now - fps.start);
    if (fpsEl) fpsEl.textContent = `FPS: ${windowFps.toFixed(1)} (avg ${avgFps.toFixed(1)}, ${frames.received} frames)`;
    fps.windowStart = now;
    fps.windowCount = 0;
  }
}

function checksum(view) {
  // Cheap content fingerprint over the frame; frames must differ over time.
  let h = 5381;
  const step = Math.max(4, (view.length / 4096) | 0) & ~3;
  for (let i = 0; i < view.length; i += step) h = ((h * 33) ^ view[i]) >>> 0;
  return h >>> 0;
}

function present(frame) {
  if (frame.width !== canvas.width) canvas.width = frame.width;
  if (frame.height !== canvas.height) canvas.height = frame.height;
  const image = new ImageData(new Uint8ClampedArray(frame.data), frame.width, frame.height);
  ctx.putImageData(image, 0, 0);
  frames.received++;
  frames.lastGeneration = frame.generation;
  const sum = checksum(new Uint8Array(frame.data));
  if (sum !== frames.lastChecksum) frames.checksums.add(sum);
  frames.lastChecksum = sum;
  tickFps();
}

window.addEventListener('DOMContentLoaded', () => {
  // Optional backend selection: display.html?backend=jit runs the animated
  // homebrew through the M14 WasmJitCPU module instead of the interpreter.
  const params = new URLSearchParams(self.location.search);
  const backend = params.get('backend');
  // Debug headroom mode: ?fastvblank=1 free-runs the vblank clock so the
  // FPS readout below measures true guest throughput, not the 60Hz cadence.
  const fastVblank = params.get('fastvblank') === '1';
  const workerUrl = (backend === 'jit' ? './worker.js?backend=jit' : './worker.js')
    + (fastVblank ? (backend === 'jit' ? '&' : '?') + 'fastvblank=1' : '');
  const backendLabel = document.querySelector('#backend-label');
  if (backendLabel) backendLabel.textContent = (backend === 'jit' ? '(WasmJitCPU)' : '(InterpreterCPU)')
    + (fastVblank ? ' [fast vblank: uncapped]' : '');
  const worker = new Worker(workerUrl, { type: 'module' });
  worker.onmessage = ({ data }) => {
    if (data.type === 'lifecycle') console.log('[vita3k-web] lifecycle:', data.state);
    if (data.type === 'ready') {
      status.textContent = 'Vita3K runtime ready. Loading homebrew…';
      // Prefer a fixture staged next to the page; fall back to user upload.
      fetch('./display-eboot.bin')
        .then((response) => (response.ok ? response.arrayBuffer() : Promise.reject(new Error('no staged fixture'))))
        .then((bytes) => worker.postMessage({ type: 'run-vita', file: new File([bytes], 'display-eboot.bin'), fastVblank }))
        .catch(() => { status.textContent = 'Select a Vita eboot.bin to run.'; });
    }
    if (data.type === 'vita-frame') present(data);
    if (data.type === 'vita-exit') {
      console.log('[vita3k-web] vita exit:', data.exitCode);
      status.textContent = `Vita process exited with code ${data.exitCode} after ${frames.received} frames.`;
      if (fpsEl && fps.start) {
        const avgFps = (frames.received * 1000) / (performance.now() - fps.start);
        fpsEl.textContent = `FPS: final avg ${avgFps.toFixed(1)} over ${frames.received} frames.`;
      }
    }
    if (data.type === 'log') console.log('[vita3k-web]', data.message);
    if (data.type === 'error') status.textContent = `Error: ${data.message}`;
  };
  worker.onerror = (event) => { status.textContent = `Worker error: ${event.message || 'unknown'}`; };
  document.querySelector('#elf-file')?.addEventListener('change', () => {
    const [file] = document.querySelector('#elf-file').files;
    if (file) worker.postMessage({ type: 'run-vita', file, fastVblank });
  });
  window.vita3kWeb = { worker, frames };
});
