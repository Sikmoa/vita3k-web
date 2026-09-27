// Runtime routes shared by limbo_serve.mjs and the Chromium probes, so a live
// session and a probe run load the same files from the same URLs: the built
// dist as deployed (GXM_RUNTIME_DIST, target vita3k_web_dist), except that
// the files of browser/web come from source so edits apply without a build.
import { readFile } from 'node:fs/promises';
import { existsSync, readdirSync, statSync } from 'node:fs';
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
    for (const name of readdirSync(dir).filter((file) => file.endsWith('.wasm'))) {
      const built = join(buildOutput, basename(name));
      if (existsSync(built) && statSync(built).mtimeMs > statSync(join(dir, name)).mtimeMs)
        throw new Error(`${join(dir, name)} is older than ${built}: build target vita3k_web_dist`);
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
    : file.endsWith('.html') ? 'text/html' : 'text/javascript';
  return { content: await readFile(file), type };
}
