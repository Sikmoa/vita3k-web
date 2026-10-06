// This site's private storage (the Origin Private File System), for the
// Files page: list, create, upload, download, rename and remove.

export interface Entry {
  name: string;
  kind: 'file' | 'directory';
  size: number; // files only
  modified: number; // files only (ms)
}

type DirectoryHandle = FileSystemDirectoryHandle & {
  entries(): AsyncIterableIterator<[string, FileSystemHandle]>;
};
type MovableHandle = FileSystemHandle & { move?: (name: string) => Promise<void> };

export const supported = () => typeof navigator !== 'undefined' && typeof navigator.storage?.getDirectory === 'function';

async function directory(path: string[], create = false): Promise<DirectoryHandle> {
  let dir = (await navigator.storage.getDirectory()) as DirectoryHandle;
  for (const part of path) dir = (await dir.getDirectoryHandle(part, { create })) as DirectoryHandle;
  return dir;
}

// Folders first, then by name.
export async function list(path: string[]): Promise<Entry[]> {
  const dir = await directory(path);
  const entries: Entry[] = [];
  for await (const [name, handle] of dir.entries()) {
    if (handle.kind === 'file') {
      const file = await (handle as FileSystemFileHandle).getFile();
      entries.push({ name, kind: 'file', size: file.size, modified: file.lastModified });
    } else {
      entries.push({ name, kind: 'directory', size: 0, modified: 0 });
    }
  }
  return entries.sort((a, b) => (a.kind === b.kind ? a.name.localeCompare(b.name, undefined, { numeric: true }) : a.kind === 'directory' ? -1 : 1));
}

// A name OPFS accepts and that names one entry.
export function validName(name: string) {
  return name.length > 0 && name.length <= 255 && name !== '.' && name !== '..' && !/[/\\\0]/.test(name);
}

async function exists(dir: DirectoryHandle, name: string) {
  for await (const [entry] of dir.entries()) if (entry === name) return true;
  return false;
}

export async function createFolder(path: string[], name: string) {
  if (!validName(name)) throw new Error(`“${name}” is not a valid name`);
  const dir = await directory(path);
  if (await exists(dir, name)) throw new Error(`“${name}” already exists here`);
  await dir.getDirectoryHandle(name, { create: true });
}

export async function remove(path: string[], name: string) {
  await (await directory(path)).removeEntry(name, { recursive: true });
}

// Streams a file into storage; relative ('a/b/c.bin') creates its folders.
export async function upload(path: string[], file: File, relative = file.name, onBytes?: (bytes: number) => void) {
  const parts = relative.split('/').filter(Boolean);
  if (!parts.length || !parts.every(validName)) throw new Error(`“${relative}” is not a valid name`);
  const dir = await directory([...path, ...parts.slice(0, -1)], true);
  const handle = await dir.getFileHandle(parts[parts.length - 1], { create: true });
  const writable = await handle.createWritable();
  let done = 0;
  await file.stream().pipeThrough(new TransformStream<Uint8Array, Uint8Array>({
    transform(chunk, controller) { done += chunk.byteLength; onBytes?.(done); controller.enqueue(chunk); },
  })).pipeTo(writable);
}

export async function getFile(path: string[], name: string) {
  return (await (await directory(path)).getFileHandle(name)).getFile();
}

// Renames in place: FileSystemHandle.move where the browser has it, else a
// copy and a removal.
export async function rename(path: string[], from: string, to: string) {
  if (from === to) return;
  if (!validName(to)) throw new Error(`“${to}” is not a valid name`);
  const dir = await directory(path);
  if (await exists(dir, to)) throw new Error(`“${to}” already exists here`);
  let handle: MovableHandle;
  try { handle = await dir.getFileHandle(from); } catch { handle = await dir.getDirectoryHandle(from); }
  if (typeof handle.move === 'function') {
    try { await handle.move(to); return; } catch { /* copy instead */ }
  }
  await copy(dir, handle, dir, to);
  await dir.removeEntry(from, { recursive: true });
}

async function copy(from: DirectoryHandle, handle: FileSystemHandle, into: DirectoryHandle, name: string) {
  if (handle.kind === 'file') {
    const file = await (handle as FileSystemFileHandle).getFile();
    const writable = await (await into.getFileHandle(name, { create: true })).createWritable();
    await file.stream().pipeTo(writable);
    return;
  }
  const source = handle as DirectoryHandle;
  const target = (await into.getDirectoryHandle(name, { create: true })) as DirectoryHandle;
  for await (const [child, childHandle] of source.entries()) await copy(source, childHandle, target, child);
}

// Total size and file count under a folder.
export async function folderSize(path: string[]): Promise<{ bytes: number; files: number }> {
  const walk = async (dir: DirectoryHandle): Promise<{ bytes: number; files: number }> => {
    let bytes = 0, files = 0;
    for await (const [, handle] of dir.entries()) {
      if (handle.kind === 'file') { bytes += (await (handle as FileSystemFileHandle).getFile()).size; files += 1; }
      else { const inner = await walk(handle as DirectoryHandle); bytes += inner.bytes; files += inner.files; }
    }
    return { bytes, files };
  };
  return walk(await directory(path));
}
