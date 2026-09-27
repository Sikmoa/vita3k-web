// Runtime routes shared by limbo_serve.mjs and the Chromium probes, so a live
// session and a probe run load the same files from the same URLs: the built
// dist as deployed (GXM_RUNTIME_DIST, target vita3k_web_dist), except that
// the files of browser/web come from source so edits apply without a build.
import { readFile } from 'node:fs/promises';
import { readdirSync } from 'node:fs';
import { resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

export const runtimeRoot = resolve(process.env.GXM_RUNTIME_DIST || 'build/web64/dist');

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
    : file.endsWith('.html') ? 'text/html' : 'text/javascript';
  return { content: await readFile(file), type };
}
