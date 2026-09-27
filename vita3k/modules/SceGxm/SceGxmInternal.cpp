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

#include "SceGxm.h"

#include <gxm/state.h>

#include <util/tracy.h>

TRACY_MODULE_NAME(SceGxmInternal);

EXPORT(int, sceGxmCheckMemoryInternal) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceGxmCreateRenderTargetInternal, const SceGxmRenderTargetParams *params, Ptr<SceGxmRenderTarget> *renderTarget) {
    TRACY_FUNC(sceGxmCreateRenderTargetInternal, params, renderTarget);
    return CALL_EXPORT(sceGxmCreateRenderTarget, params, renderTarget);
}

// libgxm 3.74 has a display queue only when sceGxmInitialize was given a
// display queue callback; without one the thread id is -1.
EXPORT(SceUID, sceGxmGetDisplayQueueThreadIdInternal) {
    TRACY_FUNC(sceGxmGetDisplayQueueThreadIdInternal);
    if (!emuenv.gxm.initialized || !emuenv.gxm.params.displayQueueCallback)
        return -1;
    return emuenv.gxm.display_queue_thread;
}

EXPORT(int, sceGxmGetRenderTargetMemSizeInternal, const SceGxmRenderTargetParams *params, uint32_t *hostMemSize) {
    TRACY_FUNC(sceGxmGetRenderTargetMemSizeInternal, params, hostMemSize);
    return CALL_EXPORT(sceGxmGetRenderTargetMemSize, params, hostMemSize);
}

EXPORT(int, sceGxmGetTopContextInternal) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceGxmInitializedInternal) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceGxmIsInitializationInternal) {
    return UNIMPLEMENTED();
}

// Checked like sceGxmMapFragmentUsseMemory; the driver maps into the internal
// USSE heap instead, which the guest address stands in for as well.
EXPORT(int, sceGxmMapFragmentUsseMemoryInternal, Ptr<void> base, uint32_t size, uint32_t *offset) {
    TRACY_FUNC(sceGxmMapFragmentUsseMemoryInternal, base, size, offset);
    if (auto error = gxm_check_usse_mapping(emuenv, export_name, base, size, offset))
        return error;
    *offset = base.address();
    return 0;
}

// Checked like sceGxmMapVertexUsseMemory; the driver maps into the internal
// USSE heap instead, which the guest address stands in for as well.
EXPORT(int, sceGxmMapVertexUsseMemoryInternal, Ptr<void> base, uint32_t size, uint32_t *offset) {
    TRACY_FUNC(sceGxmMapVertexUsseMemoryInternal, base, size, offset);
    if (auto error = gxm_check_usse_mapping(emuenv, export_name, base, size, offset))
        return error;
    *offset = base.address();
    return 0;
}

EXPORT(int, sceGxmRenderingContextIsWithinSceneInternal) {
    TRACY_FUNC(sceGxmRenderingContextIsWithinSceneInternal);
    return gxm_immediate_context_within_scene(emuenv) ? 1 : 0;
}

EXPORT(int, sceGxmSetCallbackInternal) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceGxmSetInitializeParamInternal) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceGxmUnmapFragmentUsseMemoryInternal, void *base) {
    TRACY_FUNC(sceGxmUnmapFragmentUsseMemoryInternal, base);
    return gxm_check_usse_unmapping(emuenv, export_name, base);
}

EXPORT(int, sceGxmUnmapVertexUsseMemoryInternal, void *base) {
    TRACY_FUNC(sceGxmUnmapVertexUsseMemoryInternal, base);
    return gxm_check_usse_unmapping(emuenv, export_name, base);
}
