# GXP → SPIR-V → WGSL shader translation

How Vita3K-web turns trusted PlayStation Vita GXP shader programs into WebGPU
WGSL **inside the browser**, with no native offline executable, no compilation
server, and no hardcoded shader table. The production Vita3K USSE recompiler
runs as Wasm; [Naga](https://github.com/gfx-rs/wgpu) (WASI build) validates and
lowers the SPIR-V to WGSL. Guests' actual GXP bytes are the only shader source
(`tools/native-tool/src/shaders/*.gxp`, public repo fixtures, no proprietary
assets).

Descriptor contract (WebGPU target, set → binding):

| set | binding | contents |
|-----|---------|----------|
| 0 | 0,1,2 | GXM fragment/vertex uniform buffers (`GxmRenderFragBufferBlock` etc.) |
| 2 | `2n` / `2n+1` | vertex texture `n` (texture / sampler) |
| 3 | `2n` / `2n+1` | fragment texture `n` (texture / sampler) |

Combined image+sampler descriptors from the Vulkan path are **not** usable by
Naga; `Target::SpirVWebGPU` (new) applies `shader::lower_webgpu_spirv()`
(`vita3k/shader/include/shader/webgpu_spirv.h`), a SPIR-V pass that splits every
combined image/sampler variable into separate texture (`2n`) and sampler
(`2n+1`) bindings and rebuilds `OpSampledImage` at each load. GXP texture
formats default to RGBA8 (`SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR`) unless the
caller passes 32 explicit `SceGxmTextureFormat` hints (16 vertex, 16 fragment).

## Dependencies

- The compiler: `browser/shaders/CMakeLists.txt` (translator sources, glslang
  SPIRV files and `external/fmt/src/format.cc`, built with
  `-DVITA3K_SHADER_SPIRV_ONLY`; GLSL/SPIRV-Cross output excluded).
- Naga (`naga-wasi-cli`, WASI `naga.wasm`) and `@bjorn3/browser_wasi_shim`:
  pinned in `browser/shaders/package-lock.json`.

## Build

The browser build produces all of it in `dist/shaders/` (target
`vita3k_web_dist`: the compiler as a separate wasm32 project, Naga and the
shim by `npm ci`). The native oracle is the same project without Emscripten:

```sh
cmake -S browser/shaders -B build/gxp-native -G Ninja && cmake --build build/gxp-native
```

Exports of the Wasm module (`gxp_compiler.mjs`, ES6, MODULARIZE):

```
_gxp_compile(u8* bytes, u32 size, u32* formatsOrNull) -> 0 ok / 1 error
_gxp_output_data() -> u32* SPIR-V words      _gxp_output_size() -> bytes
_gxp_error() -> char* (valid after failure)  _malloc/_free for argument buffers
```

## Translate a GXP file (native oracle + Naga CLI)

```sh
naga=build/web64/browser/shader_deps/node_modules/naga-wasi-cli/bin/naga.mjs
# Naga's WASI sandbox sees only the working directory: use relative paths.
build/gxp-native/gxp_compile tools/native-tool/src/shaders/texture_f.gxp build/texture_f.spv
node $naga --keep-coordinate-space build/texture_f.spv build/texture_f.wgsl
node $naga --bulk-validate build/texture_f.spv   # CPU-side validation, no browser
```

## In-browser translation (no native binary)

```js
import { createGXPShaderAdapter } from './gxp_shader_adapter.js'; // served from dist
const adapter = await createGXPShaderAdapter({
  compilerURL: './shaders/gxp_compiler.mjs',
  nagaURL: './shaders/naga.wasm',
  wasiShimURL: './shaders/wasi/index.js',
});
const { spirv, wgsl } = await adapter.translate(gxpBytes, { textureFormats });
```

Per call: fresh in-memory WASI filesystem, no host filesystem/environment
access, Naga instantiated from bytes. The compiler instance is synchronous and
not reentrant during a translation — wrap it in a Web Worker for untrusted
guest shaders and timeouts. The GXP decoder is not a hardened parser; only feed
it trusted program data.

## Verification

```sh
# Full browser test: translates all 7 repo GXP fixtures IN CHROMIUM, validates
# WGSL via createShaderModule/getCompilationInfo, rejects bad GXP, then renders
# real texture_v/texture_f pixels (4 distinct texels via guest UV attributes,
# then re-uploads the texture and verifies the output changes):
PLAYWRIGHT_MODULE_URL="file://$PWD/build/playwright/node_modules/playwright/index.mjs" \
  timeout -s KILL 60s node browser/tests/gxp_translation_smoke.mjs
# → {"checks":22,"browserTranslation":true,"texturedPixels":true,"guestExecution":false,...}
```

Note: on this host Playwright Chromium fails to
create shared memory inside the repo's AppArmor-limited `TMPDIR`, so browser
tests must run with `TMPDIR=/tmp` **and** rely on the
`ignoreDefaultArgs: ['--disable-dev-shm-usage']` launch option (already set in
the test files) to keep Chromium on `/dev/shm`.

Known differences from the Vulkan path (intentional, WebGPU only): separate
texture/sampler bindings (`2n`/`2n+1`), same SPIR-V 1.0 feature set otherwise.

The runtime consumes these bindings in `browser/web/gxm_scene.js`. Guest-side
execution of translated shaders is still false by design in these tests.
