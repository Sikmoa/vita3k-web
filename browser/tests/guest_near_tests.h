// SceNearUtil against firmware 3.74 over an empty near.db: initialization
// checks, empty neighbour and gift lists, the discovered-gift lookups, and
// the own gift's set/get/delete round trip.
#pragma once
#include <np/functions.h>
#include <np/state.h>
#include <cstring>

inline void test_guest_near(EmuEnvState &env, ThreadState &thread) {
    auto &cpu = *thread.cpu;
    const Address block = alloc(env.mem, 0x4000, "near fixture");
    REQUIRE(block);
    const auto call = [&](uint32_t nid, std::initializer_list<uint32_t> args) {
        const Address saved_sp = read_sp(cpu);
        const Address sp = block + 0x3f00;
        write_sp(cpu, sp);
        uint32_t i = 0;
        for (const uint32_t value : args) {
            if (i < 4)
                write_reg(cpu, i, value);
            else
                *Ptr<uint32_t>(sp + 4 * (i - 4)).get(env.mem) = value;
            ++i;
        }
        call_import(env, cpu, nid, thread.id);
        REQUIRE(env.missing_nids.empty());
        write_sp(cpu, saved_sp);
        return read_reg(cpu, 0);
    };
    constexpr uint32_t initialize = 0xBBCA5813, finalize = 0xFF3BC581, refresh = 0x2ED36EE2, neighbors = 0xAD264F5F,
                       gifts = 0xDE6F0859, gift_info = 0x773ABEA9, convert = 0x80D6C552, delete_discovered = 0x8CBEB2DA,
                       open_image = 0xF8C15008, read_image = 0x2364D6BD, close_data = 0x2F35C196, set_gift = 0x146BE236,
                       get_gift = 0xDB8BAC35, delete_gift = 0x52021026;
    const Address comm_id = block, bad_comm_id = block + 0x10, param = block + 0x20, num = block + 0x30, list = block + 0x40,
                  text = block + 0x100, thumbnail = block + 0x300, data = block + 0x400, gift_param = block + 0x500,
                  out_text = block + 0x600, out_thumb = block + 0x800, out_data = block + 0x900, out_id = block + 0xa00,
                  sizes = block + 0xa10, out_param = block + 0xa40;
    std::memcpy(Ptr<char>(comm_id).get(env.mem), "NPWR00001\0\x01", 12);
    std::memcpy(Ptr<char>(bad_comm_id).get(env.mem), "NPW100001\0\x01", 12);
    auto words = [&](Address at) { return Ptr<uint32_t>(at).get(env.mem); };

    REQUIRE(call(gifts, { num, list }) == 0x80104905); // before sceNearInitialize
    words(param)[0] = block + 0x1000; // mem
    words(param)[1] = 0x3ffff; // memSize
    REQUIRE(call(initialize, { comm_id, param, 1 }) == 0x80104904);
    words(param)[1] = 0x40000;
    REQUIRE(call(initialize, { 0, param, 1 }) == 0x80104901);
    REQUIRE(call(initialize, { bad_comm_id, param, 1 }) == 0x80104902);
    REQUIRE(call(initialize, { comm_id, param, 1 }) == 0 && call(initialize, { comm_id, param, 1 }) == 0x80104903);
    REQUIRE(call(refresh, { comm_id }) == 0 && call(refresh, { bad_comm_id }) == 0x80104902);

    // Empty lists: a query returns 0, a copy clamps *num to 0.
    *words(num) = 0;
    REQUIRE(call(neighbors, { num, list }) == 0);
    *words(num) = 5;
    words(list)[0] = block + 0x80;
    REQUIRE(call(neighbors, { num, list }) == 0 && *words(num) == 0);
    *words(num) = 5;
    REQUIRE(call(gifts, { num, 0 }) == 0x80104901);
    REQUIRE(call(gifts, { num, list }) == 0 && *words(num) == 0);
    REQUIRE(call(gift_info, { 7, out_text }) == 0x8010491e && call(gift_info, { 0, out_text }) == 0x80104912);
    REQUIRE(call(delete_discovered, { 7 }) == 0x8010491e && call(delete_discovered, { 0 }) == 0x80104918);
    REQUIRE(call(open_image, { 7 }) == 0x8010491e && call(read_image, { 7, out_data, 16, 0, 0 }) == 0x8010491b);
    REQUIRE(call(close_data, { 7 }) == 0x8010491b);
    // A Near gift event of this title: the gift is not among the (no) discovered ones.
    std::memcpy(Ptr<uint8_t>(block + 0xb00).get(env.mem), Ptr<uint8_t>(comm_id).get(env.mem), 12);
    words(block + 0xb00)[3] = 7; // giftid
    words(block + 0xb00)[13] = 2; // version above the initialized 1
    REQUIRE(call(convert, { block + 0xb00, out_id }) == 0x80104920);
    words(block + 0xb00)[13] = 1;
    REQUIRE(call(convert, { block + 0xb00, out_id }) == 0x8010491e);

    // Own gift: argument checks, then set / get / delete.
    auto *t = Ptr<uint8_t>(text).get(env.mem);
    std::memset(t, 0, 0x1a0);
    words(text)[0] = 4;
    std::memcpy(t + 4, "Gift", 4);
    words(text + 0x8c)[0] = 5;
    std::memcpy(t + 0x90, "Hello", 5);
    std::memset(Ptr<uint8_t>(thumbnail).get(env.mem), 0xab, 16);
    std::memset(Ptr<uint8_t>(data).get(env.mem), 0xcd, 32);
    std::memset(Ptr<uint8_t>(gift_param).get(env.mem), 0, 0x2c);
    Ptr<uint8_t>(gift_param).get(env.mem)[0x28] = 101;
    REQUIRE(call(set_gift, { 9, text, 16, thumbnail, 32, data, 3600, gift_param }) == 0x8010490a);
    Ptr<uint8_t>(gift_param).get(env.mem)[0x28] = 50;
    REQUIRE(call(set_gift, { 9, text, 0x2001, thumbnail, 32, data, 3600, gift_param }) == 0x80104907);
    REQUIRE(call(set_gift, { 9, text, 16, thumbnail, 32, data, 0x80000000, gift_param }) == 0x80104909);
    words(text)[0] = 135;
    REQUIRE(call(set_gift, { 9, text, 16, thumbnail, 32, data, 3600, gift_param }) == 0x80104906);
    words(text)[0] = 4;
    REQUIRE(call(get_gift, { out_id, 0, 0, 0, 0, 0, 0, 0 }) == 0); // no gift yet
    REQUIRE(call(delete_gift, { 9 }) == 0x80104918);
    REQUIRE(call(set_gift, { 9, text, 16, thumbnail, 32, data, 3600, gift_param }) == 0);
    words(sizes)[0] = 0; // query the thumbnail size
    words(sizes)[1] = 8; // too small for the data
    REQUIRE(call(get_gift, { out_id, out_text, sizes, out_thumb, sizes + 4, out_data, sizes + 8, out_param }) == 0x80104914);
    REQUIRE(words(sizes)[0] == 16);
    words(sizes)[1] = 64;
    REQUIRE(call(get_gift, { out_id, out_text, sizes, out_thumb, sizes + 4, out_data, sizes + 8, out_param }) == 1);
    REQUIRE(*words(out_id) == 9 && words(sizes)[1] == 32 && words(sizes)[2] == 3600);
    REQUIRE(std::memcmp(Ptr<uint8_t>(out_text).get(env.mem), t, 0x1a0) == 0);
    REQUIRE(Ptr<uint8_t>(out_thumb).get(env.mem)[15] == 0xab && Ptr<uint8_t>(out_data).get(env.mem)[31] == 0xcd);
    REQUIRE(Ptr<uint8_t>(out_param).get(env.mem)[0x28] == 50);
    REQUIRE(call(delete_gift, { 8 }) == 0x80104918 && call(delete_gift, { 9 }) == 0);
    REQUIRE(call(get_gift, { out_id, 0, 0, 0, 0, 0, 0, 0 }) == 0);

    REQUIRE(call(finalize, { bad_comm_id }) == 0x80104902 && call(finalize, { comm_id }) == 0);
    REQUIRE(call(finalize, { comm_id }) == 0x80104905);

    // The Near app launches only queue their URI. ForUpdate checks nothing;
    // FinalizeAndLaunch type 1 finalizes, type 2 needs a discovered gift.
    constexpr uint32_t finalize_and_launch = 0x3F3F6D92, launch_for_update = 0xF3398774;
    REQUIRE(call(launch_for_update, {}) == 0);
    REQUIRE(call(finalize_and_launch, { 1, 0, 0 }) == 0x80104905);
    REQUIRE(call(initialize, { comm_id, param, 1 }) == 0);
    REQUIRE(call(finalize_and_launch, { 1, 4, 0 }) == 0x80104901);
    REQUIRE(call(finalize_and_launch, { 2, 0, out_id }) == 0x80104901);
    REQUIRE(call(finalize_and_launch, { 3, 0, 0 }) == 0x80104901);
    REQUIRE(call(finalize_and_launch, { 2, 4, 0 }) == 0x80104901);
    *words(out_id) = 0;
    REQUIRE(call(finalize_and_launch, { 2, 4, out_id }) == 0x80104912);
    *words(out_id) = 7;
    REQUIRE(call(finalize_and_launch, { 2, 4, out_id }) == 0x8010491E);
    REQUIRE(env.np.near.inited && call(launch_for_update, {}) == 0);
    REQUIRE(call(finalize_and_launch, { 1, 0, 0 }) == 0 && !env.np.near.inited);
    REQUIRE(call(finalize, { comm_id }) == 0x80104905);

    // The process ends without Finalize or the other libraries' Term: the
    // next process starts clean.
    REQUIRE(call(initialize, { comm_id, param, 1 }) == 0);
    REQUIRE(call(set_gift, { 9, text, 16, thumbnail, 32, data, 3600, gift_param }) == 0);
    env.np.basic_inited = env.np.signaling_inited = env.np.auth_inited = env.np.commerce2_inited = true;
    env.np.signaling_ctxs[1] = { {}, 0x81000001, 0 };
    reset_process(env.np);
    REQUIRE(!env.np.near.inited && !env.np.near.has_gift && !env.np.basic_inited && !env.np.signaling_inited);
    REQUIRE(!env.np.auth_inited && !env.np.commerce2_inited && env.np.signaling_ctxs.empty());
    REQUIRE(call(initialize, { comm_id, param, 1 }) == 0 && call(finalize, { comm_id }) == 0);
    free(env.mem, block);
    std::puts("Guest Near: initialization, empty lists, discovered-gift lookups, the own gift and the Near app launches passed");
}
