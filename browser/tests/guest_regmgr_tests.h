// Production sceRegMgrSystemParamGetInt over a registry template loaded the way
// vita_app.cpp loads the staged firmware's os0/kd/registry.db0.
#pragma once
#include <regmgr/functions.h>
#include <filesystem>
#include <fstream>
#include <string>

inline void test_guest_regmgr(EmuEnvState &env, ThreadState &thread) {
    // registry.db0: a 138-byte header, then the template text XORed with the
    // firmware key (regmgr.cpp decryptRegistryFile). [BASE maps numbers to path
    // parts; [REG-BAS entries are <path>=<type>:<size>:...:<default>.
    const std::string text = "[BASE\n1=CONFIG/\n2=NET/\n3=ssl_cert_ignorable\n"
                             "[REG-BAS\n/1/2/3=0:4:0:1\n[REG-J1\n";
    constexpr unsigned char key[16] = { 0x89, 0xFA, 0x95, 0x48, 0xCB, 0x6D, 0x77, 0x9D,
        0xA2, 0x25, 0x34, 0xFD, 0xA9, 0x35, 0x59, 0x6E };
    std::string file(138, '\0');
    for (size_t i = 0; i < text.size(); ++i)
        file += static_cast<char>(text[i] ^ key[i & 15]);
    const std::filesystem::path root = "/regmgr-fixture";
    std::filesystem::create_directories(root / "os0/kd");
    std::ofstream(root / "os0/kd/registry.db0", std::ios::binary) << file;

    const Address out = alloc(env.mem, 4, "regmgr fixture");
    REQUIRE(out);
    const auto get_int = [&](uint32_t id) {
        *Ptr<int32_t>(out).get(env.mem) = -1;
        write_reg(*thread.cpu, 0, id);
        write_reg(*thread.cpu, 1, out);
        call_import(env, *thread.cpu, 0x347C1BDB, thread.id); // sceRegMgrSystemParamGetInt
        REQUIRE(env.missing_nids.empty());
        REQUIRE(read_reg(*thread.cpu, 0) == 0);
        return *Ptr<int32_t>(out).get(env.mem);
    };
    // No template loaded: the desktop no-firmware answer.
    REQUIRE(get_int(0x00563BFE) == 0);
    regmgr::init_regmgr(env.regmgr, fs::path(root.string()));
    REQUIRE(get_int(0x00563BFE) == 1); // /CONFIG/NET/ssl_cert_ignorable default
    // Known id without a template entry (/CONFIG/SYSTEM/language): no read past an empty value.
    REQUIRE(get_int(0x00037502) == 0);
    free(env.mem, out);
    std::puts("Guest registry: sceRegMgrSystemParamGetInt reads the firmware template default");
}
