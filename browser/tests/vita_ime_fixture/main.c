// Genuine VitaSDK on-screen keyboard (SceIme) fixture: opens the IME with an
// initial text and, as titles do, calls sceImeUpdate once per presented frame
// until its event handler sees Enter. Exit code 100 when the text the user
// entered is "Grüße, Vita!" and every event reached the handler once, in
// order (the handler calls sceImeUpdate itself, which must deliver nothing);
// 101 when the user closed the keyboard; 1..12 are failures (see main()).
#include <stdint.h>
#include <string.h>

#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/libime.h>

#define SCREEN_W 960
#define SCREEN_H 544
#define MAX_TEXT 64

static SceWChar16 input[MAX_TEXT + 1];
static uint8_t work[0x5000];
static SceWChar16 seen[MAX_TEXT + 1]; // text of the last UPDATE_TEXT event
static int update_events, enter_events, close_events, wrong_arg, out_of_order, depth;

static void handler(void *arg, const SceImeEventData *e) {
    if (arg != work)
        wrong_arg = 1;
    if (depth)
        out_of_order = 1; // an event delivered again from inside its own handler
    ++depth;
    sceImeUpdate(); // the event is already consumed: this delivers nothing
    --depth;
    if (enter_events || close_events)
        out_of_order = 1; // nothing may follow the final key
    switch (e->id) {
    case SCE_IME_EVENT_UPDATE_TEXT:
        ++update_events;
        memcpy(seen, e->param.text.str, sizeof(seen));
        break;
    case SCE_IME_EVENT_PRESS_ENTER: ++enter_events; break;
    case SCE_IME_EVENT_PRESS_CLOSE: ++close_events; break;
    default: break;
    }
}

static int same(const SceWChar16 *a, const SceWChar16 *b) {
    for (;; ++a, ++b) {
        if (*a != *b)
            return 0;
        if (!*a)
            return 1;
    }
}

int main(void) {
    const SceUID fb_uid = sceKernelAllocMemBlock("ime_fixture_fb",
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCREEN_W * SCREEN_H * 4, NULL);
    if (fb_uid < 0) return 1;
    void *base = NULL;
    if (sceKernelGetMemBlockBase(fb_uid, &base) < 0 || !base) return 2;
    uint32_t *vram = base;
    for (unsigned i = 0; i < SCREEN_W * SCREEN_H; ++i) vram[i] = 0xff204010;
    const SceDisplayFrameBuf fb = { sizeof(fb), base, SCREEN_W, SCE_DISPLAY_PIXELFORMAT_A8B8G8R8, SCREEN_W, SCREEN_H };

    static const SceWChar16 initial[] = { 'V', 'i', 't', 'a', 0 };
    static const SceWChar16 expected[] = { 'G', 'r', 0xfc, 0xdf, 'e', ',', ' ', 'V', 'i', 't', 'a', '!', 0 };
    SceImeParam param;
    sceImeParamInit(&param);
    param.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
    param.type = SCE_IME_TYPE_DEFAULT;
    param.work = work;
    param.arg = work;
    param.handler = handler;
    param.initialText = (SceWChar16 *)initial;
    param.maxTextLength = MAX_TEXT;
    param.inputTextBuffer = input;
    if (sceImeOpen(&param) != 0) return 3;
    if (sceImeOpen(&param) != (int)SCE_IME_ERROR_ALREADY_OPENED) return 4;
    while (!enter_events && !close_events) {
        if (sceImeUpdate() != 0) return 5;
        if (sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0) return 6;
        sceDisplayWaitVblankStart();
    }
    if (sceImeClose() != 0) return 7;
    if (sceImeClose() != (int)SCE_IME_ERROR_NOT_OPENED || sceImeUpdate() != (int)SCE_IME_ERROR_NOT_OPENED) return 8;
    sceKernelFreeMemBlock(fb_uid);
    if (wrong_arg || out_of_order) return 9;
    if (close_events) return 101;
    if (enter_events != 1 || update_events < 1) return 10;
    if (!same(seen, expected)) return 11;
    if (!same(input, expected)) return 12;
    return 100;
}
