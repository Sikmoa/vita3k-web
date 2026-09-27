// SPDX-License-Identifier: GPL-2.0-or-later
// sceMsgDialog in the browser: the page renders the dialog Vita3K's HLE keeps
// in DialogState and answers with pad buttons, as the desktop overlay does
// (vita3k/overlay/src/common_dialog.cpp). The Worker forwards both directions:
// vita3kWebOnDialog posts 'vita-dialog', 'dialog-press' calls the export below.
#include "msg_dialog_bridge.h"
#include "page_json.h"

#include <config/state.h>
#include <ctrl/ctrl.h>
#include <dialog/state.h>
#include <emuenv/state.h>
#include <util/system.h>

#include <emscripten.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>

EM_JS(void, web_msg_dialog_post, (const char *json, uint32_t size), {
    if (typeof globalThis.vita3kWebOnDialog !== 'function') return;
    globalThis.vita3kWebOnDialog(JSON.parse(new TextDecoder().decode(Module['vita3kHostBytes'](json, size).slice())));
});

namespace {
struct Press {
    uint32_t id = 0, button = 0, selected = 0;
};
struct PageDialog {
    uint32_t shown = 0, next = 1; // id of the dialog the page shows (0 = none)
    std::string message;
    uint32_t percent = 0;
    Press press;
};
PageDialog &page() {
    static PageDialog state;
    return state;
}

void post(const std::string &json) {
    web_msg_dialog_post(json.data(), static_cast<uint32_t>(json.size()));
}

void post_shown(const EmuEnvState &env, uint32_t id, const char *state) {
    const auto &msg = env.common_dialog.msg;
    std::string json = "{\"id\":" + std::to_string(id) + ",\"state\":\"" + state + "\",\"message\":";
    browser::json_quote(json, msg.message);
    json += ",\"buttons\":[";
    for (uint8_t i = 0; i < msg.btn_num && i < 3; ++i) {
        if (i) json += ',';
        browser::json_quote(json, msg.btn[i]);
    }
    json += "],\"progress\":";
    json += msg.has_progress_bar ? std::to_string(msg.bar_percent) : "null";
    json += ",\"enterButton\":\"";
    json += env.cfg.sys_button == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE ? "circle" : "cross";
    json += "\"}";
    post(json);
}

// Desktop common_dialog_overlay::handle_message_input: the enter button picks
// the highlighted button, the other one picks the last of several buttons.
void apply(DialogState &dialog, const Press &press, int sys_button) {
    const uint32_t confirm = sys_button == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
    const uint32_t cancel = confirm == SCE_CTRL_CROSS ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
    const uint8_t count = std::min<uint8_t>(dialog.msg.btn_num, 3);
    int index = -1;
    if (press.button == confirm && press.selected < count)
        index = static_cast<int>(press.selected);
    else if (press.button == cancel && count > 1)
        index = count - 1;
    if (index < 0)
        return;
    dialog.msg.status = dialog.msg.btn_val[index];
    dialog.result = SCE_COMMON_DIALOG_RESULT_OK;
    dialog.status = SCE_COMMON_DIALOG_STATUS_FINISHED;
}
} // namespace

namespace browser {
uint32_t sync_message_dialog(EmuEnvState &env) {
    auto &state = page();
    auto &dialog = env.common_dialog;
    if (!state.shown && dialog.type != MESSAGE_DIALOG)
        return 0;
    const std::lock_guard<std::recursive_mutex> lock(dialog.mutex);
    const auto running = [&] {
        return dialog.type == MESSAGE_DIALOG && dialog.status == SCE_COMMON_DIALOG_STATUS_RUNNING;
    };
    // A press names the dialog it answers; one for a dialog that is gone is dropped.
    if (state.press.id && state.press.id == state.shown && running())
        apply(dialog, state.press, env.cfg.sys_button);
    state.press = {};
    if (!running()) {
        if (state.shown) {
            // Closed by a press, or by the guest (Close, Abort, Term).
            post("{\"id\":" + std::to_string(state.shown) + ",\"state\":\"close\",\"buttonId\":"
                + std::to_string(dialog.type == MESSAGE_DIALOG ? dialog.msg.status : 0)
                + ",\"result\":" + std::to_string(static_cast<int>(dialog.result)) + "}");
            state.shown = 0;
        }
        return 0;
    }
    const auto &msg = dialog.msg;
    const uint32_t percent = msg.has_progress_bar ? msg.bar_percent : 0;
    if (!state.shown) {
        state.shown = state.next++;
        post_shown(env, state.shown, "open");
    } else if (msg.message != state.message || percent != state.percent) {
        post_shown(env, state.shown, "update");
    } else {
        return state.shown;
    }
    state.message = msg.message;
    state.percent = percent;
    return state.shown;
}

void press_message_dialog(uint32_t id, uint32_t button, uint32_t selected) {
    page().press = { id, button, selected };
}
} // namespace browser

extern "C" EMSCRIPTEN_KEEPALIVE void vita3k_web_msg_dialog_press(uint32_t id, uint32_t button, uint32_t selected) {
    browser::press_message_dialog(id, button, selected);
}
