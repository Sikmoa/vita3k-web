// Decrypts an uploaded NoNpDrm dump (its sce_pfs/ image and its SELFs) into
// a title's persistent content, as desktop Vita3K does when it installs one.
// The module (dist/decrypt, browser/decrypt/decrypt.cpp) works one file per
// call so that each output file streams into browser storage (OPFS) through
// a synchronous access handle opened between calls: memory stays bounded by
// one SELF, whatever the game's size.
//
// In:  { type: 'decrypt', archive: File, key, appRoot: 'ux0/app/<id>/',
//        files: [{ path, size, entry }] } — packageContents() of the archive
// Out: { type: 'progress', done, total, path } … then
//      { type: 'done', files: [{ path, size }], bytes, decrypted, selfs } or
//      { type: 'error', message }
import { zipEntrySource, extractZipEntry } from './zip.js';
import { cacheClear, cacheGetFile, cacheOpenSync, cacheWriteFile, cacheWriteManifest, cacheWriteStream } from './content_cache.js';

const SELF = /(^|\/)eboot\.bin$|\.(suprx|skprx|self)$/i;
const TEMP_KEY = '_decrypt/_input'; // deflated inputs, inflated once

let modulePromise = null;
function loadModule() {
  modulePromise ??= import('./decrypt/vita3k_decrypt.mjs').then(({ default: create }) =>
    create({ print: () => {}, printErr: (text) => console.warn('[decrypt]', text) }));
  return modulePromise;
}

onmessage = async ({ data }) => {
  if (data?.type !== 'decrypt') return;
  try {
    postMessage({ type: 'done', ...await decrypt(data) });
  } catch (error) {
    try { await cacheClear(data.key); } catch {}
    postMessage({ type: 'error', message: String(error?.message ?? error) });
  } finally {
    try { await cacheClear(TEMP_KEY); } catch {}
  }
};

async function decrypt({ archive, key, appRoot, files }) {
  const M = await loadModule();
  const { FS } = M;
  const call = (name, ...args) => M.ccall(name, 'number', args.map((a) => typeof a === 'string' ? 'string' : 'number'), args);
  const error = () => M.UTF8ToString(M._vd_error());
  const total = files.reduce((sum, file) => sum + file.size, 0);
  let done = 0;
  const progress = (path, bytes = 0) => { done += bytes; postMessage({ type: 'progress', done, total, path }); };

  await cacheClear(key);
  // Input: the app directory, read lazily from the archive (WORKERFS reads
  // Blobs synchronously). Deflated entries are inflated into storage first.
  await cacheClear(TEMP_KEY);
  const appFiles = files.filter((file) => file.path.startsWith(appRoot));
  const blobs = [];
  for (const file of appFiles) {
    const relative = file.path.slice(appRoot.length);
    let source = await zipEntrySource(archive, file.entry);
    if (!(source instanceof Blob)) {
      progress(`Inflating ${relative}`);
      await cacheWriteStream(TEMP_KEY, relative, source);
      source = await cacheGetFile(TEMP_KEY, relative);
    }
    blobs.push({ name: relative, data: source });
  }
  const byPath = new Map(blobs.map((blob) => [blob.name, blob.data]));
  for (const dir of ['/src', '/out', '/self']) {
    try { FS.unmount(dir); } catch {}
    try { FS.rmdir(dir); } catch {}
  }
  FS.mkdir('/src');
  FS.mount(M.WORKERFS, { blobs }, '/src');
  FS.mkdir('/out');
  FS.mkdir('/self');

  const count = call('vd_open', '/src', '/src/sce_sys/package/work.bin');
  if (count < 0) throw new Error(error());
  const stored = [];
  let bytes = 0, decrypted = 0, selfs = 0;
  const store = (relative, size) => { stored.push({ path: appRoot + relative, size }); bytes += size; };
  try {
    for (let index = 0; index < count; ++index) {
      const [kind, sizeText, relative] = M.UTF8ToString(M._vd_entry(index)).split('\t');
      const size = Number(sizeText);
      if (kind === 'dir') continue;
      progress(relative);
      if (kind === 'empty') {
        await cacheWriteFile(key, appRoot + relative, new Uint8Array(0));
        store(relative, 0);
        continue;
      }
      const self = SELF.test(relative);
      if (kind === 'copy' && !self) {
        const source = byPath.get(relative);
        store(relative, await cacheWriteStream(key, appRoot + relative, source.stream()));
      } else if (self) {
        // SELFs are small: decrypt (or copy) into memory, then the SELF layer.
        const plain = '/self/plain', elf = '/self/elf';
        if (kind === 'copy') FS.writeFile(plain, new Uint8Array(await byPath.get(relative).arrayBuffer()));
        else {
          FS.mkdirTree(dirOf('/out/' + relative));
          if (call('vd_run', index, '/out') < 0) throw new Error(error());
          FS.rename('/out/' + relative, plain);
        }
        const result = call('vd_decrypt_self', plain, elf);
        if (result < 0) throw new Error(error());
        const out = FS.readFile(result ? elf : plain);
        await cacheWriteFile(key, appRoot + relative, out);
        store(relative, out.byteLength);
        selfs += result;
        for (const path of [plain, elf]) try { FS.unlink(path); } catch {}
      } else {
        store(relative, await decryptToStorage(M, call, error, index, key, appRoot, relative));
        decrypted += 1;
      }
      progress(relative, size);
    }
  } finally {
    call('vd_close');
    try { FS.unmount('/src'); } catch {}
  }

  // The archive's other files (trophy data, firmware it carries) as they are.
  for (const file of files.filter((file) => !file.path.startsWith(appRoot))) {
    progress(file.path);
    const payload = await extractZipEntry(archive, file.entry);
    await cacheWriteFile(key, file.path, payload);
    stored.push({ path: file.path, size: payload.byteLength });
    bytes += payload.byteLength;
    progress(file.path, file.size);
  }
  await cacheWriteManifest(key, stored);
  return { files: stored, bytes, decrypted, selfs };
}

const dirOf = (path) => path.slice(0, path.lastIndexOf('/')) || '/';

// One decrypted file straight into storage: a MEMFS node at the output path
// whose stream writes go to the file's synchronous access handle.
async function decryptToStorage(M, call, error, index, key, appRoot, relative) {
  const { FS } = M;
  const path = '/out/' + relative;
  const access = await cacheOpenSync(key, appRoot + relative);
  try {
    FS.mkdirTree(dirOf(path));
    FS.writeFile(path, new Uint8Array(0));
    const node = FS.lookupPath(path).node;
    node.stream_ops = {
      llseek(stream, offset, whence) {
        const position = whence === 1 ? stream.position + offset : whence === 2 ? access.getSize() + offset : offset;
        if (position < 0) throw new FS.ErrnoError(28);
        return position;
      },
      read(stream, buffer, offset, length, position) {
        return access.read(buffer.subarray(offset, offset + length), { at: position });
      },
      write(stream, buffer, offset, length, position) {
        return access.write(buffer.subarray(offset, offset + length), { at: position });
      },
    };
    if (call('vd_run', index, '/out') < 0) throw new Error(error());
    access.flush();
    return access.getSize();
  } finally {
    access.close();
    try { FS.unlink(path); } catch {}
  }
}
