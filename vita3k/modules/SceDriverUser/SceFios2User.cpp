// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <module/module.h>

#include <io/functions.h>
#include <kernel/state.h>

#include <util/tracy.h>
TRACY_MODULE_NAME(SceFios2User);

enum SceFiosErrorCode {
    SCE_FIOS_OK = 0,
    SCE_FIOS_ERROR_BAD_PTR = 0x80820006
};

typedef SceUID SceFiosOverlayID;

enum SceFiosOverlayResolveMode {
    SCE_FIOS_OVERLAY_RESOLVE_FOR_READ = 0,
    SCE_FIOS_OVERLAY_RESOLVE_FOR_WRITE = 1
};

template <>
std::string to_debug_str<SceFiosOverlayResolveMode>(const MemState &mem, SceFiosOverlayResolveMode type) {
    switch (type) {
    case SCE_FIOS_OVERLAY_RESOLVE_FOR_READ:
        return "SCE_FIOS_OVERLAY_RESOLVE_FOR_READ";
    case SCE_FIOS_OVERLAY_RESOLVE_FOR_WRITE:
        return "SCE_FIOS_OVERLAY_RESOLVE_FOR_WRITE";
    }
    return std::to_string(type);
}

// The overlay functions call SceFios2Kernel as a game, which is neither a
// system nor a root program: it may only name its own process.

EXPORT(int, sceFiosOverlayAddForProcess02, SceUID processId, SceFiosProcessOverlay *pOverlay, SceFiosOverlayID *pOutID) {
    TRACY_FUNC(sceFiosOverlayAddForProcess02, processId, pOverlay, pOutID);
    if (pOverlay && pOverlay->type != SCE_FIOS_OVERLAY_TYPE_OPAQUE)
        LOG_WARN("Using unimplemented overlay type {}.", fmt::underlying(pOverlay->type));
    return create_overlay(emuenv.io, KernelState::process_id, processId, pOverlay, pOutID);
}

EXPORT(int, sceFiosOverlayGetInfoForProcess02, SceUID processId, SceFiosOverlayID id, SceFiosProcessOverlay *pOutOverlay) {
    TRACY_FUNC(sceFiosOverlayGetInfoForProcess02, processId, id, pOutOverlay);
    return get_overlay(emuenv.io, KernelState::process_id, processId, id, pOutOverlay);
}

EXPORT(int, sceFiosOverlayGetList02, SceUID processId, uint32_t minOrder, uint32_t maxOrder, SceFiosOverlayID *pOutIDs, SceUInt32 maxIDs, SceUInt32 *pActualIDs) {
    TRACY_FUNC(sceFiosOverlayGetList02, processId, minOrder, maxOrder, pOutIDs, maxIDs, pActualIDs);
    if (!pOutIDs && maxIDs)
        return SCE_FIOS_ERROR_BAD_PTR;
    if (pOutIDs)
        memset(pOutIDs, 0, maxIDs * sizeof(SceFiosOverlayID));
    const std::lock_guard<std::mutex> guard(emuenv.io.overlay_mutex);
    // Another process's overlays and privileged ones are left out, not refused.
    SceUInt32 count = 0;
    for (const auto &overlay : emuenv.io.overlays) {
        if (processId != KernelState::process_id || overlay.process_id != processId || overlay.order >= 0x80)
            continue;
        if (overlay.order < minOrder || overlay.order > maxOrder)
            continue;
        if (pOutIDs && count < maxIDs)
            pOutIDs[count] = overlay.id;
        ++count;
    }
    if (pActualIDs)
        *pActualIDs = count;
    return SCE_FIOS_OK;
}

EXPORT(int, sceFiosOverlayGetRecommendedScheduler02, int param1, const char *path) {
    TRACY_FUNC(sceFiosOverlayGetRecommendedScheduler02, param1, path);
    // reversed engineered
    if (param1 <= 1)
        return 0;

    // returns if path starts with hostk: with k an integer
    if (strlen(path) < strlen("host0:"))
        return 0;

    return memcmp(path, "host", 4) == 0 && path[4] <= '9' && path[5] == ':';
}

EXPORT(int, sceFiosOverlayModifyForProcess02, SceUID processId, SceFiosOverlayID id, const SceFiosProcessOverlay *pNewValue) {
    TRACY_FUNC(sceFiosOverlayModifyForProcess02, processId, id, pNewValue);
    return modify_overlay(emuenv.io, KernelState::process_id, processId, id, pNewValue);
}

EXPORT(int, sceFiosOverlayRemoveForProcess02, SceUID processId, SceFiosOverlayID id) {
    TRACY_FUNC(sceFiosOverlayRemoveForProcess02, processId, id);
    return remove_overlay(emuenv.io, KernelState::process_id, processId, id);
}

EXPORT(int, sceFiosOverlayResolveSync02) {
    TRACY_FUNC(sceFiosOverlayResolveSync02);
    return UNIMPLEMENTED();
}

EXPORT(int, sceFiosOverlayResolveWithRangeSync02, SceUID processId, SceFiosOverlayResolveMode resolveFlag, const char *pInPath, char *pOutPath, SceUInt32 maxPath, SceUInt32 min_order, SceUInt32 max_order) {
    TRACY_FUNC(sceFiosOverlayResolveWithRangeSync02, processId, resolveFlag, pInPath, pOutPath, maxPath, min_order, max_order);
    if (!pInPath || !pOutPath)
        return SCE_FIOS_ERROR_BAD_PTR;
    // A thread that disabled overlays still sees the privileged ones.
    if (emuenv.kernel.get_thread(thread_id)->fios_overlays_disabled)
        min_order = std::max<SceUInt32>(min_order, 0x80);
    std::string resolved;
    if (const int error = resolve_path(emuenv.io, processId, pInPath, resolved, min_order, max_order))
        return error;
    if (maxPath) {
        const size_t length = std::min<size_t>(resolved.size(), maxPath - 1);
        memcpy(pOutPath, resolved.data(), length);
        pOutPath[length] = '\0';
    }
    return SCE_FIOS_OK;
}

EXPORT(int, sceFiosOverlayThreadIsDisabled02) {
    TRACY_FUNC(sceFiosOverlayThreadIsDisabled02);
    return emuenv.kernel.get_thread(thread_id)->fios_overlays_disabled;
}

EXPORT(int, sceFiosOverlayThreadSetDisabled02, SceInt32 disabled) {
    TRACY_FUNC(sceFiosOverlayThreadSetDisabled02, disabled);
    emuenv.kernel.get_thread(thread_id)->fios_overlays_disabled = disabled != 0;
    return SCE_FIOS_OK;
}
