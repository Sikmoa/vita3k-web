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

#pragma once

#undef st_atime
#undef st_ctime
#undef st_mtime

#include <io/VitaIoDevice.h>
#include <io/state.h>
#include <io/types.h>

#include <util/fs.h>

#include <string>

struct IOState;

inline SceUID invalid_fd = -1;

void init_device_paths(IOState &io);
bool init_savedata_app_path(IOState &io, const fs::path &vita_fs_path);
bool init(IOState &io, const fs::path &cache_path, const fs::path &log_path, const fs::path &vita_fs_path, bool redirect_stdio);
void io_deinit(IOState &io);

bool find_case_isens_path(IOState &io, VitaIoDevice &device, const fs::path &translated_path, const fs::path &system_path);
fs::path find_in_cache(IOState &io, const std::string &system_path);

fs::path expand_path(IOState &io, const char *path, const fs::path &vita_fs_path);
// Rewrites a path on one of the app's vs0 user drives to its vs0: path.
std::string resolve_user_mount(const IOState &io, const char *path);
std::string translate_path(const char *path, VitaIoDevice &device, const IOState::DevicePaths &device_paths);

/**
 * @brief Copy all files from a source path to the corresponding path in the emulated PS Vita filesystem
 * and delete the source path
 *
 * @param src_path Path from the host filesystem
 * @param vita_fs_path Vita emulated filesystem
 * @param app_title_id App title ID (`PCSXXXXXX`)
 * @param app_category Content type ID as specified by `param.sfo` file
 * @return true Success
 * @return false Error
 */
bool copy_path(const fs::path &src_path, const fs::path &vita_fs_path, const std::string &app_title_id, const std::string &app_category);

SceUID open_file(IOState &io, const char *path, const int flags, const fs::path &vita_fs_path, const char *export_name);
int read_file(void *data, IOState &io, SceUID fd, SceSize size, const char *export_name);
int write_file(SceUID fd, const void *data, SceSize size, const IOState &io, const char *export_name);
int truncate_file(SceUID fd, unsigned long long length, const IOState &io, const char *export_name);
SceOff seek_file(SceUID fd, SceOff offset, SceIoSeekMode whence, IOState &io, const char *export_name);
SceOff tell_file(IOState &io, const SceUID fd, const char *export_name);
int stat_file(IOState &io, const char *file, SceIoStat *statp, const fs::path &vita_fs_path, const char *export_name, SceUID fd = invalid_fd);
int stat_file_by_fd(IOState &io, const SceUID fd, SceIoStat *statp, const fs::path &vita_fs_path, const char *export_name);
int close_file(IOState &io, SceUID fd, const char *export_name);
int remove_file(IOState &io, const char *file, const fs::path &vita_fs_path, const char *export_name);
int rename(IOState &io, const char *old_name, const char *new_name, const fs::path &vita_fs_path, const char *export_name);

SceUID open_dir(IOState &io, const char *path, const fs::path &vita_fs_path, const char *export_name);
SceUID read_dir(IOState &io, SceUID fd, SceIoDirent *dent, const fs::path &vita_fs_path, const char *export_name);
int create_dir(IOState &io, const char *dir, int mode, const fs::path &vita_fs_path, const char *export_name, const bool recursive = false);
int close_dir(IOState &io, SceUID fd, const char *export_name);
int remove_dir(IOState &io, const char *dir, const fs::path &vita_fs_path, const char *export_name);

// SceFios functions
// SceFios2Kernel overlays of firmware 3.74, for a caller that is not a system
// program: it may only see and change its own process's app overlays
// (order < 0x80). Results are 0 or a SCE_FIOS error.
int create_overlay(IOState &io, SceUID caller, SceUID pid, const SceFiosProcessOverlay *fios_overlay, SceUID *id);
int get_overlay(IOState &io, SceUID caller, SceUID pid, SceUID id, SceFiosProcessOverlay *out);
int modify_overlay(IOState &io, SceUID caller, SceUID pid, SceUID id, const SceFiosProcessOverlay *overlay);
int remove_overlay(IOState &io, SceUID caller, SceUID pid, SceUID id);
// Paths are normalized the way the kernel does; errors are SCE_FIOS errors.
int resolve_path(IOState &io, SceUID pid, const char *input, std::string &output, SceUInt32 min_order, SceUInt32 max_order);
