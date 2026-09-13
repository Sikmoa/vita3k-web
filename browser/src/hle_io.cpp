// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later

// Declare Vita's SceIoStat with the musl aliases suppressed first. The original
// io.cpp also needs those aliases when reading host struct stat, then explicitly
// undefines them before accessing the guest structure. Preserve that sequence.
#ifdef __EMSCRIPTEN__
#include <io/functions.h>
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec
#endif
#include "../../vita3k/io/src/io.cpp"
