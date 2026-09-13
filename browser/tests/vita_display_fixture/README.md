# Genuine VitaSDK CPU-rendered display fixture

A real VitaSDK homebrew that renders an unmistakably animated 960x544 image
**on the CPU** (no vita2d, no SceGxm, no graphics SDK) into a framebuffer
allocated with `sceKernelAllocMemBlock`, presents it with
`sceDisplaySetFrameBuf`, paces itself with `sceDisplayWaitVblankStart`, and
after a fixed number of frames exits with a distinctive status.

This fixture exists so the browser runtime can be validated end to end:
animated pixels on the HTML canvas come from a genuine guest binary, and the
frame index can be read back out of the framebuffer contents.

## Screen and framebuffer

| Property | Value |
| --- | --- |
| Resolution | 960x544 (Vita native) |
| Pitch | 960 pixels (`SceDisplayFrameBuf.pitch` is in pixels) |
| Pixel format | `SCE_DISPLAY_PIXELFORMAT_A8B8G8R8` (the only format Vita3K's `_sceDisplaySetFrameBuf` accepts) |
| Pixel word | bits 31:24 = A, 23:16 = B, 15:8 = G, 7:0 = R (little-endian `uint32_t`) |
| Memory | `sceKernelAllocMemBlock("vita3k_display_fixture_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 960*544*4, NULL)` |
| Sync mode | `SCE_DISPLAY_SETBUF_IMMEDIATE` |

The resolution/pitch values also satisfy `_sceDisplaySetFrameBuf`'s minimum
checks (width >= 480, height >= 272, pitch >= 480, pitch >= width,
`size == sizeof(SceDisplayFrameBuf)`).

## Animation

Rendering is split for interpreter-speed reasons (see `bench_report.md`):
the **gradient background is rendered once** with `f = 0` at startup, then
each frame redraws only the regions that change. Per frame:

1. **Background restore** — the previous frame's rectangle bounding box is
   restored from a saved pristine copy of the frame-0 gradient.
2. **Filled moving rectangle** — 192x108, top-left at
   `((13f)&511, (7f)&255)`, magenta-ish border (`0xFF3020FF`), green-ish fill
   (`0xFF40C000`), wrapping over the screen (no clipping needed: the wrap
   limits keep the box inside 960x544).
3. **Frame-code pixels** (drawn last, always on top):

The static background keeps every frame's checksum distinct (the moving box
and markers change), while cutting guest cost from ~32M instructions/frame
(full-screen gradient) to ~1.6M (~0.3 s/frame on the optimized interpreter,
≈3 fps; 600 frames ≈ 3 minutes). On a real Vita this is of course immediate.

### Frame-encoding scheme (validator-facing)

Given the 32-bit little-endian pixel word at `(x, y)`
(`fb[y*960 + x]`):

- **Direct counter — pixel (0,0):**
  `fb[0] == 0xFF000000 | (f & 0x00FFFFFF)` — the frame index is recoverable as
  `f = fb[0] & 0x00FFFFFF` (for `f < 2^24`, and the default frame count is
  600).
- **Bit strip — pixels (1..32, 0):** pixel `(1+j, 0)` is `0xFFFFFFFF` (white)
  when bit `j` of `f` is set, `0xFF000000` (black) otherwise. Reconstruct:
  `f = sum over j of ((fb[1+j] >> 8) & 1) << j` (any non-black value counts
  as a set bit). This is a redundant encoding that survives format conversions
  that mangle color channels.
- **Liveness heartbeat — pixel (959, 543):** alternates `0xFF00FFFF` /
  `0xFFFF0000` every frame, so "two captured frames differ" holds even for
  captures that miss the code strip.

## Main loop and exit codes

```
for f in 0 .. VITA_DISPLAY_FRAME_COUNT-1:
    render_frame(f)
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_IMMEDIATE)   # checked, fail -> exit 3
    sceDisplayWaitVblankStart()
sceKernelFreeMemBlock(fb_uid)
return 77
```

- `VITA_DISPLAY_FRAME_COUNT` defaults to **600** (~10 s at 60 fps); it can be
  overridden at build time with
  `-DVITA_DISPLAY_FRAME_COUNT=<n>` (CMake cache variable of the same name).
- Exit codes: **77** = all frames rendered and returned normally (deliberately
  different from the exit-42 startup fixture); **1** = memblock allocation
  failed; **2** = `sceKernelGetMemBlockBase` failed; **3** =
  `sceDisplaySetFrameBuf` returned an error. The SDK crt0 forwards `main`'s
  return value to `sceKernelExitProcess`.

## Imports (what the guest actually needs from the host)

Direct imports of `main.c`:

| Function | Library (module) | NID |
| --- | --- | --- |
| `sceKernelAllocMemBlock` | SceSysmem | `0xB9D5EBDE` |
| `sceKernelGetMemBlockBase` | SceSysmem | `0xB8EF5818` |
| `sceKernelFreeMemBlock` | SceSysmem | `0xA91E15EE` |
| `sceDisplaySetFrameBuf` | SceDisplayUser (module SceDisplay) | `0x7A410B64` |
| `sceDisplayWaitVblankStart` | SceDisplay (module SceDisplay) | `0x5795E898` |

plus the default crt0/libc startup set (`sceKernelExitProcess`
`0x7595D9AA`, LwMutex/mutex ops, TLS, `sceIoOpen`/`sceIoClose` on tty0, etc.)
already exercised by the exit-42 fixture. `sceClibMemset`/`sceClibMemcpy` are
**not** referenced by this fixture's own code — all fills are explicit loops
so the interpreter sees plain stores.

Note: Vita3K implements user-mode `sceDisplaySetFrameBuf`
(`SceDriverUser/SceDisplayUser.cpp`) as a `CALL_EXPORT` wrapper around
`_sceDisplaySetFrameBuf` (`modules/SceDisplay/SceDisplay.cpp`), and
`sceDisplayWaitVblankStart` parks the calling thread via `wait_vblank`.
The browser HLE selection must cover both display exports (and compile both
module sources) for this fixture to run.

## Build

Nothing SDK-generated is committed; the binary directory is the single source
of artifacts.

Standalone:

```sh
cmake -S browser/tests/vita_display_fixture -B build/vita-display-fixture \
  -DVITASDK=/opt/vitasdk/vitasdk
cmake --build build/vita-display-fixture --target vita3k_vita_display_fixture
```

As part of the browser build (the fixture directory is added by
`browser/CMakeLists.txt` behind `VITA3K_WEB_DISPLAY_FIXTURE=ON`):

```sh
emcmake cmake -S . -B build/web -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DVITASDK=/opt/vitasdk/vitasdk
cmake --build build/web --target vita3k_vita_display_fixture
```

The toolchain chain is `arm-vita-eabi-gcc -c` (default crt0/libc, no -nostdlib,
no hand-written entry), link with `-Wl,-q` plus `-lSceDisplay_stub` (the only
stub library linked explicitly; everything else comes from the SDK defaults),
then `vita-elf-create` and `vita-make-fself`. Outputs (`main.o`,
`fixture.elf`, `fixture.velf`, `eboot.bin`) land in
`<binary-dir>/browser/tests/vita_display_fixture/` when built through
`build/web`.

Sanity check of the produced SELF: `eboot.bin` must start with the
`\x00SCE` magic (`00 53 43 45`) and differ non-trivially from `fixture.velf`
(the FSELF wraps the VELF):

```sh
xxd -l 16 build/.../eboot.bin
cmp build/.../eboot.bin build/.../fixture.velf   # must differ
```
