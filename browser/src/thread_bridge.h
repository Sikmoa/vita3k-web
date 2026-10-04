// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace browser {
// Execute a browser operation on the coordinator. Only the calling pthread
// waits; a JS promise may finish the operation on a later event-loop turn.
int coordinator_call(const char *operation, std::array<uint64_t, 8> arguments = {});
// One line of per-operation totals since the last call: calls and the time the
// calling threads spent blocked in them (threaded diagnostics).
std::string coordinator_report();
// Start a browser operation without waiting for it (it must not answer).
// Posted operations run in order with coordinator_call ones from the same
// thread. False when there is no coordinator (Node null-GPU bench).
bool coordinator_post(const char *operation, std::array<uint64_t, 8> arguments = {});
}
