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

#include <np/common.h>
#include <np/state.h>

// Firmware 3.74 np_signaling.suprx. Contexts are local; a connection only
// exists after sceNpSignalingActivateConnection, which is not implemented
// (its failure event comes from the library's worker thread), so no
// connection is ever found.
enum SceNpSignalingError : uint32_t {
    SCE_NP_ERROR_INVALID_NPID = 0x80550605, // name unknown
    SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED = 0x80552701,
    SCE_NP_SIGNALING_ERROR_ALREADY_INITIALIZED = 0x80552702,
    SCE_NP_SIGNALING_ERROR_CTX_MAX = 0x80552704,
    SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND = 0x80552705,
    SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND = 0x8055270e,
    SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT = 0x80552715,
};
constexpr int max_signaling_ctxs = 8;

EXPORT(int, sceNpSignalingActivateConnection) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpSignalingCancelPeerNetInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpSignalingCreateCtx, const np::SceNpId *np_id, Ptr<void> handler, Ptr<void> arg, SceInt32 *ctx_id) {
    if (!emuenv.np.signaling_inited)
        return RET_ERROR(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!np_id || !ctx_id)
        return RET_ERROR(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    if (np_id->isIdValid != 1)
        return RET_ERROR(SCE_NP_ERROR_INVALID_NPID);
    for (int id = 1; id <= max_signaling_ctxs; ++id) {
        if (emuenv.np.signaling_ctxs.emplace(id, handler.address()).second) {
            *ctx_id = id;
            return 0;
        }
    }
    return RET_ERROR(SCE_NP_SIGNALING_ERROR_CTX_MAX);
}

EXPORT(int, sceNpSignalingDeactivateConnection) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpSignalingDestroyCtx, SceInt32 ctx_id) {
    if (!emuenv.np.signaling_inited)
        return RET_ERROR(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    emuenv.np.signaling_ctxs.erase(ctx_id); // 0 even for an unknown id
    return 0;
}

EXPORT(int, sceNpSignalingGetConnectionFromNpId) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpSignalingGetConnectionFromPeerAddress) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpSignalingGetConnectionInfo, SceInt32 ctx_id, SceInt32 conn_id, SceInt32 code, Ptr<void> info) {
    if (!emuenv.np.signaling_inited)
        return RET_ERROR(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!info)
        return RET_ERROR(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    if (!emuenv.np.signaling_ctxs.contains(ctx_id))
        return RET_ERROR(SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND);
    return RET_ERROR(SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND);
}

EXPORT(int, sceNpSignalingGetConnectionStatus) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpSignalingGetCtxOpt) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpSignalingGetLocalNetInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpSignalingGetMemoryInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpSignalingGetPeerNetInfo) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpSignalingGetPeerNetInfoResult) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceNpSignalingInit, SceSize pool_size, SceInt32 thread_priority, SceInt32 cpu_affinity, SceSize stack_size) {
    if (emuenv.np.signaling_inited)
        return RET_ERROR(SCE_NP_SIGNALING_ERROR_ALREADY_INITIALIZED);
    emuenv.np.signaling_inited = true;
    return 0;
}

EXPORT(int, sceNpSignalingSetCtxOpt, SceInt32 ctx_id, SceInt32 option, SceInt32 value) {
    if (!emuenv.np.signaling_inited)
        return RET_ERROR(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!emuenv.np.signaling_ctxs.contains(ctx_id))
        return RET_ERROR(SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND);
    if (option != 1 || (value != 0 && value != 1))
        return RET_ERROR(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    return 0;
}

EXPORT(int, sceNpSignalingTerm) {
    if (!emuenv.np.signaling_inited)
        return RET_ERROR(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    emuenv.np.signaling_inited = false;
    emuenv.np.signaling_ctxs.clear();
    return 0;
}

EXPORT(int, sceNpSignalingTerminateConnection, SceInt32 ctx_id, SceInt32 conn_id) {
    if (!emuenv.np.signaling_inited)
        return RET_ERROR(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!emuenv.np.signaling_ctxs.contains(ctx_id))
        return RET_ERROR(SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND);
    return RET_ERROR(SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND);
}
