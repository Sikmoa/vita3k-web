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

#include <module/module.h>

#include <emuenv/state.h>
#include <kernel/state.h>
#include <net/types.h>

// SceNet's convention (libnet 0x81003b18): a failed call stores its errno in
// the thread's sce_net_errno TLS word and returns 0x80410100 | errno.
inline int ret_net_errno(EmuEnvState &emuenv, int thread_id, int ret) {
    if (ret < 0) {
        auto addr = emuenv.kernel.get_thread_tls_addr(emuenv.mem, thread_id, TLS_NET_ERRNO);
        if (addr) {
            auto inner_ptr = addr.get(emuenv.mem);
            if (inner_ptr)
                *reinterpret_cast<int *>(inner_ptr) = ret & 0xff;
        }
    }

    return ret;
}

#define RET_NET_ERRNO(ret)                                \
    do {                                                  \
        int _r = ret_net_errno(emuenv, thread_id, (ret)); \
        return (_r < 0 ? RET_ERROR(_r) : _r);             \
    } while (0)

DECL_EXPORT(int, sceNetBind, int sid, const SceNetSockaddr *addr, unsigned int addrlen);
DECL_EXPORT(int, sceNetInetPton, int af, const char *src, void *dst);
DECL_EXPORT(int, sceNetSetsockopt, int sid, SceNetProtocol level, SceNetSocketOption optname, const void *optval, unsigned int optlen);
DECL_EXPORT(int, sceNetShutdown, int sid, int how);
DECL_EXPORT(int, sceNetSocket, const char *name, int domain, SceNetSocketType type, SceNetProtocol protocol);
DECL_EXPORT(int, sceNetSocketAbort, int sid, int flags);
DECL_EXPORT(int, sceNetSocketClose, int sid);
