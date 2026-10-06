import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';
import UnoCSS from 'unocss/vite';
import { execSync } from 'node:child_process';

// The app is built into the player site next to the runtime (worker.js,
// library.js, vita_session.js, wasm64/, …), which it loads at run time from
// its own directory (src/runtime.ts): base './' keeps every URL relative, so
// the site works under any path (GitHub Pages serves it under /<repo>/).
//
// \`bun run dev\` serves the app with the runtime proxied from the dev server
// (browser/tests/limbo_serve.mjs, VITA3K_RUNTIME, default
// http://localhost:8080), with the cross-origin isolation headers the
// threaded runtime needs.
const runtime = process.env.VITA3K_RUNTIME || 'http://localhost:8080';
const runtimePaths = ['/worker.js', '/thread_bridge.js', '/library.js', '/vita_session.js', '/pad_input.js', '/capabilities.js',
  '/content_cache.js', '/zip.js', '/decrypt_worker.js', '/save_sync.js', '/gxm_scene.js', '/gpu_queue.js', '/gxp_shader_adapter.js',
  '/gles_webgl.js', '/audio_input.js', '/audio_ring_worklet.js', '/storage.js', '/player-config.json', '/manifest.json',
  '/coi.js', '/coi_sw.js', '/wasm64', '/wasm32', '/shaders', '/decrypt', '/stage', '/aot', '/aot.wasm', '/aot-mt'];

const commit = (() => {
  try { return execSync('git rev-parse --short HEAD', { stdio: ['ignore', 'pipe', 'ignore'] }).toString().trim(); } catch { return ''; }
})();

export default defineConfig({
  base: './',
  plugins: [vue(), UnoCSS()],
  define: {
    __APP_COMMIT__: JSON.stringify(commit),
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    target: 'es2022',
  },
  server: {
    headers: {
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
    },
    proxy: Object.fromEntries(runtimePaths.map((path) => [path, { target: runtime, changeOrigin: true }])),
  },
});
