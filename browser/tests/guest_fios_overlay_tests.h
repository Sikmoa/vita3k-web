// SceFios2User overlays against SceFios2Kernel of firmware 3.74: a game may
// only name its own process, lookups skip privileged overlays, paths are
// normalized like the kernel does, and a thread can disable app overlays.
#pragma once
#include <io/state.h>
#include <io/types.h>
#include <cstring>
#include <string>

inline void test_guest_fios_overlay(EmuEnvState &env, ThreadState &thread) {
    constexpr uint32_t add_nid = 0x6C4BE9CD, get_info = 0xAB7B4213, get_list = 0x1DD808D1, modify = 0xCA388053,
                       remove_nid = 0xE5D1B6F5, resolve_nid = 0x61C4AAC4, is_disabled = 0x23B8DB1D,
                       set_disabled = 0x70321220;
    constexpr uint32_t self = KernelState::process_id, bad_path = 0x80820005, bad_ptr = 0x80820006, access = 0x80820013,
                       too_long = 0x80820018, too_many = 0x80820019, bad_overlay = 0x8082001A;
    const Address data = alloc(env.mem, 4096, "fios overlay fixture");
    REQUIRE(data);
    const Address overlay = data, out_id = data + 0x400, in_path = data + 0x410, out_path = data + 0x500,
                  info = data + 0x800, ids = data + 0xB00, actual = data + 0xB40;
    // The thread has not started: its SP is not a stack yet, so stack
    // arguments go to a scratch area of the fixture.
    auto call = [&](uint32_t nid, std::initializer_list<uint32_t> args) {
        auto &cpu = *thread.cpu;
        const auto saved_sp = read_sp(cpu);
        const Address sp = data + 0xF00;
        write_sp(cpu, sp);
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
        write_sp(cpu, saved_sp);
        return read_reg(cpu, 0);
    };
    auto *o = Ptr<SceFiosProcessOverlay>(overlay).get(env.mem);
    auto fill = [&](uint8_t order, const char *dst, const char *src) {
        std::memset(o, 0, sizeof(*o));
        o->type = SCE_FIOS_OVERLAY_TYPE_OPAQUE;
        o->order = order;
        std::strcpy(o->dst, dst);
        std::strcpy(o->src, src);
    };
    auto *id = Ptr<uint32_t>(out_id).get(env.mem);
    auto add = [&](uint32_t pid, uint8_t order, const char *dst, const char *src) {
        fill(order, dst, src);
        *id = 0xcccccccc;
        return call(add_nid, { pid, overlay, out_id });
    };
    auto resolve = [&](const char *path, uint32_t min_order, uint32_t max_order) {
        std::strcpy(Ptr<char>(in_path).get(env.mem), path);
        // sceFiosOverlayResolveWithRangeSync02(pid, mode, in, out, max, min, max)
        REQUIRE(call(resolve_nid, { self, 0, in_path, out_path, 256, min_order, max_order }) == 0);
        return std::string(Ptr<const char>(out_path).get(env.mem));
    };

    // Range filtering does not depend on the order overlays were added in.
    for (const bool low_first : { true, false }) {
        env.io.overlays.clear();
        if (low_first) {
            REQUIRE(add(self, 10, "/a", "/low") == 0 && add(self, 20, "/a", "/high") == 0);
        } else {
            REQUIRE(add(self, 20, "/a", "/high") == 0 && add(self, 10, "/a", "/low") == 0);
        }
        REQUIRE(resolve("/a/file", 0, 10) == "/low/file");
        REQUIRE(resolve("/a/file", 20, 20) == "/high/file");
        REQUIRE(resolve("/a/file", 11, 19) == "/a/file");
    }
    env.io.overlays.clear();

    // Add: pointers, device roots (checked before the pid), pid, validation.
    REQUIRE(call(add_nid, { self, 0, out_id }) == bad_ptr);
    fill(1, "app0:data", "ux0:data");
    REQUIRE(call(add_nid, { self, overlay, 0 }) == bad_ptr);
    REQUIRE(add(0, 1, "ux0:/", "ux0:data") == bad_path && *id == 0);
    REQUIRE(add(0, 1, "app0:data", "ux0:data") == access && *id == 0);
    REQUIRE(add(static_cast<uint32_t>(-1), 1, "app0:data", "ux0:data") == access);
    fill(1, "app0:data", "ux0:data");
    o->type = static_cast<SceFiosOverlayType>(4);
    REQUIRE(call(add_nid, { self, overlay, out_id }) == bad_overlay);
    fill(1, "app0:data", "ux0:data");
    std::memset(o->dst, 'a', sizeof(o->dst));
    REQUIRE(call(add_nid, { self, overlay, out_id }) == too_long);
    REQUIRE(add(self, 1, "app0:data", "ux0:data//patch/./x/..") == 0);
    const uint32_t first = *id;
    REQUIRE(first != 0);

    // GetInfo: the kernel's normalized copy, zeros on any error.
    auto *got = Ptr<SceFiosProcessOverlay>(info).get(env.mem);
    REQUIRE(call(get_info, { self, first, info }) == 0);
    REQUIRE(got->order == 1 && got->process_id == static_cast<SceUID>(self) && got->id == static_cast<SceUID>(first));
    REQUIRE(std::string(got->dst) == "app0:/data" && std::string(got->src) == "ux0:/data/patch");
    REQUIRE(got->dst_len == 10 && got->src_len == 15);
    REQUIRE(call(get_info, { 0, first, info }) == access && got->order == 0 && got->dst[0] == 0);
    REQUIRE(call(get_info, { self, first + 100, info }) == bad_overlay && got->process_id == 0);
    REQUIRE(call(get_info, { self, first, 0 }) == bad_ptr);

    // Resolution matches whole path components.
    REQUIRE(resolve("app0:data/file.bin", 0, 0x7F) == "ux0:/data/patch/file.bin");
    REQUIRE(resolve("app0:/data", 0, 0x7F) == "ux0:/data/patch");
    REQUIRE(resolve("app0:database", 0, 0x7F) == "app0:/database");

    // A thread that disables overlays resolves without the app ones.
    REQUIRE(call(is_disabled, {}) == 0);
    REQUIRE(call(set_disabled, { 1 }) == 0 && call(is_disabled, {}) == 1);
    REQUIRE(resolve("app0:data/file.bin", 0, 0x7F) == "app0:/data/file.bin");
    REQUIRE(call(set_disabled, { 0 }) == 0 && call(is_disabled, {}) == 0);

    // Modify re-sorts by order; GetList reports the process's app overlays.
    REQUIRE(add(self, 5, "app0:other", "ux0:other") == 0);
    const uint32_t second = *id;
    auto *list = Ptr<uint32_t>(ids).get(env.mem);
    auto *count = Ptr<uint32_t>(actual).get(env.mem);
    fill(9, "app0:data", "ux0:data2");
    REQUIRE(call(modify, { 0, first, overlay }) == access);
    REQUIRE(call(modify, { self, first + 100, overlay }) == bad_overlay);
    REQUIRE(call(modify, { self, first, 0 }) == bad_ptr);
    REQUIRE(call(modify, { self, first, overlay }) == 0);
    REQUIRE(call(get_list, { self, 0, 0x7F, ids, 4, actual }) == 0 && *count == 2);
    REQUIRE(list[0] == second && list[1] == first && list[2] == 0);
    REQUIRE(call(get_list, { 0, 0, 0x7F, ids, 4, actual }) == 0 && *count == 0);
    REQUIRE(call(get_list, { self, 0, 0x7F, 0, 4, actual }) == bad_ptr);
    REQUIRE(call(get_info, { self, first, info }) == 0 && got->order == 9 && std::string(got->src) == "ux0:/data2");

    // Remove: own pid only, then gone.
    REQUIRE(call(remove_nid, { 0, first }) == access);
    REQUIRE(call(remove_nid, { self, first }) == 0 && call(remove_nid, { self, first }) == bad_overlay);
    REQUIRE(call(remove_nid, { self, second }) == 0);

    // At most 64 app overlays per process.
    for (int i = 0; i < SCE_FIOS_OVERLAY_MAX_OVERLAYS; ++i)
        REQUIRE(add(self, 1, "app0:a", "ux0:b") == 0);
    REQUIRE(add(self, 1, "app0:a", "ux0:b") == too_many && *id == 0);
    env.io.overlays.clear();
    free(env.mem, data);
    std::puts("Fios overlays: kernel access rules, normalized paths, per-thread disable and range resolution passed");
}
