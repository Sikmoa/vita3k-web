// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <io/device.h>
#include <io/functions.h>
#include <io/io.h>
#include <io/state.h>
#include <io/types.h>
#include <io/util.h>
#include <io/vfs.h>

#include <rtc/rtc.h>
#include <util/log.h>
#include <util/preprocessor.h>
#include <util/string_utils.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <sys/utime.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cassert>
#include <cctype>
#include <iostream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#if defined(__aarch64__) && defined(__APPLE__)
#define stat64 stat
#endif

// ****************************
// * Utility functions *
// ****************************

static int io_error_impl(const int retval, const char *export_name, const char *func_name) {
    LOG_WARN("{} ({}) returned {}", func_name, export_name, log_hex(retval));
    return retval;
}

#define IO_ERROR(retval) io_error_impl(retval, export_name, __func__)
#define IO_ERROR_UNK() IO_ERROR(-1)

constexpr bool log_file_op = true;
constexpr bool log_file_read = false;
constexpr bool log_file_seek = false;
constexpr bool log_file_stat = false;

namespace vfs {

bool read_file(const VitaIoDevice device, FileBuffer &buf, const fs::path &vita_fs_path, const fs::path &vfs_file_path) {
    const auto host_file_path = device::construct_emulated_path(device, vfs_file_path, vita_fs_path).generic_path();
    return fs_utils::read_data(host_file_path, buf);
}

bool read_app_file(FileBuffer &buf, const fs::path &vita_fs_path, const std::string &app_path, const fs::path &vfs_file_path) {
    return read_file(VitaIoDevice::ux0, buf, vita_fs_path, fs::path("app") / app_path / vfs_file_path);
}

SceSize get_directory_used_size(const VitaIoDevice device, const std::string &vfs_path, const fs::path &vita_fs_path) {
    const auto emuenv_path = device::construct_emulated_path(device, vfs_path, vita_fs_path);

    SceSize total_size = 0;
    for (const auto &entry : fs::recursive_directory_iterator(emuenv_path)) {
        if (fs::is_regular_file(entry.path()))
            total_size += fs::file_size(entry.path());
    }

    return total_size;
}

} // namespace vfs

// ****************************
// * End utility functions *
// ****************************

static bool is_valid_output_path(const VitaIoDevice device) {
    return !(device == VitaIoDevice::savedata0 || device == VitaIoDevice::savedata1 || device == VitaIoDevice::app0
        || device == VitaIoDevice::_INVALID || device == VitaIoDevice::addcont0 || device == VitaIoDevice::tty0
        || device == VitaIoDevice::tty1 || device == VitaIoDevice::tty2 || device == VitaIoDevice::tty3
        || device == VitaIoDevice::music0 || device == VitaIoDevice::photo0 || device == VitaIoDevice::video0);
}

bool init(IOState &io, const fs::path &cache_path, const fs::path &log_path, const fs::path &vita_fs_path, bool redirect_stdio) {
    // Iterate through the entire list of devices and create the subdirectories if they do not exist
    boost::mp11::mp_for_each<boost::describe::describe_enumerators<VitaIoDevice>>([&vita_fs_path](auto i) {
        if (is_valid_output_path(i.value))
            fs::create_directories(vita_fs_path / i.name);
    });

    const fs::path ux0{ vita_fs_path / "ux0" };
    const fs::path uma0{ vita_fs_path / "uma0" };
    const fs::path vd0{ vita_fs_path / "vd0" };

    fs::create_directories(ux0 / "data");
    fs::create_directories(ux0 / "app");
    fs::create_directories(ux0 / "music");
    fs::create_directories(ux0 / "picture");
    fs::create_directories(ux0 / "theme");
    fs::create_directories(ux0 / "video");
    fs::create_directories(ux0 / "user");
    fs::create_directories(uma0 / "data");
    fs::create_directories(vd0 / "registry");
    fs::create_directories(vd0 / "network");

    fs::create_directories(cache_path / "shaders");
    fs::create_directory(log_path / "shaderlog");
    fs::create_directory(log_path / "texturelog");

    io.redirect_stdio = redirect_stdio;

#ifndef _WIN32
    io.case_isens_find_enabled = true;
#endif

    return true;
}

void io_deinit(IOState &io) {
    io.std_files.clear();
    io.dir_entries.clear();
    io.tty_files.clear();

    io.next_fd = 0;

    io.device_paths = {};
    io.addcont.clear();
    io.content_id.clear();
    io.savedata.clear();
    io.title_id.clear();
    io.app_path.clear();

    io.cachemap.clear();

    {
        std::lock_guard<std::mutex> lock(io.overlay_mutex);
        io.overlays.clear();
        io.next_overlay_id = 1;
    }
}

void init_device_paths(IOState &io) {
    io.device_paths.savedata0 = "user/" + io.user_id + "/savedata/" + io.savedata;
    io.device_paths.app0 = "app/" + io.app_path;
    io.device_paths.addcont0 = "addcont/" + io.addcont;
    // SceAppMgr mounts vs0:sys/external and vs0:data/external for each app
    // under "sd" + 12 random lowercase hex digits + ':'.
    std::random_device random;
    const auto drive = [&] {
        char name[16];
        std::snprintf(name, sizeof(name), "sd%04x%04x%04x:", random() & 0xffff, random() & 0xffff, random() & 0xffff);
        return std::string(name);
    };
    io.vs0_module_drive = drive();
    io.vs0_data_drive = drive();
}

std::string resolve_user_mount(const IOState &io, const char *path) {
    if (!path)
        return {};
    const std::string_view view(path);
    for (const auto &[drive, target] : { std::pair{ &io.vs0_module_drive, "vs0:sys/external" }, std::pair{ &io.vs0_data_drive, "vs0:data/external" } }) {
        if (!drive->empty() && view.starts_with(*drive))
            return target + std::string(view.substr(drive->size()));
    }
    return path;
}

bool init_savedata_app_path(IOState &io, const fs::path &vita_fs_path) {
    const fs::path user_id_path{ vita_fs_path / "ux0" / "user" / io.user_id };
    const fs::path savedata_path{ user_id_path / "savedata" };
    const fs::path savedata_game_path{ savedata_path / io.savedata };

    fs::create_directories(user_id_path);
    fs::create_directories(savedata_path);
    fs::create_directories(savedata_game_path);

    return true;
}

bool find_case_isens_path(IOState &io, VitaIoDevice &device, const fs::path &translated_path, const fs::path &system_path) {
    std::string final_path{};

    switch (device) {
    case VitaIoDevice::app0: {
        std::string app_id = translated_path.string().substr(0, 14);
        final_path = system_path.string().substr(0, system_path.string().find(app_id)) + app_id;
        break;
    }
    case VitaIoDevice::addcont0: {
        std::string addcont_id = translated_path.string().substr(0, 18);
        final_path = system_path.string().substr(0, system_path.string().find(addcont_id)) + addcont_id;
        break;
    }
    case VitaIoDevice::vs0: {
        // This only works if ALL the parent folders of the path are the correct case or are in a case insensitive fs
        // Only the file's name is searched for, not the parent folders
        final_path = system_path.string().substr(0, system_path.string().find_last_of('/'));
        break;
    }
    case VitaIoDevice::savedata0: {
        // Save data is written through SceAppUtil in the case the title chose
        // and may be read back in another case (Limbo: SAVEGAME.TXT vs savegame.txt).
        const std::string &root = io.device_paths.savedata0;
        const auto root_at = system_path.string().find(root);
        if (root.empty() || root_at == std::string::npos)
            return false;
        final_path = system_path.string().substr(0, root_at + root.size());
        break;
    }
    default: {
        return false;
    }
    }

    if (!fs::exists(final_path))
        return false;

    for (const auto &file : fs::recursive_directory_iterator(final_path)) {
        io.cachemap.emplace(string_utils::tolower(file.path().string()), file.path().string());
    }

    return true;
}

fs::path find_in_cache(IOState &io, const std::string &system_path) {
    const auto find_path = io.cachemap.find(system_path);
    if (find_path == io.cachemap.end())
        return fs::path{};
    // Writable devices (savedata0) rename and remove files: a mapping whose
    // target is gone is dropped so the caller rescans the directory.
    fs::path cached{ find_path->second.c_str() };
    if (!fs::exists(cached)) {
        io.cachemap.erase(find_path);
        return fs::path{};
    }
    return cached;
}

std::string translate_path(const char *path, VitaIoDevice &device, const IOState::DevicePaths &device_paths) {
    auto relative_path = device::remove_duplicate_device(path, device);

    // replace invalid slashes with proper forward slash
    string_utils::replace(relative_path, "\\", "/");
    string_utils::replace(relative_path, "/./", "/");
    string_utils::replace(relative_path, "//", "/");
    // TODO: Handle dot-dot paths

    switch (device) {
    case VitaIoDevice::savedata0: // Redirect savedata0: to ux0:user/00/savedata/<title_id>
    case VitaIoDevice::savedata1: {
        relative_path = device::remove_device_from_path(relative_path, device, device_paths.savedata0);
        device = VitaIoDevice::ux0;
        break;
    }
    case VitaIoDevice::app0: { // Redirect app0: to ux0:app/<title_id>
        relative_path = device::remove_device_from_path(relative_path, device, device_paths.app0);
        device = VitaIoDevice::ux0;
        break;
    }
    case VitaIoDevice::addcont0: { // Redirect addcont0: to ux0:addcont/<title_id>
        relative_path = device::remove_device_from_path(relative_path, device, device_paths.addcont0);
        device = VitaIoDevice::ux0;
        break;
    }
    case VitaIoDevice::music0: { // Redirect music0: to ux0:music
        relative_path = device::remove_device_from_path(relative_path, device, "music");
        device = VitaIoDevice::ux0;
        break;
    }
    case VitaIoDevice::photo0: { // Redirect photo0: to ux0:picture
        relative_path = device::remove_device_from_path(relative_path, device, "picture");
        device = VitaIoDevice::ux0;
        break;
    }
    case VitaIoDevice::video0: { // Redirect video0: to ux0:video
        relative_path = device::remove_device_from_path(relative_path, device, "video");
        device = VitaIoDevice::ux0;
        break;
    }

    case VitaIoDevice::host0:
    case VitaIoDevice::gro0:
    case VitaIoDevice::grw0:
    case VitaIoDevice::imc0:
    case VitaIoDevice::os0:
    case VitaIoDevice::pd0:
    case VitaIoDevice::sa0:
    case VitaIoDevice::sd0:
    case VitaIoDevice::tm0:
    case VitaIoDevice::ud0:
    case VitaIoDevice::uma0:
    case VitaIoDevice::ur0:
    case VitaIoDevice::ux0:
    case VitaIoDevice::vd0:
    case VitaIoDevice::vs0:
    case VitaIoDevice::xmc0: {
        relative_path = device::remove_device_from_path(relative_path, device);
        break;
    }
    case VitaIoDevice::tty0:
    case VitaIoDevice::tty1:
    case VitaIoDevice::tty2:
    case VitaIoDevice::tty3: {
        return std::string{};
    }
    default: {
        LOG_CRITICAL_IF(relative_path.contains(':'), "Unknown device with path {} used. Report this to the developers!", relative_path);
        return std::string{};
    }
    }

    // If the path is empty, the request is the device itself
    if (relative_path.empty())
        return std::string{};

    if (relative_path.front() == '/' || relative_path.front() == '\\')
        relative_path.erase(0, 1);

    return relative_path;
}

fs::path expand_path(IOState &io, const char *path_in, const fs::path &vita_fs_path) {
    const std::string path_resolved = resolve_user_mount(io, path_in);
    const char *path = path_in ? path_resolved.c_str() : nullptr;
    auto device = device::get_device(path);

    const auto translated_path = translate_path(path, device, io.device_paths);
    return device::construct_emulated_path(device, translated_path, vita_fs_path, io.redirect_stdio).string();
}

SceUID open_file(IOState &io, const char *path_in, const int flags, const fs::path &vita_fs_path, const char *export_name) {
    const std::string path_resolved = resolve_user_mount(io, path_in);
    const char *path = path_in ? path_resolved.c_str() : nullptr;
    auto device = device::get_device(path);
    auto device_for_icase = device;
    if (device == VitaIoDevice::_INVALID) {
        LOG_ERROR("Cannot find device for path: {}", path);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    if ((device == VitaIoDevice::tty0) || (device == VitaIoDevice::tty1) || (device == VitaIoDevice::tty2) || (device == VitaIoDevice::tty3)) {
        assert(flags >= 0);

        auto tty_type = TTY_UNKNOWN;
        if (flags & SCE_O_RDONLY)
            tty_type |= TTY_IN;
        if (flags & SCE_O_WRONLY)
            tty_type |= TTY_OUT;

        const auto fd = io.next_fd++;
        io.tty_files.emplace(fd, tty_type);

        LOG_TRACE_IF(log_file_op, "{}: Opening terminal {}:", export_name, device);
        return fd;
    }

    const auto translated_path = translate_path(path, device, io.device_paths);
    if (translated_path.empty()) {
        LOG_ERROR("Cannot translate path: {}", path);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    auto system_path = device::construct_emulated_path(device, translated_path, vita_fs_path, io.redirect_stdio);
    if (fs::is_directory(system_path)) {
        LOG_ERROR("Cannot open directory: {}", system_path);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    // Do not allow any new files if they do not have a write flag.
    if (!fs::exists(system_path)) {
        if (!(flags & SCE_O_CREAT)) {
            if (io.case_isens_find_enabled) {
                // Attempt a case-insensitive file search.
                const auto original_system_path = system_path;
                const auto cached_path = find_in_cache(io, string_utils::tolower(system_path.string()));
                if (!cached_path.empty()) {
                    system_path = cached_path;
                    LOG_TRACE("Found cached filepath at {}", system_path);
                } else {
                    const bool path_found = find_case_isens_path(io, device_for_icase, translated_path, system_path);
                    system_path = find_in_cache(io, string_utils::tolower(system_path.string()));
                    if (!system_path.empty() && path_found) {
                        LOG_TRACE("Found file on case-sensitive filesystem at {}", system_path);
                    } else {
                        LOG_ERROR("Missing file at {} (target path: {})", original_system_path, path);
                        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
                    }
                }
            } else {
                LOG_ERROR("Missing file at {} (target path: {})", system_path, path);
                return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
            }
        } else {
            if (!fs::exists(system_path.parent_path())) {
                fs::create_directories(system_path.parent_path());
            }
            fs::ofstream file(system_path);
        }
    }

    const auto normalized_path = device::construct_normalized_path(device, translated_path);

    FileStats f{ path, normalized_path, system_path, flags };
    const auto fd = io.next_fd++;
    io.std_files.emplace(fd, f);

    LOG_TRACE_IF(log_file_op, "{}: Opening file {} ({}), fd: {}", export_name, path, normalized_path, log_hex(fd));
    return fd;
}

int read_file(void *data, IOState &io, const SceUID fd, const SceSize size, const char *export_name) {
    assert(data != nullptr);
    assert(size >= 0);

    const auto file = io.std_files.find(fd);
    if (file != io.std_files.end()) {
        const auto read = file->second.read(data, 1, size);
        LOG_TRACE_IF(log_file_op && log_file_read, "{}: Reading {} bytes of fd {}", export_name, read, log_hex(fd));
        return static_cast<int>(read);
    }

    const auto tty_file = io.tty_files.find(fd);
    if (tty_file != io.tty_files.end()) {
        if (tty_file->second == TTY_IN) {
            std::cin.read(static_cast<char *>(data), size);
            LOG_TRACE_IF(log_file_op && log_file_read, "{}: Reading terminal fd: {}, size: {}", export_name, log_hex(fd), size);
            return size;
        }
        return IO_ERROR_UNK();
    }

    return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
}

int write_file(SceUID fd, const void *data, const SceSize size, const IOState &io, const char *export_name) {
    assert(data != nullptr);
    assert(size >= 0);

    if (fd < 0) {
        LOG_WARN("Error writing fd: {}, size: {}", log_hex(fd), size);
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    }

    const auto tty_file = io.tty_files.find(fd);
    if (tty_file != io.tty_files.end()) {
        if (tty_file->second & TTY_OUT) {
            std::string s(static_cast<char const *>(data), size);

            // trim newline
            if (io.redirect_stdio) {
                std::cout << s;
            } else {
                if (s.back() == '\n')
                    s.pop_back();
                LOG_TRACE_IF(log_file_op, "*** TTY: {}", s);
            }

            return size;
        }
        return IO_ERROR_UNK();
    }

    const auto file = io.std_files.find(fd);
    if (file == io.std_files.end())
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);

    if (!fs::is_directory(file->second.get_system_location().parent_path())) {
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT); // TODO: Is it the right error code?
    }

    if (file->second.can_write_file()) {
        const auto written = file->second.write(data, 1, size);
        LOG_TRACE_IF(log_file_op, "{}: Writing to fd: {}, size: {}", export_name, log_hex(fd), size);
        return static_cast<int>(written);
    }

    return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
}

int truncate_file(const SceUID fd, unsigned long long length, const IOState &io, const char *export_name) {
    if (fd < 0)
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);

    const auto file = io.std_files.find(fd);
    if (file == io.std_files.end())
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    auto trunc = file->second.truncate(length);
    LOG_TRACE_IF(log_file_op, "{}: Truncating fd: {}, to size: {}", export_name, log_hex(fd), length);
    return trunc;
}

SceOff seek_file(const SceUID fd, const SceOff offset, const SceIoSeekMode whence, IOState &io, const char *export_name) {
    if (!(whence == SCE_SEEK_SET || whence == SCE_SEEK_CUR || whence == SCE_SEEK_END))
        return IO_ERROR(SCE_ERROR_ERRNO_EOPNOTSUPP);

    if (fd < 0)
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);

    const auto file = io.std_files.find(fd);
    if (file == io.std_files.end())
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    if (!file->second.seek(offset, whence))
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);

    const auto log_mode = [](const SceIoSeekMode whence) -> const char * {
        switch (whence) {
            STR_CASE(SCE_SEEK_SET);
            STR_CASE(SCE_SEEK_CUR);
            STR_CASE(SCE_SEEK_END);
        default:
            return "INVALID";
        }
    };

    LOG_TRACE_IF(log_file_op && log_file_seek, "{}: Seeking fd: {}, offset: {}, whence: {}", export_name, log_hex(fd), log_hex(offset), log_mode(whence));
    return file->second.tell();
}

SceOff tell_file(IOState &io, const SceUID fd, const char *export_name) {
    if (fd < 0)
        return IO_ERROR(SCE_ERROR_ERRNO_EMFILE);

    const auto std_file = io.std_files.find(fd);

    if (std_file == io.std_files.end()) {
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    }

    return std_file->second.tell();
}

int stat_file(IOState &io, const char *file_in, SceIoStat *statp, const fs::path &vita_fs_path, const char *export_name, const SceUID fd) {
    const std::string file_resolved = resolve_user_mount(io, file_in);
    const char *file = file_in ? file_resolved.c_str() : nullptr;
    assert(statp != nullptr);

    memset(statp, '\0', sizeof(SceIoStat));

    fs::path file_path = "";
    if (fd == invalid_fd) {
        auto device = device::get_device(file);
        auto device_for_icase = device;
        if (device == VitaIoDevice::_INVALID) {
            LOG_ERROR("Cannot find device for path: {}", file);
            return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
        }

        const auto translated_path = translate_path(file, device, io.device_paths);
        file_path = device::construct_emulated_path(device, translated_path, vita_fs_path, io.redirect_stdio);

        if (!fs::exists(file_path)) {
            if (io.case_isens_find_enabled) {
                // Attempt a case-insensitive file search.
                const auto original_file_path = file_path;
                const auto cached_path = find_in_cache(io, string_utils::tolower(file_path.string()));
                if (!cached_path.empty()) {
                    file_path = cached_path;
                    LOG_TRACE("Found cached filepath at {}", file_path);
                } else {
                    const bool path_found = find_case_isens_path(io, device_for_icase, translated_path, file_path);
                    file_path = find_in_cache(io, string_utils::tolower(file_path.string()));
                    if (!file_path.empty() && path_found) {
                        LOG_TRACE("Found file on case-sensitive filesystem at {}", file_path);
                    } else {
                        LOG_ERROR("Missing file at {} (target path: {})", original_file_path, file);
                        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
                    }
                }
            } else {
                LOG_ERROR("Missing file at {} (target path: {})", file_path, file);
                return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
            }
        }
        LOG_TRACE_IF(log_file_op && log_file_stat, "{}: Statting file: {} ({})", export_name, file, device::construct_normalized_path(device, translated_path));
    } else { // We have previously opened and defined the location
        const auto fd_file = io.std_files.find(fd);
        if (fd_file == io.std_files.end())
            return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);

        file_path = fd_file->second.get_system_location();
        LOG_TRACE_IF(log_file_op && log_file_stat, "{}: Statting fd: {}", export_name, log_hex(fd));

        statp->st_attr = fd_file->second.get_file_mode();
    }

    std::uint64_t last_access_time_ticks;
    std::uint64_t creation_time_ticks;
    std::uint64_t last_modification_time_ticks;

#ifdef _WIN32
    struct _stati64 sb;
    if (_wstati64(file_path.generic_path().wstring().c_str(), &sb) < 0)
        return IO_ERROR_UNK();
#else
    struct stat64 sb;
    if (stat64(file_path.generic_path().string().c_str(), &sb) < 0)
        return IO_ERROR_UNK();
#endif

    last_access_time_ticks = RTC_OFFSET + (uint64_t)sb.st_atime * VITA_CLOCKS_PER_SEC;
    creation_time_ticks = RTC_OFFSET + (uint64_t)sb.st_ctime * VITA_CLOCKS_PER_SEC;
    last_modification_time_ticks = RTC_OFFSET + (uint64_t)sb.st_mtime * VITA_CLOCKS_PER_SEC;

#ifndef _WIN32
#undef st_atime
#undef st_mtime
#undef st_ctime
#endif

    // report regular files as readable but not executable
    statp->st_mode = SCE_S_IRUSR | SCE_S_IRGRP | SCE_S_IROTH;

    if (fs::is_regular_file(file_path)) {
        statp->st_size = fs::file_size(file_path);
        statp->st_attr = SCE_SO_IFREG;
        statp->st_mode |= SCE_S_IFREG;
    }
    if (fs::is_directory(file_path)) {
        statp->st_attr = SCE_SO_IFDIR;
        statp->st_mode |= SCE_S_IFDIR | SCE_S_IXUSR | SCE_S_IXGRP | SCE_S_IXOTH;
    }

    if (const auto set = io.chstat_times.find(file_path.generic_path().string()); set != io.chstat_times.end()) {
        if (set->second.created)
            creation_time_ticks = RTC_OFFSET + static_cast<uint64_t>(*set->second.created) * VITA_CLOCKS_PER_SEC;
        if (set->second.accessed)
            last_access_time_ticks = RTC_OFFSET + static_cast<uint64_t>(*set->second.accessed) * VITA_CLOCKS_PER_SEC;
    }
    __RtcTicksToPspTime(&statp->st_atime, last_access_time_ticks);
    __RtcTicksToPspTime(&statp->st_mtime, last_modification_time_ticks);
    __RtcTicksToPspTime(&statp->st_ctime, creation_time_ticks);

    return 0;
}

int stat_file_by_fd(IOState &io, const SceUID fd, SceIoStat *statp, const fs::path &vita_fs_path, const char *export_name) {
    assert(statp != nullptr);
    memset(statp, '\0', sizeof(SceIoStat));

    const auto std_file = io.std_files.find(fd);
    if (std_file == io.std_files.end()) {
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
    }

    return stat_file(io, std_file->second.get_vita_loc(), statp, vita_fs_path, export_name, fd);
}

int close_file(IOState &io, const SceUID fd, const char *export_name) {
    if (fd < 0)
        return IO_ERROR(SCE_ERROR_ERRNO_EMFILE);

    LOG_TRACE_IF(log_file_op, "{}: Closing file fd: {}", export_name, log_hex(fd));

    io.tty_files.erase(fd);
    io.std_files.erase(fd);

    return 0;
}

int lookup_path(IOState &io, const char *path_in, const fs::path &vita_fs_path, const char *export_name, VitaIoDevice &device, fs::path &host_path, bool &volume_root) {
    if (!path_in)
        return IO_ERROR(SCE_ERROR_ERRNO_EFAULT);
    // The kernel copies at most 0x400 bytes of the path.
    if (strnlen(path_in, 0x400) == 0x400)
        return IO_ERROR(SCE_KERNEL_ERROR_UNTERMINATED_STRING);
    const std::string resolved = resolve_user_mount(io, path_in);
    const auto colon = resolved.find(':');
    if (colon == std::string::npos) {
        // Only an absolute path may leave out the device.
        if (resolved.empty() || resolved[0] != '/')
            return IO_ERROR(SCE_ERROR_ERRNO_EINVAL);
        return IO_ERROR(SCE_ERROR_ERRNO_ENODEV); // no current device for a game
    }
    if (colon > 30)
        return IO_ERROR(SCE_ERROR_ERRNO_ENAMETOOLONG);
    device = device::get_device(resolved);
    if (device == VitaIoDevice::_INVALID)
        return IO_ERROR(SCE_ERROR_ERRNO_ENODEV);
    const std::string_view rest = std::string_view(resolved).substr(colon + 1);
    volume_root = rest.find_first_not_of('/') == std::string_view::npos;
    VitaIoDevice redirected = device;
    const auto translated_path = translate_path(resolved.c_str(), redirected, io.device_paths);
    host_path = device::construct_emulated_path(redirected, translated_path, vita_fs_path, io.redirect_stdio);
    if (fs::exists(host_path))
        return 0;
    if (io.case_isens_find_enabled) {
        if (auto cached = find_in_cache(io, string_utils::tolower(host_path.string())); !cached.empty()) {
            host_path = cached;
            return 0;
        }
        const bool found = find_case_isens_path(io, redirected, translated_path, host_path);
        if (auto cached = find_in_cache(io, string_utils::tolower(host_path.string())); found && !cached.empty()) {
            host_path = cached;
            return 0;
        }
    }
    return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
}

// Seconds since the epoch of a SceDateTime, as stat_file reads them back.
static time_t io_time(const SceDateTime &time) {
    return static_cast<time_t>((__RtcPspTimeToTicks(&time) - RTC_OFFSET) / VITA_CLOCKS_PER_SEC);
}

// exfatfs 0x810016bc: exFAT can hold these dates.
static bool valid_io_time(const SceDateTime &time) {
    return time.year >= 1980 && time.year <= 2107 && time.month >= 1 && time.month <= 12 && time.day >= 1
        && time.day <= 31 && time.hour <= 23 && time.minute <= 59 && time.second <= 59;
}

// iofilemgr 0x810169cc/0x8100694c, ksceVopChstat 0x8100f248 and exfatfs
// 0x81009b18. app0: is a read-only PFS mount; savedata0: passes MODE and
// SIZE through PfsMgr and the times to exFAT, as ux0: has them. A game
// thread's MODE only sets or clears the read-only attribute
// (ksceSblACMgrConvertModeToFsAttribute 0x81001350).
int chstat_path(IOState &io, const char *path, const SceIoStat *stat, SceUInt32 bits, const fs::path &vita_fs_path, const char *export_name) {
    VitaIoDevice device;
    fs::path host_path;
    bool volume_root = false;
    if (const int error = lookup_path(io, path, vita_fs_path, export_name, device, host_path, volume_root))
        return error;
    bits &= ~0x10000u; // the raw attribute is for the kernel only
    const bool directory = fs::is_directory(host_path);
    if ((bits & SCE_CST_SIZE) && directory)
        return IO_ERROR(SCE_ERROR_ERRNO_EISDIR);
    if (!stat)
        return IO_ERROR(SCE_ERROR_ERRNO_EINVAL);
    if (device == VitaIoDevice::app0)
        return IO_ERROR(SCE_ERROR_ERRNO_EROFS);
    if (volume_root && (bits & (SCE_CST_MODE | SCE_CST_CT | SCE_CST_AT | SCE_CST_MT)))
        return IO_ERROR(SCE_ERROR_ERRNO_EACCES);
    if (((bits & SCE_CST_CT) && !valid_io_time(stat->st_ctime)) || ((bits & SCE_CST_AT) && !valid_io_time(stat->st_atime))
        || ((bits & SCE_CST_MT) && !valid_io_time(stat->st_mtime)))
        return IO_ERROR(SCE_ERROR_ERRNO_EINVAL);
    // A game's read (0x100) and write (0x80) bits, SCE_S_IROTH/IWOTH here.
    const unsigned access = stat->st_mode & (SCE_S_IROTH | SCE_S_IWOTH);
    if ((bits & SCE_CST_MODE) && access != (SCE_S_IROTH | SCE_S_IWOTH) && access != SCE_S_IROTH)
        return IO_ERROR(SCE_ERROR_ERRNO_EINVAL);

    boost::system::error_code error;
    if (bits & SCE_CST_SIZE) {
        fs::resize_file(host_path, static_cast<uintmax_t>(stat->st_size), error);
        if (error)
            return IO_ERROR(SCE_ERROR_ERRNO_ENOSPC);
    }
    if (bits & SCE_CST_MODE) {
        fs::permissions(host_path, fs::perms::owner_write | (access == SCE_S_IROTH ? fs::perms::remove_perms : fs::perms::add_perms), error);
        if (error)
            return IO_ERROR_UNK();
    }
    if (bits & (SCE_CST_AT | SCE_CST_MT)) {
#ifdef _WIN32
        struct _stati64 sb;
        if (_wstati64(host_path.generic_path().wstring().c_str(), &sb) < 0)
            return IO_ERROR_UNK();
        struct __utimbuf64 times = { sb.st_atime, sb.st_mtime };
        if (bits & SCE_CST_AT)
            times.actime = io_time(stat->st_atime);
        if (bits & SCE_CST_MT)
            times.modtime = io_time(stat->st_mtime);
        if (_wutime64(host_path.generic_path().wstring().c_str(), &times) < 0)
            return IO_ERROR_UNK();
#else
        // The date's microseconds are dropped (exfatfs 0x810047e6).
        struct timespec times[2] = { { 0, UTIME_OMIT }, { 0, UTIME_OMIT } };
        if (bits & SCE_CST_AT)
            times[0] = { io_time(stat->st_atime), 0 };
        if (bits & SCE_CST_MT)
            times[1] = { io_time(stat->st_mtime), 0 };
        if (utimensat(AT_FDCWD, host_path.generic_path().string().c_str(), times, 0) < 0)
            return IO_ERROR_UNK();
#endif
    }
    if (bits & (SCE_CST_CT | SCE_CST_AT)) {
        auto &times = io.chstat_times[host_path.generic_path().string()];
        if (bits & SCE_CST_CT)
            times.created = io_time(stat->st_ctime);
        if (bits & SCE_CST_AT)
            times.accessed = io_time(stat->st_atime);
    }
    return 0;
}

// iofilemgr 0x81002144/0x810075c8: a volume root flushes the volume, any
// other existing path its file, whatever the flags. Host files are written
// through: nothing is left to flush.
int sync_path(IOState &io, const char *path, const fs::path &vita_fs_path, const char *export_name) {
    VitaIoDevice device;
    fs::path host_path;
    bool volume_root = false;
    const int error = lookup_path(io, path, vita_fs_path, export_name, device, host_path, volume_root);
    // The root of a mount is always there, even before a file is on it.
    if (error == SCE_ERROR_ERRNO_ENOENT && volume_root)
        return 0;
    return error;
}

bool get_volume_info(const fs::path &host_path, VolumeInfo &info) {
#ifdef _WIN32
    const std::wstring root = host_path.root_path().wstring();
    ULARGE_INTEGER available, capacity;
    DWORD sectors_per_cluster, bytes_per_sector, free_clusters, clusters, serial;
    if (!GetDiskFreeSpaceExW(host_path.wstring().c_str(), &available, &capacity, nullptr)
        || !GetDiskFreeSpaceW(root.c_str(), &sectors_per_cluster, &bytes_per_sector, &free_clusters, &clusters)
        || !GetVolumeInformationW(root.c_str(), nullptr, 0, &serial, nullptr, nullptr, nullptr, 0))
        return false;
    info = { capacity.QuadPart, available.QuadPart, static_cast<uint32_t>(sectors_per_cluster * bytes_per_sector), serial };
#else
    struct statvfs volume;
    if (statvfs(host_path.generic_path().string().c_str(), &volume) < 0)
        return false;
    info = { static_cast<uint64_t>(volume.f_blocks) * volume.f_frsize, static_cast<uint64_t>(volume.f_bavail) * volume.f_frsize,
        static_cast<uint32_t>(volume.f_frsize), static_cast<uint32_t>(volume.f_fsid) };
#endif
    return true;
}

int remove_file(IOState &io, const char *file_in, const fs::path &vita_fs_path, const char *export_name) {
    const std::string file_resolved = resolve_user_mount(io, file_in);
    const char *file = file_in ? file_resolved.c_str() : nullptr;
    auto device = device::get_device(file);
    if (device == VitaIoDevice::_INVALID) {
        LOG_ERROR("Cannot find device for path: {}", file);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    const auto translated_path = translate_path(file, device, io.device_paths);
    if (translated_path.empty()) {
        LOG_ERROR("Cannot translate path: {}", translated_path);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    const auto emulated_path = device::construct_emulated_path(device, translated_path, vita_fs_path, io.redirect_stdio);
    if (!fs::exists(emulated_path) || fs::is_directory(emulated_path)) {
        LOG_ERROR("File does not exist at path: {} (target path: {})", emulated_path, file);
    }

    LOG_TRACE_IF(log_file_op, "{}: Removing file {} ({})", export_name, file, device::construct_normalized_path(device, translated_path));

    boost::system::error_code error_code{};
    auto res = fs::detail::remove(emulated_path, &error_code);

    if (!(res && !(error_code.value()))) {
        LOG_ERROR("Cannot remove file: {} ({})", file, device::construct_normalized_path(device, translated_path));
        LOG_ERROR("Error code: {} ({})", error_code.value(), error_code.message());
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }
    io.chstat_times.erase(emulated_path.generic_path().string());

    return 0;
}

int rename(IOState &io, const char *old_name_in, const char *new_name_in, const fs::path &vita_fs_path, const char *export_name) {
    const std::string old_resolved = resolve_user_mount(io, old_name_in), new_resolved = resolve_user_mount(io, new_name_in);
    const char *old_name = old_name_in ? old_resolved.c_str() : nullptr;
    const char *new_name = new_name_in ? new_resolved.c_str() : nullptr;
    auto device = device::get_device(old_name);
    if (device == VitaIoDevice::_INVALID) {
        LOG_ERROR("Cannot find device for path: {}", old_name);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    const auto translated_old_path = translate_path(old_name, device, io.device_paths);
    if (translated_old_path.empty()) {
        LOG_ERROR("Cannot translate path: {}", translated_old_path);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    // The new name carries its own device (savedata0:, app0:, ...).
    auto new_device = device::get_device(new_name);
    if (new_device == VitaIoDevice::_INVALID) {
        LOG_ERROR("Cannot find device for path: {}", new_name);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }
    const auto translated_new_path = translate_path(new_name, new_device, io.device_paths);
    if (translated_new_path.empty()) {
        LOG_ERROR("Cannot translate path: {}", translated_new_path);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    const auto emulated_old_path = device::construct_emulated_path(device, translated_old_path, vita_fs_path, io.redirect_stdio);
    if (!fs::exists(emulated_old_path)) {
        LOG_ERROR("File does not exist at path: {} (target path: {})", emulated_old_path, old_name);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    const auto emulated_new_path = device::construct_emulated_path(new_device, translated_new_path, vita_fs_path, io.redirect_stdio);

    LOG_TRACE_IF(log_file_op, "{}: Renaming file {} to {} ({} to {})", export_name, old_name, new_name, emulated_old_path, emulated_new_path);

    boost::system::error_code error_code{};
    fs::rename(emulated_old_path, emulated_new_path, error_code);

    if (error_code.value()) {
        LOG_ERROR("Cannot rename file: {} to {} ({} to {})", old_name, new_name, emulated_old_path, emulated_new_path);
        LOG_ERROR("Error code: {} ({})", error_code.value(), error_code.message());
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }
    io.chstat_times.erase(emulated_new_path.generic_path().string());
    if (auto times = io.chstat_times.extract(emulated_old_path.generic_path().string())) {
        times.key() = emulated_new_path.generic_path().string();
        io.chstat_times.insert(std::move(times));
    }

    return 0;
}

SceUID open_dir(IOState &io, const char *path_in, const fs::path &vita_fs_path, const char *export_name) {
    const std::string path_resolved = resolve_user_mount(io, path_in);
    const char *path = path_in ? path_resolved.c_str() : nullptr;
    auto device = device::get_device(path);
    auto device_for_icase = device;
    const auto translated_path = translate_path(path, device, io.device_paths);

    auto dir_path = device::construct_emulated_path(device, translated_path, vita_fs_path, io.redirect_stdio) / "";
    if (!fs::exists(dir_path)) {
        if (io.case_isens_find_enabled) {
            // Attempt a case-insensitive file search.
            const auto original_dir_path = dir_path;
            const auto cached_path = find_in_cache(io, string_utils::tolower(dir_path.string()));
            if (!cached_path.empty()) {
                dir_path = cached_path;
                LOG_TRACE("Found cached directory path at {}", dir_path);
            } else {
                const bool path_found = find_case_isens_path(io, device_for_icase, translated_path, dir_path);
                dir_path = find_in_cache(io, string_utils::tolower(dir_path.string().substr(0, dir_path.string().size() - 1)));
                if (!dir_path.empty() && path_found) {
                    LOG_TRACE("Found directory on case-sensitive filesystem at {}", dir_path);
                } else {
                    LOG_ERROR("Directory does not exist at {} (target path: {})", original_dir_path, path);
                    return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
                }
            }
        } else {
            LOG_ERROR("Directory does not exist at: {} (target path: {})", dir_path, path);
            return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
        }
    }

    const DirPtr opened = create_shared_dir(dir_path);
    if (!opened) {
        LOG_ERROR("Failed to open directory at: {} (target path: {})", dir_path, path);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    const auto normalized = device::construct_normalized_path(device, translated_path);
    const DirStats d{ path, normalized, dir_path, opened };
    const auto fd = io.next_fd++;
    io.dir_entries.emplace(fd, d);

    LOG_TRACE_IF(log_file_op, "{}: Opening dir {} ({}), fd: {}", export_name, path, normalized, log_hex(fd));

    return fd;
}

SceUID read_dir(IOState &io, const SceUID fd, SceIoDirent *dent, const fs::path &vita_fs_path, const char *export_name) {
    assert(dent != nullptr);

    memset(dent->d_name, '\0', sizeof(dent->d_name));

    const auto dir = io.dir_entries.find(fd);

    if (dir != io.dir_entries.end()) {
        // Refuse any fd that is not explicitly a directory
        if (!dir->second.is_directory())
            return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);

        const auto d = dir->second.get_dir_ptr();
        if (!d)
            return 0;

        const auto d_name_utf8 = get_file_in_dir(d);
        strncpy(dent->d_name, d_name_utf8.c_str(), sizeof(dent->d_name));

        const auto cur_path = dir->second.get_system_location() / d_name_utf8;
        if (!(cur_path.filename_is_dot() || cur_path.filename_is_dot_dot())) {
            const auto file_path = std::string(dir->second.get_vita_loc()) + '/' + d_name_utf8;

            LOG_TRACE_IF(log_file_op, "{}: Reading entry {} of fd: {}", export_name, file_path, log_hex(fd));
            if (stat_file(io, file_path.c_str(), &dent->d_stat, vita_fs_path, export_name) < 0)
                return IO_ERROR(SCE_ERROR_ERRNO_EMFILE);
            else
                return 1; // move to the next file
        }
        return read_dir(io, fd, dent, vita_fs_path, export_name);
    }

    return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);
}

bool copy_path(const fs::path &src_path, const fs::path &vita_fs_path, const std::string &app_title_id, const std::string &app_category) {
    // Check if is path
    if (app_category.contains("gp")) {
        const auto app_path{ vita_fs_path / "ux0/app" / app_title_id };
        const auto result = fs_utils::copy_directory_contents(src_path, app_path);

        fs::remove_all(src_path);

        return result;
    }

    return true;
}

int create_dir(IOState &io, const char *dir_in, int mode, const fs::path &vita_fs_path, const char *export_name, const bool recursive) {
    const std::string dir_resolved = resolve_user_mount(io, dir_in);
    const char *dir = dir_in ? dir_resolved.c_str() : nullptr;
    auto device = device::get_device(dir);
    const auto translated_path = translate_path(dir, device, io.device_paths);
    if (translated_path.empty()) {
        LOG_ERROR("Failed to translate path: {}", dir);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    const auto emulated_path = device::construct_emulated_path(device, translated_path, vita_fs_path, io.redirect_stdio);
    if (recursive)
        return fs::create_directories(emulated_path);
    if (fs::exists(emulated_path))
        return IO_ERROR(SCE_ERROR_ERRNO_EEXIST);

    const auto parent_path = fs::path(emulated_path).remove_trailing_separator().parent_path();
    if (!fs::exists(parent_path)) // Vita cannot recursively create directories
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);

    LOG_TRACE_IF(log_file_op, "{}: Creating new dir {} ({})", export_name, dir, device::construct_normalized_path(device, translated_path));

    if (!fs::create_directory(emulated_path)) {
        LOG_ERROR("Failed to create directory at {} (target path: {})", emulated_path, dir);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    return 0;
}

int close_dir(IOState &io, const SceUID fd, const char *export_name) {
    if (fd < 0)
        return IO_ERROR(SCE_ERROR_ERRNO_EMFILE);

    const auto erased_entries = io.dir_entries.erase(fd);

    LOG_TRACE_IF(log_file_op, "{}: Closing dir fd: {}", export_name, log_hex(fd));

    if (erased_entries == 0)
        return IO_ERROR(SCE_ERROR_ERRNO_EBADFD);

    return 0;
}

int remove_dir(IOState &io, const char *dir_in, const fs::path &vita_fs_path, const char *export_name) {
    const std::string dir_resolved = resolve_user_mount(io, dir_in);
    const char *dir = dir_in ? dir_resolved.c_str() : nullptr;
    auto device = device::get_device(dir);
    if (device == VitaIoDevice::_INVALID) {
        LOG_ERROR("Cannot find device for path: {}", dir);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    const auto translated_path = translate_path(dir, device, io.device_paths);
    if (translated_path.empty()) {
        LOG_ERROR("Cannot translate path: {}", dir);
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    LOG_TRACE_IF(log_file_op, "{}: Removing dir {} ({})", export_name, dir, device::construct_normalized_path(device, translated_path));

    if (!fs::remove_all(device::construct_emulated_path(device, translated_path, vita_fs_path, io.redirect_stdio))) {
        LOG_ERROR("Cannot remove dir: {} ({})", dir, device::construct_normalized_path(device, translated_path));
        return IO_ERROR(SCE_ERROR_ERRNO_ENOENT);
    }

    return 0;
}

std::optional<std::string> normalize_fios_path(std::string_view path, size_t capacity) {
    if (path.empty())
        return std::nullopt;
    const auto is_separator = [](char c) { return c == '/' || c == '\\'; };
    std::string out;
    std::string_view rest = path;
    bool absolute = is_separator(path.front());
    const size_t colon = path.find(':');
    if (colon != std::string_view::npos && colon >= 1 && colon <= 16
        && std::all_of(path.begin(), path.begin() + colon, [](char c) { return std::isalnum(static_cast<unsigned char>(c)); })) {
        out = path.substr(0, colon + 1);
        rest = path.substr(colon + 1);
        absolute = true;
        if (colon == 5 && path.starts_with("host")) {
            // A host PC path: an optional UNC prefix (two backslashes) and drive letter, then
            // a path that is relative unless it follows the drive with a separator.
            if (rest.starts_with("\\\\"))
                out += "\\\\";
            while (!rest.empty() && is_separator(rest.front()))
                rest.remove_prefix(1);
            absolute = false;
            if (rest.size() >= 2 && std::isalpha(static_cast<unsigned char>(rest[0])) && rest[1] == ':') {
                out += static_cast<char>(std::toupper(static_cast<unsigned char>(rest[0])));
                out += ':';
                rest.remove_prefix(2);
                absolute = !rest.empty() && is_separator(rest.front());
            }
        }
    }
    std::vector<std::string_view> parts;
    while (!rest.empty()) {
        const auto end = std::find_if(rest.begin(), rest.end(), is_separator);
        const std::string_view part(rest.begin(), end);
        rest.remove_prefix(end == rest.end() ? rest.size() : part.size() + 1);
        if (part.empty() || part == ".")
            continue;
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..")
                parts.pop_back();
            else if (!absolute)
                parts.push_back(part);
            continue;
        }
        parts.push_back(part);
    }
    if (absolute)
        out += '/';
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i)
            out += '/';
        out += parts[i];
    }
    if (!absolute && parts.empty())
        out += '.';
    if (out.size() >= capacity)
        return std::nullopt;
    return out;
}

namespace {
constexpr int SCE_FIOS_ERROR_BAD_PATH = 0x80820005, SCE_FIOS_ERROR_BAD_PTR = 0x80820006, SCE_FIOS_ERROR_ACCESS = 0x80820013,
              SCE_FIOS_ERROR_PATH_TOO_LONG = 0x80820018, SCE_FIOS_ERROR_TOO_MANY_OVERLAYS = 0x80820019,
              SCE_FIOS_ERROR_BAD_OVERLAY = 0x8082001A; // names from the FIOS2 SDK numbering
constexpr uint8_t privileged_order = 0x80;

// SceFios2Kernel's validator: type <= 3, both paths terminated within
// SCE_FIOS_OVERLAY_POINT_MAX and short enough once normalized.
int validate_overlay(const SceFiosProcessOverlay &overlay, std::string &dst, std::string &src) {
    if (overlay.type > SCE_FIOS_OVERLAY_TYPE_WRITABLE)
        return SCE_FIOS_ERROR_BAD_OVERLAY;
    if (strnlen(overlay.dst, SCE_FIOS_OVERLAY_POINT_MAX) == SCE_FIOS_OVERLAY_POINT_MAX
        || strnlen(overlay.src, SCE_FIOS_OVERLAY_POINT_MAX) == SCE_FIOS_OVERLAY_POINT_MAX)
        return SCE_FIOS_ERROR_PATH_TOO_LONG;
    const auto normalized_dst = normalize_fios_path(overlay.dst, SCE_FIOS_OVERLAY_POINT_MAX);
    const auto normalized_src = normalize_fios_path(overlay.src, SCE_FIOS_OVERLAY_POINT_MAX);
    if (!normalized_dst || !normalized_src)
        return SCE_FIOS_ERROR_BAD_PATH;
    dst = *normalized_dst;
    src = *normalized_src;
    return 0;
}

// A caller that is not privileged may name only its own process.
int check_access(SceUID caller, SceUID pid) {
    return pid == -1 || pid != caller ? SCE_FIOS_ERROR_ACCESS : 0;
}

// Its lookups skip privileged overlays and other processes' ones.
std::vector<FiosOverlay>::iterator find_overlay(IOState &io, SceUID pid, SceUID id) {
    return std::find_if(io.overlays.begin(), io.overlays.end(), [&](const FiosOverlay &overlay) {
        return overlay.id == id && overlay.order < privileged_order && overlay.process_id == pid;
    });
}

// Before the first overlay whose order is not lower.
void insert_overlay(IOState &io, FiosOverlay overlay) {
    const auto at = std::find_if(io.overlays.begin(), io.overlays.end(), [&](const FiosOverlay &other) { return other.order >= overlay.order; });
    io.overlays.insert(at, std::move(overlay));
}

// Apps may not overlay a whole device: "ux0:", "ux0:/" or "ux0:.".
bool is_device_root(const char *dst) {
    size_t i = 0;
    while (i < SCE_FIOS_OVERLAY_POINT_MAX && std::isalnum(static_cast<unsigned char>(dst[i])))
        ++i;
    if (i == 0 || i + 2 >= SCE_FIOS_OVERLAY_POINT_MAX || dst[i] != ':')
        return false;
    const char *rest = dst + i + 1;
    return rest[0] == '\0' || (rest[1] == '\0' && (rest[0] == '.' || rest[0] == '/'));
}
} // namespace

int create_overlay(IOState &io, SceUID caller, SceUID pid, const SceFiosProcessOverlay *fios_overlay, SceUID *id) {
    if (!fios_overlay || !id)
        return SCE_FIOS_ERROR_BAD_PTR;
    // The syscall copies the id out whatever the result.
    *id = 0;
    if (is_device_root(fios_overlay->dst))
        return SCE_FIOS_ERROR_BAD_PATH;
    if (const int error = check_access(caller, pid))
        return error;
    std::lock_guard<std::mutex> lock(io.overlay_mutex);
    std::string dst, src;
    if (const int error = validate_overlay(*fios_overlay, dst, src))
        return error;
    // A process table holds 128 entries, at most 64 of them app overlays.
    const auto process_overlays = std::count_if(io.overlays.begin(), io.overlays.end(), [&](const FiosOverlay &overlay) { return overlay.process_id == pid; });
    const auto app_overlays = std::count_if(io.overlays.begin(), io.overlays.end(), [&](const FiosOverlay &overlay) {
        return overlay.process_id == pid && overlay.order < privileged_order;
    });
    if ((fios_overlay->order < privileged_order && app_overlays >= SCE_FIOS_OVERLAY_MAX_OVERLAYS) || process_overlays >= 2 * SCE_FIOS_OVERLAY_MAX_OVERLAYS)
        return SCE_FIOS_ERROR_TOO_MANY_OVERLAYS;
    *id = io.next_overlay_id++;
    if (io.next_overlay_id == 0)
        io.next_overlay_id = 1;
    insert_overlay(io, { .id = *id, .type = fios_overlay->type, .order = fios_overlay->order, .process_id = pid, .dst = dst, .src = src });
    return 0;
}

int get_overlay(IOState &io, SceUID caller, SceUID pid, SceUID id, SceFiosProcessOverlay *out) {
    if (!out)
        return SCE_FIOS_ERROR_BAD_PTR;
    // The kernel copies its whole buffer back: zeros unless an entry was found.
    memset(out, 0, sizeof(*out));
    std::lock_guard<std::mutex> lock(io.overlay_mutex);
    if (const int error = check_access(caller, pid))
        return error;
    const auto overlay = find_overlay(io, pid, id);
    if (overlay == io.overlays.end())
        return SCE_FIOS_ERROR_BAD_OVERLAY;
    out->type = overlay->type;
    out->order = overlay->order;
    out->dst_len = static_cast<int16_t>(overlay->dst.size());
    out->src_len = static_cast<int16_t>(overlay->src.size());
    out->process_id = overlay->process_id;
    out->id = overlay->id;
    strncpy(out->dst, overlay->dst.c_str(), sizeof(out->dst) - 1);
    strncpy(out->src, overlay->src.c_str(), sizeof(out->src) - 1);
    return 0;
}

int modify_overlay(IOState &io, SceUID caller, SceUID pid, SceUID id, const SceFiosProcessOverlay *fios_overlay) {
    if (!fios_overlay)
        return SCE_FIOS_ERROR_BAD_PTR;
    std::lock_guard<std::mutex> lock(io.overlay_mutex);
    if (const int error = check_access(caller, pid))
        return error;
    std::string dst, src;
    if (const int error = validate_overlay(*fios_overlay, dst, src))
        return error;
    const auto overlay = find_overlay(io, pid, id);
    if (overlay == io.overlays.end())
        return SCE_FIOS_ERROR_BAD_OVERLAY;
    FiosOverlay replacement{ .id = id, .type = fios_overlay->type, .order = fios_overlay->order, .process_id = pid, .dst = dst, .src = src };
    if (replacement.order == overlay->order) {
        *overlay = std::move(replacement);
    } else {
        io.overlays.erase(overlay);
        insert_overlay(io, std::move(replacement));
    }
    return 0;
}

int remove_overlay(IOState &io, SceUID caller, SceUID pid, SceUID id) {
    std::lock_guard<std::mutex> lock(io.overlay_mutex);
    if (const int error = check_access(caller, pid))
        return error;
    const auto overlay = find_overlay(io, pid, id);
    if (overlay == io.overlays.end())
        return SCE_FIOS_ERROR_BAD_OVERLAY;
    io.overlays.erase(overlay);
    return 0;
}

int resolve_path(IOState &io, SceUID pid, const char *input, std::string &output, const SceUInt32 min_order, const SceUInt32 max_order) {
    constexpr size_t capacity = 1024;
    if (strnlen(input, capacity) == capacity)
        return SCE_FIOS_ERROR_PATH_TOO_LONG;
    auto path = normalize_fios_path(input, capacity);
    if (!path)
        return SCE_FIOS_ERROR_BAD_PATH;
    const auto is_separator = [](char c) { return c == '/' || c == '\\'; };
    std::lock_guard<std::mutex> lock(io.overlay_mutex);
    for (const FiosOverlay &overlay : io.overlays) {
        if ((overlay.process_id != -1 && overlay.process_id != pid) || overlay.order < min_order || overlay.order > max_order)
            continue;
        // The overlay covers dst itself and paths below it.
        const std::string &dst = overlay.dst;
        if (!path->starts_with(dst))
            continue;
        if (!dst.empty() && !is_separator(dst.back()) && path->size() > dst.size() && !is_separator((*path)[dst.size()]))
            continue;
        auto replaced = normalize_fios_path(overlay.src + "/" + path->substr(dst.size()), capacity);
        if (!replaced)
            return SCE_FIOS_ERROR_BAD_PATH;
        path = std::move(replaced);
    }
    output = std::move(*path);
    return 0;
}
