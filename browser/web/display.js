// Vita3K Web — M9 framebuffer presentation.
// Receives real sceDisplaySetFrameBuf frames (tight RGBA8) from the Worker
// and paints them on a canvas. No WebGPU/GL: this is the CPU-rendered Vita
// framebuffer path, blitted with Canvas2D ImageData.
const status = document.querySelector('#status');
const canvas = document.querySelector('#vita-canvas');
const ctx = canvas.getContext('2d', { alpha: false });

const frames = { received: 0, lastGeneration: -1, lastChecksum: 0, checksums: new Set() };

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
}

window.addEventListener('DOMContentLoaded', () => {
  const worker = new Worker('./worker.js', { type: 'module' });
  worker.onmessage = ({ data }) => {
    if (data.type === 'lifecycle') console.log('[vita3k-web] lifecycle:', data.state);
    if (data.type === 'ready') {
      status.textContent = 'Vita3K runtime ready. Loading homebrew…';
      // Prefer a fixture staged next to the page; fall back to user upload.
      fetch('./display-eboot.bin')
        .then((response) => (response.ok ? response.arrayBuffer() : Promise.reject(new Error('no staged fixture'))))
        .then((bytes) => worker.postMessage({ type: 'run-vita', file: new File([bytes], 'display-eboot.bin') }))
        .catch(() => { status.textContent = 'Select a Vita eboot.bin to run.'; });
    }
    if (data.type === 'vita-frame') present(data);
    if (data.type === 'vita-exit') {
      console.log('[vita3k-web] vita exit:', data.exitCode);
      status.textContent = `Vita process exited with code ${data.exitCode} after ${frames.received} frames.`;
    }
    if (data.type === 'log') console.log('[vita3k-web]', data.message);
    if (data.type === 'error') status.textContent = `Error: ${data.message}`;
  };
  worker.onerror = (event) => { status.textContent = `Worker error: ${event.message || 'unknown'}`; };
  document.querySelector('#elf-file')?.addEventListener('change', () => {
    const [file] = document.querySelector('#elf-file').files;
    if (file) worker.postMessage({ type: 'run-vita', file });
  });
  window.vita3kWeb = { worker, frames };
});
