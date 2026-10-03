// Runtime routes shared by limbo_serve.mjs and the Chromium probes, so a live
// session and a probe run load the same files from the same URLs: the built
// dist as deployed (GXM_RUNTIME_DIST, target vita3k_web_dist), except that
// the files of browser/web come from source so edits apply without a build.
import { readFile } from 'node:fs/promises';
import { createReadStream, existsSync, readFileSync, readdirSync, statSync } from 'node:fs';
import { basename, join, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

export const runtimeRoot = resolve(process.env.GXM_RUNTIME_DIST || 'build/web64/dist');

// Fail before serving a missing or stale dist: the page would otherwise stop
// with an opaque error, or a run would measure old code.
if (!existsSync(join(runtimeRoot, 'shaders/gxp_compiler.mjs')))
  throw new Error(`${runtimeRoot} is not a built dist: build target vita3k_web_dist or set GXM_RUNTIME_DIST`);
{
  const buildOutput = resolve(runtimeRoot, '../browser');
  for (const dir of [runtimeRoot, join(runtimeRoot, 'wasm64')]) {
    if (!existsSync(dir)) continue;
    // Each module is a generated .js/.wasm pair; either can be the stale one
    // (a --post-js change can leave the .wasm identical).
    const modules = readdirSync(dir).filter((file) => file.endsWith('.wasm'));
    for (const name of modules.flatMap((file) => [file, file.replace(/\.wasm$/, '.js')])) {
      const built = join(buildOutput, basename(name));
      // copy_if_different keeps an identical copy's older mtime: compare bytes.
      if (existsSync(built) && statSync(built).mtimeMs > statSync(join(dir, name)).mtimeMs
          && !readFileSync(built).equals(readFileSync(join(dir, name))))
        throw new Error(`${join(dir, name)} differs from the newer ${built}: build target vita3k_web_dist`);
    }
  }
}

const webRoot = fileURLToPath(new URL('../web', import.meta.url));
const webFiles = new Set(readdirSync(webRoot));

function runtimeFile(path) {
  if (webFiles.has(path.slice(1))) return resolve(webRoot, path.slice(1));
  const file = resolve(runtimeRoot, `.${path}`);
  if (!file.startsWith(runtimeRoot + sep)) throw new Error(`path escapes ${runtimeRoot}`);
  return file;
}

// Content and type for a decoded request path; rejects when the file is
// missing or the path leaves its root.
export async function readRuntimeFile(path) {
  const file = runtimeFile(path);
  const type = file.endsWith('.wasm') ? 'application/wasm'
    : file.endsWith('.html') ? 'text/html'
    : file.endsWith('.css') ? 'text/css' : 'text/javascript';
  return { content: await readFile(file), type };
}

// Staged Vita content for the Worker's stage-files: every file under `stage`
// plus the title patches shipped with the port (browser/patches), staged as
// <vita fs>/patch/ like desktop Vita3K's patch directory. A patch of the same
// name in the stage's own patch/ wins. Each entry keeps its source file for
// readStageFile; the manifest sent to the page omits it.
const patchRoot = fileURLToPath(new URL('../patches', import.meta.url));
export async function stageFiles(stage) {
  const files = new Map();
  const walk = async (directory, prefix) => {
    for (const entry of readdirSync(directory, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
      const full = resolve(directory, entry.name);
      const path = prefix + entry.name;
      if (entry.isDirectory()) await walk(full, path + '/');
      else if (entry.isFile() && !files.has(path)) {
        const stat = statSync(full);
        files.set(path, { path, size: stat.size, version: Math.floor(stat.mtimeMs), source: full });
      }
    }
  };
  await walk(stage, '');
  if (existsSync(patchRoot)) await walk(patchRoot, 'patch/');
  return [...files.values()];
}
// `version` (modification time) lets a browser content cache tell a restaged
// file from its stored copy of the same size.
export const stageManifest = (files) => files.map(({ path, size, version }) => ({ path, size, version }));
export async function readStageFile(files, path) {
  const entry = files.find((file) => file.path === path);
  if (!entry) throw Object.assign(new Error(`not staged: ${path}`), { code: 'ENOENT' });
  return readFile(entry.source);
}

// Streams a staged file, honouring a single `Range: bytes=a-b` (206) and HEAD.
// The Worker reads large files (a retail title's multi-GiB archives) through
// ranged requests instead of staging them whole (worker.js stageLazyFile).
export async function sendStageFile(req, res, files, path) {
  const entry = files.find((file) => file.path === path);
  if (!entry) throw Object.assign(new Error(`not staged: ${path}`), { code: 'ENOENT' });
  const size = entry.size;
  const headers = { 'Content-Type': 'application/octet-stream', 'Accept-Ranges': 'bytes', 'Cache-Control': 'no-store' };
  let start = 0, end = size - 1, status = 200;
  const range = /^bytes=(\d*)-(\d*)$/.exec(req.headers.range || '');
  if (range && (range[1] || range[2])) {
    if (range[1]) { start = Number(range[1]); if (range[2]) end = Math.min(Number(range[2]), size - 1); }
    else start = Math.max(0, size - Number(range[2]));
    if (start > end || start >= size) {
      res.writeHead(416, { ...headers, 'Content-Range': `bytes */${size}` });
      return res.end();
    }
    status = 206;
    headers['Content-Range'] = `bytes ${start}-${end}/${size}`;
  }
  headers['Content-Length'] = size ? end - start + 1 : 0;
  res.writeHead(status, headers);
  if (req.method === 'HEAD' || !size) return res.end();
  await new Promise((done, fail) => {
    const stream = createReadStream(entry.source, { start, end });
    stream.on('error', fail);
    res.on('close', done);
    stream.pipe(res);
  });
}
