// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace browser {
// Execute a browser operation on the coordinator. Only the calling pthread
// waits; a JS promise may finish the operation on a later event-loop turn.
int coordinator_call(const char *operation, std::array<uint64_t, 8> arguments = {});
}
