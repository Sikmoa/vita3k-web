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

#include "SceNet.h"

#include <kernel/callback.h>
#include <kernel/state.h>

#include <net/state.h>

#include <util/lock_and_find.h>
#include <util/net_utils.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <thread>

#ifdef __EMSCRIPTEN__
#include <net/offline_socket.h>
#endif

#ifdef __APPLE__
#include "macos_net_helper.h"
#include <net/if.h>
#endif

#include <util/tracy.h>
TRACY_MODULE_NAME(SceNet);

template <>
std::string to_debug_str<SceNetEpollControlFlag>(const MemState &mem, SceNetEpollControlFlag type) {
    switch (type) {
    case SCE_NET_EPOLL_CTL_ADD:
        return "SCE_NET_EPOLL_CTL_ADD";
    case SCE_NET_EPOLL_CTL_MOD:
        return "SCE_NET_EPOLL_CTL_MOD";
    case SCE_NET_EPOLL_CTL_DEL:
        return "SCE_NET_EPOLL_CTL_DEL";
    }
    return std::to_string(type);
}

template <>
std::string to_debug_str<SceNetProtocol>(const MemState &mem, SceNetProtocol type) {
    switch (type) {
    case SCE_NET_IPPROTO_IP: return "SCE_NET_IPPROTO_IP";
    case SCE_NET_IPPROTO_ICMP: return "SCE_NET_IPPROTO_ICMP";
    case SCE_NET_IPPROTO_IGMP: return "SCE_NET_IPPROTO_IGMP";
    case SCE_NET_IPPROTO_TCP: return "SCE_NET_IPPROTO_TCP";
    case SCE_NET_IPPROTO_UDP: return "SCE_NET_IPPROTO_UDP";
    case SCE_NET_SOL_SOCKET: return "SCE_NET_SOL_SOCKET";
    }
    return std::to_string(type);
}

template <>
std::string to_debug_str<SceNetSocketType>(const MemState &mem, SceNetSocketType type) {
    switch (type) {
    case SCE_NET_SOCK_STREAM: return "SCE_NET_SOCK_STREAM";
    case SCE_NET_SOCK_DGRAM: return "SCE_NET_SOCK_DGRAM";
    case SCE_NET_SOCK_RAW: return "SCE_NET_SOCK_RAW";
    case SCE_NET_SOCK_DGRAM_P2P: return "SCE_NET_SOCK_DGRAM_P2P";
    case SCE_NET_SOCK_STREAM_P2P: return "SCE_NET_SOCK_STREAM_P2P";
    }
    return std::to_string(type);
}

template <>
std::string to_debug_str<SceNetSocketOption>(const MemState &mem, SceNetSocketOption type) {
    switch (type) {
    /* IP */
    case SCE_NET_IP_HDRINCL: return "SCE_NET_IP_HDRINCL or SCE_NET_TCP_MAXSEG";
    case SCE_NET_IP_TOS: return "SCE_NET_IP_TOS or SCE_NET_TCP_MSS_TO_ADVERTISE";
    case SCE_NET_IP_TTL: return "SCE_NET_IP_TTL or SCE_NET_SO_REUSEADDR";
    case SCE_NET_IP_MULTICAST_IF: return "SCE_NET_IP_MULTICAST_IF";
    case SCE_NET_IP_MULTICAST_TTL: return "SCE_NET_IP_MULTICAST_TTL";
    case SCE_NET_IP_MULTICAST_LOOP: return "SCE_NET_IP_MULTICAST_LOOP";
    case SCE_NET_IP_ADD_MEMBERSHIP: return "SCE_NET_IP_ADD_MEMBERSHIP";
    case SCE_NET_IP_DROP_MEMBERSHIP: return "SCE_NET_IP_DROP_MEMBERSHIP";
    case SCE_NET_IP_TTLCHK: return "SCE_NET_IP_TTLCHK";
    case SCE_NET_IP_MAXTTL: return "SCE_NET_IP_MAXTTL";
    /* TCP */
    case SCE_NET_TCP_NODELAY: return "SCE_NET_TCP_NODELAY";
    // case SCE_NET_TCP_MAXSEG: return "SCE_NET_TCP_MAXSEG";
    // case SCE_NET_TCP_MSS_TO_ADVERTISE: return "SCE_NET_TCP_MSS_TO_ADVERTISE";
    /* SOCKET */
    // case SCE_NET_SO_REUSEADDR: return "SCE_NET_SO_REUSEADDR";
    case SCE_NET_SO_KEEPALIVE: return "SCE_NET_SO_KEEPALIVE";
    case SCE_NET_SO_BROADCAST: return "SCE_NET_SO_BROADCAST";
    case SCE_NET_SO_LINGER: return "SCE_NET_SO_LINGER";
    case SCE_NET_SO_OOBINLINE: return "SCE_NET_SO_OOBINLINE";
    case SCE_NET_SO_REUSEPORT: return "SCE_NET_SO_REUSEPORT";
    case SCE_NET_SO_ONESBCAST: return "SCE_NET_SO_ONESBCAST";
    case SCE_NET_SO_USECRYPTO: return "SCE_NET_SO_USECRYPTO";
    case SCE_NET_SO_USESIGNATURE: return "SCE_NET_SO_USESIGNATURE";
    case SCE_NET_SO_SNDBUF: return "SCE_NET_SO_SNDBUF";
    case SCE_NET_SO_RCVBUF: return "SCE_NET_SO_RCVBUF";
    case SCE_NET_SO_SNDLOWAT: return "SCE_NET_SO_SNDLOWAT";
    case SCE_NET_SO_RCVLOWAT: return "SCE_NET_SO_RCVLOWAT";
    case SCE_NET_SO_SNDTIMEO: return "SCE_NET_SO_SNDTIMEO";
    case SCE_NET_SO_RCVTIMEO: return "SCE_NET_SO_RCVTIMEO";
    case SCE_NET_SO_ERROR: return "SCE_NET_SO_ERROR";
    case SCE_NET_SO_TYPE: return "SCE_NET_SO_TYPE";
    case SCE_NET_SO_NBIO: return "SCE_NET_SO_NBIO";
    case SCE_NET_SO_TPPOLICY: return "SCE_NET_SO_TPPOLICY";
    case SCE_NET_SO_NAME: return "SCE_NET_SO_NAME";
    }
    return std::to_string(type);
}

#ifdef __EMSCRIPTEN__
// The browser has no host sockets: every socket is an OfflineSocket
// (net/offline_socket.h), and the calls that wait park the guest thread.
static std::shared_ptr<OfflineSocket> find_offline_socket(EmuEnvState &emuenv, int sid) {
    return std::static_pointer_cast<OfflineSocket>(lock_and_find(sid, emuenv.net.socks, emuenv.kernel.mutex));
}

static uint32_t elapsed_us(std::chrono::steady_clock::time_point start) {
    return static_cast<uint32_t>(std::min<int64_t>(UINT32_MAX,
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()));
}

// Blocking receive: waits for a datagram, the SO_RCVTIMEO timeout (EAGAIN, as
// BSD reports it) or sceNetSocketAbort (EINTR).
static int recv_offline(EmuEnvState &emuenv, SceUID thread_id, int sid, void *buf, unsigned int len, int flags, SceNetSockaddr *from, unsigned int *fromlen) {
    const auto start = std::chrono::steady_clock::now();
    const auto first = find_offline_socket(emuenv, sid);
    if (!first)
        return SCE_NET_ERROR_EBADF;
    const unsigned int generation = first->abort_generation;
    for (;;) {
        const auto sock = find_offline_socket(emuenv, sid);
        if (!sock)
            return SCE_NET_ERROR_EBADF; // closed while waiting
        if (sock->abort_generation != generation)
            return SCE_NET_ERROR_EINTR;
        const int result = sock->recv_packet(buf, len, flags, from, fromlen);
        if (result != static_cast<int>(SCE_NET_ERROR_EAGAIN) || sock->nonblocking(flags))
            return result;
        std::optional<uint32_t> remaining;
        if (const auto timeout = sock->receive_timeout()) {
            const uint32_t elapsed = elapsed_us(start);
            if (elapsed >= *timeout)
                return SCE_NET_ERROR_EAGAIN;
            remaining = *timeout - elapsed;
        }
        ++sock->recv_waiters;
        const auto parked = offline_net_park(emuenv.kernel, emuenv.net, thread_id, remaining);
        --sock->recv_waiters;
        if (parked == KernelExecutionHost::WaitResult::cancelled)
            return SCE_NET_ERROR_EINTR;
    }
}

// Epoll readiness of a socket, or of a resolver whose lookup has finished.
// Evaluating a socket consumes its sceNetInternalIcmConnect completion
// (SceNetPs 0x81009774 clears it at 0x810097d8), which only a socket entry
// that registered SCE_NET_EPOLL_ICM_DONE receives (0x81009ee2).
static unsigned int offline_ready_events(EmuEnvState &emuenv, int id, const EpollSocket &entry) {
    if (const auto resolver = emuenv.net.resolvers.find(id); resolver != emuenv.net.resolvers.end())
        return resolver->second ? SCE_NET_EPOLLIN : 0;
    const auto sock = entry.sock.lock();
    if (!sock)
        return 0;
    auto &offline = static_cast<OfflineSocket &>(*sock);
    unsigned int events = offline.poll_events();
    if (offline.icm_completed) {
        offline.icm_completed = false;
        events |= SCE_NET_EPOLL_ICM_DONE;
    }
    return events;
}

// A wait with callbacks (sceNetEpollWaitCB: SceNetPs waits with
// ksceKernelWaitEventFlagCB, 0x8102a4c8) runs the thread's notified
// callbacks whenever it would wait, then waits on.
static int epoll_wait_offline(EmuEnvState &emuenv, SceUID thread_id, int eid, SceNetEpollEvent *events, int maxevents, int timeout_us, bool callbacks) {
    if (!events || maxevents <= 0)
        return SCE_NET_ERROR_EINVAL;
    const auto start = std::chrono::steady_clock::now();
    const auto first = lock_and_find(eid, emuenv.net.epolls, emuenv.kernel.mutex);
    if (!first)
        return SCE_NET_ERROR_EBADF;
    const unsigned int generation = first->abort_generation;
    for (;;) {
        const auto epoll = lock_and_find(eid, emuenv.net.epolls, emuenv.kernel.mutex);
        if (!epoll)
            return SCE_NET_ERROR_EBADF; // destroyed while waiting
        if (epoll->abort_preserved || epoll->abort_generation != generation)
            return SCE_NET_ERROR_EINTR;
        int count = 0;
        for (const auto &[id, entry] : epoll->eventEntries) {
            if (count == maxevents)
                break; // later entries are not evaluated
            const unsigned int ready = offline_ready_events(emuenv, id, entry) & (entry.events | SCE_NET_EPOLLERR);
            if (!ready)
                continue;
            events[count] = {};
            events[count].events = ready;
            events[count].data = entry.data;
            ++count;
        }
        if (count || timeout_us == 0)
            return count;
        std::optional<uint32_t> remaining; // a negative timeout waits without limit
        if (timeout_us > 0) {
            const uint32_t elapsed = elapsed_us(start);
            if (elapsed >= static_cast<uint32_t>(timeout_us))
                return 0;
            remaining = static_cast<uint32_t>(timeout_us) - elapsed;
        }
        // A callback may have made an entry ready: scan again before waiting.
        if (callbacks && process_callbacks(emuenv.kernel, thread_id) > 0)
            continue;
        // The scan marks the directions it waits for (SceNetPs 0x81009822,
        // 0x81009846); leaving the wait clears them.
        std::vector<std::pair<std::shared_ptr<OfflineSocket>, unsigned int>> waiting;
        for (const auto &[id, entry] : epoll->eventEntries) {
            if (const auto sock = std::static_pointer_cast<OfflineSocket>(entry.sock.lock()); sock && !sock->so_error) {
                const unsigned int directions = entry.events & (SCE_NET_EPOLLIN | SCE_NET_EPOLLOUT);
                waiting.emplace_back(sock, directions);
                sock->epoll_recv_waiters += (directions & SCE_NET_EPOLLIN) != 0;
                sock->epoll_send_waiters += (directions & SCE_NET_EPOLLOUT) != 0;
            }
        }
        const auto parked = offline_net_park(emuenv.kernel, emuenv.net, thread_id, remaining);
        for (const auto &[sock, directions] : waiting) {
            sock->epoll_recv_waiters -= (directions & SCE_NET_EPOLLIN) != 0;
            sock->epoll_send_waiters -= (directions & SCE_NET_EPOLLOUT) != 0;
        }
        if (parked == KernelExecutionHost::WaitResult::cancelled)
            return SCE_NET_ERROR_EINTR;
    }
}
#endif

EXPORT(int, sceNetAccept, int sid, SceNetSockaddr *addr, unsigned int *addrlen) {
    TRACY_FUNC(sceNetAccept, sid, addr, addrlen);
    auto sock = lock_and_find(sid, emuenv.net.socks, emuenv.kernel.mutex);
    if (!sock)
        RET_NET_ERRNO(SCE_NET_ERROR_EBADF);

    int err = 0;
    auto newsock = sock->accept(addr, addrlen, err);
    if (!newsock)
        RET_NET_ERRNO(err);

    auto id = ++emuenv.net.next_id;
    emuenv.net.socks.emplace(id, newsock);
    return id;
}

EXPORT(int, sceNetBind, int sid, const SceNetSockaddr *addr, unsigned int addrlen) {
    TRACY_FUNC(sceNetBind, sid, addr, addrlen);
    auto sock = lock_and_find(sid, emuenv.net.socks, emuenv.kernel.mutex);

    RET_NET_ERRNO(sock ? sock->bind(addr, addrlen) : SCE_NET_ERROR_EBADF);
}

EXPORT(int, sceNetClearDnsCache) {
    TRACY_FUNC(sceNetClearDnsCache);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetConnect, int sid, const SceNetSockaddr *addr, unsigned int addrlen) {
    TRACY_FUNC(sceNetConnect, sid, addr, addrlen);
    auto sock = lock_and_find(sid, emuenv.net.socks, emuenv.kernel.mutex);

    RET_NET_ERRNO(sock ? sock->connect(addr, addrlen) : SCE_NET_ERROR_EBADF);
}

EXPORT(int, sceNetDumpAbort) {
    TRACY_FUNC(sceNetDumpAbort);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetDumpCreate) {
    TRACY_FUNC(sceNetDumpCreate);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetDumpDestroy) {
    TRACY_FUNC(sceNetDumpDestroy);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetDumpRead) {
    TRACY_FUNC(sceNetDumpRead);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetEmulationGet) {
    TRACY_FUNC(sceNetEmulationGet);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetEmulationSet) {
    TRACY_FUNC(sceNetEmulationSet);
    return UNIMPLEMENTED();
}

#ifdef __EMSCRIPTEN__
EXPORT(int, sceNetEpollAbort, int eid, int flags) {
    TRACY_FUNC(sceNetEpollAbort, eid, flags);
    constexpr int preservation = 1; // SCE_NET_EPOLL_ABORT_FLAG_PRESERVATION
    if (flags & ~preservation)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    const auto epoll = lock_and_find(eid, emuenv.net.epolls, emuenv.kernel.mutex);
    if (!epoll)
        RET_NET_ERRNO(SCE_NET_ERROR_EBADF);
    ++epoll->abort_generation;
    epoll->abort_preserved = epoll->abort_preserved || (flags & preservation);
    offline_net_wake(emuenv.net);
    return 0;
}
#else
EXPORT(int, sceNetEpollAbort) {
    TRACY_FUNC(sceNetEpollAbort);
    return UNIMPLEMENTED();
}
#endif

EXPORT(int, sceNetEpollControl, int eid, SceNetEpollControlFlag op, int id, SceNetEpollEvent *ev) {
    TRACY_FUNC(sceNetEpollControl, eid, op, id, ev);
    auto epoll = lock_and_find(eid, emuenv.net.epolls, emuenv.kernel.mutex);
    if (!epoll)
        RET_NET_ERRNO(SCE_NET_ERROR_EBADF);

#ifdef __EMSCRIPTEN__
    if ((op == SCE_NET_EPOLL_CTL_ADD || op == SCE_NET_EPOLL_CTL_MOD) && !ev)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    if (op == SCE_NET_EPOLL_CTL_ADD && emuenv.net.resolvers.contains(id)) {
        const int result = epoll->add(id, {}, ev);
        if (result >= 0)
            offline_net_wake(emuenv.net);
        RET_NET_ERRNO(result);
    }
#else
    if (id == emuenv.net.resolver_id) {
        STUBBED("Async DNS resolve is not supported");
        return 0;
    }
#endif

#ifdef __EMSCRIPTEN__
    // A new or changed subscription can make a parked sceNetEpollWait ready.
    int result = SCE_NET_ERROR_EINVAL;
    switch (op) {
    case SCE_NET_EPOLL_CTL_ADD: {
        const auto sock = lock_and_find(id, emuenv.net.socks, emuenv.kernel.mutex);
        result = sock ? epoll->add(id, sock, ev) : SCE_NET_ERROR_EBADF;
        break;
    }
    case SCE_NET_EPOLL_CTL_DEL: result = epoll->del(id); break;
    case SCE_NET_EPOLL_CTL_MOD: result = epoll->mod(id, ev); break;
    }
    if (result >= 0)
        offline_net_wake(emuenv.net);
    RET_NET_ERRNO(result);
#endif
    switch (op) {
    case SCE_NET_EPOLL_CTL_ADD: {
        const auto sock = lock_and_find(id, emuenv.net.socks, emuenv.kernel.mutex);
        RET_NET_ERRNO(sock ? epoll->add(id, sock, ev) : SCE_NET_ERROR_EBADF);
    }
    case SCE_NET_EPOLL_CTL_DEL:
        RET_NET_ERRNO(epoll->del(id));
    case SCE_NET_EPOLL_CTL_MOD:
        RET_NET_ERRNO(epoll->mod(id, ev));
    default:
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    }
}

EXPORT(int, sceNetEpollCreate, const char *name, int flags) {
    TRACY_FUNC(sceNetEpollCreate, name, flags);
    auto id = ++emuenv.net.next_epoll_id;
    auto epoll = std::make_shared<Epoll>();
    if (name)
        epoll->name.assign(name, strnlen(name, 31));
    const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
    emuenv.net.epolls.emplace(id, epoll);
    return id;
}

EXPORT(int, sceNetEpollDestroy, int eid) {
    TRACY_FUNC(sceNetEpollDestroy, eid);
    const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
#ifdef __EMSCRIPTEN__
    offline_net_wake(emuenv.net); // waiters see the epoll gone
#endif

    RET_NET_ERRNO(emuenv.net.epolls.erase(eid) ? 0 : SCE_NET_ERROR_EBADF);
}

EXPORT(int, sceNetEpollWait, int eid, SceNetEpollEvent *events, int maxevents, int timeout) {
    TRACY_FUNC(sceNetEpollWait, eid, events, maxevents, timeout);
#ifdef __EMSCRIPTEN__
    if (!emuenv.net.inited)
        return RET_ERROR(SCE_NET_ERROR_ENOTINIT); // libnet 0x81004324, errno untouched
    RET_NET_ERRNO(epoll_wait_offline(emuenv, thread_id, eid, events, maxevents, timeout, false));
#else
    auto epoll = lock_and_find(eid, emuenv.net.epolls, emuenv.kernel.mutex);

    RET_NET_ERRNO(epoll ? epoll->wait(events, maxevents, timeout) : SCE_NET_ERROR_EBADF);
#endif
}

EXPORT(int, sceNetEpollWaitCB, int eid, SceNetEpollEvent *events, int maxevents, int timeout) {
    TRACY_FUNC(sceNetEpollWaitCB, eid, events, maxevents, timeout);
#ifdef __EMSCRIPTEN__
    if (!emuenv.net.inited)
        return RET_ERROR(SCE_NET_ERROR_ENOTINIT); // libnet 0x8100438c, errno untouched
    RET_NET_ERRNO(epoll_wait_offline(emuenv, thread_id, eid, events, maxevents, timeout, true));
#else
    return UNIMPLEMENTED();
#endif
}

EXPORT(Ptr<int>, sceNetErrnoLoc) {
    TRACY_FUNC(sceNetErrnoLoc);
    // TLS id was taken from disasm source
    auto addr = emuenv.kernel.get_thread_tls_addr(emuenv.mem, thread_id, TLS_NET_ERRNO);
    return addr.cast<int>();
}

EXPORT(int, sceNetEtherNtostr, SceNetEtherAddr *n, char *str, unsigned int len) {
    TRACY_FUNC(sceNetEtherNtostr, n, str, len);
    if (!emuenv.net.inited)
        RET_NET_ERRNO(SCE_NET_ERROR_ENOTINIT);

    if (!n || !str || len <= 0x11)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);

    snprintf(str, len, "%02x:%02x:%02x:%02x:%02x:%02x",
        n->data[0], n->data[1], n->data[2], n->data[3], n->data[4], n->data[5]);
    return 0;
}

EXPORT(int, sceNetEtherStrton, const char *str, SceNetEtherAddr *n) {
    TRACY_FUNC(sceNetEtherStrton, str, n);
    if (!emuenv.net.inited)
        RET_NET_ERRNO(SCE_NET_ERROR_ENOTINIT);

    if (!str || !n)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);

    sscanf(str, "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
        &n->data[0], &n->data[1], &n->data[2], &n->data[3], &n->data[4], &n->data[5]);

    return 0;
}

EXPORT(int, sceNetGetMacAddress, SceNetEtherAddr *addr, int flags) {
    TRACY_FUNC(sceNetGetMacAddress, addr, flags);
    if (addr == nullptr) {
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    }
#ifdef _WIN32
    IP_ADAPTER_INFO AdapterInfo[16];
    DWORD dwBufLen = sizeof(AdapterInfo);
    if (GetAdaptersInfo(AdapterInfo, &dwBufLen) != ERROR_SUCCESS)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    else
        memcpy(addr->data, AdapterInfo[0].Address, 6);
#elif defined(__unix__)
    struct ifreq ifr;
    struct ifconf ifc;
    bool success = false;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock == -1) {
        LOG_ERROR("Failed to open socket");
        assert(false);
        return RET_ERROR(SCE_NET_ERROR_ENOTSOCK);
    };

    char buf[1024];
    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;
    if (ioctl(sock, SIOCGIFCONF, &ifc) == -1) {
        LOG_ERROR("Failed to fetch infconf from socket {}", sock);
        assert(false);
        return RET_ERROR(SCE_NET_ERROR_EINTERNAL);
    }

    struct ifreq *it = ifc.ifc_req;
    const struct ifreq *const end = it + (ifc.ifc_len / sizeof(struct ifreq));

    // TODO: If multiple adapters, which one to choose?
    // Only getting the first one that isn't loopback
    // Meaning if you use WIFI it will probably get the ethernet addr instead
    for (; it != end; ++it) {
        strcpy(ifr.ifr_name, it->ifr_name);
        if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
            if (!(ifr.ifr_flags & IFF_LOOPBACK)) { // don't count loopback
                if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
                    success = true;
                    break;
                }
            }
        } else {
            LOG_ERROR("Failed to fetch flags from socket {}, name={}", sock, ifr.ifr_name);
            assert(false);
            return RET_ERROR(SCE_NET_ERROR_EINTERNAL);
        }
    }

    if (success)
        memcpy(addr->data, ifr.ifr_hwaddr.sa_data, 6);
    else {
        // If there are no adapters connected (why?), use a predefiend one

        // MAC addresses consists of 6 octets, the first half is the organization while the other half
        // is the NIC (Network Interface Controller)
        uint8_t magicMac[6] = {
            // Organization
            0xEE,
            0xEE, // EE as in ExtremeExploit (why not?)
            0xEE,
            // NIC
            0xBA,
            0xDA, // Badass (sounds cool ig)
            0x55,
        };
        memcpy(addr->data, magicMac, 6);
    }
#elif defined(__APPLE__)
    char hint[IFNAMSIZ] = {};
    get_primary_interface_name(hint, sizeof(hint));

    if (!get_mac_address(hint, addr->data)) {
        uint8_t magicMac[6] = {
            0x02, // LAA
            0x41, // 'A'
            0x50, // 'P'
            0x50, // 'P'
            0x4C, // 'L'
            0x45, // 'E'
        };
        memcpy(addr->data, magicMac, 6);
    }
#else
    return UNIMPLEMENTED();
#endif
    return 0;
}

EXPORT(int, sceNetGetSockIdInfo) {
    TRACY_FUNC(sceNetGetSockIdInfo);
    return UNIMPLEMENTED();
}

#ifdef __EMSCRIPTEN__
// SceNetPs 0x8100a1dc: one entry of a socket, all of which belong to the
// game (SELF). Addresses and ports stay in network byte order.
static SceNetSockInfo offline_sock_info(int id, const OfflineSocket &sock, int flags) {
    SceNetSockInfo info{};
    if (flags & 2)
        std::memcpy(info.name, sock.name.data(), sock.name.size());
    info.pid = KernelState::process_id;
    info.s = id;
    info.socket_type = static_cast<uint8_t>(sock.sce_type);
    info.recv_queue_length = static_cast<int>(sock.queued_bytes); // payload and sender address per datagram
    info.flags = SCE_NET_SOCKINFO_F_SELF | (sock.recv_waiters ? SCE_NET_SOCKINFO_F_RECV_WAIT : 0)
        | (sock.epoll_recv_waiters ? SCE_NET_SOCKINFO_F_RECV_EWAIT : 0)
        | (sock.epoll_send_waiters ? SCE_NET_SOCKINFO_F_SEND_EWAIT : 0);
    // UDP is always "opened"; TCP never leaves CLOSED offline, and a refused
    // connect leaves it without a control block, reported as opened.
    info.state = sock.stream() && !sock.pcb_dropped ? SCE_NET_SOCKINFO_STATE_CLOSED : SCE_NET_SOCKINFO_STATE_OPENED;
    if (sock.pcb_dropped)
        return info;
    if (sock.bound) {
        info.local_adr = sock.local.sin_addr;
        info.local_port = sock.local.sin_port;
    }
    if (sock.connected) {
        info.remote_adr = sock.peer.sin_addr;
        info.remote_port = sock.peer.sin_port;
    }
    if (sock.p2p()) {
        info.local_vport = sock.bound ? sock.local.sin_vport : 0;
        info.remote_vport = sock.connected ? sock.peer.sin_vport : 0;
    }
    return info;
}
#endif

// SceNetPs 0x8100a510: one socket, or all of the process's: TCP sockets
// that lost their control block in id order, then TCP, UDP and P2P
// datagram sockets, each newest first, then (flag 0x20) the epolls. Flag 2
// adds names; flag 1 widens the list for system programs only. A null info
// counts the entries; otherwise at most n are written.
EXPORT(int, sceNetGetSockInfo, int s, SceNetSockInfo *info, int n, int flags) {
    TRACY_FUNC(sceNetGetSockInfo, s, info, n, flags);
#ifdef __EMSCRIPTEN__
    if (!emuenv.net.inited)
        return RET_ERROR(SCE_NET_ERROR_ENOTINIT); // libnet 0x81003b18, errno untouched
    if ((flags & ~0x23) || n < 0)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    std::vector<SceNetSockInfo> entries;
    const auto offline = [](const SocketPtr &sock) -> const OfflineSocket & { return static_cast<const OfflineSocket &>(*sock); };
    if (s >= 0) {
        const auto sock = lock_and_find(s, emuenv.net.socks, emuenv.kernel.mutex);
        if (!sock)
            RET_NET_ERRNO(SCE_NET_ERROR_EBADF);
        entries.push_back(offline_sock_info(s, offline(sock), flags));
    } else {
        const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        for (const auto &[id, sock] : emuenv.net.socks) {
            if (offline(sock).pcb_dropped)
                entries.push_back(offline_sock_info(id, offline(sock), flags));
        }
        // Ids grow with creation: newest first is descending id order.
        const auto list = [&](auto belongs) {
            for (auto it = emuenv.net.socks.rbegin(); it != emuenv.net.socks.rend(); ++it) {
                const auto &sock = offline(it->second);
                if (!sock.pcb_dropped && belongs(sock))
                    entries.push_back(offline_sock_info(it->first, sock, flags));
            }
        };
        list([](const OfflineSocket &sock) { return sock.stream(); });
        list([](const OfflineSocket &sock) { return sock.sce_type == SCE_NET_SOCK_DGRAM; });
        list([](const OfflineSocket &sock) { return sock.sce_type == SCE_NET_SOCK_DGRAM_P2P; });
        if (flags & 0x20) {
            for (const auto &[id, epoll] : emuenv.net.epolls) {
                SceNetSockInfo entry{};
                if (flags & 2)
                    std::memcpy(entry.name, epoll->name.data(), epoll->name.size());
                entry.pid = KernelState::process_id;
                entry.s = id;
                entry.socket_type = 11; // epoll
                entry.state = SCE_NET_SOCKINFO_STATE_OPENED;
                entry.flags = SCE_NET_SOCKINFO_F_SELF;
                entries.push_back(entry);
            }
        }
    }
    if (!info)
        return static_cast<int>(entries.size());
    const size_t written = std::min(entries.size(), static_cast<size_t>(n));
    std::copy_n(entries.begin(), written, info);
    return static_cast<int>(written);
#else
    return UNIMPLEMENTED();
#endif
}

EXPORT(int, sceNetGetStatisticsInfo) {
    TRACY_FUNC(sceNetGetStatisticsInfo);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetGetpeername, int sid, SceNetSockaddr *addr, unsigned int *addrlen) {
    TRACY_FUNC(sceNetGetpeername, sid, addr, addrlen);
    auto sock = lock_and_find(sid, emuenv.net.socks, emuenv.kernel.mutex);
    RET_NET_ERRNO(sock ? sock->get_peer_address(addr, addrlen) : SCE_NET_ERROR_EBADF);
}

EXPORT(int, sceNetGetsockname, int sid, SceNetSockaddr *addr, unsigned int *addrlen) {
    TRACY_FUNC(sceNetGetsockname, sid, addr, addrlen);
    auto sock = lock_and_find(sid, emuenv.net.socks, emuenv.kernel.mutex);

    RET_NET_ERRNO(sock ? sock->get_socket_address(addr, addrlen) : SCE_NET_ERROR_EBADF);
}

EXPORT(int, sceNetGetsockopt, int sid, int level, int optname, void *optval, unsigned int *optlen) {
    TRACY_FUNC(sceNetGetsockopt, sid, level, optname, optval, optlen);
    auto sock = lock_and_find(sid, emuenv.net.socks, emuenv.kernel.mutex);

    RET_NET_ERRNO(sock ? sock->get_socket_options(level, optname, optval, optlen) : SCE_NET_ERROR_EBADF);
}

EXPORT(SceUInt32, sceNetHtonl, SceUInt32 n) {
    TRACY_FUNC(sceNetHtonl, n);
    return htonl(n);
}

EXPORT(SceUInt64, sceNetHtonll, SceUInt64 n) {
    TRACY_FUNC(sceNetHtonll, n);
    return HTONLL(n);
}

EXPORT(SceUInt16, sceNetHtons, SceUInt16 n) {
    TRACY_FUNC(sceNetHtons, n);
    return htons(n);
}

EXPORT(Ptr<const char>, sceNetInetNtop, int af, const void *src, Ptr<char> dst, unsigned int size) {
    TRACY_FUNC(sceNetInetNtop, af, src, dst, size);
    char *dst_ptr = dst.get(emuenv.mem);
#ifdef _WIN32
    const char *res = InetNtop(af, src, dst_ptr, size);
#else
    const char *res = inet_ntop(af, src, dst_ptr, size);
#endif
    if (res == nullptr) {
        ret_net_errno(emuenv, thread_id, SCE_NET_ERROR_EAFNOSUPPORT);
        return Ptr<char>();
    }
    return dst;
}

EXPORT(int, sceNetInetPton, int af, const char *src, void *dst) {
    TRACY_FUNC(sceNetInetPton, af, src, dst);

    if (af != SCE_NET_AF_INET)
        RET_NET_ERRNO(SCE_NET_ERROR_EAFNOSUPPORT);

#ifdef _WIN32
    int res = InetPton(af, src, dst);
#else
    int res = inet_pton(af, src, dst);
#endif

#ifdef __EMSCRIPTEN__
    RET_NET_ERRNO(res == 0 ? SCE_NET_ERROR_EINVAL : res); // af is checked above: res is 1
#else
    RET_NET_ERRNO(res == 0 ? SCE_NET_ERROR_EINVAL : PosixSocket::translate_return_value(res));
#endif
}

EXPORT(int, sceNetInit, SceNetInitParam *param) {
    TRACY_FUNC(sceNetInit, param);
    if (emuenv.net.inited)
        RET_NET_ERRNO(SCE_NET_ERROR_EBUSY);

    if (!param || !param->memory.address() || param->size < 0x4000 || param->flags != 0)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);

#ifdef _WIN32
    WORD versionWanted = MAKEWORD(2, 2);
    WSADATA wsaData;
    WSAStartup(versionWanted, &wsaData);
#endif
    emuenv.net.state = 0;
    emuenv.net.inited = true;
    emuenv.net.resolver_id = ++emuenv.net.next_id;
    net_utils::init_address(emuenv.cfg.adhoc_addr, emuenv.net.netAddr, emuenv.net.broadcastAddr);
    emuenv.net.current_addr_index = emuenv.cfg.adhoc_addr;
    return 0;
}

EXPORT(int, sceNetListen, int sid, int backlog) {
    TRACY_FUNC(sceNetListen, sid, backlog);
    auto sock = lock_and_find(sid, emuenv.net.socks, emuenv.kernel.mutex);
    if (!sock) {
        RET_NET_ERRNO(SCE_NET_ERROR_EBADF);
    }
    RET_NET_ERRNO(sock->listen(backlog));
}

EXPORT(SceUInt32, sceNetNtohl, SceUInt32 n) {
    TRACY_FUNC(sceNetNtohl, n);
    return ntohl(n);
}

EXPORT(SceUInt64, sceNetNtohll, SceUInt64 n) {
    TRACY_FUNC(sceNetNtohll, n);
    return NTOHLL(n);
}

EXPORT(SceUInt16, sceNetNtohs, SceUInt16 n) {
    TRACY_FUNC(sceNetNtohs, n);
    return ntohs(n);
}

EXPORT(int, sceNetRecv, int sid, void *buf, unsigned int len, int flags) {
    TRACY_FUNC(sceNetRecv, sid, buf, len, flags);
#ifdef __EMSCRIPTEN__
    RET_NET_ERRNO(recv_offline(emuenv, thread_id, sid, buf, len, flags, nullptr, nullptr));
#else
    auto sock = lock_and_find(sid, emuenv.net.socks, emuenv.kernel.mutex);

    RET_NET_ERRNO(sock ? sock->recv_packet(buf, len, flags, nullptr, 0) : SCE_NET_ERROR_EBADF);
#endif
}

EXPORT(int, sceNetRecvfrom, int sid, void *buf, unsigned int len, int flags, SceNetSockaddr *from, unsigned int *fromlen) {
    TRACY_FUNC(sceNetRecvfrom, sid, buf, len, flags, from, fromlen);
#ifdef __EMSCRIPTEN__
    RET_NET_ERRNO(recv_offline(emuenv, thread_id, sid, buf, len, flags, from, fromlen));
#else
    auto sock = lock_and_find(sid, emuenv.net.socks, emuenv.kernel.mutex);

    RET_NET_ERRNO(sock ? sock->recv_packet(buf, len, flags, from, fromlen) : SCE_NET_ERROR_EBADF);
#endif
}

EXPORT(int, sceNetRecvmsg) {
    TRACY_FUNC(sceNetRecvmsg);
    return UNIMPLEMENTED();
}

#ifdef __EMSCRIPTEN__
// Offline resolver: no DNS server is configured, so every lookup finishes at
// once with RESOLVER_ENODNS. Ids share the socket descriptor space (epoll
// takes both); a finished lookup reports EPOLLIN.
EXPORT(int, sceNetResolverAbort, int rid, int flags) {
    TRACY_FUNC(sceNetResolverAbort, rid, flags);
    const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
    RET_NET_ERRNO(emuenv.net.resolvers.contains(rid) ? 0 : SCE_NET_ERROR_EBADF); // nothing is in progress
}

EXPORT(int, sceNetResolverCreate, const char *name, void *param, int flags) {
    TRACY_FUNC(sceNetResolverCreate, name, param, flags);
    const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
    const int id = ++emuenv.net.next_id;
    emuenv.net.resolvers.emplace(id, std::nullopt);
    return id;
}

EXPORT(int, sceNetResolverDestroy, int rid) {
    TRACY_FUNC(sceNetResolverDestroy, rid);
    const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
    RET_NET_ERRNO(emuenv.net.resolvers.erase(rid) ? 0 : SCE_NET_ERROR_EBADF);
}

EXPORT(int, sceNetResolverGetError, int rid, int *result) {
    TRACY_FUNC(sceNetResolverGetError, rid, result);
    const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
    const auto resolver = emuenv.net.resolvers.find(rid);
    if (resolver == emuenv.net.resolvers.end())
        RET_NET_ERRNO(SCE_NET_ERROR_EBADF);
    if (!result)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
    *result = resolver->second.value_or(0);
    return 0;
}
#else
EXPORT(int, sceNetResolverAbort) {
    TRACY_FUNC(sceNetResolverAbort);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetResolverCreate, const char *name, void *param, int flags) {
    TRACY_FUNC(sceNetResolverCreate, name, param, flags);
    STUBBED("Fake id");
    return emuenv.net.resolver_id;
}

EXPORT(int, sceNetResolverDestroy, int rid) {
    TRACY_FUNC(sceNetResolverDestroy, rid);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetResolverGetError) {
    TRACY_FUNC(sceNetResolverGetError);
    return UNIMPLEMENTED();
}
#endif

EXPORT(int, sceNetResolverStartAton, int rid, const SceNetInAddr *addr, char *hostname, int len, int timeout, int retry, int flags) {
    TRACY_FUNC(sceNetResolverStartAton, rid, addr, hostname, len, timeout, retry, flags);
    struct hostent *resolved = gethostbyaddr((const char *)addr, len, AF_INET);
    strcpy(hostname, resolved->h_name);
    return 0;
}

EXPORT(int, sceNetResolverStartNtoa, int rid, const char *hostname, SceNetInAddr *addr, int timeout, int retry, int flags) {
    TRACY_FUNC(sceNetResolverStartNtoa, rid, hostname, addr, timeout, retry, flags);
#ifdef __EMSCRIPTEN__
    {
        const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        const auto resolver = emuenv.net.resolvers.find(rid);
        if (resolver == emuenv.net.resolvers.end())
            RET_NET_ERRNO(SCE_NET_ERROR_EBADF);
        if (!hostname || !addr)
            RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);
        resolver->second = static_cast<int>(SCE_NET_ERROR_RESOLVER_ENODNS);
    }
    offline_net_wake(emuenv.net);
    RET_NET_ERRNO(SCE_NET_ERROR_RESOLVER_ENODNS);
#else
    struct hostent *resolved = gethostbyname(hostname);
    if (resolved == nullptr) {
        memset(addr, 0, sizeof(*addr));
        RET_NET_ERRNO(SCE_NET_ERROR_EHOSTUNREACH);
    }
    memcpy(addr, resolved->h_addr, sizeof(uint32_t));
    return 0;
#endif
}

EXPORT(int, sceNetSend, int sid, const void *msg, unsigned int len, int flags) {
    TRACY_FUNC(sceNetSend, sid, msg, len, flags);
    auto sock = lock_and_find(sid, emuenv.net.socks, emuenv.kernel.mutex);

    RET_NET_ERRNO(sock ? sock->send_packet(msg, len, flags, nullptr, 0) : SCE_NET_ERROR_EBADF);
}

EXPORT(int, sceNetSendmsg, int sid, const SceNetMsghdr *msg, int flags) {
    TRACY_FUNC(sceNetSendmsg, sid, msg, flags);

    auto sock = lock_and_find(sid, emuenv.net.socks, emuenv.kernel.mutex);
    if (!sock)
        RET_NET_ERRNO(SCE_NET_ERROR_EBADF);

    size_t total_len = 0;
    for (int i = 0; i < msg->msg_iovlen; ++i) {
        const SceNetIovec &iov = msg->msg_iov.get(emuenv.mem)[i];
        total_len += iov.iov_len;
    }

    std::vector<char> buf;
    buf.reserve(total_len);

    for (int i = 0; i < msg->msg_iovlen; ++i) {
        const SceNetIovec &iov = msg->msg_iov.get(emuenv.mem)[i];
        const char *data = reinterpret_cast<const char *>(iov.iov_base.get(emuenv.mem));
        buf.insert(buf.end(), data, data + iov.iov_len);
    }

    RET_NET_ERRNO(sock->send_packet(buf.data(), total_len, flags, msg->msg_name.get(emuenv.mem), msg->msg_namelen));
}

EXPORT(int, sceNetSendto, int sid, const void *msg, unsigned int len, int flags, const SceNetSockaddr *to, unsigned int tolen) {
    TRACY_FUNC(sceNetSendto, sid, msg, len, flags, to, tolen);
    auto sock = lock_and_find(sid, emuenv.net.socks, emuenv.kernel.mutex);
    if (!sock)
        RET_NET_ERRNO(SCE_NET_ERROR_EBADF);
#ifdef __EMSCRIPTEN__
    // No broadcast rewrite: offline, lo0 is the only interface and has no
    // broadcast address, so 255.255.255.255 has no route.
    RET_NET_ERRNO(sock->send_packet(msg, len, flags, to, tolen));
#else
    SceNetSockaddrIn to_in;
    std::memcpy(&to_in, to, sizeof(SceNetSockaddrIn));
    if (!sock->sockopt_so_onesbcast && (to_in.sin_addr.s_addr == INADDR_BROADCAST))
        to_in.sin_addr.s_addr = emuenv.net.broadcastAddr;

    RET_NET_ERRNO(sock->send_packet(msg, len, flags, (SceNetSockaddr *)&to_in, tolen));
#endif
}

EXPORT(int, sceNetSetDnsInfo) {
    TRACY_FUNC(sceNetSetDnsInfo);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetSetsockopt, int sid, SceNetProtocol level, SceNetSocketOption optname, const void *optval, unsigned int optlen) {
    TRACY_FUNC(sceNetSetsockopt, sid, level, optname, optval, optlen);
    if (optname == 0x40000) {
        LOG_ERROR("Unknown socket option {}", log_hex(optname));
        return 0;
    }
    auto sock = lock_and_find(sid, emuenv.net.socks, emuenv.kernel.mutex);

    RET_NET_ERRNO(sock ? sock->set_socket_options(level, optname, optval, optlen) : SCE_NET_ERROR_EBADF);
}

EXPORT(int, sceNetShowIfconfig) {
    TRACY_FUNC(sceNetShowIfconfig);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetShowNetstat) {
    TRACY_FUNC(sceNetShowNetstat);
    if (!emuenv.net.inited) {
        RET_NET_ERRNO(SCE_NET_ERROR_ENOTINIT);
    }
    return 0;
}

EXPORT(int, sceNetShowRoute) {
    TRACY_FUNC(sceNetShowRoute);
    return UNIMPLEMENTED();
}

EXPORT(int, sceNetShutdown, int sid, int how) {
    TRACY_FUNC(sceNetShutdown, sid, how);
    auto sock = lock_and_find(sid, emuenv.net.socks, emuenv.kernel.mutex);

    RET_NET_ERRNO(sock ? sock->shutdown_socket(how) : SCE_NET_ERROR_EBADF);
}

EXPORT(int, sceNetSocket, const char *name, int domain, SceNetSocketType type, SceNetProtocol protocol) {
    TRACY_FUNC(sceNetSocket, name, domain, type, protocol);
#ifdef __EMSCRIPTEN__
    if (!name)
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL); // SceNetPs 0x810074a8
    if (domain != SCE_NET_AF_INET)
        RET_NET_ERRNO(SCE_NET_ERROR_EAFNOSUPPORT);
    const bool stream = type == SCE_NET_SOCK_STREAM || type == SCE_NET_SOCK_STREAM_P2P;
    // SOCK_RAW is not modelled by the offline stack.
    if ((!stream && type != SCE_NET_SOCK_DGRAM && type != SCE_NET_SOCK_DGRAM_P2P)
        || (protocol != SCE_NET_IPPROTO_IP && protocol != (stream ? SCE_NET_IPPROTO_TCP : SCE_NET_IPPROTO_UDP)))
        RET_NET_ERRNO(SCE_NET_ERROR_EPROTONOSUPPORT);
    const std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
    const int id = ++emuenv.net.next_id;
    const auto sock = std::make_shared<OfflineSocket>(emuenv.net, type);
    sock->name.assign(name, strnlen(name, 31));
    emuenv.net.socks.emplace(id, sock);
    return id;
#else
    bool isP2P = (type == SCE_NET_SOCK_DGRAM_P2P || type == SCE_NET_SOCK_STREAM_P2P);

    SocketPtr sock = isP2P ? std::make_shared<P2PSocket>(domain, type, protocol) : std::make_shared<PosixSocket>(domain, type, protocol);

    auto id = ++emuenv.net.next_id;
    emuenv.net.socks.emplace(id, sock);
    return id;
#endif
}

EXPORT(int, sceNetSocketAbort, int sid, int flags) {
    TRACY_FUNC(sceNetSocketAbort, sid);
    if ((sid < 0) || (flags < 0) || (flags > (SCE_NET_SOCKET_ABORT_FLAG_RCV_PRESERVATION | SCE_NET_SOCKET_ABORT_FLAG_SND_PRESERVATION)))
        RET_NET_ERRNO(SCE_NET_ERROR_EINVAL);

    auto sock = lock_and_find(sid, emuenv.net.socks, emuenv.kernel.mutex);
    RET_NET_ERRNO(sock ? sock->abort(flags) : SCE_NET_ERROR_EBADF);
}

EXPORT(int, sceNetSocketClose, int sid) {
    TRACY_FUNC(sceNetSocketClose, sid);
    int result = 0;
    {
        std::lock_guard<std::mutex> lock(emuenv.kernel.mutex);
        const auto sock = util::find(sid, emuenv.net.socks);
        if (sock) {
            result = sock->close();
            if (result >= 0)
                emuenv.net.socks.erase(sid);
        } else
            result = SCE_NET_ERROR_EBADF;
    }

    RET_NET_ERRNO(result);
}

EXPORT(int, sceNetTerm) {
    TRACY_FUNC(sceNetTerm);
    if (!emuenv.net.inited) {
        RET_NET_ERRNO(SCE_NET_ERROR_ENOTINIT);
    }
#ifdef _WIN32
    WSACleanup();
#endif
    emuenv.net.inited = false;
    return 0;
}
