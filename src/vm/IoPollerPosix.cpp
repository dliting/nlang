/*---
    IoPollerPosix.cpp — epoll implementation of IoPoller.h. The poller
    fd travels through the opaque handle as an intptr_t; fds are plain
    OS fds, so no conversion is needed for them.
---*/
#include "IoPoller.h"

#include <sys/epoll.h>
#include <unistd.h>

#include <cstring>

namespace nlang {
namespace {

uint32_t ToEpollEvents(uint32_t events) {
    uint32_t e = 0;
    if (events & IO_EV_READ) e |= EPOLLIN;
    if (events & IO_EV_WRITE) e |= EPOLLOUT;
    return e;
}

uint32_t FromEpollEvents(uint32_t events) {
    uint32_t e = 0;
    if (events & (EPOLLIN | EPOLLRDHUP)) e |= IO_EV_READ;
    if (events & EPOLLOUT) e |= IO_EV_WRITE;
    if (events & EPOLLHUP) e |= IO_EV_HUP;
    if (events & EPOLLERR) e |= IO_EV_ERR;
    return e;
}

int PollerCtl(void* poller, int op, int fd, uint32_t events) {
    epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = ToEpollEvents(events);
    ev.data.fd = fd;
    return epoll_ctl(static_cast<int>(reinterpret_cast<intptr_t>(poller)), op, fd, &ev);
}

//Completions harvested per wait (the epoll wait buffer).
constexpr int kReadyBatch = 64;

}  //namespace

void* IoPollerCreate() {
    const int ep = epoll_create1(0);
    return ep < 0 ? nullptr : reinterpret_cast<void*>(static_cast<intptr_t>(ep));
}

void IoPollerClose(void* poller) {
    if (poller) close(static_cast<int>(reinterpret_cast<intptr_t>(poller)));
}

int IoPollerAdd(void* poller, int fd, uint32_t events) {
    return PollerCtl(poller, EPOLL_CTL_ADD, fd, events) == 0 ? 0 : -1;
}

int IoPollerMod(void* poller, int fd, uint32_t events) {
    return PollerCtl(poller, EPOLL_CTL_MOD, fd, events) == 0 ? 0 : -1;
}

int IoPollerDel(void* poller, int fd) {
    return PollerCtl(poller, EPOLL_CTL_DEL, fd, 0) == 0 ? 0 : -1;
}

int IoPollerWait(void* poller, std::vector<IoPollerEvent>& out, int timeoutMs) {
    epoll_event ready[kReadyBatch];
    const int n = epoll_wait(static_cast<int>(reinterpret_cast<intptr_t>(poller)),
                             ready, kReadyBatch, timeoutMs);
    out.clear();
    if (n <= 0) return n < 0 ? -1 : 0;
    for (int i = 0; i < n; ++i) {
        IoPollerEvent ev;
        ev.fd = ready[i].data.fd;
        ev.events = FromEpollEvents(ready[i].events);
        out.push_back(ev);
    }
    return n;
}

}  //namespace nlang
