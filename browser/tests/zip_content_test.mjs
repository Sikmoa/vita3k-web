// Unit tests for browser/web/zip.js and browser/web/content_cache.js.
// Fixture zips are built with python3 zipfile; OPFS is an in-memory fake.
// Run: node browser/tests/zip_content_test.mjs
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { readFileSync, mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const dir = join(tmpdir(), 'vita3k-zip-test');
mkdirSync(dir, { recursive: true });
const pkg = join(dir, 'game.zip');           // app/<id>/… (no device root)
const prefixed = join(dir, 'prefixed.zip');  // wrapper/ux0/app/<id>/…
const other = join(dir, 'other.zip');        // a different title entirely
const noapp = join(dir, 'noapp.zip');        // nothing that looks like a game
const withFirmware = join(dir, 'firmware.zip'); // whole Vita tree (ux0/ + os0/)
const bareZip = join(dir, 'bare.zip');          // bare .vpk: root is the app dir
const nosfoZip = join(dir, 'nosfo.zip');        // root files, but no param.sfo
const badsfoZip = join(dir, 'badsfo.zip');      // unreadable param.sfo
const expectPath = join(dir, 'expect.json');

const fixture = `
import json, random, zipfile
random.seed(42)
# A bare ux0 tree: the app plus firmware side by side (no device root for the
# app files, exactly how the Limbo dump and most homebrew zips are packed).
base = {
  'app/TEST00001/eboot.bin': random.randbytes(1024),
  'app/TEST00001/data/blob.bin': random.randbytes(2 * 1024 * 1024),
  'app/TEST00001/empty.txt': b'',
  'app/TEST00001/donn\\u00e9es.bin': random.randbytes(65536),
  'os0/kd/boot.bin': random.randbytes(4096),
  'os0/kd/stored.bin': random.randbytes(8192),
}
with zipfile.ZipFile(r'${pkg}', 'w', zipfile.ZIP_DEFLATED) as z:
    for name, data in base.items():
        method = zipfile.ZIP_STORED if name.endswith('stored.bin') else zipfile.ZIP_DEFLATED
        z.writestr(name, data, compress_type=method)
    z.writestr('__MACOSX/._junk', b'junk')
    z.writestr('app/TEST00001/.DS_Store', b'junk')
    z.writestr('../evil.txt', b'evil')
with zipfile.ZipFile(r'${prefixed}', 'w', zipfile.ZIP_DEFLATED) as z:
    for name, data in base.items():
        z.writestr('pkg/' + name, data)
with zipfile.ZipFile(r'${other}', 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('app/OTHER0001/eboot.bin', random.randbytes(555))
with zipfile.ZipFile(r'${noapp}', 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('readme.txt', b'nothing to see')
    z.writestr('assets/thing.bin', random.randbytes(64))
with zipfile.ZipFile(r'${withFirmware}', 'w', zipfile.ZIP_DEFLATED) as z:
    for name, data in base.items():
        z.writestr(name if name.startswith('os0/') else 'ux0/' + name, data)
    z.writestr('vs0/sys/external/libhttp.suprx', random.randbytes(2048))
def make_sfo(title_id):
    import struct
    pairs = [(b'TITLE_ID', title_id)]
    n = len(pairs)
    key_start = 20 + 16 * n
    keys = b''.join(k + b'\\0' for k, _ in pairs)
    data_start = key_start + len(keys)
    out = [struct.pack('<4sIIII', b'PSF\\0', 0x101, key_start, data_start, n)]
    off, data = 0, b''
    for i, (k, v) in enumerate(pairs):
        out.append(struct.pack('<HHIII', sum(len(kk) + 1 for kk, _ in pairs[:i]), 0x0004, len(v), len(v), off))
        data += v
        off += len(v)
    return b''.join(out) + keys + data
with zipfile.ZipFile(r'${bareZip}', 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('sce_sys/param.sfo', make_sfo(b'BARE00001\\0'))
    z.writestr('eboot.bin', random.randbytes(512))
    z.writestr('data/x.bin', random.randbytes(1024))
with zipfile.ZipFile(r'${nosfoZip}', 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('eboot.bin', random.randbytes(512))
with zipfile.ZipFile(r'${badsfoZip}', 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('sce_sys/param.sfo', b'not a param sfo file at all........')
    z.writestr('eboot.bin', random.randbytes(512))
with open(r'${expectPath}', 'w') as f:
    f.write(json.dumps([{'n': n, 's': len(d), 'head': list(d[:8])} for n, d in base.items()]))
`;
const run = spawnSync('python3', ['-c', fixture], { encoding: 'utf8' });
if (run.status !== 0) throw new Error('python fixture failed: ' + run.stderr);
const expected = JSON.parse(readFileSync(expectPath, 'utf8'));

const zip = await import('../web/zip.js');
const cache = await import('../web/content_cache.js');

// --- zip reader ---
assert.equal(zip.zipCrc32(new TextEncoder().encode('123456789')), 0xCBF43926);
const blob = new Blob([readFileSync(pkg)]);
const entries = await zip.listZipEntries(blob);
const byName = new Map(entries.map((e) => [e.name, e]));
assert.equal(byName.get('os0/kd/stored.bin').method, 0);
assert.equal(byName.get('app/TEST00001/data/blob.bin').method, 8);
for (const { n, s, head } of expected) {
  const bytes = await zip.extractZipEntry(blob, byName.get(n));
  assert.equal(bytes.byteLength, s, n);
  assert.deepEqual(Array.from(bytes.subarray(0, 8)), head, n);
}
// Path safety and junk.
assert.equal(zip.normalizeEntryPath('../evil.txt'), null);
assert.equal(zip.normalizeEntryPath('/abs.txt'), null);
assert.equal(zip.normalizeEntryPath('a/./b'), null);
assert.equal(zip.normalizeEntryPath('a//b'), null);
assert.equal(zip.normalizeEntryPath('__MACOSX/._junk'), null);
assert.equal(zip.normalizeEntryPath('app/TEST00001/.DS_Store'), null);
await assert.rejects(zip.listZipEntries(new Blob([new Uint8Array(10)])), /not a zip archive/);
await assert.rejects(zip.listZipEntries(new Blob([readFileSync(pkg).subarray(0, 100)])), /not a zip archive|truncated|corrupt/);

// --- the package names its own title ---
const contents = await cache.packageContents(blob);
assert.equal(contents.title, 'TEST00001');
assert.equal(contents.app, 'TEST00001');
const shipped = new Map(contents.files.map((f) => [f.path, f.size]));
assert.equal(shipped.get('ux0/app/TEST00001/eboot.bin'), 1024);
assert.equal(shipped.get('ux0/app/TEST00001/data/blob.bin'), 2 * 1024 * 1024);
assert.equal(shipped.get('os0/kd/boot.bin'), 4096, 'firmware in a package keeps its device root');
assert.equal(shipped.get('os0/kd/stored.bin'), 8192);
assert.equal(shipped.has('__MACOSX/._junk'), false);
assert.equal(shipped.has('../evil.txt'), false);
assert.equal(shipped.has('ux0/app/TEST00001/.DS_Store'), false, 'junk is dropped from the shipped set');

// A wrapper folder around ux0/ is stripped the same way.
const prefixedContents = await cache.packageContents(new Blob([readFileSync(prefixed)]));
assert.equal(prefixedContents.title, 'TEST00001');
assert.deepEqual([...prefixedContents.files.map((f) => f.path)].sort(), [...shipped.keys()].sort());

// A different title is read from its own directory; whole-tree archives keep
// both ux0/ and os0/ roots.
assert.equal((await cache.packageContents(new Blob([readFileSync(other)]))).title, 'OTHER0001');
const tree = await cache.packageContents(new Blob([readFileSync(withFirmware)]));
assert.ok(tree.files.some((f) => f.path === 'vs0/sys/external/libhttp.suprx'));
assert.ok(tree.files.some((f) => f.path === 'ux0/app/TEST00001/eboot.bin'));
assert.ok(!tree.files.some((f) => f.path.startsWith('ux0/os0/')), 'device roots are not re-rooted under ux0');

// Not a game package at all.
await assert.rejects(cache.packageContents(new Blob([readFileSync(noapp)])), /no game found/);
// Bare .vpk: no app/ folder, the title id comes from sce_sys/param.sfo and
// the root becomes the title's ux0/app/<id> tree.
const bareContents = await cache.packageContents(new Blob([readFileSync(bareZip)]));
assert.equal(bareContents.title, 'BARE00001');
assert.deepEqual(bareContents.files.map((f) => f.path).sort(),
  ['ux0/app/BARE00001/data/x.bin', 'ux0/app/BARE00001/eboot.bin', 'ux0/app/BARE00001/sce_sys/param.sfo']);
// Without a readable param.sfo there is nothing to name the title after.
await assert.rejects(cache.packageContents(new Blob([readFileSync(nosfoZip)])), /no game found/);
await assert.rejects(cache.packageContents(new Blob([readFileSync(badsfoZip)])), /cannot read the title id/);
await assert.rejects(cache.packageContents(new Blob([new Uint8Array(64)])), /not a zip archive/);

// --- OPFS cache ---
function makeFakeOPFS() {
  const files = new Map(), dirs = new Set(['']);
  const handleFor = (p) => ({
    kind: 'file',
    async getFile() {
      if (!files.has(p)) throw new DOMException('missing', 'NotFoundError');
      const bytes = files.get(p);
      return {
        async arrayBuffer() { return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength); },
        async text() { return new TextDecoder().decode(bytes); },
      };
    },
    async createWritable() {
      let buf = new Uint8Array(0);
      return {
        async write(data) {
          buf = data instanceof Uint8Array ? data.slice() : new TextEncoder().encode(String(data?.buffer ?? data));
        },
        async close() { files.set(p, buf); },
      };
    },
  });
  const makeDir = (path) => ({
    kind: 'directory',
    async getDirectoryHandle(name, { create } = {}) {
      const p = path ? `${path}/${name}` : name;
      if (!dirs.has(p)) {
        if (!create) throw new DOMException('missing', 'NotFoundError');
        dirs.add(p);
      }
      return makeDir(p);
    },
    async getFileHandle(name, { create } = {}) {
      const p = path ? `${path}/${name}` : name;
      if (!files.has(p)) {
        if (!create) throw new DOMException('missing', 'NotFoundError');
        files.set(p, new Uint8Array(0));
      }
      return handleFor(p);
    },
    async *entries() {
      const prefix = path ? path + '/' : '';
      const names = new Set();
      for (const key of [...files.keys(), ...dirs]) {
        if (!key.startsWith(prefix) || key === path) continue;
        names.add(key.slice(prefix.length).split('/')[0]);
      }
      for (const name of [...names].sort()) {
        const child = prefix + name;
        yield [name, dirs.has(child) ? makeDir(child) : handleFor(child)];
      }
    },
    async removeEntry(name, { recursive } = {}) {
      const p = path ? `${path}/${name}` : name;
      if (files.has(p)) { files.delete(p); return; }
      if (dirs.has(p)) {
        const kids = [...files.keys(), ...dirs].filter((k) => k !== p && k.startsWith(p + '/'));
        if (kids.length && !recursive) throw new DOMException('not empty', 'InvalidModificationError');
        for (const k of [...files.keys()]) if (k === p || k.startsWith(p + '/')) files.delete(k);
        for (const k of [...dirs]) if (k === p || k.startsWith(p + '/')) dirs.delete(k);
        return;
      }
      throw new DOMException('missing', 'NotFoundError');
    },
  });
  return { files, async getDirectory() { return makeDir(''); } };
}
Object.defineProperty(globalThis, 'navigator', { value: { storage: makeFakeOPFS() }, configurable: true });
assert.equal(cache.contentCacheSupported(), true);
assert.equal(cache.cacheKeyFor('PCSE00268', 'PCSE00268'), 'PCSE00268/PCSE00268');

// Unpacking stores every shipped file under the package's own key.
const key = cache.cacheKeyFor(contents.title, contents.app);
const progress = [];
const result = await cache.unpackPackageToCache(blob, key,
  (done, total, path) => progress.push([done, total, path]));
assert.equal(result.title, 'TEST00001');
assert.equal(result.files, contents.files.length);
assert.equal(progress.length, contents.files.length);
const manifest = await cache.cacheReadManifest(key);
assert.equal(manifest.files.length, contents.files.length);
for (const file of contents.files) {
  const bytes = await cache.cacheReadFile(key, file.path);
  assert.ok(bytes && bytes.byteLength === file.size, file.path);
}
assert.equal(await cache.cacheReadFile(key, 'ux0/app/TEST00001/nope.bin'), null);

// The stored manifest is what a boot trusts: path+size index, nothing else.
assert.equal(cache.cacheIndexFor(manifest.files, manifest.files).size, contents.files.length);
assert.equal(cache.cacheIndexFor(manifest.files, [{ path: 'ux0/app/TEST00001/eboot.bin', size: 999 }]).size, 0);
assert.equal(cache.cacheIndexFor(null, manifest.files).size, 0);
// Server files carry a version: a stored copy needs the same one (restaged files refetch).
assert.equal(cache.cacheIndexFor([{ path: 'a', size: 4, version: 7 }], [{ path: 'a', size: 4, version: 7 }]).size, 1);
assert.equal(cache.cacheIndexFor([{ path: 'a', size: 4, version: 7 }], [{ path: 'a', size: 4, version: 8 }]).size, 0);
assert.equal(cache.cacheIndexFor([{ path: 'a', size: 4 }], [{ path: 'a', size: 4, version: 8 }]).size, 0);
assert.equal(cache.cacheIndexFor([{ path: 'a', size: 4, version: 7 }], [{ path: 'a', size: 4 }]).size, 1);
// The bare layout unpacks under the title id from param.sfo (needs OPFS).
assert.equal((await cache.unpackPackageToCache(new Blob([readFileSync(bareZip)]),
  cache.cacheKeyFor('BARE00001', 'BARE00001'), () => {})).files, 3);
assert.ok(await cache.cacheReadFile(cache.cacheKeyFor('BARE00001', 'BARE00001'), 'ux0/app/BARE00001/eboot.bin'));

// The picker lists what this browser holds.
const listed = await cache.listCachedTitles();
assert.deepEqual(listed.map((entry) => [entry.title, entry.files]),
  [['BARE00001', 3], ['TEST00001', contents.files.length]]);

// Each title keeps its own namespace: a second package under a different
// key leaves the first untouched, and both show up in the picker's list.
const otherBlob = new Blob([readFileSync(other)]);
const otherKey = cache.cacheKeyFor('OTHER0001', 'OTHER0001');
await cache.unpackPackageToCache(otherBlob, otherKey, () => {});
assert.ok(await cache.cacheReadFile(key, 'ux0/app/TEST00001/eboot.bin'), 'other title did not clobber this one');
assert.ok(await cache.cacheReadFile(otherKey, 'ux0/app/OTHER0001/eboot.bin'));
assert.deepEqual((await cache.listCachedTitles()).map((entry) => entry.title), ['BARE00001', 'OTHER0001', 'TEST00001']);

// Re-uploading under the same key replaces its content: no stale files.
await cache.unpackPackageToCache(otherBlob, key, () => {});
assert.equal(await cache.cacheReadFile(key, 'ux0/app/TEST00001/eboot.bin'), null, 'replaced by the new package');
assert.equal(await cache.cacheReadFile(key, 'ux0/app/TEST00001/data/blob.bin'), null);
assert.ok(await cache.cacheReadFile(key, 'ux0/app/OTHER0001/eboot.bin'));

// A corrupt/truncated archive fails before touching what is already stored.
const before = await cache.cacheReadManifest(key);
await assert.rejects(
  cache.unpackPackageToCache(new Blob([readFileSync(pkg).subarray(0, 2048)]), key, () => {}),
  /not a zip archive|truncated|corrupt/);
assert.deepEqual(await cache.cacheReadManifest(key), before, 'a rejected upload changes nothing');

console.log(`PASS: zip reader + package titles (${contents.files.length} files, ` +
  `${(result.bytes / 1048576).toFixed(1)} MiB stored under ${key})`);
