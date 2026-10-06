import { reactive } from 'vue';
import { loadLibrary, loadSession, type FirmwareStatus, type ImportProgress, type ImportResult } from './runtime';

// What the Library page shows: this browser's packages and the games a
// development server stages (none on a static host such as GitHub Pages).
export interface Game {
  title: string;
  app: string;
  name: string;
  version: string;
  icon: string | null; // object URL of sce_sys/icon0.png
  source: 'package' | 'server';
  bytes: number;
}

export const library = reactive({
  loading: true,
  storage: true,
  isStatic: true,
  games: [] as Game[],
  firmware: { files: 0, bytes: 0, system: false, fonts: false } as FirmwareStatus,
});

let icons: string[] = [];

export async function refreshLibrary() {
  const lib = await loadLibrary();
  library.storage = lib.storageSupported();
  const [packages, firmware, target] = await Promise.all([
    lib.listPackages(),
    lib.firmwareStatus(),
    loadSession().then((s) => s.resolveTarget({})).catch(() => null),
  ]);
  library.isStatic = target?.isStatic ?? true;
  const entries: { title: string; app: string; source: Game['source']; bytes: number }[] = [
    ...packages.map((p) => ({ title: p.title, app: p.app, source: 'package' as const, bytes: p.bytes })),
    ...(target?.stagedTitles ?? []).map((title) => ({ title, app: title, source: 'server' as const, bytes: 0 })),
  ];
  const games = await Promise.all(entries.map(async (entry) => {
    const info = await lib.titleInfo(entry.title, entry.app);
    return { ...entry, name: info.name, version: info.version, icon: info.icon ? URL.createObjectURL(info.icon) : null };
  }));
  for (const url of icons) URL.revokeObjectURL(url);
  icons = games.flatMap((game) => (game.icon ? [game.icon] : []));
  library.games = games.sort((a, b) => a.name.localeCompare(b.name) || a.source.localeCompare(b.source));
  library.firmware = firmware;
  library.loading = false;
}

// The import in progress (one at a time), shown by ImportDialog.
export const importJob = reactive({
  active: false,
  fileName: '',
  progress: null as ImportProgress | null,
  result: null as ImportResult | null,
  error: '',
});

export async function importFile(file: File, expect: 'game' | 'firmware', zrif: string) {
  if (importJob.active) return;
  Object.assign(importJob, { active: true, fileName: file.name, progress: null, result: null, error: '' });
  try {
    const lib = await loadLibrary();
    importJob.result = await lib.importFile(file, {
      expect,
      zrif: () => zrif,
      onProgress: (progress) => { importJob.progress = progress; },
    });
    await refreshLibrary();
  } catch (error) {
    importJob.error = error instanceof Error ? error.message : String(error);
  } finally {
    importJob.active = false;
  }
}
