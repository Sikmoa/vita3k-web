#pragma once

#include <net/socket.h>

struct EpollSocket {
    unsigned int events;
    SceNetEpollData data;
    std::weak_ptr<Socket> sock;
};

struct Epoll {
    std::map<int, EpollSocket> eventEntries;
    // sceNetEpollAbort (offline stack): ends the waits in progress; the
    // PRESERVATION flag also fails later waits.
    unsigned abort_generation = 0;
    bool abort_preserved = false;

    int add(int id, std::weak_ptr<Socket> sock, SceNetEpollEvent *ev);
    int del(int id);
    int mod(int id, SceNetEpollEvent *ev);
    int wait(SceNetEpollEvent *events, int maxevents, int timeout); // host sockets only
};

typedef std::shared_ptr<Epoll> EpollPtr;
