// SceAppMgr, SceRtc and sysmodule kernel imports against firmware 3.74
// (expected values from the firmware code run in an emulator, see the
// commit message).
#pragma once
#include <io/functions.h>
#include <rtc/rtc.h>
#include <cstring>
#include <string>

inline void test_guest_appmgr_rtc(EmuEnvState &env, ThreadState &thread) {
    auto &cpu = *thread.cpu;
    const auto call = [&](uint32_t nid, std::initializer_list<uint32_t> args) {
        uint32_t reg = 0;
        for (const uint32_t value : args)
            write_reg(cpu, reg++, value);
        call_import(env, cpu, nid, thread.id);
        REQUIRE(env.missing_nids.empty());
        return read_reg(cpu, 0);
    };
    constexpr uint32_t receive_event = 0x10B5765F, is_game = 0xFFF8F7F0, convert_vs0 = 0xADAA658E,
                       data_drive = 0xC0631748, module_drive = 0x906154DE, parse_date_time = 0x2347CE12,
                       parse_rfc3339 = 0x2D18AEEC, format_rfc3339 = 0xCCEA2B54, check_valid = 0xD7622935,
                       get_thread_id = 0x59D06540, memcpy_to_user = 0x6D88EF8A,
                       acmgr_is_game = 0x1298C647, acmgr_psm = 0xC98D82EE, acmgr_dev = 0xE87D1777,
                       processmgr_dipsw = 0x61B9B6FA;
    const Address block = alloc(env.mem, 0x800, "appmgr fixture");
    REQUIRE(block);
    auto *bytes = Ptr<uint8_t>(block).get(env.mem);
    const auto put = [&](Address at, const char *value) { std::strcpy(Ptr<char>(at).get(env.mem), value); return at; };
    const auto text = [&](Address at) { return std::string(Ptr<const char>(at).get(env.mem)); };

    // No system event: 64 zero bytes and 0x80802013; NULL is 0x80802016.
    std::memset(bytes, 0xcc, 0x40);
    REQUIRE(call(receive_event, { block }) == 0x80802013);
    for (int i = 0; i < 0x40; ++i)
        REQUIRE(bytes[i] == 0);
    REQUIRE(call(receive_event, { 0 }) == 0x80802016);

    // App state: nothing pending; the checks come before the pointer's, and
    // the 128 bytes are zeroed whatever the result.
    constexpr uint32_t app_state = 0x5E86319A;
    std::memset(bytes, 0xcc, 0x80);
    REQUIRE(call(app_state, { block, 0x80, 0x02000000 }) == 0);
    for (int i = 0; i < 0x80; ++i)
        REQUIRE(bytes[i] == 0);
    std::memset(bytes, 0xcc, 0x80);
    REQUIRE(call(app_state, { block, 0x7c, 0x02000000 }) == 0x8080201A && bytes[0x7f] == 0);
    REQUIRE(call(app_state, { block, 0x80, 0x03740012 }) == 0x8080201A);
    REQUIRE(call(app_state, { 0, 0x80, 0x02000000 }) == 0x80802016);
    REQUIRE(call(app_state, { 0, 0x40, 0x02000000 }) == 0x8080201A);

    // A game has PAID class 0x210 (Limbo: 0x210000101CCA010C).
    const uint64_t saved_paid = env.kernel.process_program_authority_id;
    env.kernel.process_program_authority_id = 0x210000101CCA010CULL;
    REQUIRE(call(is_game, {}) == 1 && call(acmgr_is_game, { 0 }) == 1 && call(acmgr_psm, { 0 }) == 0);
    env.kernel.process_program_authority_id = 0x2800000000000001ULL;
    REQUIRE(call(is_game, {}) == 0);
    env.kernel.process_program_authority_id = 0x210000101CD20009ULL; // PSM Developer Assistant range
    REQUIRE(call(acmgr_psm, { 0 }) == 1);
    env.kernel.process_program_authority_id = saved_paid;
    REQUIRE(call(acmgr_dev, {}) == 0 && call(processmgr_dipsw, { 0, 17 }) == 0x80029008);

    // vs0 user drives: sd + 12 hex digits + ':', resolved by the IO layer.
    init_device_paths(env.io);
    REQUIRE(call(data_drive, { block + 0x100 }) == 0);
    const std::string drive = text(block + 0x100);
    REQUIRE(drive.size() == 15 && drive.rfind("sd", 0) == 0 && drive.back() == ':' && drive == env.io.vs0_data_drive);
    REQUIRE(call(module_drive, { block + 0x100 }) == 0 && text(block + 0x100) == env.io.vs0_module_drive);
    const Address path = block + 0x200, out = block + 0x400;
    put(path, "vs0:/data/external/cert/CA_LIST.cer");
    REQUIRE(call(convert_vs0, { path, out, 64 }) == 0 && text(out) == drive + "/cert/CA_LIST.cer");
    REQUIRE(resolve_user_mount(env.io, text(out).c_str()) == "vs0:data/external/cert/CA_LIST.cer");
    put(path, "vs0:sys/external/libhttp.suprx");
    REQUIRE(call(convert_vs0, { path, out, 64 }) == 0 && text(out) == env.io.vs0_module_drive + "/libhttp.suprx");
    put(path, "ux0:data/file.txt"); // not a vs0 user path: copied
    REQUIRE(call(convert_vs0, { path, out, 64 }) == 0 && text(out) == "ux0:data/file.txt");
    REQUIRE(call(convert_vs0, { path, out, 32 }) == 0x80800001 && Ptr<uint8_t>(out).get(env.mem)[0] == 0);
    REQUIRE(call(convert_vs0, { path, 0, 64 }) == 0x80800001);

    // RTC: RFC 3339 formatting and RFC 1123 / RFC 3339 parsing.
    const auto tick_of = [](const char *rfc3339) {
        uint64_t tick = 0;
        REQUIRE(rtc_parse_rfc3339(&tick, rfc3339) == 0);
        return tick;
    };
    auto *tick = Ptr<uint64_t>(block + 0x600).get(env.mem);
    *tick = tick_of("2024-03-05T07:08:09.123456Z");
    REQUIRE(call(format_rfc3339, { out, block + 0x600, 540 }) == 0 && text(out) == "2024-03-05T16:08:09.12+09:00");
    REQUIRE(call(format_rfc3339, { out, block + 0x600, 0 }) == 0 && text(out) == "2024-03-05T07:08:09.12Z");
    REQUIRE(call(format_rfc3339, { out, block + 0x600, 1440 }) == 0x80251000);
    put(path, "Tue, 05 Mar 2024 07:08:09 +0900");
    REQUIRE(call(parse_date_time, { block + 0x600, path }) == 0 && *tick == tick_of("2024-03-04T22:08:09Z"));
    put(path, "Tue Mar  5 07:08:09 2024");
    REQUIRE(call(parse_date_time, { block + 0x600, path }) == 0 && *tick == tick_of("2024-03-05T07:08:09Z"));
    put(path, "05 Mar 2024 07:08:09 GMT"); // the weekday is required
    *tick = 7;
    REQUIRE(call(parse_date_time, { block + 0x600, path }) == 0x80251080 && *tick == 7);
    put(path, "2024-03-05T07:08:60Z"); // second 60 rolls over
    REQUIRE(call(parse_rfc3339, { block + 0x600, path }) == 0 && *tick == tick_of("2024-03-05T07:09:00Z"));
    put(path, "2024-02-30T07:08:09Z");
    REQUIRE(call(parse_rfc3339, { block + 0x600, path }) == 0x80251083);
    auto *date = Ptr<uint16_t>(block + 0x700).get(env.mem);
    const uint16_t valid[6] = { 2024, 3, 5, 7, 8, 9 };
    std::memcpy(date, valid, sizeof(valid));
    *Ptr<uint32_t>(block + 0x70C).get(env.mem) = 999999;
    REQUIRE(call(check_valid, { block + 0x700 }) == 0);
    date[0] = 0;
    REQUIRE(call(check_valid, { block + 0x700 }) == 0x80251081);

    // Kernel imports of sysmodule.
    REQUIRE(call(get_thread_id, {}) == static_cast<uint32_t>(thread.id));
    std::memset(bytes + 0x780, 0, 4);
    *Ptr<uint32_t>(block + 0x790).get(env.mem) = 0x12345678;
    REQUIRE(call(memcpy_to_user, { block + 0x780, block + 0x790, 4 }) == 0 && *Ptr<uint32_t>(block + 0x780).get(env.mem) == 0x12345678);
    // System software version and the system clock's low word.
    constexpr uint32_t sw_version = 0x5182E212, time_low = 0x47F6DE49;
    auto *version = Ptr<uint32_t>(block + 0x700).get(env.mem);
    std::memset(version, 0xcc, 0x28);
    version[0] = 0x24;
    REQUIRE(call(sw_version, { block + 0x700 }) == 0x80020005);
    version[0] = 0x28;
    REQUIRE(call(sw_version, { block + 0x700 }) == 0);
    REQUIRE(version[0] == 0x28 && text(block + 0x704) == "3.74" && version[8] == 0x03740011 && version[9] == 0);
    REQUIRE(call(sw_version, { 0 }) == 0x80022005);
    const uint32_t first_low = call(time_low, {});
    REQUIRE(call(time_low, {}) - first_low < 10000000u); // microseconds, monotonic within the test
    free(env.mem, block);
    std::puts("Guest AppMgr/RTC: system events, game program, vs0 drives, RFC 3339/1123 and sysmodule kernel imports passed");
}
