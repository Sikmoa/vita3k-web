// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <module/module.h>
#include "startup_bridge_selection.inc"

// EXPORT normally registers every bridge in a translation unit via dynamic
// initialization. Merely shrinking module_parent.cpp's switch cannot dead-strip
// those initializers, even with LTO. Keep every original export function body
// (including CALL_EXPORT dependencies), but register only selected bridges.
// Selected bridges use the original make_bridge and AAPCS marshalling unchanged.
#define VITA3K_HLE_EMIT_BRIDGE(name) \
    extern const ImportFn import_##name = make_bridge<&export_##name>(#name, &export_##name);
#define VITA3K_HLE_SKIP_BRIDGE(name)
#undef EXPORT
#define EXPORT(ret, name, ...) \
    DECL_EXPORT(ret, name, ##__VA_ARGS__); \
    VITA3K_HLE_BRIDGE_##name(name) \
    DECL_EXPORT(ret, name, ##__VA_ARGS__)
