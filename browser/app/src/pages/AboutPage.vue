<script setup lang="ts">
import { onMounted, reactive } from 'vue';
import { loadCapabilities, loadLibrary, formatBytes } from '../runtime';
import AppLogo from '../components/AppLogo.vue';

const commit = __APP_COMMIT__;
interface Check { name: string; detail: string; ok: boolean | null; why: string }
const device = reactive<{ checks: Check[]; loading: boolean }>({ checks: [], loading: true });

onMounted(async () => {
  const caps = (await loadCapabilities()).detectBrowserCapabilities();
  const nav = navigator as Navigator & { deviceMemory?: number; gpu?: { requestAdapter(): Promise<{ info?: Record<string, string>; isFallbackAdapter?: boolean } | null> } };
  let gpu = { ok: false as boolean | null, detail: 'Not exposed by this browser' };
  if (nav.gpu) {
    try {
      const adapter = await nav.gpu.requestAdapter();
      const info = adapter?.info ?? {};
      const name = [info.vendor, info.architecture, info.description].filter(Boolean).join(' · ') || 'Adapter available';
      gpu = !adapter ? { ok: false, detail: 'No adapter: the GPU is blocked' }
        : adapter.isFallbackAdapter || /swiftshader/i.test(name) ? { ok: null, detail: `${name} (CPU fallback, slow)` } : { ok: true, detail: name };
    } catch (error) { gpu = { ok: false, detail: String(error) }; }
  }
  const estimate = await (await loadLibrary()).storageEstimate();
  device.checks = [
    { name: 'WebGPU', ...gpu, why: 'Draws the games.' },
    { name: 'WebAssembly Memory64', ok: caps.memory64, detail: caps.memory64 ? 'Supported' : 'Not supported', why: 'The emulator\'s 64-bit memory.' },
    { name: 'Cross-origin isolation', ok: caps.crossOriginIsolated && caps.sharedArrayBuffer,
      detail: caps.crossOriginIsolated ? 'SharedArrayBuffer available' : 'Not isolated', why: 'The multithreaded runtime.' },
    { name: 'Persistent storage', ok: caps.opfs,
      detail: caps.opfs ? (estimate?.quota ? `${formatBytes(estimate.usage ?? 0)} used of ${formatBytes(estimate.quota)}` : 'Available') : 'Unavailable',
      why: 'Keeps games, firmware and saves.' },
    { name: 'AudioWorklet', ok: caps.audioWorklet, detail: caps.audioWorklet ? 'Supported' : 'Not supported', why: 'Sound in the multithreaded runtime.' },
    { name: 'Gamepad API', ok: caps.gamepad, detail: caps.gamepad ? 'Supported' : 'Not supported', why: 'Controllers.' },
    { name: 'CPU threads', ok: null, detail: String(navigator.hardwareConcurrency || '—'), why: 'Logical cores this browser reports.' },
    { name: 'Device memory', ok: null, detail: nav.deviceMemory ? `${nav.deviceMemory} GB or more` : 'Not reported', why: 'As the browser reports it.' },
  ];
  device.loading = false;
});
</script>

<template>
  <div class="max-w-3xl mx-auto px-4 sm:px-8 pt-2 lg:pt-10 pb-16">
    <section class="card flex flex-col sm:flex-row items-center sm:items-start gap-6 p-6 sm:p-8 text-center sm:text-left">
      <AppLogo :size="88" />
      <div class="flex-1">
        <h1 class="m-0 text-3xl font-normal">Vita3K Web</h1>
        <p class="text-on-surface-variant leading-relaxed mt-2 mb-4">
          A web port of Vita3K, the PlayStation Vita emulator, running in your browser on WebAssembly and WebGPU.
          Your firmware, games and saves stay in this browser's storage: nothing is uploaded anywhere.
        </p>
        <div class="flex flex-wrap justify-center sm:justify-start gap-2">
          <a class="btn-tonal no-underline" href="https://github.com/notwindstone/vita3k-web" target="_blank" rel="noreferrer"><span class="i-lucide-github" />Source</a>
          <a class="btn-text no-underline" href="https://vita3k.org" target="_blank" rel="noreferrer"><span class="i-lucide-external-link" />Vita3K</a>
          <a class="btn-text no-underline" href="https://vita3k.org/compatibility.html" target="_blank" rel="noreferrer"><span class="i-lucide-list-checks" />Compatibility</a>
        </div>
        <p v-if="commit" class="text-xs text-outline mt-4 mb-0">Build {{ commit }}</p>
      </div>
    </section>

    <h2 class="mt-8 mb-3 mx-1 text-sm font-medium text-primary">This device</h2>
    <div class="card divide-y divide-surface">
      <div v-if="device.loading" class="p-5 text-sm text-on-surface-variant">Checking…</div>
      <div v-for="check in device.checks" :key="check.name" class="flex items-center gap-4 px-5 py-3.5">
        <span
          class="flex items-center justify-center w-9 h-9 rounded-full shrink-0"
          :class="check.ok === true ? 'bg-primary-container text-on-primary-container'
            : check.ok === false ? 'bg-error-container text-on-error-container' : 'bg-secondary-container text-on-secondary-container'"
        >
          <span :class="check.ok === true ? 'i-lucide-check' : check.ok === false ? 'i-lucide-x' : 'i-lucide-info'" />
        </span>
        <div class="flex-1 min-w-0">
          <div class="text-base">{{ check.name }}</div>
          <div class="text-xs text-outline">{{ check.why }}</div>
        </div>
        <div class="text-sm text-on-surface-variant text-right max-w-[45%] break-words">{{ check.detail }}</div>
      </div>
    </div>

    <h2 class="mt-8 mb-3 mx-1 text-sm font-medium text-primary">Credits</h2>
    <div class="card p-5 text-sm text-on-surface-variant leading-relaxed">
      Built on <a class="text-primary" href="https://github.com/Vita3K/Vita3K" target="_blank" rel="noreferrer">Vita3K</a> by the Vita3K team,
      with psvpfstools (PFS decryption), OpenSSL, FFmpeg, Dynarmic, Naga and many more. Licensed under the GNU GPL v2 or later.
      PlayStation and PS Vita are trademarks of Sony Interactive Entertainment; this project is not affiliated with Sony.
      Bring your own firmware and games.
    </div>
  </div>
</template>
