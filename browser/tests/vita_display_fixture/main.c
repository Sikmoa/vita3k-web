// Genuine VitaSDK CPU-rendered display fixture for the Vita3K browser bring-up.
//
// No vita2d, no SceGxm, no graphics SDK: the framebuffer is a plain memory
// block allocated with sceKernelAllocMemBlock and written by the CPU. The
// animated gradient is rendered once at startup (a full-screen redraw costs
// ~32M guest instructions, far too slow for an interpreted CPU), and each
// frame then redraws only the moving rectangle's old and new regions plus the
// frame markers (~1.6M instructions/frame, a few fps under the interpreter,
// 60fps on real hardware). Every frame is unmistakably animated, and the
// frame index is encoded in pixels so a host-side validator can read the
// frame number straight out of the framebuffer contents (see README.md for
// the exact scheme).

#include <stdint.h>

#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>

#define SCREEN_W 960
#define SCREEN_H 544
#define SCREEN_PITCH 960 // in pixels, per SceDisplayFrameBuf
#define BYTES_PER_PIXEL 4

// How many frames to render before exiting (~10 s at 60 fps). Overridable at
// configure time for slow interpreter bring-up runs.
#ifndef VITA_DISPLAY_FRAME_COUNT
#define VITA_DISPLAY_FRAME_COUNT 600
#endif

// Exit codes: 77 means "rendered all frames and returned normally", chosen to
// differ from the exit-42 startup fixture so logs distinguish the two. 1..4
// are early failures (see main()).
#define EXIT_FRAMES_DONE 77

// Moving filled rectangle.
#define RECT_W 192
#define RECT_H 108
#define RECT_X_STEP 13
#define RECT_X_WRAP 512 // RECT_X_WRAP - 1 + RECT_W < SCREEN_W
#define RECT_Y_STEP 7
#define RECT_Y_WRAP 256 // RECT_Y_WRAP - 1 + RECT_H < SCREEN_H

static uint32_t *vram;

// SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 packs A:[31:24] B:[23:16] G:[15:8] R:[7:0]
// into the 32-bit little-endian word at each pixel.
static uint32_t background_pixel(unsigned int x, unsigned int y, unsigned int f) {
    const unsigned int r = (x + 3u * f) & 0xFFu;
    const unsigned int g = (y + 5u * f) & 0xFFu;
    const unsigned int b = ((x + y) + 7u * f) & 0xFFu;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static void render_background(void) {
    // Static diagonal gradient, rendered once. (Animating it per frame would
    // cost a full-screen redraw every frame.)
    for (unsigned int y = 0; y < SCREEN_H; y++) {
        uint32_t *const row = vram + y * SCREEN_PITCH;
        for (unsigned int x = 0; x < SCREEN_W; x++) {
            row[x] = background_pixel(x, y, 0);
        }
    }
}

static void restore_region(unsigned int x0, unsigned int y0, unsigned int w, unsigned int h) {
    // Repaint the static gradient over a region the moving rectangle vacated.
    for (unsigned int y = y0; y < y0 + h; y++) {
        uint32_t *const row = vram + y * SCREEN_PITCH;
        for (unsigned int x = x0; x < x0 + w; x++) {
            row[x] = background_pixel(x, y, 0);
        }
    }
}

static void draw_rect(unsigned int rx, unsigned int ry) {
    // Filled moving rectangle with a contrasting border, wrapping over the
    // screen. Drawn over the gradient.
    const uint32_t border = 0xFF3020FFu; // A B=30 G=20 R=FF
    const uint32_t fill = 0xFF40C000u; // A B=40 G=C0 R=00
    for (unsigned int y = ry; y < ry + RECT_H; y++) {
        uint32_t *const row = vram + y * SCREEN_PITCH;
        for (unsigned int x = rx; x < rx + RECT_W; x++) {
            const unsigned int edge = (y == ry) || (y == ry + RECT_H - 1)
                || (x == rx) || (x == rx + RECT_W - 1);
            row[x] = edge ? border : fill;
        }
    }
}

static void draw_markers(unsigned int f) {
    // Frame-code pixels, drawn LAST so they are always visible on top of
    // everything (the rectangle can overlap row 0).
    //
    // a) Direct encoding: pixel (0,0) packs the frame index into RGB.
    vram[0] = 0xFF000000u | (f & 0x00FFFFFFu);
    // b) Redundant bit strip: row 0, x = 1..32 hold bit j of the frame index
    //    (white = 1, black = 0), one pixel per bit.
    for (unsigned int j = 0; j < 32; j++) {
        vram[1 + j] = ((f >> j) & 1u) ? 0xFFFFFFFFu : 0xFF000000u;
    }
    // c) Liveness heartbeat: bottom-right pixel alternates two colors.
    vram[(SCREEN_H - 1) * SCREEN_PITCH + (SCREEN_W - 1)]
        = (f & 1u) ? 0xFF00FFFFu : 0xFFFF0000u;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    // CPU-accessible framebuffer; USER_RW_UNCACHE is the classic homebrew
    // choice for CPU-rendered display buffers.
    const SceUID fb_uid = sceKernelAllocMemBlock("vita3k_display_fixture_fb",
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
        SCREEN_W * SCREEN_H * BYTES_PER_PIXEL, NULL);
    if (fb_uid < 0) {
        return 1; // allocation failed
    }

    void *base = NULL;
    if (sceKernelGetMemBlockBase(fb_uid, &base) < 0 || base == NULL) {
        sceKernelFreeMemBlock(fb_uid);
        return 2; // could not map the block
    }
    vram = (uint32_t *)base;

    SceDisplayFrameBuf fb;
    fb.size = sizeof(fb);
    fb.base = base;
    fb.pitch = SCREEN_PITCH;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = SCREEN_W;
    fb.height = SCREEN_H;

    // Static background once, then animate only the moving rectangle and the
    // frame markers per frame. Render -> present -> wait for vblank, every
    // iteration.
    render_background();
    unsigned int prev_rx = 0, prev_ry = 0;
    for (unsigned int frame = 0; frame < (unsigned int)VITA_DISPLAY_FRAME_COUNT; frame++) {
        const unsigned int rx = (RECT_X_STEP * frame) & (RECT_X_WRAP - 1);
        const unsigned int ry = (RECT_Y_STEP * frame) & (RECT_Y_WRAP - 1);

        if (frame > 0) {
            // Restore the gradient over the previous rectangle's box (the
            // rectangle never wraps past its box bounds: see the WRAP defines).
            restore_region(prev_rx, prev_ry, RECT_W, RECT_H);
        }
        draw_rect(rx, ry);
        draw_markers(frame);

        if (sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_IMMEDIATE) < 0) {
            sceKernelFreeMemBlock(fb_uid);
            return 3; // present rejected the framebuffer
        }

        sceDisplayWaitVblankStart();

        prev_rx = rx;
        prev_ry = ry;
    }

    sceKernelFreeMemBlock(fb_uid);
    return EXIT_FRAMES_DONE;
}
