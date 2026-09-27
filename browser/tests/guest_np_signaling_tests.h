// sceNpSignalingActivateConnection offline, as firmware 3.74 np_signaling
// handles it: checks, a connection id, and the dead event its context
// handler gets on SceNpSignalingMain before the connection is freed.
#pragma once
#include "guest_sync_delete_tests.h"
#include <np/state.h>

inline void test_guest_np_signaling(EmuEnvState &env, vita3k::web::GuestThreadRuntime &runtime) {
    constexpr uint32_t sig_init = 0x4B6ACF47, sig_term = 0xBC892D18, create_ctx = 0xF77EF683, destroy_ctx = 0xEAA4B1F3,
                       activate = 0x92FFBDE3, terminate = 0xA413F8C2, get_thread_id = 0x0FB972F9;
    const Address code = alloc(env.mem, 0x1000, "signaling code");
    const Address data = alloc(env.mem, 0x1000, "signaling data");
    REQUIRE(code && data);
    std::memset(Ptr<uint8_t>(data).get(env.mem), 0, 0x1000);
    const auto word = [&](Address offset) -> uint32_t & { return *Ptr<uint32_t>(data + offset).get(env.mem); };
    env.kernel.call_import = [&](CPUState &cpu, uint32_t nid, SceUID tid) {
        call_import(env, cpu, nid, tid);
        REQUIRE(env.missing_nids.empty());
    };
    REQUIRE(runtime.attach(env));
    auto host = env.kernel.create_thread(env.mem, "signaling fixture", Ptr<const void>(code), SCE_KERNEL_DEFAULT_PRIORITY_USER,
        SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
    REQUIRE(host);
    const auto call = [&](uint32_t nid, std::initializer_list<uint32_t> args) {
        uint32_t reg = 0;
        for (const uint32_t value : args)
            write_reg(*host->cpu, reg++, value);
        call_import(env, *host->cpu, nid, host->id);
        REQUIRE(env.missing_nids.empty());
        return read_reg(*host->cpu, 0);
    };
    const auto run_until = [&](auto done) {
        const auto limit = vita3k::web::GuestThreadRuntime::now_us() + 2000000;
        while (!done()) {
            REQUIRE(runtime.resume(64).failed == 0);
            REQUIRE(vita3k::web::GuestThreadRuntime::now_us() < limit);
        }
    };
    const Address own = data + 0x800, peer = data + 0x830, peer2 = data + 0x860, bad = data + 0x890;
    const Address ctx_out = data + 0x10, conn_out = data + 0x14, conn2_out = data + 0x18;
    const auto set_id = [&](Address at, const char *name, int8_t valid) {
        auto *id = Ptr<np::SceNpId>(at).get(env.mem);
        *id = {};
        std::strcpy(id->handle.data, name);
        id->isIdValid = valid;
    };
    set_id(own, "own", 1);
    set_id(peer, "peer", 1);
    set_id(peer2, "peer2", 1);
    set_id(bad, "peer", 0);

    // The handler records (ctx, conn, event, error, arg, thread) per call at
    // 0x40 + 0x20 * n; the first call activates a second peer from inside it.
    const Address handler = code + 0x400, stubs = code + 0x700;
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t stub[] = { 0xef000000, 0xe1a0f00e, i ? activate : get_thread_id };
        std::memcpy(Ptr<void>(stubs + 16 * i).get(env.mem), stub, sizeof(stub));
    }
    {
        guest_thread_fixture::Arm p(handler);
        p.emit(0xe92d4070); // push {r4-r6, lr}
        p.emit(0xe59dc010); // ldr r12, [sp, #16]: arg
        p.constant(4, data);
        p.load(5, 0x3c);
        p.emit(0xe0846285); // add r6, r4, r5, lsl #5
        p.emit(0xe2866040); // add r6, r6, #0x40
        p.emit(0xe5860000); // str r0, [r6]
        p.emit(0xe5861004); // str r1, [r6, #4]
        p.emit(0xe5862008); // str r2, [r6, #8]
        p.emit(0xe586300c); // str r3, [r6, #12]
        p.emit(0xe586c010); // str r12, [r6, #16]
        p.emit(0xe2855001); // add r5, r5, #1
        p.store(5, 0x3c);
        p.call(stubs);
        p.emit(0xe5860014); // str r0, [r6, #20]
        p.emit(0xe3550001); // cmp r5, #1
        p.emit(0x1a000006); // bne: skip the nested activation (7 instructions)
        p.emit(0xe5960000); // ldr r0, [r6]
        p.constant(1, peer2);
        p.constant(2, conn2_out);
        p.call(stubs + 16);
        p.emit(0xe5860018); // str r0, [r6, #24]
        p.emit(0xe3a00000); // mov r0, #0
        p.emit(0xe8bd8070); // pop {r4-r6, pc}
        p.finish(env.mem);
    }

    REQUIRE(call(activate, { 1, peer, conn_out }) == 0x80552701);
    REQUIRE(call(sig_init, { 0, 0, 0, 0 }) == 0);
    const SceUID main_thread = env.np.signaling_main_thread;
    const auto main = env.kernel.get_thread(main_thread);
    REQUIRE(main && main->name == "SceNpSignalingMain");
    const auto main_waiting = [&] { return main->status == ThreadStatus::wait; };
    run_until(main_waiting); // for its first message
    REQUIRE(call(create_ctx, { own, handler, 0x1234, ctx_out }) == 0);
    const uint32_t ctx = word(0x10);
    REQUIRE(call(activate, { ctx, 0, conn_out }) == 0x80552715 && call(activate, { ctx, peer, 0 }) == 0x80552715);
    REQUIRE(call(activate, { ctx, bad, conn_out }) == 0x80550605);
    REQUIRE(call(activate, { ctx + 1, peer, conn_out }) == 0x80552705);
    REQUIRE(call(activate, { ctx, own, conn_out }) == 0x80552716 && word(0x14) == 0);

    const bool netctl_was_inited = env.netctl.inited;
    env.netctl.inited = true;
    guest_sync_delete::build_call(env.mem, code, activate, { ctx, peer, conn_out, 0, 0 }, data + 0x20);
    word(0x20) = 0xcccccccc;
    auto caller = env.kernel.create_thread(env.mem, "signaling caller", Ptr<const void>(code), SCE_KERNEL_DEFAULT_PRIORITY_USER,
        SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
    REQUIRE(caller && caller->start(0, Ptr<void>{}, false) == 0);
    run_until([&] { return caller->status == ThreadStatus::dormant && word(0x3c) == 2 && main_waiting(); });
    REQUIRE(word(0x20) == 0 && word(0x14) == 1);
    // The dead event (0) with the NetCtl error, on SceNpSignalingMain; the
    // connection activated from the handler is reported after it.
    REQUIRE(word(0x40) == ctx && word(0x44) == 1 && word(0x48) == 0 && word(0x4c) == 0x80412108);
    REQUIRE(word(0x50) == 0x1234 && word(0x54) == uint32_t(main_thread) && word(0x58) == 0 && word(0x18) == 2);
    REQUIRE(word(0x60) == ctx && word(0x64) == 2 && word(0x68) == 0 && word(0x6c) == 0x80412108);
    REQUIRE(word(0x74) == uint32_t(main_thread));
    // Both connections were freed after their events.
    REQUIRE(call(terminate, { ctx, 1 }) == 0x8055270e && call(terminate, { ctx, 2 }) == 0x8055270e);

    // Activation does not wait for the handler. Before the event is handled
    // the connection is live: the same peer gets its id again, and another
    // context attached to it gets the event as well.
    const Address peer3 = data + 0x8c0;
    set_id(peer3, "peer3", 1);
    REQUIRE(call(create_ctx, { own, handler, 0x5678, ctx_out }) == 0);
    const uint32_t ctx2 = word(0x10);
    word(0x3c) = 1; // no nested activation this time
    REQUIRE(call(activate, { ctx, peer3, conn_out }) == 0 && word(0x14) == 3);
    REQUIRE(call(activate, { ctx, peer3, conn_out }) == 0 && word(0x14) == 3);
    REQUIRE(call(activate, { ctx2, peer3, conn_out }) == 0 && word(0x14) == 3);
    REQUIRE(word(0x3c) == 1);
    run_until([&] { return word(0x3c) == 3 && main_waiting(); });
    REQUIRE(word(0x60) == ctx && word(0x64) == 3 && word(0x70) == 0x1234);
    REQUIRE(word(0x80) == ctx2 && word(0x84) == 3 && word(0x90) == 0x5678);
    // A context destroyed before the event is handled gets nothing, even if
    // its id is given to a new context.
    REQUIRE(call(activate, { ctx2, peer2, conn_out }) == 0 && word(0x14) == 4);
    REQUIRE(call(destroy_ctx, { ctx2 }) == 0 && call(create_ctx, { own, handler, 0x9abc, ctx_out }) == 0 && word(0x10) == ctx2);
    run_until(main_waiting);
    REQUIRE(word(0x3c) == 3);
    // Before sceNetCtlInit the error is NetCtl's NOT_INITIALIZED.
    env.netctl.inited = false;
    word(0x3c) = 0;
    REQUIRE(caller->start(0, Ptr<void>{}, false) == 0);
    run_until([&] { return caller->status == ThreadStatus::dormant && word(0x3c) == 2 && main_waiting(); });
    REQUIRE(word(0x20) == 0 && word(0x14) == 5 && word(0x18) == 6 && word(0x4c) == 0x80412101);
    env.netctl.inited = netctl_was_inited;
    // A context without a handler gets no event.
    REQUIRE(call(create_ctx, { own, 0, 0, ctx_out }) == 0);
    word(0x3c) = 0;
    REQUIRE(call(activate, { word(0x10), peer, conn_out }) == 0 && word(0x14) == 7);
    run_until(main_waiting);
    REQUIRE(word(0x3c) == 0);

    // Term handles the queued messages first, then ends the thread.
    word(0x3c) = 1;
    REQUIRE(call(activate, { ctx, peer3, conn_out }) == 0 && word(0x14) == 8);
    guest_sync_delete::build_call(env.mem, code, sig_term, { 0, 0, 0, 0, 0 }, data + 0x20);
    word(0x20) = 0xcccccccc;
    REQUIRE(caller->start(0, Ptr<void>{}, false) == 0);
    run_until([&] { return caller->status == ThreadStatus::dormant && !env.kernel.threads.contains(main_thread); });
    REQUIRE(word(0x20) == 0 && word(0x3c) == 2 && word(0x64) == 8 && !env.np.signaling_inited);
    REQUIRE(call(destroy_ctx, { ctx }) == 0x80552701);
    REQUIRE(runtime.shutdown());
    REQUIRE(env.kernel.threads.empty());
    free(env.mem, data);
    free(env.mem, code);
    std::puts("Guest NP signaling: ActivateConnection checks, ids and the dead event on SceNpSignalingMain passed");
}
