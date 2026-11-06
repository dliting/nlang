/*---
    VmWorkPool.h — generic native worker pool (r1 note 4: the concurrent
    library's native engine layer, surfaced through the ABI v11
    submitWork callback). Workers hold no GIL by contract: a job is
    native code (a resolver lookup, a syscall burst) that must never
    touch VM stores. A completion optionally acks one byte to a
    caller-owned socket fd — the waiting context parks on that fd
    (io.parkForIo), so the handoff costs no scheduler wake machinery.
    Two-level priority queue: a slow getaddrinfo never starves short
    jobs (libuv uv_queue_work precedent). Lazily started on the first
    submit; Shutdown drains the queue, then joins.
---*/
#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace nlang {

class VmWorkPool {
public:
    //Default worker count: libuv's default thread pool size (4) —
    //enough to overlap resolver/short jobs without oversubscribing.
    static constexpr int DEFAULT_WORK_POOL_THREADS = 4;

    explicit VmWorkPool(int threadCount = DEFAULT_WORK_POOL_THREADS);
    ~VmWorkPool();                       //Shutdown

    VmWorkPool(const VmWorkPool&) = delete;
    VmWorkPool& operator=(const VmWorkPool&) = delete;

    //Queue fn(arg); on completion the worker sends one byte to ackFd
    //(-1 = no ack). priority != 0 runs before queued priority-0 jobs.
    //Starts the workers lazily. Submitting after Shutdown queues the
    //job but starts nothing (caller error; the scheduler stops
    //submitting before joining the pool).
    void Submit(void (*fn)(void*), void* arg, int ackFd, int priority);

    //Set the stop flag and join every worker. Idempotent. The flag
    //takes effect only after the queue drains: every job submitted
    //before Shutdown runs before the workers exit (a net-module
    //teardown cv-waits on its queued jobs — the drain satisfies it).
    void Shutdown();

    //Number of started workers (0 before the first submit, 0 after
    //Shutdown — observable laziness for tests and diagnostics).
    int Running() const;

private:
    struct Job {
        void (*fn)(void*);
        void* arg;
        int ackFd;
    };
    void WorkerLoop();

    int m_threadCount;
    bool m_shutdown = false;             //drain-first stop flag
    std::deque<Job> m_high;              //priority 1, FIFO
    std::deque<Job> m_normal;            //priority 0, FIFO
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<std::thread> m_threads;  //started lazily on first Submit
};

} // namespace nlang
