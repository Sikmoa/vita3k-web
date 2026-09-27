// The offline network stack against firmware 3.74 SceNetPs, libnet and the
// shell's NetCtl service: ICM connect, epoll waits with callbacks and the
// disconnected NetCtl info.
#pragma once
#include "guest_sync_delete_tests.h"
#include <kernel/callback.h>
#include <net/state.h>

inline void test_guest_net_offline(EmuEnvState &env, vita3k::web::GuestThreadRuntime &runtime) {
    constexpr uint32_t socket_nid = 0xF084FCE3, bind_nid = 0x1296A94B, sendto_nid = 0x52DB31D5,
                       epoll_create = 0xF9D102AE, epoll_control = 0x4C8764AC, epoll_wait = 0x45CE337D,
                       epoll_wait_cb = 0x92D3E767, icm_connect = 0x93F2FF08, socket_close = 0x29822B4D,
                       epoll_destroy = 0x7915CAF3, inet_get_info = 0xB26D07F3;
    constexpr uint32_t icm_done = 0x40000;
    const Address code = alloc(env.mem, 0x1000, "net offline code");
    const Address data = alloc(env.mem, 0x1000, "net offline data");
    REQUIRE(code && data);
    const auto word = [&](Address offset) -> uint32_t & { return *Ptr<uint32_t>(data + offset).get(env.mem); };
    env.kernel.call_import = [&](CPUState &cpu, uint32_t nid, SceUID tid) {
        call_import(env, cpu, nid, tid);
        REQUIRE(env.missing_nids.empty());
    };
    REQUIRE(runtime.attach(env));
    auto host = env.kernel.create_thread(env.mem, "net fixture", Ptr<const void>(code), SCE_KERNEL_DEFAULT_PRIORITY_USER,
        SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
    REQUIRE(host);
    auto &cpu = *host->cpu;
    const auto call = [&](uint32_t nid, std::initializer_list<uint32_t> args) {
        uint32_t reg = 0;
        for (const uint32_t value : args)
            write_reg(cpu, reg++, value);
        call_import(env, cpu, nid, host->id);
        REQUIRE(env.missing_nids.empty());
        return read_reg(cpu, 0);
    };
    auto &net_errno = host->tls.get_ptr<uint32_t>().get(env.mem)[TLS_NET_ERRNO];
    const Address name = data + 0x100, events = data + 0x200, addr = data + 0x300, message = data + 0x320;
    std::strcpy(Ptr<char>(name).get(env.mem), "net fixture");
    std::strcpy(Ptr<char>(message).get(env.mem), "woken");
    auto *event = Ptr<SceNetEpollEvent>(events).get(env.mem);
    const auto event_id = [&] {
        uint32_t id;
        std::memcpy(&id, event->data.data, sizeof(id));
        return id;
    };
    const auto add = [&](uint32_t eid, uint32_t op, uint32_t id, uint32_t mask) {
        const Address ev = data + 0x280;
        Ptr<SceNetEpollEvent>(ev).get(env.mem)->events = mask;
        std::memcpy(Ptr<SceNetEpollEvent>(ev).get(env.mem)->data.data, &id, sizeof(id));
        return call(epoll_control, { eid, op, id, ev });
    };

    const bool net_was_inited = env.net.inited;
    env.net.inited = false;
    net_errno = 0x77;
    REQUIRE(call(icm_connect, { 1, 0x80 }) == 0x804101c8 && call(epoll_wait_cb, { 1, events, 1, 0 }) == 0x804101c8);
    REQUIRE(net_errno == 0x77); // libnet refuses before the syscall, errno untouched
    env.net.inited = true;

    // ICM connect with no interface: 0 at once, one ICM-done event for the
    // entry that registered it, consumed by the first scan that evaluates it.
    const uint32_t first = call(socket_nid, { name, SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0 });
    const uint32_t second = call(socket_nid, { name, SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0 });
    REQUIRE(static_cast<int32_t>(first) > 0 && second > first);
    REQUIRE(call(icm_connect, { 0x7fff, 0 }) == 0x80410109 && net_errno == SCE_NET_EBADF);
    const uint32_t eid = call(epoll_create, { name, 0 });
    REQUIRE(static_cast<int32_t>(eid) > 0);
    REQUIRE(add(eid, SCE_NET_EPOLL_CTL_ADD, first, icm_done | SCE_NET_EPOLLIN) == 0);
    REQUIRE(add(eid, SCE_NET_EPOLL_CTL_ADD, second, SCE_NET_EPOLLIN) == 0);
    REQUIRE(call(icm_connect, { first, 0x80 }) == 0 && call(icm_connect, { second, 0 }) == 0);
    REQUIRE(call(epoll_wait, { eid, events, 4, 0 }) == 1);
    REQUIRE(event->events == icm_done && event_id() == first);
    REQUIRE(call(epoll_wait, { eid, events, 4, 0 }) == 0);
    REQUIRE(add(eid, SCE_NET_EPOLL_CTL_MOD, second, icm_done) == 0);
    REQUIRE(call(epoll_wait, { eid, events, 4, 0 }) == 0); // the second socket's was consumed unseen
    // Once maxevents are filled, later entries are not evaluated.
    REQUIRE(call(icm_connect, { first, 0 }) == 0 && call(icm_connect, { second, 0 }) == 0);
    REQUIRE(call(epoll_wait, { eid, events, 1, 0 }) == 1 && event_id() == first);
    REQUIRE(call(epoll_wait, { eid, events, 1, 0 }) == 1 && event_id() == second);
    REQUIRE(add(eid, SCE_NET_EPOLL_CTL_DEL, first, 0) == 0 && add(eid, SCE_NET_EPOLL_CTL_DEL, second, 0) == 0);

    // sceNetEpollWaitCB runs the waiter's notified callbacks when it would
    // wait; one that makes an entry ready ends the wait with it.
    auto *to = Ptr<SceNetSockaddrIn>(addr).get(env.mem);
    *to = {};
    to->sin_len = sizeof(SceNetSockaddrIn);
    to->sin_family = SCE_NET_AF_INET;
    to->sin_port = __builtin_bswap16(40123);
    to->sin_addr.s_addr = __builtin_bswap32(0x7f000001);
    REQUIRE(call(bind_nid, { first, addr, sizeof(SceNetSockaddrIn) }) == 0);
    REQUIRE(add(eid, SCE_NET_EPOLL_CTL_ADD, first, SCE_NET_EPOLLIN) == 0);
    const Address callback = code + 0x800, sendto_stub = code + 0x900;
    {
        const uint32_t stub[] = { 0xef000000, 0xe1a0f00e, sendto_nid };
        std::memcpy(Ptr<void>(sendto_stub).get(env.mem), stub, sizeof(stub));
        guest_thread_fixture::Arm p(callback);
        p.emit(0xe92d4010); // push {r4,lr}
        p.constant(4, data);
        p.store(2, 0x40); // the notify argument
        p.emit(0xe24dd008); // sub sp, sp, #8
        p.constant(0, addr);
        p.emit(0xe58d0000); // str r0, [sp]: to
        p.constant(0, sizeof(SceNetSockaddrIn));
        p.emit(0xe58d0004); // str r0, [sp, #4]: tolen
        p.constant(0, second);
        p.constant(1, message);
        p.constant(2, 5);
        p.constant(3, 0);
        p.call(sendto_stub);
        p.store(0, 0x44);
        p.emit(0xe28dd008); // add sp, sp, #8
        p.constant(0, 0); // keep the callback
        p.emit(0xe8bd8010); // pop {r4,pc}
        p.finish(env.mem);
    }
    std::string callback_name = "net fixture callback";
    const auto run_until = [&](auto done) {
        const auto limit = vita3k::web::GuestThreadRuntime::now_us() + 2000000;
        while (!done()) {
            REQUIRE(runtime.resume(64).failed == 0);
            REQUIRE(vita3k::web::GuestThreadRuntime::now_us() < limit);
        }
    };
    const auto wait_with = [&](uint32_t nid, uint32_t timeout_us, bool notify) {
        guest_sync_delete::build_call(env.mem, code, nid, { eid, events, 1, timeout_us, 0 }, data + 0x48);
        word(0x40) = word(0x44) = word(0x48) = 0xcccccccc;
        auto waiter = env.kernel.create_thread(env.mem, "net waiter", Ptr<const void>(code), SCE_KERNEL_DEFAULT_PRIORITY_USER,
            SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        REQUIRE(waiter);
        auto cb = std::make_shared<Callback>(waiter->id, callback_name, Ptr<SceKernelCallbackFunction>(callback), Ptr<void>(data));
        waiter->callbacks.push_back(cb);
        if (notify)
            cb->direct_notify(7);
        REQUIRE(waiter->start(0, Ptr<void>{}, false) == 0);
        run_until([&] { return waiter->status == ThreadStatus::dormant; });
        return cb;
    };
    auto cb = wait_with(epoll_wait_cb, 300000, true);
    REQUIRE(word(0x48) == 1 && event->events == SCE_NET_EPOLLIN && event_id() == first);
    REQUIRE(word(0x40) == 7 && word(0x44) == 5 && !cb->is_executable());
    // sceNetEpollWait runs none; a CB wait with nothing notified just times out.
    constexpr uint32_t recv_nid = 0x023643B7;
    REQUIRE(call(recv_nid, { first, data + 0x3c0, 16, SCE_NET_MSG_DONTWAIT }) == 5);
    cb = wait_with(epoll_wait, 1000, true);
    REQUIRE(word(0x48) == 0 && word(0x40) == 0xcccccccc && cb->is_executable());
    cb = wait_with(epoll_wait_cb, 1000, false);
    REQUIRE(word(0x48) == 0 && word(0x40) == 0xcccccccc);

    REQUIRE(call(epoll_destroy, { eid }) == 0);
    REQUIRE(call(socket_close, { first }) == 0 && call(socket_close, { second }) == 0);

    // NetCtl disconnected: every code, even an invalid one, is NOT_CONNECTED
    // and the info buffer is not written.
    const bool netctl_was_inited = env.netctl.inited;
    env.netctl.inited = true;
    const Address info = data + 0x400;
    word(0x400) = 0xcccccccc;
    for (const uint32_t info_code : { 1u, 15u, 22u, 23u, 0u, 99u })
        REQUIRE(call(inet_get_info, { info_code, info }) == 0x80412108 && word(0x400) == 0xcccccccc);
    REQUIRE(call(inet_get_info, { 1, 0 }) == 0x80412107);
    env.netctl.inited = false;
    REQUIRE(call(inet_get_info, { 1, info }) == 0x80412101);
    env.netctl.inited = netctl_was_inited;

    env.net.inited = net_was_inited;
    REQUIRE(runtime.shutdown());
    REQUIRE(env.kernel.threads.empty());
    free(env.mem, data);
    free(env.mem, code);
    std::puts("Guest net offline: ICM connect, epoll waits with callbacks and NetCtl info passed");
}
