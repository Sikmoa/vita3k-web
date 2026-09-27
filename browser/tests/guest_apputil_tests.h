// SceAppUtil, SceLiveAreaUtil and SceCommonDialog configuration checks of
// firmware 3.74 (apputil.suprx, livearea_util.suprx, libcdlg.suprx).
#pragma once
#include <gxm/state.h>
#include <rtc/rtc.h>
#include <cstring>
#include <string>

inline void test_guest_apputil(EmuEnvState &env, ThreadState &thread) {
    auto &cpu = *thread.cpu;
    const auto call = [&](uint32_t nid, std::initializer_list<uint32_t> args) {
        uint32_t reg = 0;
        for (const uint32_t value : args)
            write_reg(cpu, reg++, value);
        call_import(env, cpu, nid, thread.id);
        REQUIRE(env.missing_nids.empty());
        return read_reg(cpu, 0);
    };
    constexpr uint32_t init = 0xDAFFE671, shutdown = 0xB220B00B, receive_event = 0xEE0DBED9,
                       bgdl_status = 0x96F478D6, system_param_int = 0x5DFB9CA0,
                       livearea_update = 0xD330285D, set_config = 0xBECD35C8, dialog_update = 0x90530F2F;
    const Address block = alloc(env.mem, 0x800, "apputil fixture");
    REQUIRE(block);
    std::memset(Ptr<uint8_t>(block).get(env.mem), 0, 0x800);
    const Address init_param = block, boot_param = block + 0x40, value = block + 0x70, bgdl = block + 0x80,
                  event = block + 0x100, text = block + 0x600;
    auto *boot = Ptr<uint32_t>(boot_param).get(env.mem);

    REQUIRE(call(system_param_int, { 1, value }) == 0x80100601); // before sceAppUtilInit
    REQUIRE(call(shutdown, {}) == 0x80100601);
    REQUIRE(call(init, { 0, boot_param }) == 0x80100600);
    Ptr<uint32_t>(init_param).get(env.mem)[0] = 16; // workBufSize must be 0
    REQUIRE(call(init, { init_param, boot_param }) == 0x80100600);
    Ptr<uint32_t>(init_param).get(env.mem)[0] = 0;
    boot[0] = boot[1] = 0xcccccccc;
    REQUIRE(call(init, { init_param, boot_param }) == 0 && boot[0] == 0 && boot[1] == 0);
    REQUIRE(call(system_param_int, { 1, value }) == 0);
    // Time zone (minutes, without daylight saving time) and summertime come
    // from the date and time settings; ids 3 (user name) and 8 are not ints.
    REQUIRE(call(system_param_int, { 7, value }) == 0);
    const int32_t summertime = *Ptr<int32_t>(value).get(env.mem);
    REQUIRE(summertime == 0 || summertime == 1);
    REQUIRE(call(system_param_int, { 6, value }) == 0);
    REQUIRE(*Ptr<int32_t>(value).get(env.mem) == rtc_local_offset_minutes() - summertime * 60);
    REQUIRE(call(system_param_int, { 3, value }) == 0x80100600 && call(system_param_int, { 8, value }) == 0x80100600);

    // No app event is queued: the event is cleared and the queue answers empty.
    std::memset(Ptr<uint8_t>(event).get(env.mem), 0xcc, 0x404);
    REQUIRE(call(receive_event, { event }) == 0x80802015);
    REQUIRE(Ptr<uint32_t>(event).get(env.mem)[0] == 0 && Ptr<uint8_t>(event).get(env.mem)[0x403] == 0);
    REQUIRE(call(receive_event, { 0 }) == 0x80100600);

    auto *status = Ptr<uint32_t>(bgdl).get(env.mem);
    status[0] = 2; // type must be 0 or 1
    REQUIRE(call(bgdl_status, { bgdl }) == 0x80100600);
    status[0] = 1;
    status[1] = status[2] = status[3] = 7;
    REQUIRE(call(bgdl_status, { bgdl }) == 0 && status[1] == 0 && status[2] == 0 && status[3] == 0);
    // App-event parsers (firmware 3.74): URL-decoded key=value text.
    constexpr uint32_t parse_invite = 0xA2496814, parse_presence = 0x28C7D4F6, parse_gift = 0x77380601,
                       parse_live_area = 0x0F4EE55F;
    auto *event_type = Ptr<uint32_t>(event).get(env.mem);
    auto *event_text = Ptr<char>(event + 4).get(env.mem);
    const Address parsed = block + 0x500; // up to 0x41b bytes, over the config and text areas
    auto *out = Ptr<uint8_t>(parsed).get(env.mem);
    const auto set_event = [&](uint32_t type, const char *text) {
        std::memset(Ptr<uint8_t>(event).get(env.mem), 0, 0x404);
        *event_type = type;
        std::strcpy(event_text, text);
    };
    set_event(1, "npcommid=NPWR00001_02&uid=12%33x&type=INVITATION_MESSAGE");
    REQUIRE(call(parse_invite, { event, parsed }) == 0);
    REQUIRE(std::memcmp(out, "NPWR00001\0\2", 11) == 0 && std::string(reinterpret_cast<char *>(out + 12)) == "123x");
    REQUIRE(call(parse_presence, { event, parsed }) == 0x80100600); // another event type
    set_event(1, "npcommid=NPWR0001_02&uid=1");
    REQUIRE(call(parse_invite, { event, parsed }) == 0x80100620);
    set_event(3, "npcommid=NPWR00001_00&jid=player_1@ab.cd.np.playstation.net/xyz");
    REQUIRE(call(parse_presence, { event, parsed }) == 0);
    REQUIRE(std::string(reinterpret_cast<char *>(out + 12)) == "player_1" && std::memcmp(out + 32, "abcdxyz", 7) == 0 && out[40] == 1);
    set_event(3, "npcommid=NPWR00001_00&jid=player_1@ab.cd.np.example.com");
    REQUIRE(call(parse_presence, { event, parsed }) == 0x80100620);
    set_event(4, "npcommid=NPWR00001_00&jid=p@ab.cd.x.playstation.net&giftid=0x10&version=3&param=hi");
    REQUIRE(call(parse_gift, { event, parsed }) == 0);
    REQUIRE(*Ptr<uint32_t>(parsed + 12).get(env.mem) == 16 && *Ptr<uint32_t>(parsed + 52).get(env.mem) == 3);
    REQUIRE(std::string(reinterpret_cast<char *>(out + 56)) == "hi" && out[16] == 'p');
    set_event(5, "psla:%2Fdetail%3F100%");
    REQUIRE(call(parse_live_area, { event, parsed }) == 0 && std::string(reinterpret_cast<char *>(out)) == "/detail?100%");
    set_event(5, "psl:x");
    REQUIRE(call(parse_live_area, { event, parsed }) == 0x80100620);
    REQUIRE(call(parse_live_area, { event, 0 }) == 0x80100600);

    REQUIRE(call(shutdown, {}) == 0 && call(shutdown, {}) == 0x80100601);
    REQUIRE(call(parse_live_area, { event, parsed }) == 0x80100601); // after sceAppUtilShutdown

    const auto put = [&](Address at, const char *value) { std::strcpy(Ptr<char>(at).get(env.mem), value); return at; };
    const Address version = put(text, "01.00"), bad_version = put(text + 8, "02.00"), xml = put(text + 16, "<frame/>"),
                  path = put(text + 32, "app0:sce_sys/livearea/contents");
    // target_type is the fifth argument, on the caller's stack.
    const Address saved_sp = read_sp(cpu);
    write_sp(cpu, block + 0x700);
    Ptr<uint32_t>(block + 0x700).get(env.mem)[0] = 0;
    REQUIRE(call(livearea_update, { version, xml, static_cast<uint32_t>(-1), path }) == 0);
    REQUIRE(call(livearea_update, { bad_version, xml, static_cast<uint32_t>(-1), path }) == 0x80104002);
    REQUIRE(call(livearea_update, { version, xml, 10240, path }) == 0x80104009);
    Ptr<uint32_t>(block + 0x700).get(env.mem)[0] = 2; // target type 0 or 1
    REQUIRE(call(livearea_update, { version, xml, static_cast<uint32_t>(-1), path }) == 0x80104002);
    write_sp(cpu, saved_sp);

    auto *config = Ptr<uint32_t>(block + 0x500).get(env.mem);
    config[1] = 20; // language
    REQUIRE(call(set_config, { block + 0x500 }) == 0x80020431);
    config[1] = 1;
    config[2] = 2; // enterButtonAssign
    REQUIRE(call(set_config, { block + 0x500 }) == 0x80020432);
    config[2] = 1;
    REQUIRE(call(set_config, { block + 0x500 }) == 0 && call(set_config, { 0 }) == 0x80020402);
    REQUIRE(call(dialog_update, { 0 }) == 0); // no dialog running: the parameter is not read
    const SceUID saved_display_thread = env.gxm.display_queue_thread;
    env.gxm.display_queue_thread = thread.id; // never from GXM's display queue thread
    REQUIRE(call(dialog_update, { 0 }) == 0x80020406);
    env.gxm.display_queue_thread = saved_display_thread;
    free(env.mem, block);
    std::puts("Guest AppUtil: init/shutdown, app event and its parsers, bgdl, LiveArea update and dialog config checks passed");
}
