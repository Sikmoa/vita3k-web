// Genuine VitaSDK message-dialog fixture: opens a Yes/No sceMsgDialog and,
// as titles do while one is up, keeps presenting its CPU-drawn frame once per
// vblank and polls the dialog until the user answers. Exit code 100 +
// SceMsgDialogButtonId (101 Yes, 102 No); 1..8 are failures (see main()).
#include <stdint.h>

#include <psp2/common_dialog.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/message_dialog.h>

#define SCREEN_W 960
#define SCREEN_H 544

int main(void) {
    const SceUID fb_uid = sceKernelAllocMemBlock("msg_dialog_fixture_fb",
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCREEN_W * SCREEN_H * 4, NULL);
    if (fb_uid < 0) return 1;
    void *base = NULL;
    if (sceKernelGetMemBlockBase(fb_uid, &base) < 0 || !base) return 2;
    uint32_t *vram = base;
    for (unsigned i = 0; i < SCREEN_W * SCREEN_H; ++i) vram[i] = 0xff402010;
    const SceDisplayFrameBuf fb = { sizeof(fb), base, SCREEN_W, SCE_DISPLAY_PIXELFORMAT_A8B8G8R8, SCREEN_W, SCREEN_H };

    SceMsgDialogUserMessageParam user = { 0 };
    user.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_YESNO;
    user.msg = (const SceChar8 *)"Message dialog fixture: continue?";
    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &user;
    if (sceMsgDialogInit(&param) != 0) return 4;
    while (sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING) {
        if (sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0) return 3;
        sceDisplayWaitVblankStart();
    }
    if (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return 5;
    SceMsgDialogResult result = { 0 };
    if (sceMsgDialogGetResult(&result) != 0) return 6;
    if (result.result != SCE_COMMON_DIALOG_RESULT_OK || result.mode != SCE_MSG_DIALOG_MODE_USER_MSG) return 7;
    if (sceMsgDialogTerm() != 0 || sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_NONE) return 8;
    sceKernelFreeMemBlock(fb_uid);
    return 100 + (int)result.buttonId;
}
