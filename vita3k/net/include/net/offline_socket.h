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

// The network stack of a Vita that is not connected: the BSD-derived stack
// (SceNetPs) with only lo0 (127.0.0.0/8) up, no default route and no DNS
// server. Used where the host gives no sockets (the browser Worker).
// Sockets exist and bind; datagrams to 127/8 are delivered to the socket
// bound there; every other destination has no route (ENETUNREACH); TCP has
// no listeners (loopback connect: ECONNREFUSED). Nothing here blocks: an
// operation that would wait returns SCE_NET_ERROR_EAGAIN and the caller
// parks the guest thread (offline_net_park) until offline_net_wake.

#include <kernel/state.h>
#include <net/state.h>

#include <deque>
#include <optional>
#include <string>
#include <vector>

struct OfflineDatagram {
    std::vector<uint8_t> data;
    SceNetSockaddrIn from;
};

struct OfflineSocket final : Socket {
    NetState &net;
    bool bound = false;
    SceNetSockaddrIn local{}; // network byte order, as the guest passes it
    bool connected = false; // datagram sockets only: TCP never connects
    SceNetSockaddrIn peer{};
    bool shut_rd = false, shut_wr = false;
    std::deque<OfflineDatagram> queue;
    size_t queued_bytes = 0; // payload + address per queued datagram, bounded by SO_RCVBUF
    int so_error = 0;
    // sceNetInternalIcmConnect found no usable interface: the next epoll
    // scan of the socket reports SCE_NET_EPOLL_ICM_DONE once.
    bool icm_completed = false;
    std::string name; // sceNetSocket's, at most 31 characters
    // A refused TCP connect drops the protocol control block: the socket
    // keeps no addresses (sceNetGetSockInfo).
    bool pcb_dropped = false;
    // Threads parked in a receive on it, and epoll waits parked while it was
    // not readable or writable (sceNetGetSockInfo wait flags).
    unsigned recv_waiters = 0, epoll_recv_waiters = 0, epoll_send_waiters = 0;
    // Abort: each sceNetSocketAbort ends the waits in progress; PRESERVATION
    // flags also fail every later receive/send with EINTR.
    unsigned abort_generation = 0;
    int preserved_abort = 0;
    std::map<std::pair<int, int>, std::vector<uint8_t>> options; // (level, name) -> value

    OfflineSocket(NetState &net, int type)
        : Socket(SCE_NET_AF_INET, type, 0)
        , net(net) {}

    bool stream() const { return sce_type == SCE_NET_SOCK_STREAM || sce_type == SCE_NET_SOCK_STREAM_P2P; }
    bool p2p() const { return sce_type == SCE_NET_SOCK_DGRAM_P2P || sce_type == SCE_NET_SOCK_STREAM_P2P; }
    bool nonblocking(int flags) const;
    // SO_RCVTIMEO in microseconds; nullopt waits without limit.
    std::optional<uint32_t> receive_timeout() const;
    // SCE_NET_EPOLLIN/OUT/ERR the socket is ready for.
    unsigned int poll_events() const;

    int abort(int flags) override;
    int close() override;
    int shutdown_socket(int how) override;
    int bind(const SceNetSockaddr *addr, unsigned int addrlen) override;
    int send_packet(const void *msg, unsigned int len, int flags, const SceNetSockaddr *to, unsigned int tolen) override;
    int recv_packet(void *buf, unsigned int len, int flags, SceNetSockaddr *from, unsigned int *fromlen) override;
    int set_socket_options(int level, int optname, const void *optval, unsigned int optlen) override;
    int get_socket_options(int level, int optname, void *optval, unsigned int *optlen) override;
    int connect(const SceNetSockaddr *addr, unsigned int addrlen) override;
    SocketPtr accept(SceNetSockaddr *addr, unsigned int *addrlen, int &err) override;
    int listen(int backlog) override;
    int get_peer_address(SceNetSockaddr *addr, unsigned int *addrlen) override;
    int get_socket_address(SceNetSockaddr *addr, unsigned int *addrlen) override;

private:
    int bind_ephemeral();
    int deliver(const void *msg, unsigned int len, const SceNetSockaddrIn &to);
};

// Parks the calling guest thread on the execution host until the next
// offline_net_wake, cancellation or `timeout_us`. Called without locks held.
KernelExecutionHost::WaitResult offline_net_park(KernelState &kernel, NetState &net, SceUID thread_id, std::optional<uint32_t> timeout_us);
// Resumes every parked thread so it re-checks its socket, epoll or abort.
void offline_net_wake(NetState &net);
