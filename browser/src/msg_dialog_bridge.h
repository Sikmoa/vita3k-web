// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

struct EmuEnvState;

namespace browser {
// Page side of sceMsgDialog (the desktop overlay's role). Called after every
// HLE import: posts the running message dialog to the page (open, text or
// progress changes, close) and applies a button press the page sent since
// the previous call. Returns the id of the dialog the page shows (0: none).
uint32_t sync_message_dialog(EmuEnvState &env);
// A pad button (SCE_CTRL_CROSS or SCE_CTRL_CIRCLE) pressed on dialog `id`
// with button `selected` highlighted; applied by the next sync.
void press_message_dialog(uint32_t id, uint32_t button, uint32_t selected);
} // namespace browser
