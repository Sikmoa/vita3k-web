// A game's log in storage (Settings → Save logs to files): one file per run
// under vita3k-logs/, readable on the Files page. Lines are appended in
// batches, so a long run costs a write every few seconds; only the newest
// KEEP files stay.

const FOLDER = 'vita3k-logs';
const KEEP = 20;
const FLUSH_MS = 2000;

type Writable = FileSystemWritableFileStream & { seek(position: number): Promise<void> };

export interface LogFile {
  add(line: string): void;
  close(): Promise<void>;
  readonly name: string;
}

const stamp = (date: Date) => {
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${date.getFullYear()}${pad(date.getMonth() + 1)}${pad(date.getDate())}-${pad(date.getHours())}${pad(date.getMinutes())}${pad(date.getSeconds())}`;
};

export async function openLogFile(title: string, header: string[]): Promise<LogFile | null> {
  if (typeof navigator.storage?.getDirectory !== 'function') return null;
  const root = await navigator.storage.getDirectory();
  const folder = await root.getDirectoryHandle(FOLDER, { create: true });
  await prune(folder);
  const name = `${title.replace(/[^A-Za-z0-9_-]+/g, '_') || 'game'}-${stamp(new Date())}.log`;
  const handle = await folder.getFileHandle(name, { create: true });
  let pending: string[] = [...header, ''];
  let size = 0;
  let writing: Promise<void> = Promise.resolve();
  let closed = false;
  const encoder = new TextEncoder();

  // Appends what is pending: a writable keeping the file's data, at its end.
  const flush = () => {
    if (!pending.length) return writing;
    const bytes = encoder.encode(pending.join('\n') + '\n');
    pending = [];
    writing = writing.then(async () => {
      const writable = (await handle.createWritable({ keepExistingData: true })) as Writable;
      await writable.seek(size);
      await writable.write(bytes);
      await writable.close();
      size += bytes.byteLength;
    }).catch(() => { /* storage full or gone: the log stays in the dialog */ });
    return writing;
  };
  const timer = setInterval(flush, FLUSH_MS);
  addEventListener('pagehide', flush);
  return {
    name: `${FOLDER}/${name}`,
    add(line) { if (!closed) pending.push(line); },
    async close() {
      if (closed) return;
      closed = true;
      clearInterval(timer);
      removeEventListener('pagehide', flush);
      await flush();
    },
  };
}

// Keeps the newest KEEP - 1 logs, making room for the next one.
async function prune(folder: FileSystemDirectoryHandle) {
  const logs: { name: string; modified: number }[] = [];
  for await (const [name, handle] of (folder as FileSystemDirectoryHandle & { entries(): AsyncIterable<[string, FileSystemHandle]> }).entries()) {
    if (handle.kind === 'file' && name.endsWith('.log'))
      logs.push({ name, modified: (await (handle as FileSystemFileHandle).getFile()).lastModified });
  }
  logs.sort((a, b) => b.modified - a.modified);
  for (const log of logs.slice(KEEP - 1)) await folder.removeEntry(log.name).catch(() => {});
}
