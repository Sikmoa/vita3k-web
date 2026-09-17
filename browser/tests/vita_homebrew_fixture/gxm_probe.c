// Real SDK lifecycle probe, no stdio: runtime logs imports and exit.
// Guest ABI + transfer fill + real GXP indexed draw; guest checks GPU readback.
#include <psp2/gxm.h>
#include "probe_shaders.h"
static unsigned char patch_heap[256 * 1024] __attribute__((aligned(16)));
static unsigned patch_used;
static void *patch_alloc(void *user, unsigned size) {
    (void)user;
    size = (size + 15) & ~15u;
    if (size > sizeof(patch_heap) - patch_used) return NULL;
    void *p = patch_heap + patch_used; patch_used += size; return p;
}
static void patch_free(void *user, void *p) { (void)user; (void)p; }
static unsigned int frame[32 * 32] __attribute__((aligned(64)));
static float vertices[] __attribute__((aligned(16))) = {
    -1,-1,0,1, 1,0,0,1, 3,-1,0,1, 1,0,0,1, -1,3,0,1, 1,0,0,1
};
// LINEAR ABGR8 width=4 has an eight-pixel physical pitch. Padding is poison.
static unsigned int texels[8 * 4] __attribute__((aligned(64)));
static unsigned int replacement[8 * 4] __attribute__((aligned(64)));
static float textured_vertices[] __attribute__((aligned(16))) = {
    -1,-1,0,1, 0,0, 3,-1,0,1, 0,0, -1,3,0,1, 0,0
};
static const unsigned short indices[] __attribute__((aligned(16))) = {0,1,2};
static float matrix[] __attribute__((aligned(16))) = {
    1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
};
static volatile unsigned int pixels[8 * 8];
static volatile unsigned int notified;
static unsigned char host[4096] __attribute__((aligned(16)));
static unsigned char vdm[65536] __attribute__((aligned(16)));
int main(void) {
    if (sceGxmInitialize(NULL) != (int)SCE_GXM_ERROR_INVALID_POINTER) return 13;
    SceGxmInitializeParams params = {0};
    params.parameterBufferSize = 256 * 1024;
    if (sceGxmInitialize(&params) != 0) return 14;
    SceGxmContextParams cp = {0};
    cp.hostMem = host;
    cp.hostMemSize = sizeof(host);
    cp.vdmRingBufferMem = vdm;
    cp.vdmRingBufferMemSize = sizeof(vdm);
    SceGxmContext *context = NULL;
    if (sceGxmCreateContext(&cp, &context) != 0) return 18;
    for (unsigned i = 0; i < 64; ++i) pixels[i] = 0x12345678;
    SceGxmNotification notification = { (volatile unsigned int *)&notified, 7 };
    if (sceGxmTransferFill(0xff3366cc, SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR,
        (void *)pixels, 1, 2, 3, 4, 8 * 4, NULL, 0, &notification) != 0) return 20;
    sceGxmFinish(context);
    if (notified != 7) return 21;
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x)
            if (pixels[y * 8 + x] != ((x >= 1 && x < 4 && y >= 2 && y < 6)
                ? 0xff3366cc : 0x12345678)) return 22;
    SceGxmShaderPatcherParams pp = {0};
    pp.hostAllocCallback = patch_alloc; pp.hostFreeCallback = patch_free;
    SceGxmShaderPatcher *patcher = NULL;
    if (sceGxmShaderPatcherCreate(&pp, &patcher)) return 30;
    SceGxmShaderPatcherId vid, fid;
    const SceGxmProgram *vgxp = (const SceGxmProgram *)color_v;
    if (sceGxmShaderPatcherRegisterProgram(patcher, vgxp, &vid)) return 31;
    if (sceGxmShaderPatcherRegisterProgram(patcher, (const SceGxmProgram *)color_f, &fid)) return 32;
    const SceGxmProgramParameter *pos = sceGxmProgramFindParameterByName(vgxp, "aPosition");
    const SceGxmProgramParameter *col = sceGxmProgramFindParameterByName(vgxp, "aColor");
    if (!pos || !col) return 33;
    SceGxmVertexAttribute attrs[2] = {{0}};
    attrs[0].format = attrs[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attrs[0].componentCount = attrs[1].componentCount = 4;
    attrs[0].regIndex = sceGxmProgramParameterGetResourceIndex(pos);
    attrs[1].regIndex = sceGxmProgramParameterGetResourceIndex(col);
    attrs[1].offset = 16;
    SceGxmVertexStream stream = {32, SCE_GXM_INDEX_SOURCE_INDEX_16BIT};
    SceGxmVertexProgram *vp; SceGxmFragmentProgram *fp;
    if (sceGxmShaderPatcherCreateVertexProgram(patcher, vid, attrs, 2, &stream, 1, &vp)) return 34;
    if (sceGxmShaderPatcherCreateFragmentProgram(patcher, fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
        SCE_GXM_MULTISAMPLE_NONE, NULL, vgxp, &fp)) return 35;
    SceGxmRenderTargetParams rp = {0}; rp.width = 32; rp.height = 32; rp.scenesPerFrame = 1; rp.driverMemBlock = -1;
    SceGxmRenderTarget *target;
    if (sceGxmCreateRenderTarget(&rp, &target)) return 36;
    SceGxmColorSurface surface;
    if (sceGxmColorSurfaceInit(&surface, SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR,
        SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
        SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, 32, 32, 32, frame)) return 37;
    if (sceGxmBeginScene(context, 0, target, NULL, NULL, NULL, &surface, NULL)) return 38;
    sceGxmSetVertexProgram(context, vp); sceGxmSetFragmentProgram(context, fp);
    if (sceGxmSetVertexStream(context, 0, vertices)) return 39;
    if (sceGxmSetVertexDefaultUniformBuffer(context, matrix)) return 40;
    if (sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, 3)) return 41;
    if (sceGxmEndScene(context, NULL, NULL)) return 43;
    sceGxmFinish(context);
    for (unsigned i = 0; i < 32 * 32; ++i) if (frame[i] != 0xff0000ff) return 44;
    // A second draw changes both vertex color and guest WVP data. Pixels
    // outside the smaller triangle must retain the previous red surface.
    for (unsigned i = 0; i < 3; ++i) {
        vertices[i * 8 + 4] = 0; vertices[i * 8 + 5] = 1;
    }
    matrix[0] = matrix[5] = 0.25f;
    if (sceGxmBeginScene(context, 0, target, NULL, NULL, NULL, &surface, NULL)) return 46;
    if (sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, 3)) return 47;
    if (sceGxmEndScene(context, NULL, NULL)) return 48;
    sceGxmFinish(context);
    if (frame[16 * 32 + 16] != 0xff00ff00) return 49;
    if (frame[0] != 0xff0000ff || frame[32 * 32 - 1] != 0xff0000ff) return 50;
    // Repository texture_v/texture_f GXP goes through the same real translator.
    // No shader lookup/substitute WGSL, and no host-injected texture pixels.
    SceGxmShaderPatcherId tvid, tfid;
    const SceGxmProgram *tvgxp = (const SceGxmProgram *)texture_v;
    if (sceGxmShaderPatcherRegisterProgram(patcher, tvgxp, &tvid)) return 51;
    if (sceGxmShaderPatcherRegisterProgram(patcher, (const SceGxmProgram *)texture_f, &tfid)) return 52;
    pos = sceGxmProgramFindParameterByName(tvgxp, "aPosition");
    const SceGxmProgramParameter *uv = sceGxmProgramFindParameterByName(tvgxp, "aTexcoord");
    if (!pos || !uv) return 53;
    attrs[0].regIndex = sceGxmProgramParameterGetResourceIndex(pos);
    attrs[1].regIndex = sceGxmProgramParameterGetResourceIndex(uv);
    attrs[1].componentCount = 2; stream.stride = 24;
    SceGxmVertexProgram *tvp; SceGxmFragmentProgram *tfp;
    if (sceGxmShaderPatcherCreateVertexProgram(patcher, tvid, attrs, 2, &stream, 1, &tvp)) return 54;
    if (sceGxmShaderPatcherCreateFragmentProgram(patcher, tfid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
        SCE_GXM_MULTISAMPLE_NONE, NULL, tvgxp, &tfp)) return 55;
    for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 8; ++x) {
        texels[y * 8 + x] = x < 4 ? ((64 + x + y) << 24) | (17 << 16) | (y * 50 << 8) | (x * 40) : 0xeeeeeeee;
        replacement[y * 8 + x] = x < 4 ? 0x11224488 : 0xeeeeeeee;
    }
    SceGxmTexture texture;
    if (sceGxmTextureInitLinear(&texture, texels, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 4, 4, 1)) return 56;
    matrix[0] = matrix[5] = 1;
    const float coords[6][2] = {{.125f,.125f}, {.875f,.875f}, {1.875f,-.125f}, {.5f,.5f}, {.5f,.5f}, {.5f,.5f}};
    const unsigned expected[6] = {0x40110000, 0x46119678, 0x46119678, 0x43114b3c, 0x80402010, 0x11224488};
    for (unsigned draw = 0; draw < 6; ++draw) {
        for (unsigned i = 0; i < 3; ++i) {
            textured_vertices[i * 6 + 4] = coords[draw][0];
            textured_vertices[i * 6 + 5] = coords[draw][1];
        }
        if (draw == 2) {
            if (sceGxmTextureSetUAddrMode(&texture, SCE_GXM_TEXTURE_ADDR_REPEAT)
                || sceGxmTextureSetVAddrMode(&texture, SCE_GXM_TEXTURE_ADDR_REPEAT)) return 57;
        }
        if (draw == 3) {
            if (sceGxmTextureSetMinFilter(&texture, SCE_GXM_TEXTURE_FILTER_LINEAR)
                || sceGxmTextureSetMagFilter(&texture, SCE_GXM_TEXTURE_FILTER_LINEAR)
                || sceGxmTextureSetUAddrMode(&texture, SCE_GXM_TEXTURE_ADDR_CLAMP)
                || sceGxmTextureSetVAddrMode(&texture, SCE_GXM_TEXTURE_ADDR_CLAMP)) return 58;
        }
        if (draw == 4) // No descriptor rebind: changing memory alone must upload anew.
            for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x)
                texels[y * 8 + x] = 0x80402010;
        if (draw == 5 && sceGxmTextureSetData(&texture, replacement)) return 59;
        if (sceGxmBeginScene(context, 0, target, NULL, NULL, NULL, &surface, NULL)) return 60;
        if (draw == 0) {
            sceGxmSetVertexProgram(context, tvp); sceGxmSetFragmentProgram(context, tfp);
            if (sceGxmSetVertexStream(context, 0, textured_vertices)
                || sceGxmSetVertexDefaultUniformBuffer(context, matrix)) return 61;
        }
        if (draw != 4 && sceGxmSetFragmentTexture(context, 0, &texture)) return 62;
        if (sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, 3)) return 63;
        if (sceGxmEndScene(context, NULL, NULL)) return 64;
        sceGxmFinish(context);
        for (unsigned i = 0; i < 32 * 32; ++i) for (unsigned c = 0; c < 4; ++c) {
            int actual = (frame[i] >> (c * 8)) & 255;
            int want = (expected[draw] >> (c * 8)) & 255;
            // Linear interpolation/UNORM rounding permits one LSB only.
            int tolerance = draw == 3 ? 1 : 0;
            if (actual < want - tolerance || actual > want + tolerance) return 65 + draw;
        }
    }
    // Stale bound texture must not force a group-3 draw on an untextured GXP.
    if (sceGxmBeginScene(context, 0, target, NULL, NULL, NULL, &surface, NULL)) return 71;
    sceGxmSetVertexProgram(context, vp); sceGxmSetFragmentProgram(context, fp);
    if (sceGxmSetVertexStream(context, 0, vertices)
        || sceGxmSetVertexDefaultUniformBuffer(context, matrix)) return 72;
    if (sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, 3)) return 73;
    if (sceGxmEndScene(context, NULL, NULL)) return 74;
    sceGxmFinish(context);
    for (unsigned i = 0; i < 32 * 32; ++i) if (frame[i] != 0xff00ff00) return 75;
    if (sceGxmDestroyRenderTarget(target)) return 45;
    if (sceGxmDestroyContext(context) != 0) return 19;
    if (sceGxmTerminate() != 0) return 15;
    // Reinitialization exercises notification-region and renderer teardown.
    if (sceGxmInitialize(&params) != 0) return 16;
    if (sceGxmTerminate() != 0) return 17;
    return 42;
}
