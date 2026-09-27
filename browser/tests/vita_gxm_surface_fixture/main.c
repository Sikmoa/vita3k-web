// Genuine VitaSDK fixture for GXM color surfaces whose memory is not a plain
// linear image: tiled and swizzled surfaces, downscaled surfaces (rendered at
// twice their size and box-filtered), multisampled render targets, textures
// over rendered surfaces (whole surfaces and rectangles inside them), and
// transfers into and out of rendered surfaces; plus sceCommonDialogUpdate's
// checks with a message dialog open.
//
// Every surface gets the same pattern (red, a green top-left strip, a blue
// bottom-right block), drawn with the repository's color GXP. Results are
// read back with transfer copies, which work with and without surface sync;
// with surface sync the fixture also checks the surfaces' own bytes against
// its tiled and Morton addressing. Exit code 100 (surface sync on) or 101
// (off) is success; anything else names the failed check (see fail()).
#include <psp2/common_dialog.h>
#include <psp2/message_dialog.h>
#include <psp2/gxm.h>
#include <string.h>

#include "fixture_shaders.h"

#define RED 0xff0000ffu
#define GREEN 0xff00ff00u
#define BLUE 0xffff0000u
#define YELLOW 0xff00ffffu
#define CYAN 0xffffff00u
#define SENTINEL 0x12345678u

#define ALIGNED __attribute__((aligned(4096)))
static unsigned int linear64[64 * 64] ALIGNED;   // linear 64x64 (the reference, and sampling results)
static unsigned int linear64b[64 * 32] ALIGNED;  // linear 64x32
static unsigned int linear48[48 * 32] ALIGNED;   // linear 48x32
static unsigned int linear32[32 * 64] ALIGNED;   // linear 32x64 or 32x32
static unsigned int tiled64[64 * 64] ALIGNED;    // tiled 64x64
static unsigned int swizzled[32 * 64] ALIGNED;   // swizzled 32x64
static unsigned int downscaled[32 * 32] ALIGNED; // 32x32, rendered as 64x64
static unsigned int msaa_down[32 * 32] ALIGNED;  // 32x32 of a 4x MSAA 32x32 target
static unsigned int msaa_full[64 * 64] ALIGNED;  // 64x64 of a 4x MSAA 32x32 target
static unsigned int keyed[32 * 64] ALIGNED;      // transfer copy source
static unsigned int halves[1024] ALIGNED; // F16x4 16x4 (and an RGBA8 surface 4 bytes in), a page of its own
static unsigned int readback[64 * 64] ALIGNED;   // transfer copy destination

static unsigned char patch_heap[256 * 1024] __attribute__((aligned(16)));
static unsigned patch_used;
static void *patch_alloc(void *user, unsigned size) {
    (void)user;
    size = (size + 15) & ~15u;
    if (size > sizeof(patch_heap) - patch_used) return 0;
    void *p = patch_heap + patch_used;
    patch_used += size;
    return p;
}
static void patch_free(void *user, void *p) { (void)user; (void)p; }
static unsigned char host[4096] __attribute__((aligned(16)));
static unsigned char vdm[65536] __attribute__((aligned(16)));
static const float identity[16] __attribute__((aligned(16))) = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static const unsigned short quad[6] __attribute__((aligned(16))) = {0, 1, 2, 0, 2, 3};

// Vertex data is read when the scene is submitted: every draw of a scene
// gets its own slot.
typedef struct { float x, y, z, w, r, g, b, a; } ColorVertex;
typedef struct { float x, y, z, w, u, v; } TexVertex;
static ColorVertex color_slots[32][4] __attribute__((aligned(16)));
static TexVertex tex_slots[8][4] __attribute__((aligned(16)));
static unsigned color_used, tex_used;

static SceGxmContext *context;
static SceGxmVertexProgram *color_vp, *tex_vp;
static SceGxmFragmentProgram *color_fp, *color_fp_msaa, *color_fp_half, *tex_fp;
static int failed;

static int fail(int code) {
    if (!failed) failed = code;
    return code;
}

// Pixel rectangle -> clip space: the default viewport maps clip y = 1 to row 0.
static void corners(float w, float h, float x0, float y0, float x1, float y1, float out[4][2]) {
    const float cx0 = -1 + 2 * x0 / w, cx1 = -1 + 2 * x1 / w, cy0 = 1 - 2 * y0 / h, cy1 = 1 - 2 * y1 / h;
    out[0][0] = cx0; out[0][1] = cy0;
    out[1][0] = cx0; out[1][1] = cy1;
    out[2][0] = cx1; out[2][1] = cy1;
    out[3][0] = cx1; out[3][1] = cy0;
}
static SceGxmFragmentProgram *next_color_fp; // overrides the color program for one draw
static void color_rect(float w, float h, float x0, float y0, float x1, float y1, unsigned int abgr, int msaa) {
    float c[4][2];
    corners(w, h, x0, y0, x1, y1, c);
    ColorVertex *v = color_slots[color_used++];
    for (int i = 0; i < 4; ++i) {
        v[i].x = c[i][0]; v[i].y = c[i][1]; v[i].z = 0; v[i].w = 1;
        v[i].r = (abgr & 255) / 255.0f; v[i].g = ((abgr >> 8) & 255) / 255.0f;
        v[i].b = ((abgr >> 16) & 255) / 255.0f; v[i].a = (abgr >> 24) / 255.0f;
    }
    sceGxmSetVertexProgram(context, color_vp);
    sceGxmSetFragmentProgram(context, next_color_fp ? next_color_fp : msaa ? color_fp_msaa : color_fp);
    next_color_fp = 0;
    sceGxmSetVertexDefaultUniformBuffer(context, identity);
    sceGxmSetVertexStream(context, 0, v);
    if (sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, quad, 6)) fail(2);
}
// The pattern, in the drawing's own coordinates (w x h).
static unsigned int pattern(unsigned w, unsigned h, unsigned x, unsigned y) {
    if (x >= w - 8 && y >= h - 16) return BLUE;
    if (x < w / 2 && y < h / 4) return GREEN;
    return RED;
}
static void draw_pattern(float w, float h, int msaa) {
    color_rect(w, h, 0, 0, w, h, RED, msaa);
    color_rect(w, h, 0, 0, w / 2, h / 4, GREEN, msaa);
    color_rect(w, h, w - 8, h - 16, w, h, BLUE, msaa);
}
// The whole texture drawn over the whole w x h surface, texel for pixel.
static void texture_quad(float w, float h, const SceGxmTexture *texture) {
    float c[4][2];
    corners(w, h, 0, 0, w, h, c);
    static const float uv[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
    TexVertex *v = tex_slots[tex_used++];
    for (int i = 0; i < 4; ++i) {
        v[i].x = c[i][0]; v[i].y = c[i][1]; v[i].z = 0; v[i].w = 1;
        v[i].u = uv[i][0]; v[i].v = uv[i][1];
    }
    sceGxmSetVertexProgram(context, tex_vp);
    sceGxmSetFragmentProgram(context, tex_fp);
    sceGxmSetVertexDefaultUniformBuffer(context, identity);
    sceGxmSetVertexStream(context, 0, v);
    if (sceGxmSetFragmentTexture(context, 0, texture)) fail(3);
    if (sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, quad, 6)) fail(4);
}

static SceGxmRenderTarget *target(unsigned w, unsigned h, SceGxmMultisampleMode msaa) {
    SceGxmRenderTargetParams p;
    memset(&p, 0, sizeof(p));
    p.width = w; p.height = h; p.scenesPerFrame = 1; p.multisampleMode = msaa; p.driverMemBlock = -1;
    SceGxmRenderTarget *rt = 0;
    if (sceGxmCreateRenderTarget(&p, &rt)) fail(5);
    return rt;
}
static void surface(SceGxmColorSurface *s, SceGxmColorSurfaceType type, SceGxmColorSurfaceScaleMode scale,
    unsigned w, unsigned h, unsigned stride, void *data) {
    if (sceGxmColorSurfaceInit(s, SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR, type, scale,
            SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, w, h, stride, data)) fail(6);
}
static void begin(SceGxmRenderTarget *rt, SceGxmColorSurface *s) {
    if (sceGxmBeginScene(context, 0, rt, 0, 0, 0, s, 0)) fail(7);
}
static void end(void) {
    if (sceGxmEndScene(context, 0, 0)) fail(8);
    sceGxmFinish(context);
    color_used = tex_used = 0;
}
static void fill(void *data, unsigned count, unsigned int value) {
    unsigned int *p = data;
    for (unsigned i = 0; i < count; ++i) p[i] = value;
}

// Texel offsets of the surface layouts (as the GPU stores them).
static unsigned tiled_index(unsigned stride, unsigned x, unsigned y) {
    return ((((y >> 5) * (stride >> 5)) + (x >> 5)) << 10) | ((y & 31) << 5) | (x & 31);
}
static unsigned spread(unsigned v) {
    v &= 0xffff;
    v = (v | (v << 8)) & 0x00ff00ff; v = (v | (v << 4)) & 0x0f0f0f0f;
    v = (v | (v << 2)) & 0x33333333; v = (v | (v << 1)) & 0x55555555;
    return v;
}
static unsigned morton_index(unsigned w, unsigned h, unsigned x, unsigned y) {
    const unsigned min = w < h ? w : h;
    unsigned k = 0;
    while ((1u << k) < min) ++k;
    return (((x >> k) | (y >> k)) << (2 * k)) | (spread(x & (min - 1)) << 1) | spread(y & (min - 1));
}

// w x h texels of a surface's memory, through its layout, into `readback`
// (tightly packed). Without surface sync the renderer reads the GPU target
// back for the copy.
static void read_surface(const void *data, SceGxmTransferType type, unsigned x, unsigned y, unsigned w, unsigned h,
    unsigned stride_px) {
    fill(readback, 64 * 64, SENTINEL);
    if (sceGxmTransferCopy(w, h, 0, 0, SCE_GXM_TRANSFER_COLORKEY_NONE,
            SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, type, data, x, y, (int)(stride_px * 4),
            SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, SCE_GXM_TRANSFER_LINEAR, readback, 0, 0, (int)(w * 4), 0, 0, 0))
        fail(9);
    sceGxmTransferFinish();
}
static int near(unsigned int a, unsigned int b) {
    for (int c = 0; c < 32; c += 8) {
        const int d = (int)((a >> c) & 255) - (int)((b >> c) & 255);
        if (d < -1 || d > 1) return 0;
    }
    return 1;
}
typedef unsigned int (*Expect)(unsigned x, unsigned y);
static int check(unsigned w, unsigned h, Expect expect, int code) {
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x)
            if (!near(readback[y * w + x], expect(x, y))) return fail(code);
    return 0;
}

static unsigned int p64(unsigned x, unsigned y) { return pattern(64, 64, x, y); }
static unsigned int p64_bottom(unsigned x, unsigned y) { return pattern(64, 64, x, y + 32); }
static unsigned int p3264(unsigned x, unsigned y) { return pattern(32, 64, x, y); }
static unsigned int p3264_bottom(unsigned x, unsigned y) { return pattern(32, 64, x, y + 32); }
static unsigned int p32(unsigned x, unsigned y) { return pattern(32, 32, x, y); }
static unsigned int p32_doubled(unsigned x, unsigned y) { return pattern(32, 32, x / 2, y / 2); }
static unsigned int p64_filled(unsigned x, unsigned y) { return x < 32 && y < 4 ? YELLOW : pattern(64, 64, x, y); }
static unsigned int p64_filled_rect(unsigned x, unsigned y) { return p64_filled(x + 24, y); }
static unsigned int p3264_keyed(unsigned x, unsigned y) {
    return x >= 8 && x < 16 && y >= 40 && y < 48 ? CYAN : pattern(32, 64, x, y);
}
static unsigned int p64_halved(unsigned x, unsigned y) { return pattern(64, 64, 2 * x, 2 * y); }
static unsigned int p64_raw64(unsigned x, unsigned y) { return x < 32 && y < 32 ? YELLOW : pattern(64, 64, x, y); }
static unsigned int all_cyan(unsigned x, unsigned y) { (void)x; (void)y; return CYAN; }
static unsigned int p64_cyan_top(unsigned x, unsigned y) { return y < 16 ? CYAN : pattern(64, 64, x, y); }
static unsigned int green_split(unsigned x, unsigned y) { return x == 0 && y == 0 ? (GREEN & 0xffff0000u) | 0xabcd : GREEN; }
static unsigned int blue_marked(unsigned x, unsigned y) { return x == 0 && y == 8 ? YELLOW : BLUE; }
static unsigned int blue_middle(unsigned x, unsigned y) { (void)x; return y >= 8 && y < 40 ? BLUE : RED; }
// 32-bit words of a row: F16x4 texels (two words each) where the RGBA8
// surface (words 1..16) did not overwrite them.
static unsigned int halves_then_green(unsigned x, unsigned y) {
    (void)y;
    if (x >= 1 && x <= 16) return GREEN;
    return x % 2 ? 0x3c003c00u : 0x00003c00u;
}
// The downscaled surface: the pattern at 64x64 plus a two-pixel blue column
// at render x 31..32, box-filtered to 32x32: texels 15 and 16 mix it with
// their other render column.
static unsigned int mix(unsigned int a, unsigned int b) {
    unsigned int out = 0;
    for (int c = 0; c < 32; c += 8) out |= (((((a >> c) & 255) + ((b >> c) & 255) + 1) / 2) & 255) << c;
    return out;
}
static unsigned int down(unsigned x, unsigned y) {
    const unsigned rx = 2 * x, ry = 2 * y;
    unsigned int left = pattern(64, 64, rx, ry), right = pattern(64, 64, rx + 1, ry);
    if (rx == 30) right = BLUE;
    if (rx == 32) left = BLUE;
    return mix(left, right);
}
static unsigned int down_filled(unsigned x, unsigned y) { return y < 4 ? CYAN : down(x, y); }

int main(void) {
    SceGxmInitializeParams params;
    memset(&params, 0, sizeof(params));
    params.parameterBufferSize = 256 * 1024;
    if (sceGxmInitialize(&params)) return 10;
    SceGxmContextParams cp;
    memset(&cp, 0, sizeof(cp));
    cp.hostMem = host; cp.hostMemSize = sizeof(host);
    cp.vdmRingBufferMem = vdm; cp.vdmRingBufferMemSize = sizeof(vdm);
    if (sceGxmCreateContext(&cp, &context)) return 11;

    SceGxmShaderPatcherParams pp;
    memset(&pp, 0, sizeof(pp));
    pp.hostAllocCallback = patch_alloc; pp.hostFreeCallback = patch_free;
    SceGxmShaderPatcher *patcher;
    if (sceGxmShaderPatcherCreate(&pp, &patcher)) return 12;
    SceGxmShaderPatcherId cv, cf, tv, tf;
    const SceGxmProgram *cvp = (const SceGxmProgram *)color_v, *tvp = (const SceGxmProgram *)texture_v;
    if (sceGxmShaderPatcherRegisterProgram(patcher, cvp, &cv) || sceGxmShaderPatcherRegisterProgram(patcher, (const SceGxmProgram *)color_f, &cf)
        || sceGxmShaderPatcherRegisterProgram(patcher, tvp, &tv) || sceGxmShaderPatcherRegisterProgram(patcher, (const SceGxmProgram *)texture_f, &tf))
        return 13;
    SceGxmVertexAttribute attrs[2];
    memset(attrs, 0, sizeof(attrs));
    attrs[0].format = attrs[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attrs[0].componentCount = attrs[1].componentCount = 4;
    attrs[0].regIndex = sceGxmProgramParameterGetResourceIndex(sceGxmProgramFindParameterByName(cvp, "aPosition"));
    attrs[1].regIndex = sceGxmProgramParameterGetResourceIndex(sceGxmProgramFindParameterByName(cvp, "aColor"));
    attrs[1].offset = 16;
    SceGxmVertexStream stream = {sizeof(ColorVertex), SCE_GXM_INDEX_SOURCE_INDEX_16BIT};
    if (sceGxmShaderPatcherCreateVertexProgram(patcher, cv, attrs, 2, &stream, 1, &color_vp)) return 14;
    if (sceGxmShaderPatcherCreateFragmentProgram(patcher, cf, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
            SCE_GXM_MULTISAMPLE_NONE, 0, cvp, &color_fp)) return 15;
    if (sceGxmShaderPatcherCreateFragmentProgram(patcher, cf, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
            SCE_GXM_MULTISAMPLE_4X, 0, cvp, &color_fp_msaa)) return 16;
    if (sceGxmShaderPatcherCreateFragmentProgram(patcher, cf, SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4,
            SCE_GXM_MULTISAMPLE_NONE, 0, cvp, &color_fp_half)) return 19;
    attrs[0].regIndex = sceGxmProgramParameterGetResourceIndex(sceGxmProgramFindParameterByName(tvp, "aPosition"));
    attrs[1].regIndex = sceGxmProgramParameterGetResourceIndex(sceGxmProgramFindParameterByName(tvp, "aTexcoord"));
    attrs[1].componentCount = 2;
    stream.stride = sizeof(TexVertex);
    if (sceGxmShaderPatcherCreateVertexProgram(patcher, tv, attrs, 2, &stream, 1, &tex_vp)) return 17;
    if (sceGxmShaderPatcherCreateFragmentProgram(patcher, tf, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
            SCE_GXM_MULTISAMPLE_NONE, 0, tvp, &tex_fp)) return 18;

    SceGxmRenderTarget *rt64 = target(64, 64, SCE_GXM_MULTISAMPLE_NONE), *rt6432 = target(64, 32, SCE_GXM_MULTISAMPLE_NONE),
                       *rt4832 = target(48, 32, SCE_GXM_MULTISAMPLE_NONE), *rt3264 = target(32, 64, SCE_GXM_MULTISAMPLE_NONE),
                       *rt32 = target(32, 32, SCE_GXM_MULTISAMPLE_NONE), *rt32msaa = target(32, 32, SCE_GXM_MULTISAMPLE_4X);
    if (failed) return failed;
    SceGxmColorSurface s_linear64, s_linear64b, s_linear48, s_linear3264, s_linear32, s_tiled, s_swizzled, s_down,
        s_msaa_down, s_msaa_full;
    surface(&s_linear64, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE, 64, 64, 64, linear64);
    surface(&s_linear64b, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE, 64, 32, 64, linear64b);
    surface(&s_linear48, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE, 48, 32, 48, linear48);
    surface(&s_linear3264, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE, 32, 64, 32, linear32);
    surface(&s_linear32, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE, 32, 32, 32, linear32);
    surface(&s_tiled, SCE_GXM_COLOR_SURFACE_TILED, SCE_GXM_COLOR_SURFACE_SCALE_NONE, 64, 64, 64, tiled64);
    surface(&s_swizzled, SCE_GXM_COLOR_SURFACE_SWIZZLED, SCE_GXM_COLOR_SURFACE_SCALE_NONE, 32, 64, 32, swizzled);
    surface(&s_down, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE, 32, 32, 32, downscaled);
    surface(&s_msaa_down, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE, 32, 32, 32, msaa_down);
    surface(&s_msaa_full, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE, 64, 64, 64, msaa_full);
    if (failed) return failed;

    // 1. The pattern into every kind of surface.
    fill(linear64, 64 * 64, SENTINEL);
    fill(tiled64, 64 * 64, SENTINEL);
    fill(swizzled, 32 * 64, SENTINEL);
    begin(rt64, &s_linear64); draw_pattern(64, 64, 0); end();
    const int sync = linear64[0] != SENTINEL;
    begin(rt64, &s_tiled); draw_pattern(64, 64, 0); end();
    begin(rt3264, &s_swizzled); draw_pattern(32, 64, 0); end();
    begin(rt64, &s_down); draw_pattern(64, 64, 0); color_rect(64, 64, 31, 0, 33, 64, BLUE, 0); end();
    begin(rt32msaa, &s_msaa_down); draw_pattern(32, 32, 1); end();
    begin(rt32msaa, &s_msaa_full); draw_pattern(32, 32, 1); end();
    if (failed) return failed;

    // 2. With surface sync, each surface's own bytes, in its layout.
    if (sync) {
        for (unsigned y = 0; y < 64; ++y)
            for (unsigned x = 0; x < 64; ++x) {
                if (linear64[y * 64 + x] != pattern(64, 64, x, y)) return 20;
                if (tiled64[tiled_index(64, x, y)] != pattern(64, 64, x, y)) return 21;
                if (msaa_full[y * 64 + x] != pattern(32, 32, x / 2, y / 2)) return 22;
            }
        for (unsigned y = 0; y < 64; ++y)
            for (unsigned x = 0; x < 32; ++x)
                if (swizzled[morton_index(32, 64, x, y)] != pattern(32, 64, x, y)) return 23;
        for (unsigned y = 0; y < 32; ++y)
            for (unsigned x = 0; x < 32; ++x) {
                if (!near(downscaled[y * 32 + x], down(x, y))) return 24;
                if (msaa_down[y * 32 + x] != pattern(32, 32, x, y)) return 25;
            }
    }

    // 3. The same through transfer copies (read back from the GPU without sync).
    read_surface(linear64, SCE_GXM_TRANSFER_LINEAR, 0, 0, 64, 64, 64);
    if (check(64, 64, p64, 30)) return failed;
    read_surface(tiled64, SCE_GXM_TRANSFER_TILED, 0, 0, 64, 64, 64);
    if (check(64, 64, p64, 31)) return failed;
    read_surface(swizzled, SCE_GXM_TRANSFER_SWIZZLED, 0, 0, 32, 64, 32);
    if (check(32, 64, p3264, 32)) return failed;
    read_surface(downscaled, SCE_GXM_TRANSFER_LINEAR, 0, 0, 32, 32, 32);
    if (check(32, 32, down, 33)) return failed;
    read_surface(msaa_down, SCE_GXM_TRANSFER_LINEAR, 0, 0, 32, 32, 32);
    if (check(32, 32, p32, 34)) return failed;
    read_surface(msaa_full, SCE_GXM_TRANSFER_LINEAR, 0, 0, 64, 64, 64);
    if (check(64, 64, p32_doubled, 35)) return failed;

    // 4. Textures over rendered surfaces, drawn texel for pixel into linear ones.
    SceGxmTexture t;
    if (sceGxmTextureInitTiled(&t, tiled64, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 64, 64, 1)) return 40;
    begin(rt64, &s_linear64); texture_quad(64, 64, &t); end();
    read_surface(linear64, SCE_GXM_TRANSFER_LINEAR, 0, 0, 64, 64, 64);
    if (check(64, 64, p64, 41)) return failed;
    // The second row of tiles.
    if (sceGxmTextureInitTiled(&t, tiled64 + 32 * 64, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 64, 32, 1)) return 42;
    begin(rt6432, &s_linear64b); texture_quad(64, 32, &t); end();
    read_surface(linear64b, SCE_GXM_TRANSFER_LINEAR, 0, 0, 64, 32, 64);
    if (check(64, 32, p64_bottom, 43)) return failed;
    if (sceGxmTextureInitSwizzled(&t, swizzled, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 32, 64, 1)) return 44;
    begin(rt3264, &s_linear3264); texture_quad(32, 64, &t); end();
    read_surface(linear32, SCE_GXM_TRANSFER_LINEAR, 0, 0, 32, 64, 32);
    if (check(32, 64, p3264, 45)) return failed;
    // The second 32x32 Morton block: the lower square.
    if (sceGxmTextureInitSwizzled(&t, swizzled + 32 * 32, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 32, 32, 1)) return 46;
    begin(rt32, &s_linear32); texture_quad(32, 32, &t); end();
    read_surface(linear32, SCE_GXM_TRANSFER_LINEAR, 0, 0, 32, 32, 32);
    if (check(32, 32, p3264_bottom, 47)) return failed;
    // A downscaled surface is sampled as its guest texels (the filtered image).
    if (sceGxmTextureInitLinear(&t, downscaled, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 32, 32, 1)) return 48;
    begin(rt32, &s_linear32); texture_quad(32, 32, &t); end();
    read_surface(linear32, SCE_GXM_TRANSFER_LINEAR, 0, 0, 32, 32, 32);
    if (check(32, 32, down, 49)) return failed;

    // 5. Linear rectangles: a 48x32 strided texture at the surface base is
    // that rectangle, not the whole surface; a linear texture of the same
    // pitch inside it starts at its row.
    begin(rt64, &s_linear64); draw_pattern(64, 64, 0); end();
    if (sceGxmTextureInitLinearStrided(&t, linear64, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 48, 32, 64 * 4)) return 50;
    begin(rt4832, &s_linear48); texture_quad(48, 32, &t); end();
    read_surface(linear48, SCE_GXM_TRANSFER_LINEAR, 0, 0, 48, 32, 48);
    if (check(48, 32, p64, 51)) return failed;
    if (sceGxmTextureInitLinear(&t, linear64 + 32 * 64, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 64, 32, 1)) return 52;
    begin(rt6432, &s_linear64b); texture_quad(64, 32, &t); end();
    read_surface(linear64b, SCE_GXM_TRANSFER_LINEAR, 0, 0, 64, 32, 64);
    if (check(64, 32, p64_bottom, 53)) return failed;

    // 6. Transfers into rendered surfaces reach their GPU copies.
    // A linear fill over the tiled surface's first 512 bytes: texels 0..127,
    // the first four rows of its first tile.
    if (sceGxmTransferFill(YELLOW, SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, tiled64, 0, 0, 64, 2, 64 * 4, 0, 0, 0)) return 60;
    sceGxmTransferFinish();
    if (sceGxmTextureInitTiled(&t, tiled64, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 64, 64, 1)) return 61;
    begin(rt64, &s_linear64); texture_quad(64, 64, &t); end();
    read_surface(linear64, SCE_GXM_TRANSFER_LINEAR, 0, 0, 64, 64, 64);
    if (check(64, 64, p64_filled, 62)) return failed;
    // A rectangle of the tiled surface across its tile boundary.
    read_surface(tiled64, SCE_GXM_TRANSFER_TILED, 24, 0, 16, 8, 64);
    if (check(16, 8, p64_filled_rect, 63)) return failed;
    // A color-keyed copy into the swizzled surface: only the cyan texels.
    fill(keyed, 32 * 64, 0);
    for (unsigned y = 40; y < 48; ++y)
        for (unsigned x = 8; x < 16; ++x) keyed[y * 32 + x] = CYAN;
    if (sceGxmTransferCopy(32, 64, 0, 0xffffffffu, SCE_GXM_TRANSFER_COLORKEY_REJECT,
            SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, SCE_GXM_TRANSFER_LINEAR, keyed, 0, 0, 32 * 4,
            SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, SCE_GXM_TRANSFER_SWIZZLED, swizzled, 0, 0, 32 * 4, 0, 0, 0)) return 64;
    sceGxmTransferFinish();
    if (sceGxmTextureInitSwizzled(&t, swizzled, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 32, 64, 1)) return 65;
    begin(rt3264, &s_linear3264); texture_quad(32, 64, &t); end();
    read_surface(linear32, SCE_GXM_TRANSFER_LINEAR, 0, 0, 32, 64, 32);
    if (check(32, 64, p3264_keyed, 66)) return failed;
    // A fill over a downscaled surface's first four rows.
    if (sceGxmTransferFill(CYAN, SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, downscaled, 0, 0, 32, 4, 32 * 4, 0, 0, 0)) return 67;
    sceGxmTransferFinish();
    if (sceGxmTextureInitLinear(&t, downscaled, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 32, 32, 1)) return 68;
    begin(rt32, &s_linear32); texture_quad(32, 32, &t); end();
    read_surface(linear32, SCE_GXM_TRANSFER_LINEAR, 0, 0, 32, 32, 32);
    if (check(32, 32, down_filled, 69)) return failed;
    // A 2x2 box downscale of a rendered surface into plain memory.
    begin(rt64, &s_linear64); draw_pattern(64, 64, 0); end();
    fill(readback, 64 * 64, SENTINEL);
    if (sceGxmTransferDownscale(SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, linear64, 0, 0, 64, 64, 64 * 4,
            SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, readback, 0, 0, 32 * 4, 0, 0, 0)) return 70;
    sceGxmTransferFinish();
    if (check(32, 32, p64_halved, 71)) return failed;

    // A RAW64 copy (16x32 texels: 32x32 RGBA8 texels) over the rendered
    // surface: every texel it covers, not every other column, reaches the
    // GPU copy, sampled into a second 64x64 surface.
    fill(keyed, 32 * 64, YELLOW);
    if (sceGxmTransferCopy(16, 32, 0, 0, SCE_GXM_TRANSFER_COLORKEY_NONE,
            SCE_GXM_TRANSFER_FORMAT_RAW64, SCE_GXM_TRANSFER_LINEAR, keyed, 0, 0, 16 * 8,
            SCE_GXM_TRANSFER_FORMAT_RAW64, SCE_GXM_TRANSFER_LINEAR, linear64, 0, 0, 64 * 4, 0, 0, 0)) return 72;
    sceGxmTransferFinish();
    SceGxmColorSurface s_scratch64;
    surface(&s_scratch64, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE, 64, 64, 64, msaa_full);
    if (sceGxmTextureInitLinear(&t, linear64, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 64, 64, 1)) return 73;
    begin(rt64, &s_scratch64); texture_quad(64, 64, &t); end();
    read_surface(msaa_full, SCE_GXM_TRANSFER_LINEAR, 0, 0, 64, 64, 64);
    if (check(64, 64, p64_raw64, 74)) return failed;
    // Guest writes after rendering are newer than the GPU copy: a transfer
    // reads them, not the rendered pixels.
    begin(rt64, &s_linear64); draw_pattern(64, 64, 0); end();
    fill(linear64, 64 * 64, CYAN);
    read_surface(linear64, SCE_GXM_TRANSFER_LINEAR, 0, 0, 64, 64, 64);
    if (check(64, 64, all_cyan, 75)) return failed;

    // Only the first 16 rows (one 4 KiB page) rewritten by the guest: a
    // transfer of the whole surface sees them and the rendered rest, and the
    // surface keeps both, in memory and in its GPU copy.
    begin(rt64, &s_linear64); draw_pattern(64, 64, 0); end();
    fill(linear64, 16 * 64, CYAN);
    read_surface(linear64, SCE_GXM_TRANSFER_LINEAR, 0, 0, 64, 64, 64);
    if (check(64, 64, p64_cyan_top, 76)) return failed;
    for (unsigned i = 0; i < 64 * 64; ++i)
        if (linear64[i] != p64_cyan_top(i % 64, i / 64)) return 77;
    if (sceGxmTextureInitLinear(&t, linear64, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 64, 64, 1)) return 78;
    begin(rt64, &s_scratch64); texture_quad(64, 64, &t); end();
    read_surface(msaa_full, SCE_GXM_TRANSFER_LINEAR, 0, 0, 64, 64, 64);
    if (check(64, 64, p64_cyan_top, 79)) return failed;
    // A 16-bit fill into the lower half of an RGBA8 texel of a surface whose
    // GPU copy is newer than memory (which holds cyan there without surface
    // sync): the upper half keeps the rendered green bytes.
    begin(rt64, &s_linear64); color_rect(64, 64, 0, 0, 64, 64, GREEN, 0); end();
    if (sceGxmTransferFill(0xabcd, SCE_GXM_TRANSFER_FORMAT_U5U6U5_BGR, linear64, 0, 0, 1, 1, 64 * 4, 0, 0, 0)) return 82;
    sceGxmTransferFinish();
    begin(rt64, &s_scratch64); texture_quad(64, 64, &t); end();
    read_surface(msaa_full, SCE_GXM_TRANSFER_LINEAR, 0, 0, 64, 64, 64);
    if (check(64, 64, green_split, 83)) return failed;
    // Two targets over the same rows: the later one (blue, 64x32 at row 32 of
    // the red 64x64 one) is what a texture over those rows shows, also after
    // a transfer wrote one texel both share.
    begin(rt64, &s_linear64); color_rect(64, 64, 0, 0, 64, 64, RED, 0); end();
    SceGxmColorSurface s_lower;
    surface(&s_lower, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE, 64, 32, 64, linear64 + 32 * 64);
    begin(rt6432, &s_lower); color_rect(64, 32, 0, 0, 64, 32, BLUE, 0); end();
    if (sceGxmTransferFill(YELLOW, SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR, linear64, 0, 40, 1, 1, 64 * 4, 0, 0, 0)) return 84;
    sceGxmTransferFinish();
    if (sceGxmTextureInitLinear(&t, linear64 + 32 * 64, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 64, 32, 1)) return 85;
    begin(rt6432, &s_linear64b); texture_quad(64, 32, &t); end();
    read_surface(linear64b, SCE_GXM_TRANSFER_LINEAR, 0, 0, 64, 32, 64);
    if (check(64, 32, blue_marked, 86)) return failed;
    // A transfer reading those shared rows sees the later render too, and
    // leaves it in the later target.
    read_surface(linear64, SCE_GXM_TRANSFER_LINEAR, 0, 32, 64, 32, 64);
    if (check(64, 32, blue_marked, 87)) return failed;
    begin(rt6432, &s_linear64b); texture_quad(64, 32, &t); end();
    read_surface(linear64b, SCE_GXM_TRANSFER_LINEAR, 0, 0, 64, 32, 64);
    if (check(64, 32, blue_marked, 88)) return failed;
    // The same with the later target starting mid-page (row 8): only its
    // rows turn blue, in memory and in the earlier target's GPU copy.
    begin(rt64, &s_linear64); color_rect(64, 64, 0, 0, 64, 64, RED, 0); end();
    SceGxmColorSurface s_middle;
    surface(&s_middle, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE, 64, 32, 64, linear64 + 8 * 64);
    begin(rt6432, &s_middle); color_rect(64, 32, 0, 0, 64, 32, BLUE, 0); end();
    read_surface(linear64, SCE_GXM_TRANSFER_LINEAR, 0, 0, 64, 64, 64);
    if (check(64, 64, blue_middle, 89)) return failed;
    if (sceGxmTextureInitLinear(&t, linear64, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 64, 64, 1)) return 90;
    begin(rt64, &s_scratch64); texture_quad(64, 64, &t); end();
    read_surface(msaa_full, SCE_GXM_TRANSFER_LINEAR, 0, 0, 64, 64, 64);
    if (check(64, 64, blue_middle, 91)) return failed;

    // An F16x4 surface under an RGBA8 one starting 4 bytes in (its rows the
    // same 128 bytes): a transfer over both keeps, per byte, the later
    // RGBA8 render where they overlap, even inside one F16x4 texel.
    SceGxmColorSurface s_half, s_quarter;
    if (sceGxmColorSurfaceInit(&s_half, SCE_GXM_COLOR_FORMAT_F16F16F16F16_ABGR, SCE_GXM_COLOR_SURFACE_LINEAR,
            SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_64BIT, 16, 4, 16, halves)) return 92;
    surface(&s_quarter, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE, 16, 4, 32, halves + 1);
    SceGxmRenderTarget *rt164 = target(16, 4, SCE_GXM_MULTISAMPLE_NONE);
    // (1, 0, 1, 1) in half floats: 0x3c00 0x0000 0x3c00 0x3c00.
    begin(rt164, &s_half); next_color_fp = color_fp_half; color_rect(16, 4, 0, 0, 16, 4, 0xffff00ffu, 0); end();
    begin(rt164, &s_quarter); color_rect(16, 4, 0, 0, 16, 4, GREEN, 0); end();
    read_surface(halves, SCE_GXM_TRANSFER_LINEAR, 0, 0, 32, 4, 32);
    if (check(32, 4, halves_then_green, 93)) return failed;

    // 7. sceCommonDialogUpdate (firmware 3.74 libcdlg). Without a dialog the
    // parameter is not read; with one, the render target is checked and the
    // call must come from outside a scene. The host draws the dialog, so the
    // target's memory is never touched.
    if (sceCommonDialogUpdate(0) != 0) return 120;
    SceMsgDialogUserMessageParam message;
    memset(&message, 0, sizeof(message));
    message.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    message.msg = (const SceChar8 *)"GXM surface fixture";
    SceMsgDialogParam dialog;
    sceMsgDialogParamInit(&dialog);
    dialog.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    dialog.userMsgParam = &message;
    if (sceMsgDialogInit(&dialog)) return 121;
    if (sceCommonDialogUpdate(0) != (int)SCE_COMMON_DIALOG_ERROR_NULL) return 122;
    SceCommonDialogUpdateParam update;
    memset(&update, 0, sizeof(update));
    update.renderTarget.colorFormat = SCE_GXM_COLOR_FORMAT_A8B8G8R8;
    update.renderTarget.surfaceType = SCE_GXM_COLOR_SURFACE_LINEAR;
    update.renderTarget.width = 960;
    update.renderTarget.height = 544;
    update.renderTarget.strideInPixels = 960;
    update.renderTarget.colorSurfaceData = linear64;
    if (sceCommonDialogUpdate(&update) != 0) return 123;
    begin(rt164, &s_quarter);
    const int within = sceCommonDialogUpdate(&update);
    end();
    if (within != (int)SCE_COMMON_DIALOG_ERROR_WITHIN_SCENE) return 124;
    if (sceCommonDialogUpdate(&update) != 0) return 125;
    update.renderTarget.width = 64;
    update.renderTarget.height = 64;
    update.renderTarget.strideInPixels = 64;
    if (sceCommonDialogUpdate(&update) != (int)SCE_COMMON_DIALOG_ERROR_INVALID_SURFACE_RESOLUTION) return 126;
    if (sceMsgDialogClose() || sceMsgDialogTerm()) return 127;

    if (failed) return failed;
    sceGxmDestroyContext(context);
    sceGxmTerminate();
    return sync ? 100 : 101;
}
