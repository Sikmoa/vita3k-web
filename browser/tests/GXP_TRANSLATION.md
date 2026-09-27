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

## Dependency versions (exact)

| component | version | notes |
|---|---|---|
| Node.js | 22.22.1 | runs tests and WASI host side |
| Emscripten `em++` | 3.1.69 | wasm32; system install is `FROZEN_CACHE=True` — never reuse or clear the shared cache; the build script sets a private `EM_CACHE` |
| clang++ | 19.1.7 | native oracle build |
| naga-wasi-cli | 0.1.0 | `.limbo_work/gxm/node_modules/naga-wasi-cli` (WASI `naga.wasm`) |
| @bjorn3/browser_wasi_shim | 0.4.2 | pure-JS WASI host for running Naga in-browser |
| Playwright (test-only) | 1.63.0 | `build/playwright/node_modules/playwright`, Chromium 1243 |
| glslang SPIRV (in-tree `external/glslang`) | repo pinned | `SpvBuilder`, `SpvPostProcess`, `InReadableOrder`, `Logger`, `disassemble`, `doc` |
| {fmt} (in-tree `external/fmt`) | 12.2.0 | `src/format.cc` only |
| spdlog (in-tree, header use) | 1.17.0 | compile-time only |

## Build the standalone shader compiler

No CMake, GUI, renderer, guest CPU or HLE involved; sequential compile, ≤240 s
budget, private Emscripten cache under `.limbo_work/gxm/shader-wasm/emcache`.

```sh
cd <repo root>
export TMPDIR="$PWD/.limbo_work/tmp"
# Browser module (.limbo_work/gxm/shader-wasm/gxp_compiler.mjs + .wasm):
timeout -s KILL 240s bash browser/tests/build_gxp_compiler.sh wasm
# Native oracle (.limbo_work/gxm/gxp_compile):
timeout -s KILL 240s bash browser/tests/build_gxp_compiler.sh native
```

Inputs: `browser/tests/gxp_compile.cpp`, `vita3k/shader/src/**`,
`vita3k/gxm/src/{gxp,attributes,color,textures}.cpp`, the glslang SPIRV files
above, `external/fmt/src/format.cc` — built with `-DVITA3K_SHADER_SPIRV_ONLY`
(GLSL/SPIRV-Cross backend excluded). Objects are cached and rebuilt when any
`vita3k/shader/include`, `vita3k/gxm/include`, `vita3k/util/include` or
`external/glslang/SPIRV` header is newer.

Exports of the Wasm module (`gxp_compiler.mjs`, ES6, MODULARIZE):

```
_gxp_compile(u8* bytes, u32 size, u32* formatsOrNull) -> 0 ok / 1 error
_gxp_output_data() -> u32* SPIR-V words      _gxp_output_size() -> bytes
_gxp_error() -> char* (valid after failure)  _malloc/_free for argument buffers
```

## Translate a GXP file (native oracle + Naga CLI)

```sh
TRACY_NO_INVARIANT_CHECK=1 .limbo_work/gxm/gxp_compile \
  tools/native-tool/src/shaders/texture_f.gxp .limbo_work/gxm/texture_f.spv
node .limbo_work/gxm/node_modules/naga-wasi-cli/bin/naga.mjs \
  --keep-coordinate-space .limbo_work/gxm/texture_f.spv .limbo_work/gxm/texture_f.wgsl
# CPU-side validation of any .spv (no browser needed):
node .limbo_work/gxm/node_modules/naga-wasi-cli/bin/naga.mjs \
  --bulk-validate .limbo_work/gxm/texture_f.spv
```

## In-browser translation (no native binary)

```js
import { createGXPShaderAdapter } from '/browser/web/gxp_shader_adapter.js';
const adapter = await createGXPShaderAdapter({
  compilerURL: '/.limbo_work/gxm/shader-wasm/gxp_compiler.mjs',
  nagaURL: '/.limbo_work/gxm/node_modules/naga-wasi-cli/wasi/naga.wasm',
  wasiShimURL: '/.limbo_work/gxm/node_modules/@bjorn3/browser_wasi_shim/dist/index.js',
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

Legacy smoke `gxp_webgpu_smoke.mjs` (8 checks, color shaders only, native
oracle + Naga CLI) also passes. Note: on this host Playwright Chromium fails to
create shared memory inside the repo's AppArmor-limited `TMPDIR`, so browser
tests must run with `TMPDIR=/tmp` **and** rely on the
`ignoreDefaultArgs: ['--disable-dev-shm-usage']` launch option (already set in
the test files) to keep Chromium on `/dev/shm`.

Known differences from the Vulkan path (intentional, WebGPU only): separate
texture/sampler bindings (`2n`/`2n+1`), same SPIR-V 1.0 feature set otherwise.

The runtime consumes these bindings in `browser/web/gxm_scene.js`. Guest-side
execution of translated shaders is still false by design in these tests.
