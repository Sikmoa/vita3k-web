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

#include <module/module.h>

#include <cstring>

EXPORT(int, sceLiveAreaGetFrameRevision) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceLiveAreaGetFrameUserData) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceLiveAreaGetRevision) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceLiveAreaGetStatus) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceLiveAreaReplaceAllAsync) {
    return UNIMPLEMENTED();
}

EXPORT(int, sceLiveAreaReplaceAllSync) {
    return UNIMPLEMENTED();
}

// Firmware 3.74 livearea_util.suprx validates the request, then submits it
// to SceShell. The browser has no LiveArea to update: an accepted request
// completes at once, so none is ever pending.
EXPORT(int, sceLiveAreaUpdateFrameAsync, const char *format_version, const char *frame_xml, SceInt32 frame_xml_length, const char *contents_path, SceUInt32 target_type) {
    constexpr uint32_t SCE_LIVEAREA_ERROR_INVALID_ARGUMENT = 0x80104002, SCE_LIVEAREA_ERROR_TOO_LARGE = 0x80104009; // names unknown
    if (!format_version || strcmp(format_version, "01.00") != 0)
        return RET_ERROR(SCE_LIVEAREA_ERROR_INVALID_ARGUMENT);
    if (!contents_path || strnlen(contents_path, 256) > 255)
        return RET_ERROR(SCE_LIVEAREA_ERROR_INVALID_ARGUMENT);
    if (!frame_xml || (target_type & ~1u))
        return RET_ERROR(SCE_LIVEAREA_ERROR_INVALID_ARGUMENT);
    const size_t length = frame_xml_length < 0 ? strnlen(frame_xml, 10240) : static_cast<size_t>(frame_xml_length);
    if (length > 10239)
        return RET_ERROR(SCE_LIVEAREA_ERROR_TOO_LARGE);
    return 0;
}

EXPORT(int, sceLiveAreaUpdateFrameSync) {
    return UNIMPLEMENTED();
}
