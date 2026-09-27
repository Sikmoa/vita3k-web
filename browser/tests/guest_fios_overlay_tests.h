// Production SceFios2User overlay bridges: order-range filtering must not
// depend on the order in which overlays were added.
#pragma once
#include <io/state.h>
#include <io/types.h>
#include <cstring>
#include <string>

inline void test_guest_fios_overlay(EmuEnvState &env, ThreadState &thread) {
    const Address data = alloc(env.mem, 4096, "fios overlay fixture");
    REQUIRE(data);
    const Address overlay = data, out_id = data + 0x400, in_path = data + 0x410, out_path = data + 0x500;
    auto call = [&](uint32_t nid, std::initializer_list<uint32_t> args) {
        auto &cpu = *thread.cpu;
        const auto sp = read_sp(cpu);
        unsigned i = 0;
        for (const uint32_t arg : args) {
            if (i < 4)
                write_reg(cpu, i, arg);
            else
                *Ptr<uint32_t>(sp + 4 * (i - 4)).get(env.mem) = arg;
            ++i;
        }
        call_import(env, cpu, nid, thread.id);
        REQUIRE(env.missing_nids.empty());
        REQUIRE(read_sp(cpu) == sp);
        return read_reg(cpu, 0);
    };
    auto add = [&](uint8_t order, const char *dst, const char *src) {
        auto *o = Ptr<SceFiosProcessOverlay>(overlay).get(env.mem);
        std::memset(o, 0, sizeof(*o));
        o->type = SCE_FIOS_OVERLAY_TYPE_OPAQUE;
        o->order = order;
        std::strcpy(o->dst, dst);
        std::strcpy(o->src, src);
        REQUIRE(call(0x6c4be9cd, {0, overlay, out_id}) == 0); // sceFiosOverlayAddForProcess02
    };
    auto resolve = [&](uint32_t min_order, uint32_t max_order) {
        std::strcpy(Ptr<char>(in_path).get(env.mem), "/a/file");
        // sceFiosOverlayResolveWithRangeSync02(pid, mode, in, out, max, min, max)
        REQUIRE(call(0x61c4aac4, {0, 0, in_path, out_path, 256, min_order, max_order}) == 0);
        return std::string(Ptr<const char>(out_path).get(env.mem));
    };
    for (const bool low_first : {true, false}) {
        env.io.overlays.clear();
        if (low_first) {
            add(10, "/a", "/low");
            add(20, "/a", "/high");
        } else {
            add(20, "/a", "/high");
            add(10, "/a", "/low");
        }
        REQUIRE(resolve(0, 10) == "/low/file");
        REQUIRE(resolve(20, 20) == "/high/file");
        REQUIRE(resolve(11, 19) == "/a/file");
    }
    env.io.overlays.clear();
    free(env.mem, data);
    std::puts("Fios overlays: range resolution independent of insertion order passed");
}
