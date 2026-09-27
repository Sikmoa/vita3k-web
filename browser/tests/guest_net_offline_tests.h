// The offline network stack against firmware 3.74 SceNetPs, libnet and the
// shell's NetCtl service: ICM connect, epoll waits with callbacks and the
// disconnected NetCtl info.
#pragma once
#include "guest_sync_delete_tests.h"
#include <kernel/callback.h>
#include <display/functions.h>
#include <display/state.h>
#include <net/state.h>

DECL_EXPORT(SceInt32, sceKernelNotifyCallback, SceUID callbackId, SceInt32 notifyArg);
DECL_EXPORT(int, sceKernelDeleteCallback, SceUID callbackId);

inline void test_guest_net_offline(EmuEnvState &env, vita3k::web::GuestThreadRuntime &runtime) {
    constexpr uint32_t socket_nid = 0xF084FCE3, bind_nid = 0x1296A94B, sendto_nid = 0x52DB31D5,
                       epoll_create = 0xF9D102AE, epoll_control = 0x4C8764AC, epoll_wait = 0x45CE337D,
                       epoll_wait_cb = 0x92D3E767, icm_connect = 0x93F2FF08, socket_close = 0x29822B4D,
                       epoll_destroy = 0x7915CAF3, inet_get_info = 0xB26D07F3, get_sock_info = 0xB1AF6840,
                       connect_nid = 0x11E5B6F6, send_nid = 0xE3DD8CD9;
    constexpr uint32_t icm_done = 0x40000;
    const Address code = alloc(env.mem, 0x1000, "net offline code");
    const Address data = alloc(env.mem, 0x1000, "net offline data");
    REQUIRE(code && data);
    const auto word = [&](Address offset) -> uint32_t & { return *Ptr<uint32_t>(data + offset).get(env.mem); };
    // Test-only import: sceKernelDeleteCallback (not registered here).
    constexpr uint32_t delete_callback_import = 0xc0de0002;
    env.kernel.call_import = [&](CPUState &cpu, uint32_t nid, SceUID tid) {
        if (nid == delete_callback_import) {
            write_reg(cpu, 0, export_sceKernelDeleteCallback(env, tid, "fixture", read_reg(cpu, 0)));
            return;
        }
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
    REQUIRE(call(get_sock_info, { 0xffffffff, 0, 0, 0 }) == 0x804101c8);
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
    // Socket info: the queued datagram counts with its 16-byte sender address.
    const Address infos = data + 0x600;
    auto *info = Ptr<SceNetSockInfo>(infos).get(env.mem);
    REQUIRE(call(get_sock_info, { first, infos, 1, 2 }) == 1);
    REQUIRE(std::string(info->name) == "net fixture" && info->pid == KernelState::process_id && info->s == int(first));
    REQUIRE(info->socket_type == SCE_NET_SOCK_DGRAM && info->state == 2 && info->flags == 1 && info->policy == 0);
    REQUIRE(info->local_adr.s_addr == to->sin_addr.s_addr && info->local_port == to->sin_port);
    REQUIRE(info->remote_adr.s_addr == 0 && info->recv_queue_length == 21 && info->send_queue_length == 0);
    REQUIRE(call(get_sock_info, { first, infos, 1, 0 }) == 1 && info->name[0] == 0); // names only with flag 2
    // sceNetEpollWait runs none; a CB wait with nothing notified just times out.
    constexpr uint32_t recv_nid = 0x023643B7;
    REQUIRE(call(recv_nid, { first, data + 0x3c0, 16, SCE_NET_MSG_DONTWAIT }) == 5);
    REQUIRE(call(get_sock_info, { first, infos, 1, 0 }) == 1 && info->recv_queue_length == 0);
    cb = wait_with(epoll_wait, 1000, true);
    REQUIRE(word(0x48) == 0 && word(0x40) == 0xcccccccc && cb->is_executable());
    cb = wait_with(epoll_wait_cb, 1000, false);
    REQUIRE(word(0x48) == 0 && word(0x40) == 0xcccccccc);
    // A notification while the CB wait is parked ends the park: the callback
    // runs, sends the datagram and the wait returns it.
    REQUIRE(call(recv_nid, { first, data + 0x3c0, 16, SCE_NET_MSG_DONTWAIT }) == 0x80410123); // nothing queued (EAGAIN)
    {
        guest_sync_delete::build_call(env.mem, code, epoll_wait_cb, { eid, events, 1, 2000000, 0 }, data + 0x48);
        word(0x40) = word(0x44) = word(0x48) = 0xcccccccc;
        auto waiter = env.kernel.create_thread(env.mem, "net waiter", Ptr<const void>(code), SCE_KERNEL_DEFAULT_PRIORITY_USER,
            SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        REQUIRE(waiter);
        auto late = std::make_shared<Callback>(waiter->id, callback_name, Ptr<SceKernelCallbackFunction>(callback), Ptr<void>(data));
        constexpr SceUID late_id = 0x7ffe0001;
        waiter->callbacks.push_back(late);
        env.kernel.callbacks.emplace(late_id, late);
        REQUIRE(waiter->start(0, Ptr<void>{}, false) == 0);
        run_until([&] { return waiter->status == ThreadStatus::wait; });
        REQUIRE(export_sceKernelNotifyCallback(env, host->id, "fixture", late_id, 9) == 0);
        run_until([&] { return waiter->status == ThreadStatus::dormant; });
        REQUIRE(word(0x48) == 1 && word(0x40) == 9 && word(0x44) == 5);
        env.kernel.callbacks.erase(late_id);
        REQUIRE(call(recv_nid, { first, data + 0x3c0, 16, SCE_NET_MSG_DONTWAIT }) == 5);
    }
    // So does a vblank notification of a callback the waiter registered.
    {
        guest_sync_delete::build_call(env.mem, code, epoll_wait_cb, { eid, events, 1, 2000000, 0 }, data + 0x48);
        word(0x40) = word(0x44) = word(0x48) = 0xcccccccc;
        auto waiter = env.kernel.create_thread(env.mem, "net waiter", Ptr<const void>(code), SCE_KERNEL_DEFAULT_PRIORITY_USER,
            SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        REQUIRE(waiter);
        auto vblank = std::make_shared<Callback>(waiter->id, callback_name, Ptr<SceKernelCallbackFunction>(callback), Ptr<void>(data));
        constexpr SceUID vblank_id = 0x7ffe0003;
        waiter->callbacks.push_back(vblank);
        env.display.vblank_callbacks.emplace(vblank_id, vblank);
        REQUIRE(waiter->start(0, Ptr<void>{}, false) == 0);
        run_until([&] { return waiter->status == ThreadStatus::wait; });
        advance_vblank(env);
        run_until([&] { return waiter->status == ThreadStatus::dormant; });
        env.display.vblank_callbacks.erase(vblank_id);
        REQUIRE(word(0x48) == 1 && word(0x44) == 5);
        REQUIRE(call(recv_nid, { first, data + 0x3c0, 16, SCE_NET_MSG_DONTWAIT }) == 5);
    }
    // A callback deleted by one that ran before it in the same pass does not run.
    {
        const Address deleter = code + 0xa00, delete_stub = code + 0xb00, marker = code + 0xb80;
        const uint32_t stub[] = { 0xef000000, 0xe1a0f00e, delete_callback_import };
        std::memcpy(Ptr<void>(delete_stub).get(env.mem), stub, sizeof(stub));
        constexpr SceUID victim_id = 0x7ffe0002;
        guest_thread_fixture::Arm a(deleter);
        a.emit(0xe92d4010); // push {r4, lr}
        a.constant(0, victim_id);
        a.call(delete_stub);
        a.constant(0, 0);
        a.emit(0xe8bd8010); // pop {r4, pc}
        a.finish(env.mem);
        guest_thread_fixture::Arm m(marker);
        m.constant(0, data + 0x4c);
        m.constant(1, 1);
        m.emit(0xe5801000); // str r1, [r0]
        m.constant(0, 0);
        m.emit(0xe12fff1e); // bx lr
        m.finish(env.mem);
        guest_sync_delete::build_call(env.mem, code, epoll_wait_cb, { eid, events, 1, 1000, 0 }, data + 0x48);
        word(0x4c) = 0;
        auto waiter = env.kernel.create_thread(env.mem, "net waiter", Ptr<const void>(code), SCE_KERNEL_DEFAULT_PRIORITY_USER,
            SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        REQUIRE(waiter);
        auto first_cb = std::make_shared<Callback>(waiter->id, callback_name, Ptr<SceKernelCallbackFunction>(deleter), Ptr<void>(data));
        auto victim = std::make_shared<Callback>(waiter->id, callback_name, Ptr<SceKernelCallbackFunction>(marker), Ptr<void>(data));
        waiter->callbacks = { first_cb, victim };
        env.kernel.callbacks.emplace(victim_id, victim);
        first_cb->direct_notify(0);
        victim->direct_notify(0);
        REQUIRE(waiter->start(0, Ptr<void>{}, false) == 0);
        run_until([&] { return waiter->status == ThreadStatus::dormant; });
        REQUIRE(word(0x48) == 0 && word(0x4c) == 0 && !env.kernel.callbacks.contains(victim_id));
    }

    // An epoll wait parked on a socket that is not readable marks it; a
    // receive parked on it marks it too; both clear when the waits end.
    const auto spawn = [&](uint32_t nid, const uint32_t (&args)[5]) {
        guest_sync_delete::build_call(env.mem, code, nid, args, data + 0x48);
        auto t = env.kernel.create_thread(env.mem, "net waiter", Ptr<const void>(code), SCE_KERNEL_DEFAULT_PRIORITY_USER,
            SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        REQUIRE(t && t->start(0, Ptr<void>{}, false) == 0);
        run_until([&] { return t->status == ThreadStatus::wait; });
        return t;
    };
    // A connected datagram socket takes lo0's address as its source.
    REQUIRE(call(connect_nid, { second, addr, sizeof(SceNetSockaddrIn) }) == 0);
    REQUIRE(call(get_sock_info, { second, infos, 1, 0 }) == 1 && info->local_adr.s_addr == to->sin_addr.s_addr);
    REQUIRE(info->local_port != 0 && info->remote_adr.s_addr == to->sin_addr.s_addr && info->remote_port == to->sin_port);
    auto waiter = spawn(epoll_wait, { eid, events, 1, 2000000, 0 });
    REQUIRE(call(get_sock_info, { first, infos, 1, 0 }) == 1 && info->flags == (1 | SCE_NET_SOCKINFO_F_RECV_EWAIT));
    REQUIRE(call(send_nid, { second, message, 5, 0 }) == 5);
    run_until([&] { return waiter->status == ThreadStatus::dormant; });
    REQUIRE(call(get_sock_info, { first, infos, 1, 0 }) == 1 && info->flags == 1);
    REQUIRE(call(recv_nid, { first, data + 0x3c0, 16, SCE_NET_MSG_DONTWAIT }) == 5);
    waiter = spawn(recv_nid, { first, data + 0x3c0, 16, 0, 0 });
    REQUIRE(call(get_sock_info, { first, infos, 1, 0 }) == 1 && info->flags == (1 | SCE_NET_SOCKINFO_F_RECV_WAIT));
    REQUIRE(call(send_nid, { second, message, 5, 0 }) == 5);
    run_until([&] { return waiter->status == ThreadStatus::dormant; });
    REQUIRE(word(0x48) == 5 && call(get_sock_info, { first, infos, 1, 0 }) == 1 && info->flags == 1);

    // The whole list: TCP without a control block (refused connect) first,
    // then TCP, UDP and P2P datagram sockets newest first, then epolls.
    const uint32_t refused = call(socket_nid, { name, SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0 });
    const uint32_t fresh = call(socket_nid, { name, SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0 });
    REQUIRE(call(connect_nid, { refused, addr, sizeof(SceNetSockaddrIn) }) == 0x8041013d); // ECONNREFUSED
    REQUIRE(call(get_sock_info, { 0xffffffff, 0, 0, 0 }) == 4 && call(get_sock_info, { 0xffffffff, 0, 0, 0x20 }) == 5);
    REQUIRE(call(get_sock_info, { 0xffffffff, infos, 1, 0x20 }) == 1 && info[0].s == int(refused));
    REQUIRE(call(get_sock_info, { 0xffffffff, infos, 8, 0x22 }) == 5);
    REQUIRE(info[0].s == int(refused) && info[0].state == 2 && info[0].local_port == 0 && info[0].socket_type == SCE_NET_SOCK_STREAM);
    REQUIRE(info[1].s == int(fresh) && info[1].state == 1 && info[1].local_port == 0);
    REQUIRE(info[2].s == int(second) && info[3].s == int(first));
    REQUIRE(info[4].s == int(eid) && info[4].socket_type == 11 && info[4].state == 2 && std::string(info[4].name) == "net fixture");
    REQUIRE(call(socket_close, { refused }) == 0 && call(socket_close, { fresh }) == 0);
    REQUIRE(call(get_sock_info, { 0, infos, 1, 4 }) == 0x80410116 && net_errno == SCE_NET_EINVAL);
    REQUIRE(call(get_sock_info, { 0xffffffff, infos, 0xffffffff, 0 }) == 0x80410116);
    REQUIRE(call(get_sock_info, { refused, infos, 1, 0 }) == 0x80410109);
    REQUIRE(call(socket_nid, { 0, SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0 }) == 0x80410116);

    REQUIRE(call(epoll_destroy, { eid }) == 0);
    REQUIRE(call(socket_close, { first }) == 0 && call(socket_close, { second }) == 0);

    // NetCtl disconnected: every code, even an invalid one, is NOT_CONNECTED
    // and the info buffer is not written.
    const bool netctl_was_inited = env.netctl.inited;
    env.netctl.inited = true;
    const Address ctl_info = data + 0x400;
    word(0x400) = 0xcccccccc;
    for (const uint32_t info_code : { 1u, 15u, 22u, 23u, 0u, 99u })
        REQUIRE(call(inet_get_info, { info_code, ctl_info }) == 0x80412108 && word(0x400) == 0xcccccccc);
    REQUIRE(call(inet_get_info, { 1, 0 }) == 0x80412107);
    env.netctl.inited = false;
    REQUIRE(call(inet_get_info, { 1, ctl_info }) == 0x80412101);
    env.netctl.inited = netctl_was_inited;

    env.net.inited = net_was_inited;
    REQUIRE(runtime.shutdown());
    REQUIRE(env.kernel.threads.empty());
    free(env.mem, data);
    free(env.mem, code);
    std::puts("Guest net offline: ICM connect, epoll waits with callbacks and NetCtl info passed");
}
