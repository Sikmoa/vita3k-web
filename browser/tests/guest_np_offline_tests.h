// NP while signed out, as firmware 3.74 answers it (np_manager, np_basic,
// np_signaling, np_activity_sdk, np_common and the shell's NP service).
#pragma once
#include <net/state.h>
#include <np/state.h>
#include <cstring>

inline void test_guest_np_offline(EmuEnvState &env, ThreadState &thread) {
    auto &cpu = *thread.cpu;
    const auto call = [&](uint32_t nid, std::initializer_list<uint32_t> args) {
        uint32_t reg = 0;
        for (const uint32_t value : args)
            write_reg(cpu, reg++, value);
        call_import(env, cpu, nid, thread.id);
        REQUIRE(env.missing_nids.empty());
        return read_reg(cpu, 0);
    };
    constexpr uint32_t np_init = 0x04D9F484, np_term = 0x19E40AE1, get_state = 0x54060DF6,
                       check_callback = 0x3B0AE9A9, register_callback = 0x44239C35,
                       rating = 0xAF0073B2, basic_init = 0xEFB91A99, basic_term = 0x389BCB3B,
                       friend_count = 0xDF41F308, online_status = 0x5183A4B5,
                       sig_init = 0x4B6ACF47, sig_term = 0xBC892D18, create_ctx = 0xF77EF683,
                       set_ctx_opt = 0x0B48FADB, conn_info = 0x51883EAE, post_status = 0xBC7FDC77,
                       platform_type = 0xE9A003DE, get_np_id = 0x3C94B4B4;
    REQUIRE(!env.cfg.current_config.psn_signed_in);
    const Address block = alloc(env.mem, 256, "np fixture");
    REQUIRE(block);
    const Address out = block, out2 = block + 4, np_id = block + 0x40;
    auto *word = Ptr<uint32_t>(out).get(env.mem);

    REQUIRE(call(get_state, { out }) == 0x80550002); // before sceNpInit
    REQUIRE(call(np_init, { 0, 0 }) == 0);
    REQUIRE(call(get_state, { 0 }) == 0x80550003);
    REQUIRE(call(get_state, { out }) == 0 && *word == 1); // SIGNED_OUT
    // One state notification per registration, not one per sceNpCheckCallback.
    REQUIRE(call(register_callback, { 0x81000001, 0 }) == 0);
    REQUIRE(env.np.state_cb_pending.size() == 1);
    call(check_callback, {});
    REQUIRE(env.np.state_cb_pending.empty());
    REQUIRE(call(check_callback, {}) == 0);
    REQUIRE(call(rating, { out, 0 }) == 0x80550503);
    *word = 0xcccccccc;
    REQUIRE(call(rating, { out, out2 }) == 0x8055050B && *word == 0xcccccccc); // no sign-in ticket

    REQUIRE(call(friend_count, { out }) == 0x80551d04); // before sceNpBasicInit
    REQUIRE(call(basic_init, { 0 }) == 0 && call(basic_init, { 0 }) == 0x80551d05);
    REQUIRE(call(friend_count, { 0 }) == 0x80551d02);
    *word = 0xcccccccc;
    REQUIRE(call(friend_count, { out }) == 0x80551d06 && *word == 0xcccccccc);
    REQUIRE(call(online_status, { np_id, out }) == 0x80551d07 && *word == 0);
    REQUIRE(call(basic_term, {}) == 0 && call(basic_term, {}) == 0x80551d04);

    REQUIRE(call(get_np_id, { np_id }) == 0);
    REQUIRE(call(platform_type, { np_id }) == 2); // "psp2"
    REQUIRE(call(platform_type, { 0 }) == 0x80550601);
    REQUIRE(call(create_ctx, { np_id, 0, 0, out }) == 0x80552701);
    REQUIRE(call(sig_init, { 0, 0, 0, 0 }) == 0 && call(sig_init, { 0, 0, 0, 0 }) == 0x80552702);
    REQUIRE(call(create_ctx, { np_id, 0x81000001, 0, out }) == 0 && *word == 1);
    REQUIRE(call(create_ctx, { np_id, 0x81000001, 0, out }) == 0 && *word == 2);
    REQUIRE(call(set_ctx_opt, { 1, 1, 1 }) == 0 && call(set_ctx_opt, { 1, 2, 1 }) == 0x80552715);
    REQUIRE(call(set_ctx_opt, { 7, 1, 1 }) == 0x80552705);
    REQUIRE(call(conn_info, { 1, 1, 1, out }) == 0x8055270e); // no connection exists
    REQUIRE(call(sig_term, {}) == 0);

    REQUIRE(call(post_status, { np_id, 0, 0 }) == 0x80552302); // sceNpActivityInit never ran

    // NpAuth and NpCommerce2 fail only on a second Init; Term always succeeds.
    constexpr uint32_t auth_init = 0x441D8B4E, auth_term = 0x6093B689, commerce_init = 0xC73F209A,
                       commerce_term = 0xB99958AE;
    REQUIRE(call(auth_init, {}) == 0 && call(auth_init, {}) == 0x80550301);
    REQUIRE(call(auth_term, {}) == 0 && call(auth_term, {}) == 0 && call(auth_init, {}) == 0);
    REQUIRE(call(auth_term, {}) == 0);
    REQUIRE(call(commerce_init, {}) == 0 && call(commerce_init, {}) == 0x80550f02);
    REQUIRE(call(commerce_term, {}) == 0 && call(commerce_term, {}) == 0 && call(commerce_init, {}) == 0);
    REQUIRE(call(commerce_term, {}) == 0);

    // Trophy handles: unique ids, at most four live, unknown ones refused.
    constexpr uint32_t create_handle = 0x4EBC6977, destroy_handle = 0xFF142071, abort_handle = 0xD55C6F4C;
    auto &trophy = env.np.trophy_state;
    REQUIRE(call(create_handle, { out }) == 0x80551601); // sceNpTrophyInit not called
    trophy.inited = true;
    REQUIRE(call(create_handle, { 0 }) == 0x80551604);
    uint32_t handles[4];
    for (auto &handle : handles) {
        REQUIRE(call(create_handle, { out }) == 0);
        handle = *word;
        REQUIRE(static_cast<int32_t>(handle) > 0);
    }
    REQUIRE(handles[0] != handles[1] && handles[1] != handles[2] && handles[2] != handles[3]);
    REQUIRE(call(create_handle, { out }) == 0x80551606 && *word == 0xffffffff);
    REQUIRE(call(abort_handle, { handles[0] }) == 0 && call(destroy_handle, { handles[0] }) == 0);
    REQUIRE(call(destroy_handle, { handles[0] }) == 0x80551608 && call(abort_handle, { handles[0] }) == 0x80551608);
    REQUIRE(call(destroy_handle, { 0xffffffff }) == 0x80551604);
    REQUIRE(call(create_handle, { out }) == 0);
    for (const uint32_t handle : { handles[1], handles[2], handles[3], *word })
        REQUIRE(call(destroy_handle, { handle }) == 0);
    trophy.inited = false;
    REQUIRE(call(destroy_handle, { handles[1] }) == 0x80551601);

    // NetCtl: a second Init is refused until Term.
    constexpr uint32_t netctl_init = 0x495CA1DB, netctl_term = 0xCD188648;
    const bool netctl_was_inited = env.netctl.inited;
    if (netctl_was_inited)
        call(netctl_term, {});
    REQUIRE(call(netctl_init, {}) == 0 && call(netctl_init, {}) == 0x80412102);
    call(netctl_term, {});
    call(netctl_term, {}); // not initialized: nothing to do
    REQUIRE(call(netctl_init, {}) == 0);
    if (!netctl_was_inited)
        call(netctl_term, {});
    call(np_term, {});
    free(env.mem, block);
    std::puts("Guest NP offline: service state, rating, NpBasic, signaling, platform, activity, NpAuth, Commerce2, trophy handles and NetCtl passed");
}
