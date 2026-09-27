// Genuine VitaSDK offline-network fixture: what a Vita without a connection
// answers (net/offline_socket.h). Exit code 100 when every check passes,
// otherwise the number of the first failed check.
#include <stdint.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>

#define CHECK(n, cond) \
    do {               \
        if (!(cond))   \
            return n;  \
    } while (0)

static SceNetSockaddrIn address(uint32_t host_addr, uint16_t port, uint16_t vport) {
    SceNetSockaddrIn addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons(port);
    addr.sin_addr.s_addr = sceNetHtonl(host_addr);
    addr.sin_vport = sceNetHtons(vport);
    return addr;
}

#define LOOPBACK 0x7F000001u
#define RECEIVER_PORT 40000

static int sendto_loopback(int s, const char *text, uint16_t port) {
    SceNetSockaddrIn to = address(LOOPBACK, port, 0);
    return sceNetSendto(s, text, strlen(text), 0, (SceNetSockaddr *)&to, sizeof(to));
}

// Helper threads: after a delay, send a datagram to the receiver or abort it.
static int helper_socket, helper_target;
static int sender(SceSize args, void *argp) {
    sceKernelDelayThread(20000);
    sendto_loopback(helper_socket, "woken", RECEIVER_PORT);
    return 0;
}
static int aborter(SceSize args, void *argp) {
    sceKernelDelayThread(20000);
    sceNetSocketAbort(helper_target, 0);
    return 0;
}
static SceUID start_helper(SceKernelThreadEntry entry) {
    const SceUID thread = sceKernelCreateThread("net helper", entry, 0x40, 0x4000, 0, 0, NULL);
    if (thread >= 0)
        sceKernelStartThread(thread, 0, NULL);
    return thread;
}

static char net_memory[16 * 1024];

int main(void) {
    SceNetInitParam init = { net_memory, sizeof(net_memory), 0 };
    CHECK(1, sceNetInit(&init) == 0);
    CHECK(2, sceNetCtlInit() == 0);

    // SceNetCtl: disconnected, nothing to read about a connection.
    int state = -1;
    CHECK(3, sceNetCtlInetGetState(&state) == 0 && state == SCE_NETCTL_STATE_DISCONNECTED);
    SceNetCtlInfo info;
    CHECK(4, sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) == (int)0x80412108); // NOT_CONNECTED

    // Datagram sockets bind on lo0; the same port twice is in use.
    const int receiver = sceNetSocket("receiver", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    const int sender_socket = sceNetSocket("sender", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    CHECK(5, receiver >= 0 && sender_socket >= 0);
    SceNetSockaddrIn bind_addr = address(LOOPBACK, RECEIVER_PORT, 0);
    CHECK(6, sceNetBind(receiver, (SceNetSockaddr *)&bind_addr, sizeof(bind_addr)) == 0);
    CHECK(7, sceNetBind(sender_socket, (SceNetSockaddr *)&bind_addr, sizeof(bind_addr)) == (int)SCE_NET_ERROR_EADDRINUSE);
    SceNetSockaddrIn any = address(0, 0, 0);
    CHECK(8, sceNetBind(sender_socket, (SceNetSockaddr *)&any, sizeof(any)) == 0);
    SceNetSockaddrIn name;
    unsigned int name_len = sizeof(name);
    CHECK(9, sceNetGetsockname(sender_socket, (SceNetSockaddr *)&name, &name_len) == 0 && name_len == sizeof(name));
    const uint16_t sender_port = sceNetNtohs(name.sin_port);
    CHECK(10, sender_port >= 49152);

    // No route off the loopback interface; broadcast included.
    SceNetSockaddrIn remote = address(0x08080808, 53, 0);
    CHECK(11, sceNetSendto(sender_socket, "x", 1, 0, (SceNetSockaddr *)&remote, sizeof(remote)) == (int)SCE_NET_ERROR_ENETUNREACH);
    CHECK(12, *sceNetErrnoLoc() == SCE_NET_ENETUNREACH);
    SceNetSockaddrIn broadcast = address(0xFFFFFFFF, RECEIVER_PORT, 0);
    CHECK(13, sceNetSendto(sender_socket, "x", 1, 0, (SceNetSockaddr *)&broadcast, sizeof(broadcast)) == (int)SCE_NET_ERROR_ENETUNREACH);

    // Loopback delivery and receive semantics.
    char buffer[32];
    SceNetSockaddrIn from;
    unsigned int from_len = sizeof(from);
    CHECK(14, sceNetRecvfrom(receiver, buffer, sizeof(buffer), SCE_NET_MSG_DONTWAIT, (SceNetSockaddr *)&from, &from_len) == (int)SCE_NET_ERROR_EAGAIN);
    CHECK(15, sendto_loopback(sender_socket, "hello", RECEIVER_PORT) == 5);
    memset(buffer, 0, sizeof(buffer));
    CHECK(16, sceNetRecvfrom(receiver, buffer, sizeof(buffer), 0, (SceNetSockaddr *)&from, &from_len) == 5 && !strcmp(buffer, "hello"));
    CHECK(17, sceNetNtohs(from.sin_port) == sender_port && sceNetNtohl(from.sin_addr.s_addr) == LOOPBACK);

    // SO_RCVTIMEO ends a blocking receive with EAGAIN.
    int timeout = 50000;
    CHECK(18, sceNetSetsockopt(receiver, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    const SceInt64 before = sceKernelGetProcessTimeWide();
    CHECK(19, sceNetRecv(receiver, buffer, sizeof(buffer), 0) == (int)SCE_NET_ERROR_EAGAIN);
    CHECK(20, sceKernelGetProcessTimeWide() - before >= 45000);
    timeout = 0;
    CHECK(21, sceNetSetsockopt(receiver, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);

    // A blocked receive wakes for a datagram from another thread...
    helper_socket = sender_socket;
    const SceUID send_thread = start_helper(sender);
    CHECK(22, send_thread >= 0);
    memset(buffer, 0, sizeof(buffer));
    CHECK(23, sceNetRecv(receiver, buffer, sizeof(buffer), 0) == 5 && !strcmp(buffer, "woken"));
    CHECK(24, sceKernelWaitThreadEnd(send_thread, NULL, NULL) == 0);
    // ...and for sceNetSocketAbort, which without PRESERVATION only ends that wait.
    helper_target = receiver;
    const SceUID abort_thread = start_helper(aborter);
    CHECK(25, abort_thread >= 0);
    CHECK(26, sceNetRecv(receiver, buffer, sizeof(buffer), 0) == (int)SCE_NET_ERROR_EINTR);
    CHECK(27, sceKernelWaitThreadEnd(abort_thread, NULL, NULL) == 0);
    CHECK(28, sceNetRecv(receiver, buffer, sizeof(buffer), SCE_NET_MSG_DONTWAIT) == (int)SCE_NET_ERROR_EAGAIN);

    // Epoll readiness.
    const int epoll = sceNetEpollCreate("epoll", 0);
    CHECK(29, epoll >= 0);
    SceNetEpollEvent event;
    memset(&event, 0, sizeof(event));
    event.events = SCE_NET_EPOLLIN;
    event.data.u32 = 0x1234;
    CHECK(30, sceNetEpollControl(epoll, SCE_NET_EPOLL_CTL_ADD, receiver, &event) == 0);
    SceNetEpollEvent ready[2];
    CHECK(31, sceNetEpollWait(epoll, ready, 2, 0) == 0);
    CHECK(32, sceNetEpollWait(epoll, ready, 2, 10000) == 0);
    CHECK(33, sendto_loopback(sender_socket, "ready", RECEIVER_PORT) == 5);
    CHECK(34, sceNetEpollWait(epoll, ready, 2, 10000) == 1 && ready[0].events == SCE_NET_EPOLLIN && ready[0].data.u32 == 0x1234);
    CHECK(35, sceNetRecv(receiver, buffer, sizeof(buffer), 0) == 5);
    CHECK(36, sceNetEpollDestroy(epoll) == 0);

    // TCP: no route off lo0, nothing listening on it, never connected.
    const int stream = sceNetSocket("stream", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    CHECK(37, stream >= 0);
    SceNetSockaddrIn web = address(0x5DB8D822, 80, 0);
    CHECK(38, sceNetConnect(stream, (SceNetSockaddr *)&web, sizeof(web)) == (int)SCE_NET_ERROR_ENETUNREACH);
    SceNetSockaddrIn local_service = address(LOOPBACK, 1, 0);
    CHECK(39, sceNetConnect(stream, (SceNetSockaddr *)&local_service, sizeof(local_service)) == (int)SCE_NET_ERROR_ECONNREFUSED);
    CHECK(40, sceNetSend(stream, "x", 1, 0) == (int)SCE_NET_ERROR_ENOTCONN);
    CHECK(41, sceNetRecv(stream, buffer, sizeof(buffer), 0) == (int)SCE_NET_ERROR_ENOTCONN);

    // Resolver: no DNS server.
    const int resolver = sceNetResolverCreate("resolver", NULL, 0);
    CHECK(42, resolver >= 0);
    SceNetInAddr resolved;
    CHECK(43, sceNetResolverStartNtoa(resolver, "example.com", &resolved, 0, 0, 0) == (int)SCE_NET_ERROR_RESOLVER_ENODNS);
    int resolver_error = 0;
    CHECK(44, sceNetResolverGetError(resolver, &resolver_error) == 0 && resolver_error == (int)SCE_NET_ERROR_RESOLVER_ENODNS);
    CHECK(45, sceNetResolverDestroy(resolver) == 0 && sceNetResolverDestroy(resolver) == (int)SCE_NET_ERROR_EBADF);

    // Address conversion needs no network.
    SceNetInAddr parsed;
    CHECK(46, sceNetInetPton(SCE_NET_AF_INET, "192.168.1.2", &parsed) == 1 && sceNetNtohl(parsed.s_addr) == 0xC0A80102);
    CHECK(47, sceNetInetPton(SCE_NET_AF_INET, "not an address", &parsed) == (int)SCE_NET_ERROR_EINVAL);

    // P2P datagram sockets share a UDP port and differ by virtual port.
    const int p2p_a = sceNetSocket("p2p a", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM_P2P, 0);
    const int p2p_b = sceNetSocket("p2p b", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM_P2P, 0);
    CHECK(48, p2p_a >= 0 && p2p_b >= 0);
    SceNetSockaddrIn p2p_addr = address(0, 3658, 3659);
    CHECK(49, sceNetBind(p2p_a, (SceNetSockaddr *)&p2p_addr, sizeof(p2p_addr)) == 0);
    p2p_addr.sin_vport = sceNetHtons(3660);
    CHECK(50, sceNetBind(p2p_b, (SceNetSockaddr *)&p2p_addr, sizeof(p2p_addr)) == 0);

    const int sockets[] = { receiver, sender_socket, stream, p2p_a, p2p_b };
    for (unsigned i = 0; i < sizeof(sockets) / sizeof(sockets[0]); ++i)
        CHECK(51, sceNetSocketClose(sockets[i]) == 0);
    CHECK(52, sceNetSocketClose(receiver) == (int)SCE_NET_ERROR_EBADF);
    sceNetCtlTerm();
    CHECK(53, sceNetTerm() == 0);
    return 100;
}
