/*---
    IoPoller.h — the VM's generic fd-readiness facility (the IO poller
    set: vendored wepoll on Windows, epoll on POSIX). The net package
    is its first consumer — the scheduler parks IO-waiting contexts
    (VmSchedulerIo.cpp) and this poller answers readiness — but the
    abstraction is fd-generic: pipes and child-process stdio from any
    future native library register the same way. Regular files are not
    pollable; async file IO goes through the work pool instead
    (design r1 note ① — libuv, Go's netpoller and Project Loom all
    draw the same line). The poller handle is opaque; fds are plain OS
    fds (on Windows, socket handles — wepoll only knows sockets).
---*/
#pragma once

#include <cstdint>
#include <vector>

namespace nlang {

//Event bits (a subset of the wepoll/epoll vocabulary). READ/WRITE carry
//the same values as the ABI parkForIo events parameter (NativeHost.h);
//HUP/ERR are report-only — a dead fd counts as ready in every
//direction, so they are never registered.
constexpr uint32_t IO_EV_READ = 1u;
constexpr uint32_t IO_EV_WRITE = 2u;
constexpr uint32_t IO_EV_HUP = 4u;
constexpr uint32_t IO_EV_ERR = 8u;

//One poller completion.
struct IoPollerEvent {
    int fd = 0;
    uint32_t events = 0;
};

//Create/close the poller set. Create initializes the OS socket
//subsystem where required (WSAStartup on Windows; idempotent through a
//once flag, plan ruling R4).
void* IoPollerCreate();
void IoPollerClose(void* poller);
int IoPollerAdd(void* poller, int fd, uint32_t events);
int IoPollerMod(void* poller, int fd, uint32_t events);
int IoPollerDel(void* poller, int fd);
//Fills out with ready events, returns their count (0 on timeout).
int IoPollerWait(void* poller, std::vector<IoPollerEvent>& out, int timeoutMs);

}  //namespace nlang
