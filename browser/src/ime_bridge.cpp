// SPDX-License-Identifier: GPL-2.0-or-later
// SceIme in the browser: the page shows a text field for the IME Vita3K's HLE
// keeps in Ime (emuenv.ime) and reports what the user does in it, as the
// desktop keyboard filter does (vita3k/gui-qt/src/ime_keyboard_filter.cpp).
// The guest's own sceImeUpdate then delivers each change to its event
// handler (SceIme.cpp). The Worker forwards both directions: vita3kWebOnIme
// posts 'vita-ime'; 'ime-input' queues page input for web_ime_take.
#include "ime_bridge.h"
#include "page_json.h"

#include <emuenv/state.h>
#include <ime/state.h>
#include <util/string_utils.h>

#include <emscripten.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <string>

EM_JS(void, web_ime_post, (const char *json, uint32_t size), {
    if (typeof globalThis.vita3kWebOnIme !== 'function') return;
    globalThis.vita3kWebOnIme(JSON.parse(new TextDecoder().decode(Module['vita3kHostBytes'](json, size).slice())));
});

// Takes the oldest input of the Worker's queue: header = id, kind, caret,
// length; the text's UTF-16 units (at most `capacity`) go to `text`.
EM_JS(uint32_t, web_ime_take, (uint32_t *header, uint16_t *text, uint32_t capacity), {
    const queue = globalThis.vita3kWebImeInputs;
    if (!queue || !queue.length) return 0;
    const input = queue.shift();
    const value = String(input.text ?? "");
    const length = Math.min(value.length, capacity);
    const head = Module['vita3kHostBytes'](header, 16);
    const fields = new DataView(head.buffer, head.byteOffset, 16);
    fields.setUint32(0, input.id >>> 0, true);
    fields.setUint32(4, input.kind >>> 0, true);
    fields.setUint32(8, (input.caret ?? length) >>> 0, true);
    fields.setUint32(12, length, true);
    const units = Module['vita3kHostBytes'](text, length * 2);
    const view = new DataView(units.buffer, units.byteOffset, length * 2);
    for (let i = 0; i < length; ++i) view.setUint16(i * 2, value.charCodeAt(i), true);
    return 1;
});

namespace {
struct PageIme {
    uint32_t shown = 0, next = 1; // id of the IME the page shows (0 = none)
    bool pending = false; // the Worker queued input since the last take
    std::deque<browser::ImeInput> inputs;
};
PageIme &page() {
    static PageIme state;
    return state;
}

void take_page_inputs() {
    uint32_t header[4];
    static char16_t text[SCE_IME_MAX_TEXT_LENGTH];
    while (web_ime_take(header, reinterpret_cast<uint16_t *>(text), SCE_IME_MAX_TEXT_LENGTH)) {
        if (header[1] > browser::ImeInput::close)
            continue;
        page().inputs.push_back({ header[0], static_cast<browser::ImeInput::Kind>(header[1]),
            std::u16string(text, header[3]), header[2] });
    }
}

void post_open(const Ime &ime, uint32_t id) {
    std::string json = "{\"id\":" + std::to_string(id) + ",\"state\":\"open\",\"text\":";
    browser::json_quote(json, string_utils::utf16_to_utf8(ime.str));
    json += ",\"caret\":" + std::to_string(ime.caretIndex);
    json += ",\"maxLength\":" + std::to_string(ime.param.maxTextLength);
    json += ",\"type\":" + std::to_string(ime.param.type);
    json += ",\"option\":" + std::to_string(ime.param.option);
    json += ",\"enterLabel\":";
    browser::json_quote(json, ime.enter_label);
    json += "}";
    web_ime_post(json.data(), static_cast<uint32_t>(json.size()));
}

// One page input becomes the IME's next event, as the desktop filter's key
// handling does: the field's text (UPDATE_TEXT, or UPDATE_CARET when only the
// caret moved), Enter (PRESS_ENTER) or Escape (PRESS_CLOSE).
void apply(Ime &ime, const browser::ImeInput &input) {
    switch (input.kind) {
    case browser::ImeInput::text: {
        const std::u16string value = input.value.substr(0, ime.param.maxTextLength);
        const auto caret = static_cast<uint32_t>(std::min<size_t>(input.caret, value.size()));
        if (value == ime.str && caret == ime.caretIndex)
            return;
        ime.event_id = value == ime.str ? SCE_IME_EVENT_UPDATE_CARET : SCE_IME_EVENT_UPDATE_TEXT;
        if (value != ime.str) {
            const auto common = std::mismatch(value.begin(), value.end(), ime.str.begin(), ime.str.end());
            ime.edit_text.editIndex = static_cast<SceUInt32>(common.first - value.begin());
            ime.edit_text.editLengthChange = static_cast<SceInt32>(value.size()) - static_cast<SceInt32>(ime.str.size());
            ime.str = value;
        }
        ime.caretIndex = ime.edit_text.caretIndex = ime.edit_text.preeditIndex = caret;
        ime.edit_text.preeditLength = 0;
        return;
    }
    case browser::ImeInput::enter: ime.event_id = SCE_IME_EVENT_PRESS_ENTER; return;
    case browser::ImeInput::close: ime.event_id = SCE_IME_EVENT_PRESS_CLOSE; return;
    }
}
} // namespace

namespace browser {
uint32_t sync_ime(EmuEnvState &env) {
    auto &state = page();
    auto &ime = env.ime;
    if (!state.shown && !ime.state && !state.pending)
        return 0;
    if (state.pending) {
        state.pending = false;
        take_page_inputs();
    }
    if (!ime.state) {
        if (state.shown) {
            // Closed by the guest (sceImeClose).
            const std::string json = "{\"id\":" + std::to_string(state.shown) + ",\"state\":\"close\"}";
            web_ime_post(json.data(), static_cast<uint32_t>(json.size()));
            state.shown = 0;
        }
        state.inputs.clear();
        return 0;
    }
    if (!state.shown) {
        state.shown = state.next++;
        post_open(ime, state.shown);
    }
    // Input names the IME it was typed into; input for one that is gone is dropped.
    while (!state.inputs.empty() && state.inputs.front().id != state.shown)
        state.inputs.pop_front();
    if (state.inputs.empty())
        return state.shown;
    // sceImeUpdate holds the mutex while the guest handler runs, which may
    // make imports; this sync then retries after a later one.
    const std::unique_lock<std::mutex> lock(ime.mutex, std::try_to_lock);
    // One event at a time: the next input waits until sceImeUpdate has
    // delivered the previous one (and reset event_id to OPEN).
    if (lock && ime.event_id == SCE_IME_EVENT_OPEN) {
        apply(ime, state.inputs.front());
        state.inputs.pop_front();
    }
    return state.shown;
}

void ime_page_input(ImeInput input) {
    page().inputs.push_back(std::move(input));
}
} // namespace browser

extern "C" EMSCRIPTEN_KEEPALIVE void vita3k_web_ime_input_ready() {
    page().pending = true;
}
