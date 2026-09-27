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
#include <vector>

#include <cpu/functions.h>
#include <kernel/state.h>
#include <kernel/sync_primitives.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <net/state.h>
#include <np/common.h>
#include <np/state.h>

// Firmware 3.74 np_signaling.suprx. Contexts are local. Offline, a
// connection lives only until the library's SceNpSignalingMain thread has
// reported it dead to its contexts' handlers; then it is freed, so no
// connection is ever found by id.
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

DECL_EXPORT(int, sceKernelWaitThreadEnd, SceUID thid, int *stat, SceUInt *timeout);

#ifdef __EMSCRIPTEN__
// SceNpSignalingMain's message pipe and the connections the messages name
// are modelled as a guest ring of events: the thread waits on a semaphore
// for each, then calls handler(ctx_id, conn_id, event, error, arg) for
// every context attached to the connection. An event with count ~0 ends
// the thread (sceNpSignalingTerm's message 1).
constexpr uint32_t signaling_ring_events = 64; // the pipe's 0x400 bytes of 16-byte messages
constexpr uint32_t SCE_KERNEL_ERROR_MPP_FULL = 0x800201b3;
constexpr uint32_t signaling_event_contexts = 8; // contexts per connection
struct SignalingEvent {
    uint32_t conn_id;
    uint32_t error;
    uint32_t count;
    struct {
        uint32_t handler; // 0: the context was destroyed meanwhile
        uint32_t ctx_id;
        uint32_t arg;
    } contexts[signaling_event_contexts];
};
static_assert(sizeof(SignalingEvent) == 108);
constexpr uint32_t signaling_code_words = 64;
struct SignalingRing {
    uint32_t read; // events the thread has finished
    uint32_t reserved;
    SignalingEvent events[signaling_ring_events];
};

static std::vector<uint32_t> signaling_main_code(Address code, SceUID sema, Address ring) {
    std::vector<uint32_t> w;
    const auto movw_movt = [&](unsigned reg, uint32_t value) {
        w.push_back(0xe3000000u | ((value & 0xf000u) << 4) | (reg << 12) | (value & 0xfffu));
        value >>= 16;
        w.push_back(0xe3400000u | ((value & 0xf000u) << 4) | (reg << 12) | (value & 0xfffu));
    };
    // Branch with condition `cond` from the next word to word index `target`.
    const auto branch = [&](uint32_t cond_op, size_t target) {
        const int32_t offset = static_cast<int32_t>(target) - static_cast<int32_t>(w.size() + 2);
        w.push_back(cond_op | (static_cast<uint32_t>(offset) & 0xffffffu));
    };
    const Address stub = code + (signaling_code_words - 4) * 4;
    w.push_back(0xe92d41f0); // push {r4-r8, lr}
    w.push_back(0xe24dd008); // sub sp, sp, #8
    movw_movt(4, ring);
    const size_t loop = w.size();
    movw_movt(0, static_cast<uint32_t>(sema));
    w.push_back(0xe3a01001); // mov r1, #1
    w.push_back(0xe3a02000); // mov r2, #0
    const int32_t to_stub = static_cast<int32_t>(stub - (code + w.size() * 4 + 8));
    w.push_back(0xeb000000u | ((static_cast<uint32_t>(to_stub) >> 2) & 0xffffffu)); // bl sceKernelWaitSema
    w.push_back(0xe3500000); // cmp r0, #0
    const size_t exit_on_error = w.size();
    w.push_back(0); // bne done (patched)
    w.push_back(0xe5945000); // ldr r5, [r4]: events finished
    w.push_back(0xe205603f); // and r6, r5, #63
    w.push_back(0xe3a0706c); // mov r7, #108
    w.push_back(0xe0264796); // mla r6, r6, r7, r4
    w.push_back(0xe2866008); // add r6, r6, #8: the event
    w.push_back(0xe5968008); // ldr r8, [r6, #8]: count
    w.push_back(0xe3780001); // cmn r8, #1
    const size_t exit_on_marker = w.size();
    w.push_back(0); // beq done (patched)
    w.push_back(0xe286700c); // add r7, r6, #12
    const size_t inner = w.size();
    w.push_back(0xe3580000); // cmp r8, #0
    const size_t to_next = w.size();
    w.push_back(0); // beq next (patched)
    w.push_back(0xe597c000); // ldr r12, [r7]: handler
    w.push_back(0xe35c0000); // cmp r12, #0
    w.push_back(0x0a000006); // beq skip (6 words on)
    w.push_back(0xe5970008); // ldr r0, [r7, #8]: arg
    w.push_back(0xe58d0000); // str r0, [sp]
    w.push_back(0xe5970004); // ldr r0, [r7, #4]: context id
    w.push_back(0xe5961000); // ldr r1, [r6]: connection id
    w.push_back(0xe3a02000); // mov r2, #0: dead
    w.push_back(0xe5963004); // ldr r3, [r6, #4]: error
    w.push_back(0xe12fff3c); // blx r12
    w.push_back(0xe287700c); // skip: add r7, r7, #12
    w.push_back(0xe2488001); // sub r8, r8, #1
    branch(0xea000000, inner); // b inner
    const size_t next = w.size();
    w.push_back(0xe2855001); // add r5, r5, #1
    w.push_back(0xe5845000); // str r5, [r4]
    branch(0xea000000, loop); // b loop
    const size_t done = w.size();
    w.push_back(0xe28dd008); // add sp, sp, #8
    w.push_back(0xe8bd81f0); // pop {r4-r8, pc}
    const auto patch = [&](size_t at, uint32_t cond_op, size_t target) {
        const int32_t offset = static_cast<int32_t>(target) - static_cast<int32_t>(at + 2);
        w[at] = cond_op | (static_cast<uint32_t>(offset) & 0xffffffu);
    };
    patch(exit_on_error, 0x1a000000, done);
    patch(exit_on_marker, 0x0a000000, done);
    patch(to_next, 0x0a000000, next);
    w.resize(signaling_code_words - 4, 0xe1a00000); // nop
    w.insert(w.end(), { 0xef000000, 0xe1a0f00e, 0x0C7B834B, 0 }); // sceKernelWaitSema stub
    return w;
}

static SignalingRing &signaling_ring(EmuEnvState &emuenv) {
    return *Ptr<SignalingRing>(emuenv.np.signaling_code + signaling_code_words * 4).get(emuenv.mem);
}

// Posts an event to SceNpSignalingMain: false when its ring is full.
static bool post_signaling_event(EmuEnvState &emuenv, const char *export_name, SceUID thread_id, const SignalingEvent &event) {
    auto &np = emuenv.np;
    auto &ring = signaling_ring(emuenv);
    if (np.signaling_queued - ring.read >= signaling_ring_events)
        return false;
    ring.events[np.signaling_queued % signaling_ring_events] = event;
    ++np.signaling_queued;
    return semaphore_signal(emuenv.kernel, export_name, thread_id, np.signaling_sema, 1) == 0;
}
#endif

static bool same_np_id(EmuEnvState &emuenv, const char *export_name, SceUID thread_id, const np::SceNpId &a, const np::SceNpId &b) {
    return CALL_EXPORT(sceNpCmpNpId, const_cast<np::SceNpId *>(&a), const_cast<np::SceNpId *>(&b)) == 0;
}

// np_signaling 0x81001052: the checks and a connection, reused while one to
// the same peer is still live (0x8100220e) or new (0x8100234a), whose id is
// written before SceNpSignalingMain handles its message. There
// (0x810051c6 -> 0x810049a0) the first send has no socket and the fallback
// reads the IP address, sceNetCtlInetGetInfo(15), whose error ends the
// connection: each attached context's handler gets the dead event with that
// error (0x81002660 -> 0x810018fc) and the connection is freed (0x810024ec).
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
    auto &ring = signaling_ring(emuenv);
    // Events the thread has finished free their connections.
    while (!np.signaling_pending.empty() && np.signaling_pending.front().seq < ring.read)
        np.signaling_pending.pop_front();
    for (const auto &live : np.signaling_pending) {
        if (!same_np_id(emuenv, export_name, thread_id, live.own_id, own_id) || !same_np_id(emuenv, export_name, thread_id, live.peer_id, *peer_id))
            continue;
        // Attached before its event is handled, the context gets it too;
        // while it is being handled, no longer.
        auto &event = ring.events[live.seq % signaling_ring_events];
        const bool attached = std::any_of(event.contexts, event.contexts + event.count, [&](const auto &c) { return c.ctx_id == uint32_t(ctx_id); });
        if (!attached && event.count < signaling_event_contexts)
            event.contexts[event.count++] = { ctx->second.handler, static_cast<uint32_t>(ctx_id), ctx->second.arg };
        *conn_id = live.id;
        return 0;
    }
    // The first id is random on the console; ids wrap from 65535 to 1.
    const uint16_t id = np.signaling_last_conn_id == 0xffff ? 1 : np.signaling_last_conn_id + 1;
    SignalingEvent event{};
    event.conn_id = id;
    event.error = emuenv.netctl.inited ? SCE_NET_CTL_ERROR_NOT_CONNECTED : SCE_NET_CTL_ERROR_NOT_INITIALIZED;
    event.count = 1;
    event.contexts[0] = { ctx->second.handler, static_cast<uint32_t>(ctx_id), ctx->second.arg };
    const uint32_t seq = np.signaling_queued;
    if (!post_signaling_event(emuenv, export_name, thread_id, event))
        return RET_ERROR(SCE_KERNEL_ERROR_MPP_FULL); // the pipe's 64 messages; the console would wait
    np.signaling_last_conn_id = id;
    np.signaling_pending.push_back({ seq, id, own_id, *peer_id });
    *conn_id = id;
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
#ifdef __EMSCRIPTEN__
    // Its handler gets no event still queued (message 20 detaches it).
    auto &ring = signaling_ring(emuenv);
    for (const auto &live : emuenv.np.signaling_pending) {
        if (live.seq < ring.read)
            continue;
        auto &event = ring.events[live.seq % signaling_ring_events];
        for (uint32_t i = 0; i < event.count; ++i) {
            if (event.contexts[i].ctx_id == uint32_t(ctx_id))
                event.contexts[i].handler = 0;
        }
    }
#endif
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
    // SceNpSignalingMain (0x81000b3e), with Init's defaults for 0 arguments;
    // it starts at once and waits for messages.
    auto &np = emuenv.np;
    const Address code = alloc(emuenv.mem, signaling_code_words * 4 + sizeof(SignalingRing), "SceNpSignalingMain");
    if (!code)
        return RET_ERROR(SCE_KERNEL_ERROR_NO_MEMORY);
    const SceUID sema = semaphore_create(emuenv.kernel, export_name, "SceNpSignalingEventQueue", thread_id, 0, 0, signaling_ring_events);
    if (sema < 0) {
        free(emuenv.mem, code);
        return sema;
    }
    const auto words = signaling_main_code(code, sema, code + signaling_code_words * 4);
    std::memcpy(Ptr<uint32_t>(code).get(emuenv.mem), words.data(), words.size() * 4);
    std::memset(Ptr<SignalingRing>(code + signaling_code_words * 4).get(emuenv.mem), 0, sizeof(SignalingRing));
    const SceUID main_thread = CALL_EXPORT(sceKernelCreateThread, "SceNpSignalingMain", SceKernelThreadEntry(code),
        thread_priority ? thread_priority : SCE_KERNEL_DEFAULT_PRIORITY_USER, stack_size ? stack_size : 0x4000, 0, cpu_affinity,
        Ptr<SceKernelThreadOptParam>());
    const ThreadStatePtr thread = main_thread < 0 ? nullptr : emuenv.kernel.get_thread(main_thread);
    const int started = thread ? thread->start(0, Ptr<void>{}) : main_thread;
    if (started < 0) {
        if (thread)
            thread->exit_delete(false);
        semaphore_delete(emuenv.kernel, export_name, thread_id, sema);
        free(emuenv.mem, code);
        return started;
    }
    np.signaling_main_thread = main_thread;
    np.signaling_sema = sema;
    np.signaling_code = code;
    np.signaling_queued = 0;
    np.signaling_last_conn_id = 0;
    np.signaling_pending.clear();
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
    // Term's message 1 comes after those queued: SceNpSignalingMain handles
    // them first, and Term waits for it to end (0x81000c60).
    auto &np = emuenv.np;
    SignalingEvent stop{};
    stop.count = ~0u;
    // Only a running guest thread can wait (not a host-side call).
    const ThreadStatePtr caller = emuenv.kernel.get_thread(thread_id);
    if (caller && caller->status == ThreadStatus::run && thread_id != np.signaling_main_thread
        && post_signaling_event(emuenv, export_name, thread_id, stop))
        CALL_EXPORT(sceKernelWaitThreadEnd, np.signaling_main_thread, nullptr, nullptr);
    semaphore_delete(emuenv.kernel, export_name, thread_id, np.signaling_sema);
    if (thread_id != np.signaling_main_thread) {
        if (const ThreadStatePtr main_thread = emuenv.kernel.get_thread(np.signaling_main_thread))
            main_thread->exit_delete(false);
        free(emuenv.mem, np.signaling_code);
    }
    np.signaling_main_thread = np.signaling_sema = 0;
    np.signaling_code = 0;
    np.signaling_pending.clear();
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
