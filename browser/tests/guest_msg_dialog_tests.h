// Production SceMsgDialog bridges; button and system texts come from the
// generated lang catalog, so this also proves lang::get links with English data.
#pragma once
#include <dialog/state.h>
#include <lang/state.h>
#include <cstring>
#include <new>

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
    free(env.mem, block);
    std::puts("Guest message dialog: init, status, close, result, term and English catalog texts passed");
}
