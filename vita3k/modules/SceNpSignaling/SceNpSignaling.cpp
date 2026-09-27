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

#include <algorithm>
#include <cstring>

#include <cpu/functions.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <net/state.h>
#include <np/common.h>
#include <np/state.h>

// Firmware 3.74 np_signaling.suprx. Contexts are local. Offline, a
// connection lives only until the library's SceNpSignalingMain thread has
// reported it dead to its context's handler; then it is freed, so outside
// that handler no connection is ever found.
enum SceNpSignalingError : uint32_t {
    SCE_NP_ERROR_INVALID_NPID = 0x80550605, // name unknown
    SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED = 0x80552701,
    SCE_NP_SIGNALING_ERROR_ALREADY_INITIALIZED = 0x80552702,
    SCE_NP_SIGNALING_ERROR_CTX_MAX = 0x80552704,
    SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND = 0x80552705,
    SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND = 0x8055270e,
    SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT = 0x80552715,
    SCE_NP_SIGNALING_ERROR_OWN_NP_ID = 0x80552716, // name unknown
};
constexpr int max_signaling_ctxs = 8;
constexpr uint32_t SCE_NP_SIGNALING_EVENT_DEAD = 0;
// sceNetCtlInetGetInfo's answers while disconnected (the shell's service)
// or before sceNetCtlInit (libnetctl).
constexpr uint32_t SCE_NET_CTL_ERROR_NOT_INITIALIZED = 0x80412101;
constexpr uint32_t SCE_NET_CTL_ERROR_NOT_CONNECTED = 0x80412108;

DECL_EXPORT(int, sceNpCmpNpId, np::SceNpId *npid1, np::SceNpId *npid2);
DECL_EXPORT(SceUID, sceKernelCreateThread, const char *name, SceKernelThreadEntry entry, int init_priority, int stack_size, SceUInt attr, int cpu_affinity_mask, Ptr<SceKernelThreadOptParam> option);

#ifdef __EMSCRIPTEN__
// Guest code run as SceNpSignalingMain's entry: calls the handler in the
// block at argp as handler(ctx_id, conn_id, event, error, arg).
constexpr uint32_t signaling_trampoline_code[] = {
    0xe92d4010, // push {r4, lr}
    0xe24dd008, // sub sp, sp, #8
    0xe1a04001, // mov r4, r1
    0xe594c000, // ldr r12, [r4]
    0xe5940014, // ldr r0, [r4, #20]
    0xe58d0000, // str r0, [sp]
    0xe5940004, // ldr r0, [r4, #4]
    0xe5941008, // ldr r1, [r4, #8]
    0xe594200c, // ldr r2, [r4, #12]
    0xe5943010, // ldr r3, [r4, #16]
    0xe12fff3c, // blx r12
    0xe28dd008, // add sp, sp, #8
    0xe8bd8010, // pop {r4, pc}
};
#endif

static bool same_np_id(EmuEnvState &emuenv, const char *export_name, SceUID thread_id, const np::SceNpId &a, const np::SceNpId &b) {
    return CALL_EXPORT(sceNpCmpNpId, const_cast<np::SceNpId *>(&a), const_cast<np::SceNpId *>(&b)) == 0;
}

// np_signaling 0x81001052: the checks and a connection, reused when one to
// the same peer is still live (0x8100220e) or new (0x8100234a), whose id is
// written before SceNpSignalingMain handles it. There (0x810051c6 ->
// 0x810049a0) the first send has no socket and the fallback reads the IP
// address, sceNetCtlInetGetInfo(15), whose error ends the connection: each
// attached context's handler gets the dead event with that error
// (0x81002660 -> 0x810018fc) and the connection is freed (0x810024ec). The
// handlers run here before this call returns, as when SceNpSignalingMain
// outranks the caller; called from a handler, the connection waits for the
// thread's current one, as its message does.
EXPORT(int, sceNpSignalingActivateConnection, SceInt32 ctx_id, np::SceNpId *peer_id, SceInt32 *conn_id) {
#ifdef __EMSCRIPTEN__
    auto &np = emuenv.np;
    if (!np.signaling_inited)
        return RET_ERROR(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!peer_id || !conn_id)
        return RET_ERROR(SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT);
    if (peer_id->isIdValid != 1)
        return RET_ERROR(SCE_NP_ERROR_INVALID_NPID);
    const auto ctx = np.signaling_ctxs.find(ctx_id);
    if (ctx == np.signaling_ctxs.end())
        return RET_ERROR(SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND);
    const np::SceNpId own_id = ctx->second.own_id;
    if (same_np_id(emuenv, export_name, thread_id, own_id, *peer_id))
        return RET_ERROR(SCE_NP_SIGNALING_ERROR_OWN_NP_ID);
    const auto same_pair = [&](const NpState::SignalingConnection &c) {
        return same_np_id(emuenv, export_name, thread_id, c.own_id, own_id) && same_np_id(emuenv, export_name, thread_id, c.peer_id, *peer_id);
    };
    // A live connection: the one being reported, or one queued behind it
    // (its dead event reaches the contexts attached when it is reported).
    if (np.signaling_dying && same_pair(*np.signaling_dying)) {
        *conn_id = np.signaling_dying->id;
        return 0;
    }
    for (auto &pending : np.signaling_pending) {
        if (same_pair(pending)) {
            if (std::ranges::find(pending.ctx_ids, ctx_id) == pending.ctx_ids.end())
                pending.ctx_ids.push_back(ctx_id);
            *conn_id = pending.id;
            return 0;
        }
    }
    // The first id is random on the console; ids wrap from 65535 to 1.
    np.signaling_last_conn_id = np.signaling_last_conn_id == 0xffff ? 1 : np.signaling_last_conn_id + 1;
    *conn_id = np.signaling_last_conn_id;
    const uint32_t error = emuenv.netctl.inited ? SCE_NET_CTL_ERROR_NOT_CONNECTED : SCE_NET_CTL_ERROR_NOT_INITIALIZED;
    np.signaling_pending.push_back({ { ctx_id }, np.signaling_last_conn_id, own_id, *peer_id, error });
    if (np.signaling_dying)
        return 0;
    const ThreadStatePtr main_thread = emuenv.kernel.get_thread(np.signaling_main_thread);
    const ThreadStatePtr caller = emuenv.kernel.get_thread(thread_id);
    while (!np.signaling_pending.empty()) {
        np.signaling_dying = np.signaling_pending.front();
        np.signaling_pending.pop_front();
        const auto dying = *np.signaling_dying;
        for (const int target_id : dying.ctx_ids) {
            // Contexts are looked up when the message is handled: one
            // destroyed meanwhile gets no event.
            const auto target = np.signaling_ctxs.find(target_id);
            if (target == np.signaling_ctxs.end() || !target->second.handler || !main_thread)
                continue;
            const uint32_t call[] = { target->second.handler, static_cast<uint32_t>(target_id), dying.id,
                SCE_NP_SIGNALING_EVENT_DEAD, dying.error, target->second.arg };
            // start() copies the block onto SceNpSignalingMain's stack.
            const Address block = stack_alloc(*caller->cpu, sizeof(call));
            std::memcpy(Ptr<uint32_t>(block).get(emuenv.mem), call, sizeof(call));
            main_thread->run_guest_function(np.signaling_trampoline, sizeof(call), Ptr<void>(block));
            stack_free(*caller->cpu, sizeof(call));
        }
        np.signaling_dying.reset();
        if (!np.signaling_inited) {
            np.signaling_pending.clear(); // sceNpSignalingTerm from a handler
            break;
        }
    }
    return 0;
#else
    return UNIMPLEMENTED();
#endif
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
        if (emuenv.np.signaling_ctxs.emplace(id, NpState::SignalingCtx{ *np_id, handler.address(), arg.address() }).second) {
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
    // Only titles built with SDK 2.00 or later name a known context.
    if (emuenv.kernel.main_module_sdk_version(emuenv.mem) >= 0x02000000 && !emuenv.np.signaling_ctxs.contains(ctx_id))
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
#ifdef __EMSCRIPTEN__
    // SceNpSignalingMain (0x81000b3e), with Init's defaults for 0 arguments.
    const Address trampoline = alloc(emuenv.mem, sizeof(signaling_trampoline_code), "SceNpSignalingMain entry");
    if (!trampoline)
        return RET_ERROR(SCE_KERNEL_ERROR_NO_MEMORY);
    std::memcpy(Ptr<uint32_t>(trampoline).get(emuenv.mem), signaling_trampoline_code, sizeof(signaling_trampoline_code));
    const SceUID main_thread = CALL_EXPORT(sceKernelCreateThread, "SceNpSignalingMain", SceKernelThreadEntry(trampoline),
        thread_priority ? thread_priority : SCE_KERNEL_DEFAULT_PRIORITY_USER, stack_size ? stack_size : 0x4000, 0, cpu_affinity,
        Ptr<SceKernelThreadOptParam>());
    if (main_thread < 0) {
        free(emuenv.mem, trampoline);
        return main_thread;
    }
    emuenv.np.signaling_main_thread = main_thread;
    emuenv.np.signaling_trampoline = trampoline;
    emuenv.np.signaling_last_conn_id = 0;
#endif
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
#ifdef __EMSCRIPTEN__
    if (const ThreadStatePtr main_thread = emuenv.kernel.get_thread(emuenv.np.signaling_main_thread))
        main_thread->exit_delete(false);
    free(emuenv.mem, emuenv.np.signaling_trampoline);
    emuenv.np.signaling_main_thread = 0;
    emuenv.np.signaling_trampoline = 0;
#endif
    return 0;
}

EXPORT(int, sceNpSignalingTerminateConnection, SceInt32 ctx_id, SceInt32 conn_id) {
    if (!emuenv.np.signaling_inited)
        return RET_ERROR(SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    if (!emuenv.np.signaling_ctxs.contains(ctx_id))
        return RET_ERROR(SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND);
    return RET_ERROR(SCE_NP_SIGNALING_ERROR_CONN_NOT_FOUND);
}
