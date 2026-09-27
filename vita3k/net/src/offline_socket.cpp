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

#include <net/offline_socket.h>

#include <kernel/thread/thread_state.h>

#include <algorithm>
#include <cstring>

// Error choices follow the BSD socket layer the Vita stack derives from
// (FreeBSD in_pcbconnect_setup / sosend_dgram / soreceive / soshutdown) with
// lo0 as the only interface; option storage follows desktop PosixSocket.

namespace {
constexpr uint32_t loopback_address = 0x7F000001; // host order
constexpr unsigned int max_datagram = 65507; // 65535 - IP and UDP headers
// Datagram socket buffer defaults: FreeBSD's udp_recvspace/udp_sendspace
// (40 * (1024 + sizeof(sockaddr_in)) and 9216); the firmware's own values
// were not recovered.
constexpr int default_udp_rcvbuf = 41600, default_udp_sndbuf = 9216;

int int_option(const OfflineSocket &socket, int level, int name, int fallback) {
    const auto option = socket.options.find({ level, name });
    if (option == socket.options.end())
        return fallback;
    int value = 0;
    std::memcpy(&value, option->second.data(), sizeof(value));
    return value;
}

uint32_t host_address(const SceNetSockaddrIn &addr) {
    return ntohl(addr.sin_addr.s_addr);
}

bool is_loopback(uint32_t host_addr) {
    return (host_addr >> 24) == 127;
}

int read_address(const SceNetSockaddr *addr, unsigned int addrlen, SceNetSockaddrIn &out) {
    if (!addr || addrlen < sizeof(SceNetSockaddrIn))
        return SCE_NET_ERROR_EINVAL;
    std::memcpy(&out, addr, sizeof(out));
    if (out.sin_family != SCE_NET_AF_INET)
        return SCE_NET_ERROR_EAFNOSUPPORT;
    return 0;
}

// Route a destination: INADDR_ANY means the first interface address, which
// offline is 127.0.0.1; 127/8 is lo0; anything else (broadcast included,
// lo0 has no IFF_BROADCAST) has no route.
int route(SceNetSockaddrIn &dst) {
    if (dst.sin_port == 0)
        return SCE_NET_ERROR_EADDRNOTAVAIL;
    if (host_address(dst) == 0)
        dst.sin_addr.s_addr = htonl(loopback_address);
    if (!is_loopback(host_address(dst)))
        return SCE_NET_ERROR_ENETUNREACH;
    return 0;
}

void write_address(const SceNetSockaddrIn &value, SceNetSockaddr *addr, unsigned int *addrlen) {
    if (!addr || !addrlen)
        return;
    std::memcpy(addr, &value, std::min<unsigned int>(*addrlen, sizeof(value)));
    *addrlen = sizeof(value);
}

OfflineSocket &offline(const SocketPtr &socket) {
    return static_cast<OfflineSocket &>(*socket); // the only socket type of this stack
}

struct OptionSpec {
    int level, name;
    unsigned int size;
    bool settable, gettable;
};

constexpr OptionSpec option_specs[] = {
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, 4, true, true },
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_KEEPALIVE, 4, true, true },
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST, 4, true, true },
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_LINGER, 8, true, true },
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_OOBINLINE, 4, true, true },
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEPORT, 4, true, true },
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_ONESBCAST, 4, true, true },
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_USECRYPTO, 4, true, true },
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_USESIGNATURE, 4, true, true },
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDBUF, 4, true, true },
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, 4, true, true },
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDLOWAT, 4, true, true },
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVLOWAT, 4, true, true },
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, 4, true, true },
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, 4, true, true },
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_ERROR, 4, false, true },
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_TYPE, 4, false, true },
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, 4, true, true },
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_TPPOLICY, 4, true, true },
    { SCE_NET_SOL_SOCKET, SCE_NET_SO_NAME, 1, false, true },
    { SCE_NET_IPPROTO_IP, SCE_NET_IP_HDRINCL, 4, true, true },
    { SCE_NET_IPPROTO_IP, SCE_NET_IP_TOS, 4, true, true },
    { SCE_NET_IPPROTO_IP, SCE_NET_IP_TTL, 4, true, true },
    { SCE_NET_IPPROTO_IP, SCE_NET_IP_MULTICAST_IF, 4, true, true },
    { SCE_NET_IPPROTO_IP, SCE_NET_IP_MULTICAST_TTL, 4, true, true },
    { SCE_NET_IPPROTO_IP, SCE_NET_IP_MULTICAST_LOOP, 4, true, true },
    { SCE_NET_IPPROTO_IP, SCE_NET_IP_ADD_MEMBERSHIP, 8, true, false },
    { SCE_NET_IPPROTO_IP, SCE_NET_IP_DROP_MEMBERSHIP, 8, true, false },
    { SCE_NET_IPPROTO_IP, SCE_NET_IP_TTLCHK, 4, true, true },
    { SCE_NET_IPPROTO_IP, SCE_NET_IP_MAXTTL, 4, true, true },
    { SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, 4, true, true },
    { SCE_NET_IPPROTO_TCP, SCE_NET_TCP_MAXSEG, 4, true, true },
    { SCE_NET_IPPROTO_TCP, SCE_NET_TCP_MSS_TO_ADVERTISE, 4, true, true },
};

const OptionSpec *find_option(int level, int name) {
    for (const auto &spec : option_specs)
        if (spec.level == level && spec.name == name)
            return &spec;
    return nullptr;
}
} // namespace

bool OfflineSocket::nonblocking(int flags) const {
    if (flags & SCE_NET_MSG_DONTWAIT)
        return true;
    const auto nbio = options.find({ SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO });
    int value = 0;
    if (nbio != options.end())
        std::memcpy(&value, nbio->second.data(), sizeof(value));
    return value != 0;
}

std::optional<uint32_t> OfflineSocket::receive_timeout() const {
    const auto timeout = options.find({ SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO });
    int value = 0;
    if (timeout != options.end())
        std::memcpy(&value, timeout->second.data(), sizeof(value));
    return value > 0 ? std::optional<uint32_t>(value) : std::nullopt;
}

unsigned int OfflineSocket::poll_events() const {
    unsigned int events = so_error ? SCE_NET_EPOLLERR : 0;
    if (!stream()) {
        if (!queue.empty() || shut_rd)
            events |= SCE_NET_EPOLLIN;
        if (!shut_wr)
            events |= SCE_NET_EPOLLOUT;
    }
    return events;
}

int OfflineSocket::abort(int flags) {
    ++abort_generation;
    preserved_abort |= flags;
    offline_net_wake(net);
    return 0;
}

int OfflineSocket::close() {
    queue.clear();
    queued_bytes = 0;
    offline_net_wake(net);
    return 0;
}

int OfflineSocket::shutdown_socket(int how) {
    enum { shut_read, shut_write, shut_both }; // SCE_NET_SHUT_RD, _WR, _RDWR
    if (how < shut_read || how > shut_both)
        return SCE_NET_ERROR_EINVAL;
    if (!connected)
        return SCE_NET_ERROR_ENOTCONN;
    shut_rd = shut_rd || how != shut_write;
    shut_wr = shut_wr || how != shut_read;
    offline_net_wake(net);
    return 0;
}

int OfflineSocket::bind(const SceNetSockaddr *addr, unsigned int addrlen) {
    SceNetSockaddrIn wanted;
    if (const int error = read_address(addr, addrlen, wanted))
        return error;
    if (bound)
        return SCE_NET_ERROR_EINVAL;
    const uint32_t address = host_address(wanted);
    if (address != 0 && !is_loopback(address))
        return SCE_NET_ERROR_EADDRNOTAVAIL; // no interface holds it
    if (wanted.sin_port == 0) {
        const int error = bind_ephemeral();
        local.sin_addr = wanted.sin_addr;
        local.sin_vport = wanted.sin_vport;
        return error;
    }
    const auto reuses = [](const OfflineSocket &socket) {
        for (const int name : { SCE_NET_SO_REUSEADDR, SCE_NET_SO_REUSEPORT }) {
            const auto option = socket.options.find({ SCE_NET_SOL_SOCKET, name });
            int value = 0;
            if (option != socket.options.end())
                std::memcpy(&value, option->second.data(), sizeof(value));
            if (value)
                return true;
        }
        return false;
    };
    for (const auto &[id, socket] : net.socks) {
        const auto &other = offline(socket);
        if (&other == this || !other.bound || other.stream() != stream() || other.p2p() != p2p()
            || other.local.sin_port != wanted.sin_port || (p2p() && other.local.sin_vport != wanted.sin_vport))
            continue;
        const uint32_t other_address = host_address(other.local);
        if ((other_address == 0 || address == 0 || other_address == address) && !(reuses(other) && reuses(*this)))
            return SCE_NET_ERROR_EADDRINUSE;
    }
    local = wanted;
    local.sin_len = sizeof(local);
    bound = true;
    return 0;
}

int OfflineSocket::bind_ephemeral() {
    for (unsigned int tries = 0; tries < 16384; ++tries) {
        const uint16_t port = net.next_ephemeral_port;
        net.next_ephemeral_port = port == 65535 ? 49152 : port + 1;
        const bool used = std::any_of(net.socks.begin(), net.socks.end(), [&](const auto &entry) {
            const auto &other = offline(entry.second);
            return other.bound && other.stream() == stream() && ntohs(other.local.sin_port) == port;
        });
        if (used)
            continue;
        local = {};
        local.sin_len = sizeof(local);
        local.sin_family = SCE_NET_AF_INET;
        local.sin_port = htons(port);
        bound = true;
        return 0;
    }
    return SCE_NET_ERROR_EADDRNOTAVAIL;
}

int OfflineSocket::deliver(const void *msg, unsigned int len, const SceNetSockaddrIn &to) {
    if (!bound) {
        if (const int error = bind_ephemeral())
            return error;
    }
    OfflineDatagram datagram;
    datagram.from = local;
    if (host_address(local) == 0)
        datagram.from.sin_addr.s_addr = htonl(loopback_address);
    const auto *bytes = static_cast<const uint8_t *>(msg);
    datagram.data.assign(bytes, bytes + len);
    const auto same_peer = [&](const OfflineSocket &receiver) {
        return receiver.peer.sin_addr.s_addr == datagram.from.sin_addr.s_addr
            && receiver.peer.sin_port == datagram.from.sin_port
            && (!p2p() || receiver.peer.sin_vport == datagram.from.sin_vport);
    };
    for (const auto &[id, socket] : net.socks) {
        auto &receiver = offline(socket);
        const uint32_t address = host_address(receiver.local);
        if (receiver.stream() || receiver.p2p() != p2p() || !receiver.bound || receiver.shut_rd
            || receiver.local.sin_port != to.sin_port || (p2p() && receiver.local.sin_vport != to.sin_vport)
            || (address != 0 && address != host_address(to)) || (receiver.connected && !same_peer(receiver)))
            continue;
        // A full receive buffer drops the datagram, as UDP does.
        const size_t cost = datagram.data.size() + sizeof(SceNetSockaddrIn);
        const int limit = int_option(receiver, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF, default_udp_rcvbuf);
        if (receiver.queued_bytes + cost > static_cast<size_t>(std::max(limit, 0)))
            break;
        receiver.queued_bytes += cost;
        receiver.queue.push_back(std::move(datagram));
        offline_net_wake(net);
        break;
    }
    // A datagram nobody is bound to is dropped, as on any UDP stack.
    return static_cast<int>(len);
}

int OfflineSocket::send_packet(const void *msg, unsigned int len, int flags, const SceNetSockaddr *to, unsigned int tolen) {
    if (preserved_abort & SCE_NET_SOCKET_ABORT_FLAG_SND_PRESERVATION)
        return SCE_NET_ERROR_EINTR;
    if (shut_wr)
        return SCE_NET_ERROR_EPIPE;
    if (stream())
        return SCE_NET_ERROR_ENOTCONN;
    SceNetSockaddrIn dst;
    if (to) {
        if (connected)
            return SCE_NET_ERROR_EISCONN;
        if (const int error = read_address(to, tolen, dst))
            return error;
    } else {
        if (!connected)
            return SCE_NET_ERROR_EDESTADDRREQ;
        dst = peer;
    }
    if (len > max_datagram)
        return SCE_NET_ERROR_EMSGSIZE;
    if (len && !msg)
        return SCE_NET_ERROR_EFAULT;
    if (const int error = route(dst))
        return error;
    return deliver(msg, len, dst);
}

int OfflineSocket::recv_packet(void *buf, unsigned int len, int flags, SceNetSockaddr *from, unsigned int *fromlen) {
    if (preserved_abort & SCE_NET_SOCKET_ABORT_FLAG_RCV_PRESERVATION)
        return SCE_NET_ERROR_EINTR;
    if (stream())
        return SCE_NET_ERROR_ENOTCONN;
    if (queue.empty())
        return shut_rd ? 0 : SCE_NET_ERROR_EAGAIN;
    const auto &datagram = queue.front();
    const auto copied = static_cast<unsigned int>(std::min<size_t>(len, datagram.data.size()));
    if (copied && !buf)
        return SCE_NET_ERROR_EFAULT;
    std::memcpy(buf, datagram.data.data(), copied);
    write_address(datagram.from, from, fromlen);
    if (!(flags & SCE_NET_MSG_PEEK)) {
        queued_bytes -= datagram.data.size() + sizeof(SceNetSockaddrIn);
        queue.pop_front();
    }
    return static_cast<int>(copied);
}

int OfflineSocket::set_socket_options(int level, int optname, const void *optval, unsigned int optlen) {
    const auto *spec = find_option(level, optname);
    if (!spec || (spec->level == SCE_NET_IPPROTO_TCP && !stream()))
        return SCE_NET_ERROR_EINVAL;
    if (!spec->settable)
        return SCE_NET_ERROR_ENOPROTOOPT;
    if (!optval || optlen != spec->size)
        return SCE_NET_ERROR_EFAULT;
    // Group membership needs a multicast interface; lo0 is not one.
    if (optname == SCE_NET_IP_ADD_MEMBERSHIP || optname == SCE_NET_IP_DROP_MEMBERSHIP)
        return SCE_NET_ERROR_EADDRNOTAVAIL;
    const auto *bytes = static_cast<const uint8_t *>(optval);
    options[{ level, optname }].assign(bytes, bytes + optlen);
    if (level == SCE_NET_SOL_SOCKET && optname == SCE_NET_SO_ONESBCAST)
        std::memcpy(&sockopt_so_onesbcast, optval, sizeof(sockopt_so_onesbcast));
    return 0;
}

int OfflineSocket::get_socket_options(int level, int optname, void *optval, unsigned int *optlen) {
    const auto *spec = find_option(level, optname);
    if (!spec || !spec->gettable || (spec->level == SCE_NET_IPPROTO_TCP && !stream()))
        return SCE_NET_ERROR_EINVAL;
    if (!optval || !optlen)
        return SCE_NET_ERROR_EFAULT;
    if (*optlen < spec->size) {
        *optlen = spec->size;
        return SCE_NET_ERROR_EFAULT;
    }
    std::vector<uint8_t> value(spec->size, 0);
    if (level == SCE_NET_SOL_SOCKET && optname == SCE_NET_SO_ERROR) {
        std::memcpy(value.data(), &so_error, sizeof(so_error));
        so_error = 0;
    } else if (level == SCE_NET_SOL_SOCKET && optname == SCE_NET_SO_TYPE) {
        std::memcpy(value.data(), &sce_type, sizeof(sce_type));
    } else if (const auto stored = options.find({ level, optname }); stored != options.end()) {
        value = stored->second;
    } else if (level == SCE_NET_SOL_SOCKET && !stream() && (optname == SCE_NET_SO_RCVBUF || optname == SCE_NET_SO_SNDBUF)) {
        const int size = optname == SCE_NET_SO_RCVBUF ? default_udp_rcvbuf : default_udp_sndbuf;
        std::memcpy(value.data(), &size, sizeof(size));
    }
    std::memcpy(optval, value.data(), value.size());
    *optlen = spec->size;
    return 0;
}

int OfflineSocket::connect(const SceNetSockaddr *addr, unsigned int addrlen) {
    if (preserved_abort & SCE_NET_SOCKET_ABORT_FLAG_SND_PRESERVATION)
        return SCE_NET_ERROR_EINTR;
    SceNetSockaddrIn dst;
    if (const int error = read_address(addr, addrlen, dst))
        return error;
    if (const int error = route(dst))
        return error;
    if (stream())
        return SCE_NET_ERROR_ECONNREFUSED; // nothing listens on lo0 here
    if (!bound) {
        if (const int error = bind_ephemeral())
            return error;
    }
    peer = dst;
    connected = true;
    return 0;
}

// sceNetListen and sceNetAccept are not registered on this stack: there are
// no incoming connections to model while offline.
SocketPtr OfflineSocket::accept(SceNetSockaddr *, unsigned int *, int &err) {
    err = SCE_NET_ERROR_EOPNOTSUPP;
    return nullptr;
}

int OfflineSocket::listen(int) {
    return SCE_NET_ERROR_EOPNOTSUPP;
}

int OfflineSocket::get_peer_address(SceNetSockaddr *addr, unsigned int *addrlen) {
    if (!connected)
        return SCE_NET_ERROR_ENOTCONN;
    if (!addr || !addrlen)
        return SCE_NET_ERROR_EFAULT;
    write_address(peer, addr, addrlen);
    return 0;
}

int OfflineSocket::get_socket_address(SceNetSockaddr *addr, unsigned int *addrlen) {
    if (!addr || !addrlen)
        return SCE_NET_ERROR_EFAULT;
    SceNetSockaddrIn value = local;
    value.sin_len = sizeof(value);
    value.sin_family = SCE_NET_AF_INET;
    write_address(value, addr, addrlen);
    return 0;
}

KernelExecutionHost::WaitResult offline_net_park(KernelState &kernel, NetState &net, SceUID thread_id, std::optional<uint32_t> timeout_us) {
    const ThreadStatePtr thread = kernel.get_thread(thread_id);
    if (!kernel.execution_host || !thread)
        return KernelExecutionHost::WaitResult::cancelled;
    {
        const std::lock_guard<std::mutex> lock(thread->mutex);
        thread->update_status(ThreadStatus::wait);
    }
    net.parked_threads.push_back(thread);
    const auto result = kernel.execution_host->wait_sync(*thread, timeout_us);
    std::erase(net.parked_threads, thread);
    {
        const std::lock_guard<std::mutex> lock(thread->mutex);
        if (thread->status != ThreadStatus::run)
            thread->update_status(ThreadStatus::run);
    }
    return result;
}

void offline_net_wake(NetState &net) {
    for (const auto &thread : net.parked_threads) {
        const std::lock_guard<std::mutex> lock(thread->mutex);
        if (thread->status == ThreadStatus::wait)
            thread->update_status(ThreadStatus::run);
    }
}
