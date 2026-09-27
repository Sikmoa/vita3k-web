// Production SceMsgDialog bridges; button and system texts come from the
// generated lang catalog, so this also proves lang::get links with English data.
#pragma once
#include "msg_dialog_bridge.h"
#include <ctrl/ctrl.h>
#include <dialog/state.h>
#include <lang/state.h>
#include <renderer/state.h>
#include <cstring>
#include <new>

// A renderer only so sceCommonDialogUpdate sees GXM as initialized.
struct CommonDialogFixtureRenderer final : renderer::State {
    bool init() override { return true; }
    void late_init(const Config &, std::string_view, MemState &) override {}
    renderer::TextureCache *get_texture_cache() override { return nullptr; }
    void render_frame(DisplayState &, const GxmState &, MemState &) override {}
    void swap_window() override {}
    std::vector<uint32_t> dump_frame(DisplayState &, uint32_t &, uint32_t &) override { return {}; }
    int get_supported_filters() override { return 0; }
    void set_screen_filter(const std::string_view &) override {}
    int get_max_anisotropic_filtering() override { return 1; }
    void set_anisotropic_filtering(int) override {}
    int get_max_2d_texture_width() override { return 4096; }
    std::string_view get_gpu_name() override { return "fixture"; }
    void precompile_shader(const renderer::ShadersHash &) override {}
    void preclose_action() override {}
};

inline void test_guest_msg_dialog(EmuEnvState &env, ThreadState &thread) {
    auto call = [&](uint32_t nid, uint32_t a = 0) {
        auto &cpu = *thread.cpu;
        write_reg(cpu, 0, a);
        const auto sp = read_sp(cpu);
        call_import(env, cpu, nid, thread.id);
        REQUIRE(env.missing_nids.empty());
        REQUIRE(read_sp(cpu) == sp);
        return read_reg(cpu, 0);
    };
    constexpr uint32_t init = 0x755FF270, get_status = 0x4107019E, get_result = 0xBB3BFC89,
                       close = 0xC296D396, term = 0x81ACF695;
    REQUIRE(lang::get(lang::str::ok) == "OK");
    REQUIRE(lang::get(lang::str::cancel) == "Cancel");
    REQUIRE(lang::get(lang::str::please_wait) == "Please wait...");

    const Address block = alloc(env.mem, 1024, "msg dialog fixture");
    REQUIRE(block);
    std::memset(Ptr<void>(block).get(env.mem), 0, 1024);
    const Address param = block, user = block + 0x100, sys = block + 0x180,
                  text = block + 0x200, result = block + 0x280;
    std::strcpy(Ptr<char>(text).get(env.mem), "fixture message");
    new (Ptr<SceMsgDialogUserMessageParam>(user).get(env.mem)) SceMsgDialogUserMessageParam{
        SCE_MSG_DIALOG_BUTTON_TYPE_OK_CANCEL, Ptr<SceChar8>(text), {}, {}};
    Ptr<SceMsgDialogSystemMessageParam>(sys).get(env.mem)->sysMsgType = SCE_MSG_DIALOG_SYSMSG_TYPE_WAIT;
    auto *p = Ptr<SceMsgDialogParam>(param).get(env.mem);
    p->userMsgParam = Ptr<SceMsgDialogUserMessageParam>(user);
    p->sysMsgParam = Ptr<SceMsgDialogSystemMessageParam>(sys);

    auto &dialog = env.common_dialog;
    REQUIRE(call(get_status) == SCE_COMMON_DIALOG_STATUS_NONE);
    p->mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    REQUIRE(call(init, param) == 0);
    REQUIRE(call(get_status) == SCE_COMMON_DIALOG_STATUS_RUNNING);
    REQUIRE(dialog.msg.message == "fixture message");
    REQUIRE(dialog.msg.btn_num == 2 && dialog.msg.btn[0] == "OK" && dialog.msg.btn[1] == "Cancel");
    REQUIRE(call(close) == 0);
    REQUIRE(call(get_status) == SCE_COMMON_DIALOG_STATUS_FINISHED);
    REQUIRE(call(get_result, result) == 0);
    const auto *r = Ptr<SceMsgDialogResult>(result).get(env.mem);
    REQUIRE(r->mode == SCE_MSG_DIALOG_MODE_USER_MSG && r->result == SCE_COMMON_DIALOG_RESULT_OK);
    REQUIRE(call(term) == 0);
    REQUIRE(dialog.type == NO_DIALOG && call(get_status) == SCE_COMMON_DIALOG_STATUS_NONE);

    p->mode = SCE_MSG_DIALOG_MODE_SYSTEM_MSG;
    REQUIRE(call(init, param) == 0);
    REQUIRE(dialog.msg.message == "Please wait..." && dialog.msg.btn_num == 0);
    REQUIRE(call(close) == 0 && call(term) == 0);

    // Page answers (msg_dialog_bridge.cpp), with the default cross enter
    // button: cross takes the highlighted button, circle the last of several.
    auto *u = Ptr<SceMsgDialogUserMessageParam>(user).get(env.mem);
    const auto answer = [&](SceMsgDialogButtonType type, uint32_t button, uint32_t selected) {
        u->buttonType = type;
        p->mode = SCE_MSG_DIALOG_MODE_USER_MSG;
        REQUIRE(call(init, param) == 0);
        const uint32_t id = browser::sync_message_dialog(env);
        REQUIRE(id != 0 && browser::sync_message_dialog(env) == id);
        browser::press_message_dialog(id + 1, SCE_CTRL_CROSS, 0); // a dialog that is gone
        REQUIRE(browser::sync_message_dialog(env) == id);
        browser::press_message_dialog(id, button, selected);
        const uint32_t shown = browser::sync_message_dialog(env);
        const uint32_t status = call(get_status);
        uint32_t button_id = SCE_MSG_DIALOG_BUTTON_ID_INVALID;
        if (status == SCE_COMMON_DIALOG_STATUS_FINISHED) {
            REQUIRE(shown == 0);
            REQUIRE(call(get_result, result) == 0);
            const auto *r = Ptr<SceMsgDialogResult>(result).get(env.mem);
            REQUIRE(r->result == SCE_COMMON_DIALOG_RESULT_OK);
            button_id = r->buttonId;
        } else {
            REQUIRE(status == SCE_COMMON_DIALOG_STATUS_RUNNING && shown == id);
            REQUIRE(call(close) == 0);
        }
        REQUIRE(call(term) == 0 && browser::sync_message_dialog(env) == 0);
        return button_id;
    };
    REQUIRE(answer(SCE_MSG_DIALOG_BUTTON_TYPE_YESNO, SCE_CTRL_CROSS, 0) == SCE_MSG_DIALOG_BUTTON_ID_YES);
    REQUIRE(answer(SCE_MSG_DIALOG_BUTTON_TYPE_YESNO, SCE_CTRL_CROSS, 1) == SCE_MSG_DIALOG_BUTTON_ID_NO);
    REQUIRE(answer(SCE_MSG_DIALOG_BUTTON_TYPE_YESNO, SCE_CTRL_CIRCLE, 0) == SCE_MSG_DIALOG_BUTTON_ID_NO);
    REQUIRE(answer(SCE_MSG_DIALOG_BUTTON_TYPE_OK, SCE_CTRL_CROSS, 0) == SCE_MSG_DIALOG_BUTTON_ID_OK);
    // Circle cannot dismiss a single button; other buttons are not answers.
    REQUIRE(answer(SCE_MSG_DIALOG_BUTTON_TYPE_OK, SCE_CTRL_CIRCLE, 0) == SCE_MSG_DIALOG_BUTTON_ID_INVALID);
    REQUIRE(answer(SCE_MSG_DIALOG_BUTTON_TYPE_YESNO, SCE_CTRL_TRIANGLE, 0) == SCE_MSG_DIALOG_BUTTON_ID_INVALID);

    // sceCommonDialogUpdate: parameter and GXM checks; the dialog itself is
    // drawn by the page, and the update completes a timed trophy setup dialog.
    constexpr uint32_t update = 0x90530F2F, trophy_status = 0xC3A59547, trophy_term = 0xA81082DD;
    const Address update_param = block + 0x300;
    static_assert(sizeof(SceCommonDialogUpdateParam) <= 0x100);
    REQUIRE(call(update, 0) == SCE_COMMON_DIALOG_ERROR_NULL);
    REQUIRE(!env.renderer);
    REQUIRE(call(update, update_param) == SCE_COMMON_DIALOG_ERROR_GXM_IS_UNINITIALIZED);
    env.renderer = std::make_unique<CommonDialogFixtureRenderer>();
    REQUIRE(call(update, update_param) == 0);
    p->mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    REQUIRE(call(init, param) == 0);
    REQUIRE(call(update, update_param) == 0 && call(get_status) == SCE_COMMON_DIALOG_STATUS_RUNNING);
    REQUIRE(call(close) == 0 && call(term) == 0);
    {
        const std::lock_guard<std::recursive_mutex> lock(dialog.mutex);
        dialog.type = TROPHY_SETUP_DIALOG;
        dialog.status = SCE_COMMON_DIALOG_STATUS_RUNNING;
        dialog.trophy.tick = 0; // due: the host shows it for no time
    }
    REQUIRE(call(update, update_param) == 0);
    REQUIRE(dialog.status == SCE_COMMON_DIALOG_STATUS_FINISHED && dialog.result == SCE_COMMON_DIALOG_RESULT_OK);
    REQUIRE(call(trophy_status) == SCE_COMMON_DIALOG_STATUS_FINISHED && call(trophy_term) == 0);
    env.renderer.reset();
    free(env.mem, block);
    std::puts("Guest message dialog: init, status, close, result, term, page answers, English catalog texts and sceCommonDialogUpdate passed");
}
