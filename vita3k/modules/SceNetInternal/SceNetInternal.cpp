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

#include "../SceNet/SceNet.h"

#include <net/state.h>
#include <util/lock_and_find.h>
#ifdef __EMSCRIPTEN__
#include <net/offline_socket.h>
#endif

#include <util/tracy.h>
TRACY_MODULE_NAME(SceNetInternal);

EXPORT(int, sceNetInternalInetPton, int af, const char *src, void *dst) {
    return CALL_EXPORT(sceNetInetPton, af, src, dst);
}

// SceNetPs 0x81007924 asks the interface connection manager for an
// interface to bring up for the socket (0x81002544). Offline none is
// registered: the socket is marked so the next epoll scan reports
// SCE_NET_EPOLL_ICM_DONE, its error is cleared and the call returns 0 at
// once, whatever the flags (0x80: do not wait for a connection).
EXPORT(int, sceNetInternalIcmConnect, int sid, int flags) {
    TRACY_FUNC(sceNetInternalIcmConnect, sid, flags);
#ifdef __EMSCRIPTEN__
    if (!emuenv.net.inited)
        return RET_ERROR(SCE_NET_ERROR_ENOTINIT); // libnet 0x81003d64, errno untouched
    const auto sock = std::static_pointer_cast<OfflineSocket>(lock_and_find(sid, emuenv.net.socks, emuenv.kernel.mutex));
    if (!sock)
        RET_NET_ERRNO(SCE_NET_ERROR_EBADF);
    const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
    sock->icm_completed = true;
    sock->so_error = 0;
    offline_net_wake(emuenv.net); // epoll waits watching the socket rescan
    return 0;
#else
    // call sceNetSyscallIcmConnect(sid, flags)
    return UNIMPLEMENTED();
#endif
}
