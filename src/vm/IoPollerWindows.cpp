/*---
    IoPollerWindows.cpp — wepoll implementation of IoPoller.h
    (src/3rdparty/wepoll, BSD-2-Clause, IOCP-backed epoll-compatible).
    Windows socket handles are 32-bit kernel handles even in 64-bit
    processes, so the SOCKET <-> int round trip is lossless; the net
    module owns the same reasoning in its net_platform_internal.h, so
    the conversion is inlined here rather than shared across the
    module boundary. WSAStartup is refcounted per process: the VM and
    the net module each init their own side, and each registered
    atexit(WSACleanup) balances its own init.
---*/
#include "IoPoller.h"

#include <winsock2.h>
#include <windows.h>

#include "wepoll.h"

#include <cstdlib>
#include <cstring>
#include <mutex>

namespace nlang {
namespace {

std::once_flag g_wsaOnce;

void WsaInitOnce() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    std::atexit([] { WSACleanup(); });   //paired with the once (plan ruling R4)
}

inline SOCKET Fd(int fd) {
    return static_cast<SOCKET>(static_cast<uintptr_t>(static_cast<intptr_t>(fd)));
}
inline int ToFd(SOCKET s) { return static_cast<int>(static_cast<uintptr_t>(s)); }

uint32_t ToEpollEvents(uint32_t events) {
    uint32_t e = 0;
    if (events & IO_EV_READ) e |= EPOLLIN | EPOLLRDNORM;
    if (events & IO_EV_WRITE) e |= EPOLLOUT | EPOLLWRNORM;
    return e;
}

uint32_t FromEpollEvents(uint32_t events) {
    uint32_t e = 0;
    if (events & (EPOLLIN | EPOLLRDNORM | EPOLLRDHUP)) e |= IO_EV_READ;
    if (events & (EPOLLOUT | EPOLLWRNORM)) e |= IO_EV_WRITE;
    if (events & EPOLLHUP) e |= IO_EV_HUP;
    if (events & EPOLLERR) e |= IO_EV_ERR;
    return e;
}

int PollerCtl(void* poller, int op, int fd, uint32_t events) {
    epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = ToEpollEvents(events);
    ev.data.sock = Fd(fd);
    return epoll_ctl(static_cast<HANDLE>(poller), op, Fd(fd), &ev);
}

//Completions harvested per wait (the wepoll wait buffer).
constexpr int kReadyBatch = 64;

}  //namespace

void* IoPollerCreate() {
    std::call_once(g_wsaOnce, WsaInitOnce);
    return epoll_create1(0);
}

void IoPollerClose(void* poller) {
    if (poller) epoll_close(static_cast<HANDLE>(poller));
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
    const int n = epoll_wait(static_cast<HANDLE>(poller), ready,
                             kReadyBatch, timeoutMs);
    out.clear();
    if (n <= 0) return n < 0 ? -1 : 0;
    for (int i = 0; i < n; ++i) {
        IoPollerEvent ev;
        ev.fd = ToFd(ready[i].data.sock);
        ev.events = FromEpollEvents(ready[i].events);
        out.push_back(ev);
    }
    return n;
}

}  //namespace nlang
