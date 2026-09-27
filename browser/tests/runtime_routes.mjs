// Runtime routes shared by limbo_serve.mjs and the Chromium probes, so a live
// session and a probe run load the same files from the same URLs:
//   /<file in browser/web>    the Worker and the modules it loads, from source
//   /shaders/...              GXP compiler, Naga and the WASI shim (GXM_SHADER_ASSETS)
//   anything else             the built runtime module (GXM_RUNTIME_DIST)
import { readFile } from 'node:fs/promises';
import { readdirSync } from 'node:fs';
import { resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

export const runtimeRoot = resolve(process.env.GXM_RUNTIME_DIST || 'build/web64/browser');
export const shaderRoot = resolve(process.env.GXM_SHADER_ASSETS || '.limbo_work/gxm');

// The Worker and every module it loads must come from one directory: the
// runtime imports gxm_scene.js relative to the Worker's own location.
const webRoot = fileURLToPath(new URL('../web', import.meta.url));
const webFiles = new Set(readdirSync(webRoot));
const shaderFiles = {
  '/shaders/gxp_compiler.mjs': resolve(shaderRoot, 'shader-wasm/gxp_compiler.mjs'),
  '/shaders/gxp_compiler.wasm': resolve(shaderRoot, 'shader-wasm/gxp_compiler.wasm'),
  '/shaders/naga.wasm': resolve(shaderRoot, 'node_modules/naga-wasi-cli/wasi/naga.wasm'),
};
const wasiRoot = resolve(shaderRoot, 'node_modules/@bjorn3/browser_wasi_shim/dist');

function inside(base, relativePath) {
  const file = resolve(base, relativePath);
  if (!file.startsWith(base + sep)) throw new Error(`path escapes ${base}`);
  return file;
}

function runtimeFile(path) {
  if (webFiles.has(path.slice(1))) return resolve(webRoot, path.slice(1));
  if (path in shaderFiles) return shaderFiles[path];
  if (path.startsWith('/shaders/wasi/')) return inside(wasiRoot, path.slice('/shaders/wasi/'.length));
  // ?memory=w64 makes the Worker request ./wasm64/<module>.js; the module
  // directory already IS the wasm64 flavor, so strip the prefix.
  return inside(runtimeRoot, `.${path.replace(/^\/wasm64\//, '/')}`);
}

// Content and type for a decoded request path; rejects when the file is
// missing or the path leaves its root.
export async function readRuntimeFile(path) {
  const file = runtimeFile(path);
  const type = file.endsWith('.wasm') ? 'application/wasm'
    : file.endsWith('.html') ? 'text/html' : 'text/javascript';
  return { content: await readFile(file), type };
}
