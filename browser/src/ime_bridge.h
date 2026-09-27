// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>

struct EmuEnvState;

namespace browser {
// What the page did in its text field for IME `id`.
struct ImeInput {
    enum Kind : uint32_t { text = 0, enter = 1, close = 2 };
    uint32_t id = 0;
    Kind kind = text;
    std::u16string value; // text: the whole field
    uint32_t caret = 0; // text: caret index in UTF-16 units
};

// Page side of SceIme (the desktop GUI's role, gui-qt ime_keyboard_filter).
// Called after every HLE import: posts the open IME to the page (open with
// its text and limits, close) and hands the page's next input to the guest
// once sceImeUpdate has delivered the previous event. Returns the id of the
// IME the page shows (0: none).
uint32_t sync_ime(EmuEnvState &env);
// Queues page input; applied by a later sync_ime, in order.
void ime_page_input(ImeInput input);
} // namespace browser
