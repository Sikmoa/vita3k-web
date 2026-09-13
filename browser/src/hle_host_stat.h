// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Emscripten's musl exposes POSIX stat times as macros. Consume that header
// before Vita's SceIoStat declarations so include order cannot rewrite their
// st_*time fields. This changes no guest structure or implementation.
#include <sys/stat.h>
#undef st_atime
#undef st_ctime
#undef st_mtime
