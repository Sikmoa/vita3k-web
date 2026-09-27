// sceKernelChangeThreadVfpException as firmware 3.74 threadmgr handles the
// mask (ksceKernelChangeThreadVfpException 0x810089e9, syscall 0x8102c609).
#pragma once
#include <kernel/types.h>

inline void test_guest_thread_vfp(EmuEnvState &env, ThreadState &thread) {
    auto &cpu = *thread.cpu;
    const auto call = [&](uint32_t clear_mask, uint32_t set_mask) {
        write_reg(cpu, 0, clear_mask);
        write_reg(cpu, 1, set_mask);
        call_import(env, cpu, 0xCC18FBAE, thread.id);
        REQUIRE(env.missing_nids.empty());
        return read_reg(cpu, 0);
    };
    auto &tls_mask = thread.tls.get_ptr<uint32_t>().get(env.mem)[TLS_VFP_EXCEPTION];
    const uint32_t saved = tls_mask;
    tls_mask = 0;
    // Only the cumulative-flag bits 0x9f and QC (0x08000000) are masks; a bit
    // both cleared and set is refused; neither changes the mask.
    REQUIRE(call(0, 0x20) == 0x80020005);
    REQUIRE(call(0x10000000, 0) == 0x80020005);
    REQUIRE(call(0x1, 0x1) == 0x80020005);
    REQUIRE(tls_mask == 0);
    // The old mask is returned; libkernel reads the new one from TLS slot 4.
    REQUIRE(call(0, 0x0800009f) == 0);
    REQUIRE(tls_mask == 0x0800009f);
    REQUIRE(call(0x08000003, 0) == 0x0800009f);
    REQUIRE(tls_mask == 0x9c);
    REQUIRE(call(0x9c, 0x1) == 0x9c);
    REQUIRE(tls_mask == 0x1);
    tls_mask = saved;
    std::puts("Guest VFP exception mask: checks, old mask returned, TLS slot 4 updated passed");
}
