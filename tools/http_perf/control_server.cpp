/*---
    control_server.cpp — experiment E0, the pure-C++ control group for the
    "about 15 ms per request" question (docs/dev/net_http_perf_design.md
    §3.3 E0).

    It reuses the VM-independent native code of the real server — the IO
    poller facade (src/vm/IoPollerWindows.cpp over vendored wepoll) and the
    socket layer (src/native/net/net_platform_windows.cpp) — and drops
    everything above it: no VM, no global interpreter lock, no carriers, no
    script. What is left is exactly the shape whose cost is in question: an
    accept thread, one OS thread per connection, one poller thread, an
    interest table, and readiness reported back to a parked reader.

    The one variable is WHERE a new interest reaches the kernel:
      round mode (default)                    the poller thread re-syncs the
                                              table at each round boundary,
                                              the way SyncPollerInterest
                                              does today (src/vm/
                                              VmSchedulerIo.cpp:126-150).
      immediate mode (-DCONTROL_REGISTER_
                      IMMEDIATE)              the parking thread registers
                                              its own fd before it sleeps —
                                              the shape ruling D1 moves the
                                              VM to.
    tools/http_perf/build_control.ps1 compiles one binary per mode, so the
    hot path carries no mode branch.

    Deliberate simplifications, each of them the point of a control group:
    the response is one constant block of bytes (the b0 answer the nlang
    server puts on the wire: status line, Content-Length: 0, one Date
    header — stdlib/http.n:1017-1024), so no per-response work of any kind
    is priced here and rulings D3/D4 are already applied; request parsing
    stops at the header terminator, because E0 prices the IO and wake shape
    while experiment E5 gives parsing its own cost table; a connection gets
    an OS thread instead of a VM context, which is what "take the VM out"
    means, not an unfairness; and no wait carries a deadline, so the poll
    timeout is always the bare slice — the real server's poll timeout is the
    nearest deadline capped by the slice (NextIoTimeoutMs), and the deadline
    form is what experiment E3 varies, so E0 measures the no-deadline arm.
    E1's mechanism check is compiled in as a third shape (round-tb): today's
    registration shape plus one timeBeginPeriod(1) call at startup. The
    quantum's claimed cause is the 10 ms poll slice rounded up by the 15.625 ms
    system clock tick, and the cheapest place to test that is here rather than
    in the VM — same poller, same socket layer, no rebuild of the VM to wait
    for. Ruling D2 (do not ship a resident timeBeginPeriod) is unaffected: this
    is a probe, not a fix.
---*/
#include "IoPoller.h"
#include "net/net_platform.h"

#ifdef CONTROL_TIMEBEGIN
#include <windows.h>      //timeBeginPeriod / timeEndPeriod
#include <mmsystem.h>     //their declarations (not pulled in by windows.h)
#endif

#include <algorithm>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

//Port the probe listens on unless --port says otherwise; distinct from the
//baseline rig's 18081 so a stray nlang server can never answer for it.
constexpr int kControlDefaultPort = 18091;

namespace {

//Mirrors kIoPollSliceMs (src/vm/VmSchedulerIo.cpp:24) and the accept-queue
//depth every net listener hands listen (kNetListenBacklog,
//src/native/net/net_native.cpp:82).
constexpr int kSliceMs = 10;
constexpr int kListenBacklog = 512;
constexpr int kReadBufBytes = 8192;
constexpr const char* kListenAddress = "127.0.0.1";

//The 0-byte b0 response, byte for byte what the nlang server writes for
//`GET /b0` to an HTTP/1.1 keep-alive peer (no Connection header in that
//case — stdlib/http.n:1026-1031). Date is a constant: ruling D4 caches the
//value per second, and a control group has no reason to pay even that.
constexpr char kResponse[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 0\r\n"
    "Date: Wed, 11 Nov 2026 00:00:00 GMT\r\n"
    "\r\n";

#ifdef CONTROL_REGISTER_IMMEDIATE
constexpr const char* kModeName = "immediate";
#elif defined(CONTROL_TIMEBEGIN)
constexpr const char* kModeName = "round-tb";
#else
constexpr const char* kModeName = "round";
#endif

//The control-group analogue of the scheduler's IO wait entry (IoWaitEntry,
//src/vm/VmSchedulerIo.cpp:46) plus the per-context wake the VM performs
//through its run queue: every waiter owns a condition variable the poller
//fires once its fd reports the wanted readiness. All fields are guarded by
//the table's one mutex, as scheduler state is by the global lock.
struct Waiter {
    int fd = -1;
    uint32_t wantEvents = 0;
    uint32_t readyEvents = 0;
    bool abandoned = false;     //the fd is going away while parked
    std::condition_variable cv;
};

//Interest table and poller loop. Every method mirrors the named site of
//src/vm/VmSchedulerIo.cpp; the only freedom taken is replacing the VM's
//context wake with a condition variable, which is the variable under test.
class InterestTable {
public:
    explicit InterestTable(void* poller) : m_poller(poller) {}

    //Parks until the waiter's fd reports one of `events` (answer: the
    //reported bits) or the fd dies / the server shuts down (answer: 0).
    uint32_t Park(Waiter& w, uint32_t events);

    //The fd is closing. Mirrors WakeIoWaitersForFd (VmSchedulerIo.cpp:77-
    //-101): the waiter and the registration baseline go, no kernel DEL —
    //the registration dies with the fd.
    void Close(Waiter& w);

    //Poller thread body: mirrors IoPollerThreadMain (VmSchedulerIo.cpp:226-
    //-255) — sleep until there is interest, sync the set, wait with the lock
    //released, harvest.
    void Run();

    void Shutdown();

private:
    //Round boundary: aggregates the wanted masks and withdraws dropped
    //interest. The add half is compiled out in immediate mode. Mirrors
    //SyncPollerInterest (VmSchedulerIo.cpp:126-150).
    void SyncRound();

    //Reports the readiness the wait returned, then drops the woken entries.
    //Mirrors HarvestIoEvents (VmSchedulerIo.cpp:181-217).
    void Harvest(std::vector<nlang::IoPollerEvent>& evs);

    //Add-or-Mod for one fd, keeping the baseline entry in step — the pair is
    //idempotent either way (VmSchedulerIo.cpp:137-139). Caller holds m_mtx.
    void Register(int fd, uint32_t events);

    void* m_poller = nullptr;
    std::mutex m_mtx;
    std::condition_variable m_pollerCv;         //m_ioCv
    std::vector<Waiter*> m_waits;               //m_ioWaits
    std::unordered_map<int, uint32_t> m_registered;  //m_ioPollerRegistered
    bool m_shutdown = false;
};

void InterestTable::Register(int fd, uint32_t events) {
    uint32_t mask = events;
    const auto prev = m_registered.find(fd);
    if (prev != m_registered.end()) {
        if ((prev->second & mask) == mask)
            return;                 //already watching every wanted bit
        mask |= prev->second;
    }
    if (nlang::IoPollerAdd(m_poller, fd, mask) != 0)
        nlang::IoPollerMod(m_poller, fd, mask);
    m_registered[fd] = mask;
}

uint32_t InterestTable::Park(Waiter& w, uint32_t events) {
    std::unique_lock<std::mutex> lk(m_mtx);
    w.wantEvents = events;
    w.readyEvents = 0;
#ifdef CONTROL_REGISTER_IMMEDIATE
    //Ruling D1's shape: the interest is in the kernel before the thread
    //sleeps, so an in-flight wait sees it — wepoll arms an already-ready fd
    //synchronously (spec §2.3, and tools/http_perf/probe_epoll_add_ready.c
    //measured the POSIX side). D1 also moves poller-handle creation to this
    //point; the control group creates the handle in main() for both modes,
    //which is the same starting condition for the two binaries.
    Register(w.fd, events);
#endif
    m_waits.push_back(&w);
    m_pollerCv.notify_all();        //ParkForIo's m_ioCv.notify_all (:53)
    w.cv.wait(lk, [&w] { return w.readyEvents != 0u || w.abandoned; });
    return w.readyEvents;
}

void InterestTable::Close(Waiter& w) {
    std::lock_guard<std::mutex> g(m_mtx);
    w.abandoned = true;
    w.cv.notify_all();
    m_waits.erase(std::remove(m_waits.begin(), m_waits.end(), &w), m_waits.end());
    m_registered.erase(w.fd);
    m_pollerCv.notify_all();        //the poller may be asleep on an empty table
}

void InterestTable::Shutdown() {
    std::lock_guard<std::mutex> g(m_mtx);
    m_shutdown = true;
    for (Waiter* w : m_waits)
        w->cv.notify_all();
    m_pollerCv.notify_all();
}

void InterestTable::SyncRound() {
    std::unordered_map<int, uint32_t> wanted;
    for (Waiter* w : m_waits)
        wanted[w->fd] |= w->wantEvents;
#ifndef CONTROL_REGISTER_IMMEDIATE
    //Today's shape (VmSchedulerIo.cpp:133-140): a new interest reaches the
    //kernel only when the poller comes back around, so a park that lands
    //while the wait is in flight has to wait for that wait to end.
    for (const auto& w : wanted) {
        const auto prev = m_registered.find(w.first);
        if (prev != m_registered.end() && prev->second == w.second)
            continue;               //a steady set costs no syscalls
        if (nlang::IoPollerAdd(m_poller, w.first, w.second) != 0)
            nlang::IoPollerMod(m_poller, w.first, w.second);
        m_registered[w.first] = w.second;
    }
#endif
    //Withdrawn interest is dropped at the round boundary in both modes —
    //the del-vanished loop (VmSchedulerIo.cpp:141-149).
    for (auto it = m_registered.begin(); it != m_registered.end();) {
        if (wanted.find(it->first) != wanted.end()) {
            ++it;
            continue;
        }
        nlang::IoPollerDel(m_poller, it->first);
        it = m_registered.erase(it);
    }
}

void InterestTable::Harvest(std::vector<nlang::IoPollerEvent>& evs) {
    std::unordered_map<int, uint32_t> ready;
    for (const nlang::IoPollerEvent& ev : evs)
        ready[ev.fd] |= ev.events;
    for (auto it = m_waits.begin(); it != m_waits.end();) {
        Waiter* w = *it;
        const auto r = ready.find(w->fd);
        const bool hit = r != ready.end()
            && ((w->wantEvents & r->second) != 0u
                || (r->second & (nlang::IO_EV_HUP | nlang::IO_EV_ERR)) != 0u);
        if (!hit) {
            ++it;
            continue;
        }
        w->readyEvents = r->second;
        w->cv.notify_all();
        it = m_waits.erase(it);     //woken entries leave the table (:195)
#ifdef CONTROL_REGISTER_IMMEDIATE
        //Ruling D1: the harvest rebuilds the set of fds still referenced, so
        //an fd that just left the table gets its kernel DEL here instead of
        //at the next round boundary.
        nlang::IoPollerDel(m_poller, w->fd);
        m_registered.erase(w->fd);
#endif
    }
    //The empty-table special case (VmSchedulerIo.cpp:212-216): the poller
    //would otherwise sleep before the del-vanished loop ever saw these fds.
    if (m_waits.empty()) {
        for (const auto& reg : m_registered)
            nlang::IoPollerDel(m_poller, reg.first);
        m_registered.clear();
    }
}

void InterestTable::Run() {
    for (;;) {
        std::unique_lock<std::mutex> lk(m_mtx);
        m_pollerCv.wait(lk, [this] { return m_shutdown || !m_waits.empty(); });
        if (m_shutdown)
            break;
        SyncRound();
        //No deadlines in the control group, so this is NextIoTimeoutMs()'s
        //no-deadline arm answering the bare slice (VmSchedulerIo.cpp:162-163).
        const int timeoutMs = kSliceMs;
        lk.unlock();                //the wait runs without the lock, as
                                    //gil.Release() does (:247)
        std::vector<nlang::IoPollerEvent> evs;
        nlang::IoPollerWait(m_poller, evs, timeoutMs);
        lk.lock();
        Harvest(evs);
    }
}

//True when `window` holds a complete header block; headerEnd then names the
//index just past the terminating CRLF CRLF.
bool HeaderComplete(const std::vector<uint8_t>& window, size_t& headerEnd) {
    if (window.size() < 4)
        return false;
    for (size_t i = 0; i + 3 < window.size(); ++i) {
        if (window[i] == '\r' && window[i + 1] == '\n'
            && window[i + 2] == '\r' && window[i + 3] == '\n') {
            headerEnd = i + 4;
            return true;
        }
    }
    return false;
}

//Writes the whole response, parking on write readiness if the send buffer is
//full (never happens on loopback at this size, but a silent spin would be
//worse than an unreachable branch).
bool WriteAll(InterestTable& table, Waiter& w, int fd, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        const int n = nlang::NetSend(fd,
            reinterpret_cast<const uint8_t*>(data) + sent, len - sent);
        if (n > 0) {
            sent += n;
            continue;
        }
        if (n != nlang::kNetWouldBlock)
            return false;
        if (table.Park(w, nlang::IO_EV_WRITE) == 0)
            return false;
    }
    return true;
}

//One connection: read a request header block, answer it, repeat until the
//peer closes. The fast path mirrors net_tcp.cpp:233-241 — only a would-block
//parks, buffered bytes are served without touching the poller.
void ServeConn(InterestTable& table, int fd) {
    nlang::NetSetNoDelay(fd, true);     //what stdlib/http.n:1133-1134 does
    nlang::NetSetKeepAlive(fd, true);   //for every accepted connection
    Waiter w;
    w.fd = fd;
    std::vector<uint8_t> window;
    for (;;) {
        size_t headerEnd = 0;
        bool closed = false;
        while (!HeaderComplete(window, headerEnd)) {
            uint8_t scratch[kReadBufBytes];
            const int n = nlang::NetRecv(fd, scratch, static_cast<int>(sizeof(scratch)));
            if (n > 0) {
                window.insert(window.end(), scratch, scratch + n);
                continue;
            }
            if (n == nlang::kNetWouldBlock) {
                if (table.Park(w, nlang::IO_EV_READ) == 0) {
                    closed = true;      //fd died or the server is stopping
                    break;
                }
                continue;
            }
            closed = true;              //n == 0 (peer closed) or an error
            break;
        }
        if (closed)
            break;
        //oha sends one request per round trip and never pipelines, so
        //whatever follows the header block is dropped with the request.
        window.erase(window.begin(), window.begin() + static_cast<long>(headerEnd));
        if (!WriteAll(table, w, fd, kResponse, static_cast<int>(sizeof(kResponse)) - 1))
            break;
    }
    table.Close(w);
    nlang::NetClose(fd);
}

//Accept loop: park on the listener, then drain the completed handshakes.
//Accepted connections get their own threads, owned by main so a shutdown can
//join them all.
void AcceptLoop(InterestTable& table, int listener, std::vector<std::thread>& conns) {
    Waiter w;
    w.fd = listener;
    while (table.Park(w, nlang::IO_EV_READ) != 0u) {
        for (;;) {
            const int c = nlang::NetAccept(listener);
            if (c == nlang::kNetWouldBlock || c < 0)
                break;                  //nothing more completed right now
            conns.emplace_back([&table, c] { ServeConn(table, c); });
        }
    }
    table.Close(w);
}

InterestTable* g_table = nullptr;   //for the Ctrl+C path only

void OnSignal(int) {
    if (g_table)
        g_table->Shutdown();          //takes a mutex: fine in a probe, not in
                                      //a signal handler one would ship
}

}  //namespace

int main(int argc, char** argv) {
    int port = kControlDefaultPort;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--port=", 7) == 0) {
            port = std::atoi(argv[i] + 7);
            continue;
        }
        std::fprintf(stderr, "usage: %s [--port=N]  (mode=%s, fixed at build time)\n",
                     argv[0], kModeName);
        return 2;
    }

    if (!nlang::NetPlatformInit()) {
        std::fprintf(stderr, "control: socket init failed: %s\n",
                     nlang::NetLastErrorText().c_str());
        return 1;
    }
#ifdef CONTROL_TIMEBEGIN
    //Asks for 1 ms timer resolution for this process's lifetime; the request
    //is released when the process exits, which is why the probe script's
    //verified teardown is what puts the machine back.
    timeBeginPeriod(1);
#endif
    const int listener = nlang::NetOpenTcpStream(nlang::NetFamily::Inet);
    if (listener < 0 || nlang::NetBind(listener, kListenAddress, port) != 0
        || nlang::NetListen(listener, kListenBacklog) != 0) {
        std::fprintf(stderr, "control: bind %s:%d failed: %s\n",
                     kListenAddress, port, nlang::NetLastErrorText().c_str());
        return 1;
    }

    void* poller = nlang::IoPollerCreate();
    if (!poller) {
        std::fprintf(stderr, "control: poller creation failed\n");
        return 1;
    }
    InterestTable table(poller);
    g_table = &table;
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    std::thread pollerThread([&table] { table.Run(); });
    std::vector<std::thread> conns;
    std::thread acceptor([&table, listener, &conns] { AcceptLoop(table, listener, conns); });

    std::printf("control listening on %s:%d mode=%s\n",
                kListenAddress, nlang::NetLocalPort(listener), kModeName);
    std::fflush(stdout);            //the driver script reads this line as its
                                    //readiness signal

    acceptor.join();                //returns once a signal or a dead listener
                                    //ends the accept park
    table.Shutdown();
    pollerThread.join();
    for (std::thread& t : conns)
        t.join();
    nlang::IoPollerClose(poller);
    nlang::NetClose(listener);
#ifdef CONTROL_TIMEBEGIN
    timeEndPeriod(1);       //the normal-exit path; termination also releases it
#endif
    return 0;
}
