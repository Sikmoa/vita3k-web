// sceIoSync, sceIoChstat, sceIoDevctl and sceIoIoctl as firmware 3.74
// iofilemgr, exfatfs and PfsMgr answer a game, on ux0:, savedata0: and app0:.
#pragma once
#include <io/functions.h>
#include <io/io.h>
#include <rtc/rtc.h>
#include <cstring>
#include <string>

inline void test_guest_io_control(EmuEnvState &env, ThreadState &thread) {
    auto &cpu = *thread.cpu;
    const Address block = alloc(env.mem, 0x2000, "io control fixture");
    REQUIRE(block);
    const auto call = [&](uint32_t nid, std::initializer_list<uint32_t> args) {
        const Address saved_sp = read_sp(cpu);
        const Address sp = block + 0x1f00;
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
    constexpr uint32_t io_sync = 0x98ACED6D, io_chstat = 0x29482F7F, io_devctl = 0x04B30CB2, io_ioctl = 0x54ABACFA,
                       io_open = 0x6C60AC61, io_close = 0xC70B8886, io_getstat = 0xBCA5B623, io_dopen = 0xA9283DD0,
                       io_dclose = 0x422A221A, io_rename = 0xF737E369;
    // A Vita file system of the fixture's own.
    const auto saved_fs = env.vita_fs_path;
    const auto saved_io = std::make_tuple(env.io.user_id, env.io.savedata, env.io.app_path, env.io.device_paths);
    env.vita_fs_path = "/io-control-fixture";
    env.io.user_id = "00";
    env.io.savedata = env.io.app_path = "IOCTL0001";
    init_device_paths(env.io);
    const fs::path ux0 = env.vita_fs_path / "ux0", app = ux0 / "app/IOCTL0001", save = ux0 / "user/00/savedata/IOCTL0001";
    fs::create_directories(app);
    fs::create_directories(ux0 / "data");
    const auto make_file = [](const fs::path &path, size_t size) {
        FILE *file = std::fopen(path.string().c_str(), "wb");
        REQUIRE(file);
        for (size_t i = 0; i < size; ++i)
            std::fputc('x', file);
        std::fclose(file);
    };
    make_file(app / "eboot.bin", 16);
    make_file(ux0 / "data/file.bin", 16);

    const Address path = block, stat = block + 0x400, out = block + 0x500, in = block + 0x580;
    const auto put = [&](const char *text) {
        std::strcpy(Ptr<char>(path).get(env.mem), text);
        return path;
    };
    auto *st = Ptr<SceIoStat>(stat).get(env.mem);
    auto *word = Ptr<uint32_t>(out).get(env.mem);

    // Path checks and lookup, shared by all four.
    REQUIRE(call(io_sync, { 0, 0 }) == 0x8001000E);
    REQUIRE(call(io_sync, { put("xyz0:"), 0 }) == 0x80010013);
    REQUIRE(call(io_sync, { put("relative"), 0 }) == 0x80010016);
    REQUIRE(call(io_sync, { put("/absolute"), 0 }) == 0x80010013);
    std::memset(Ptr<char>(path).get(env.mem), 'a', 0x400);
    REQUIRE(call(io_sync, { path, 0 }) == 0x8002710B); // longer than the kernel copies
    REQUIRE(call(io_sync, { put("ux0:"), 0 }) == 0 && call(io_sync, { put("ux0:data/file.bin"), 0xffffffff }) == 0);
    REQUIRE(call(io_sync, { put("ux0:data/missing"), 0 }) == 0x80010002);
    REQUIRE(call(io_sync, { put("savedata0:"), 0 }) == 0); // the mount exists before its directory does
    fs::create_directories(save);
    make_file(save / "save.bin", 100);
    REQUIRE(call(io_sync, { put("savedata0:save.bin"), 0 }) == 0 && call(io_sync, { put("app0:"), 0 }) == 0);

    // sceIoChstat.
    std::memset(st, 0, sizeof(*st));
    st->st_size = 40;
    REQUIRE(call(io_chstat, { put("savedata0:missing"), stat, SCE_CST_SIZE }) == 0x80010002);
    REQUIRE(call(io_chstat, { put("savedata0:"), stat, SCE_CST_SIZE }) == 0x80010015);
    REQUIRE(call(io_chstat, { put("savedata0:save.bin"), 0, SCE_CST_SIZE }) == 0x80010016);
    REQUIRE(call(io_chstat, { put("app0:eboot.bin"), stat, SCE_CST_SIZE }) == 0x8001001E && fs::file_size(app / "eboot.bin") == 16);
    REQUIRE(call(io_chstat, { put("ux0:"), stat, SCE_CST_MT }) == 0x8001000D);
    REQUIRE(call(io_chstat, { put("savedata0:save.bin"), stat, SCE_CST_SIZE }) == 0 && fs::file_size(save / "save.bin") == 40);
    st->st_size = 64;
    REQUIRE(call(io_chstat, { put("savedata0:save.bin"), stat, SCE_CST_SIZE | 0x10000 }) == 0 && fs::file_size(save / "save.bin") == 64);
    const auto writable = [&](const fs::path &file) { return (fs::status(file).permissions() & fs::perms::owner_write) != fs::perms::no_perms; };
    st->st_mode = 0x80; // write without read
    REQUIRE(call(io_chstat, { put("ux0:data/file.bin"), stat, SCE_CST_MODE }) == 0x80010016 && writable(ux0 / "data/file.bin"));
    st->st_mode = SCE_S_IFREG | 0x100 | 0x6;
    REQUIRE(call(io_chstat, { put("ux0:data/file.bin"), stat, SCE_CST_MODE }) == 0 && !writable(ux0 / "data/file.bin"));
    st->st_mode = 0x1ff;
    REQUIRE(call(io_chstat, { put("ux0:data/file.bin"), stat, SCE_CST_MODE }) == 0 && writable(ux0 / "data/file.bin"));
    // Times: validated first, stored to the second, read back by getstat.
    const SceDateTime created{ 2001, 2, 3, 4, 5, 6, 700 }, accessed{ 2010, 11, 12, 13, 14, 15, 0 }, modified{ 2020, 1, 31, 23, 59, 58, 999999 };
    st->st_ctime = created;
    st->st_atime = accessed;
    st->st_mtime = modified;
    st->st_mtime.month = 13;
    REQUIRE(call(io_chstat, { put("savedata0:save.bin"), stat, SCE_CST_MT | SCE_CST_SIZE }) == 0x80010016);
    st->st_mtime = modified;
    st->st_atime.year = 1979;
    REQUIRE(call(io_chstat, { put("savedata0:save.bin"), stat, SCE_CST_AT }) == 0x80010016);
    st->st_atime = accessed;
    REQUIRE(call(io_chstat, { put("savedata0:save.bin"), stat, SCE_CST_CT | SCE_CST_AT | SCE_CST_MT }) == 0);
    const auto same_second = [](const SceDateTime &a, const SceDateTime &b) {
        return a.year == b.year && a.month == b.month && a.day == b.day && a.hour == b.hour && a.minute == b.minute && a.second == b.second
            && a.microsecond == 0;
    };
    const Address got = block + 0x600;
    auto *got_stat = Ptr<SceIoStat>(got).get(env.mem);
    REQUIRE(call(io_getstat, { put("savedata0:save.bin"), got }) == 0 && got_stat->st_size == 64);
    REQUIRE(same_second(got_stat->st_ctime, created) && same_second(got_stat->st_atime, accessed) && same_second(got_stat->st_mtime, modified));
    const Address new_name = block + 0x700;
    std::strcpy(Ptr<char>(new_name).get(env.mem), "savedata0:renamed.bin");
    REQUIRE(call(io_rename, { put("savedata0:save.bin"), new_name }) == 0);
    REQUIRE(call(io_getstat, { new_name, got }) == 0 && same_second(got_stat->st_ctime, created));
    REQUIRE(call(io_rename, { new_name, put("savedata0:save.bin") }) == 0);

    // getstat shows the write bit MODE sets and clears.
    st->st_mode = 0x100;
    REQUIRE(call(io_chstat, { put("savedata0:save.bin"), stat, SCE_CST_MODE }) == 0);
    REQUIRE(call(io_getstat, { put("savedata0:save.bin"), got }) == 0 && (got_stat->st_mode & 0x180) == 0x100);
    st->st_mode = 0x180;
    REQUIRE(call(io_chstat, { put("savedata0:save.bin"), stat, SCE_CST_MODE }) == 0);
    REQUIRE(call(io_getstat, { put("savedata0:save.bin"), got }) == 0 && (got_stat->st_mode & 0x180) == 0x180);
    // Dates move with a renamed directory; its old name keeps none.
    fs::create_directories(save / "dir");
    make_file(save / "dir/inner.bin", 1);
    REQUIRE(call(io_chstat, { put("savedata0:dir/inner.bin"), stat, SCE_CST_CT }) == 0);
    std::strcpy(Ptr<char>(new_name).get(env.mem), "savedata0:moved");
    REQUIRE(call(io_rename, { put("savedata0:dir"), new_name }) == 0);
    REQUIRE(call(io_getstat, { put("savedata0:moved/inner.bin"), got }) == 0 && same_second(got_stat->st_ctime, created));
    fs::create_directories(save / "dir");
    make_file(save / "dir/inner.bin", 1);
    REQUIRE(call(io_getstat, { put("savedata0:dir/inner.bin"), got }) == 0 && !same_second(got_stat->st_ctime, created));
    // A case-insensitive lookup searches the mount the path names.
    make_file(save / "UPPER.BIN", 1);
    const bool saved_case = env.io.case_isens_find_enabled;
    env.io.case_isens_find_enabled = true;
    env.io.cachemap.clear();
    REQUIRE(call(io_chstat, { put("savedata0:upper.bin"), stat, SCE_CST_MT }) == 0 && call(io_sync, { path, 0 }) == 0);
    env.io.case_isens_find_enabled = saved_case;
    // Sync writes out what the host still buffers for the file.
    constexpr uint32_t io_write = 0x34EFD876, io_sync_by_fd = 0x16512F59;
    const uint32_t writer = call(io_open, { put("savedata0:buffered.bin"), SCE_O_WRONLY | SCE_O_CREAT, 0 });
    REQUIRE(static_cast<int32_t>(writer) >= 0);
    REQUIRE(call(io_write, { writer, block + 0x780, 4 }) == 4 && fs::file_size(save / "buffered.bin") == 0);
    REQUIRE(call(io_sync, { put("savedata0:buffered.bin"), 0 }) == 0 && fs::file_size(save / "buffered.bin") == 4);
    REQUIRE(call(io_write, { writer, block + 0x780, 4 }) == 4 && fs::file_size(save / "buffered.bin") == 4);
    REQUIRE(call(io_sync, { put("savedata0:"), 0 }) == 0 && fs::file_size(save / "buffered.bin") == 8);
    REQUIRE(call(io_write, { writer, block + 0x780, 4 }) == 4);
    REQUIRE(call(io_sync_by_fd, { writer, 0 }) == 0 && fs::file_size(save / "buffered.bin") == 12);
    REQUIRE(call(io_close, { writer }) == 0);

    // sceIoDevctl: capacity of the volume behind ux0:, 32 MiB kept back.
    VolumeInfo volume{};
    REQUIRE(get_volume_info(ux0, volume));
    REQUIRE(call(io_devctl, { put("ux0:"), 0x3801, 0, 0, out, 0x18 }) == 0x80010030); // system programs only
    REQUIRE(call(io_devctl, { put("ux0:"), 0x80000001, 0, 0, out, 0x18 }) == 0x80010030);
    REQUIRE(call(io_devctl, { put("ux0:data"), 0x3001, 0, 0, out, 0x18 }) == 0x80010013);
    REQUIRE(call(io_devctl, { put("app0:"), 0x3001, 0, 0, out, 0x18 }) == 0x80010001);
    REQUIRE(call(io_devctl, { put("ux0:"), 0x3002, 0, 0, out, 0x18 }) == 0x80010030);
    std::memset(word, 0xcc, 0x28);
    REQUIRE(call(io_devctl, { put("ux0:"), 0x3001, 0, 0, out, 0x20 }) == 0x80010016 && word[0] == 0xcccccccc);
    REQUIRE(call(io_devctl, { put("ux0:"), 0x3001, 0, 0, out, 0x18 }) == 0);
    auto *dev_info = Ptr<SceIoDevInfo>(out).get(env.mem);
    constexpr uint64_t reserved = 32 * 1024 * 1024;
    REQUIRE(uint64_t(dev_info->max_size) == volume.capacity && dev_info->cluster_size == volume.cluster_size);
    REQUIRE(uint64_t(dev_info->free_size) == (volume.available > reserved ? volume.available - reserved : 0));
    REQUIRE(dev_info->unk == 0xffffffff && word[6] == 0xcccccccc); // untouched past 0x18
    REQUIRE(call(io_devctl, { put("savedata0:"), 0x3001, 0, 0, out, 0x28 }) == 0);
    REQUIRE(dev_info->unk == 0 && dev_info->serial == volume.serial && std::string(dev_info->label, 11) == "           " && dev_info->zero == 0);

    // sceIoIoctl: the buffer cache of a file; exFAT has no commands.
    REQUIRE(call(io_ioctl, { 0x7fffffff, 0x1002, 0, 0, out, 0x20 }) == 0x80010009);
    const uint32_t fd = call(io_open, { put("ux0:data/file.bin"), SCE_O_RDONLY, 0 });
    REQUIRE(static_cast<int32_t>(fd) >= 0);
    std::memset(word, 0, 0x20);
    REQUIRE(call(io_ioctl, { fd, 0x1002, 0, 0, out, 0x1f }) == 0x80010016);
    word[6] = 1;
    REQUIRE(call(io_ioctl, { fd, 0x1002, 0, 0, out, 0x20 }) == 0x80010016);
    word[6] = 0;
    REQUIRE(call(io_ioctl, { fd, 0x1002, 0, 0, 0, 0x20 }) == 0x80010016);
    const uint32_t ux0_block = std::min<uint32_t>(volume.cluster_size, 0x8000);
    REQUIRE(call(io_ioctl, { fd, 0x1002, 0, 0, out, 0x20 }) == 0);
    REQUIRE(word[0] == 0x2000 && word[1] == 0x200 && word[2] == 2 && word[3] == ux0_block && word[4] == 0 && word[5] == 0);
    auto *set = Ptr<uint32_t>(in).get(env.mem);
    std::memset(set, 0, 0x1c);
    set[0] = 0x4000;
    set[2] = 3; // not selected by the mask
    set[5] = 1; // total
    REQUIRE(call(io_ioctl, { fd, 0x1001, in, 0x1c, 0, 0 }) == 0);
    set[5] = 4; // ways
    REQUIRE(call(io_ioctl, { fd, 0x1001, in, 0x1c, 0, 0 }) == 0x80010016);
    set[6] = 1;
    set[5] = 1;
    REQUIRE(call(io_ioctl, { fd, 0x1001, in, 0x1c, 0, 0 }) == 0x80010016);
    // Sizes whose line (unit * ways) overflows 32 bits are refused.
    set[6] = 0;
    set[0] = 0x2000, set[1] = 0x80000000, set[2] = 2, set[3] = 0x80000000, set[5] = 15;
    REQUIRE(call(io_ioctl, { fd, 0x1001, in, 0x1c, 0, 0 }) == 0x80010016);
    set[0] = 0x4000, set[1] = 0x200, set[2] = 3, set[3] = 0x200, set[5] = 1;
    REQUIRE(call(io_ioctl, { fd, 0x1002, 0, 0, out, 0x20 }) == 0 && word[0] == 0x4000 && word[2] == 2);
    REQUIRE(call(io_ioctl, { fd, 0x3001, 0, 0, out, 0x20 }) == 0x80010030);
    REQUIRE(call(io_ioctl, { fd, 0x0801, 0, 0, out, 0x20 }) == 0x80010030);
    REQUIRE(call(io_ioctl, { fd, 0x1800, 0, 0, out, 0x20 }) == 0x80010030);
    REQUIRE(call(io_ioctl, { fd, 0x9000, 0, 0, out, 0x20 }) == 0x80010016);
    REQUIRE(call(io_close, { fd }) == 0);
    const uint32_t app_fd = call(io_open, { put("app0:eboot.bin"), SCE_O_RDONLY, 0 });
    REQUIRE(static_cast<int32_t>(app_fd) >= 0);
    REQUIRE(call(io_ioctl, { app_fd, 0x3001, 0, 0, out, 0x20 }) == 0x80010001);
    REQUIRE(call(io_ioctl, { app_fd, 0x1002, 0, 0, out, 0x20 }) == 0 && word[3] == 0x8000); // PfsMgr's block
    REQUIRE(call(io_close, { app_fd }) == 0);
    const uint32_t unbuffered = call(io_open, { put("ux0:data/file.bin"), SCE_O_RDONLY | SCE_O_NOBUF, 0 });
    REQUIRE(call(io_ioctl, { unbuffered, 0x1002, 0, 0, out, 0x20 }) == 0x80010069 && call(io_close, { unbuffered }) == 0);
    const uint32_t dir = call(io_dopen, { put("ux0:data") });
    REQUIRE(static_cast<int32_t>(dir) >= 0);
    REQUIRE(call(io_ioctl, { dir, 0x1002, 0, 0, out, 0x20 }) == 0x80010015 && call(io_dclose, { dir }) == 0);

    fs::remove_all(env.vita_fs_path);
    env.io.chstat_times.clear();
    env.io.buffer_caches.clear();
    env.vita_fs_path = saved_fs;
    std::tie(env.io.user_id, env.io.savedata, env.io.app_path, env.io.device_paths) = saved_io;
    free(env.mem, block);
    std::puts("Guest IO control: sceIoSync, sceIoChstat, sceIoDevctl and sceIoIoctl on ux0:, savedata0: and app0: passed");
}
