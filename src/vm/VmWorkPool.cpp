/*---
    VmWorkPool.cpp — see VmWorkPool.h. The queue lock is a leaf: it is
    never held across fn (workers run jobs GIL-free) and never taken
    under the GIL by a submitter that then blocks.
---*/
#include "VmWorkPool.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace nlang {

namespace {

//One-byte completion ack. The fd is caller-owned (a socketpair member
//or a datagram socket); a failed or short ack is not an error — the
//job already ran, and the waiter's own deadline covers a lost ack.
void SendAckByte(int fd) {
    if (fd < 0)
        return;
    const char byte = 1;
    const char* p = &byte;
    size_t left = 1;
    while (left > 0) {
#ifdef _WIN32
        int n = ::send(static_cast<unsigned int>(fd), p,
                       static_cast<int>(left), 0);
#else
        ssize_t n = ::send(fd, p, left, 0);
#endif
        if (n <= 0)
            break;
        p += n;
        left -= static_cast<size_t>(n);
    }
}

} // namespace

VmWorkPool::VmWorkPool(int threadCount)
    : m_threadCount(threadCount) {}

VmWorkPool::~VmWorkPool() {
    Shutdown();
}

void VmWorkPool::Submit(void (*fn)(void*), void* arg,
                        int ackFd, int priority) {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        Job job{fn, arg, ackFd};
        if (priority != 0)
            m_high.push_back(job);
        else
            m_normal.push_back(job);
        //Lazy start: workers exist only once there is work.
        while (!m_shutdown
               && m_threads.size() < static_cast<size_t>(m_threadCount))
            m_threads.emplace_back([this] { WorkerLoop(); });
    }
    m_cv.notify_all();
}

void VmWorkPool::WorkerLoop() {
    for (;;) {
        Job job{nullptr, nullptr, -1};
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [this] {
                return m_shutdown || !m_high.empty() || !m_normal.empty();
            });
            //Drain-first: a queued job outranks the stop flag, so
            //every pre-Shutdown submit runs before this worker exits.
            if (!m_high.empty()) {
                job = m_high.front();
                m_high.pop_front();
            } else if (!m_normal.empty()) {
                job = m_normal.front();
                m_normal.pop_front();
            } else {
                return;                  //shutdown && drained
            }
        }
        job.fn(job.arg);                 //no GIL held (contract)
        SendAckByte(job.ackFd);
    }
}

void VmWorkPool::Shutdown() {
    std::vector<std::thread> toJoin;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_shutdown && m_threads.empty())
            return;                      //idempotent (already joined)
        m_shutdown = true;
        toJoin.swap(m_threads);
    }
    m_cv.notify_all();
    for (std::thread& t : toJoin)
        t.join();
}

int VmWorkPool::Running() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return static_cast<int>(m_threads.size());
}

} // namespace nlang
