/*---
    VmDomainLocks.h - K8: per-domain store locks replacing the single
    K7 store lock. Domains follow the spec's write-kind classification:
    Container < Sync < Stream < StrMaint < Growth is the fixed acquisition
    order; a reverse acquisition trips the wire (terminate in production).
    Leaf domains (StrMaint, Growth) never nest outward.
---*/
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <utility>

namespace nlang {

enum class DomainId : uint8_t {
    Container = 0,   //List/Dict structure and reads
    Sync = 1,        //mutex/channel side tables and waiter registry
    Stream = 2,      //byte/file stream stores
    StrMaint = 3,    //cons flatten, const-string cache (leaf)
    Growth = 4,      //shard/chunked-array growth (leaf)
    Count = 5
};

class DomainLockSet {
public:
    std::mutex& Mutex(DomainId id) const { return m_mutexes[Idx(id)]; }

    uint64_t Acquisitions(DomainId id) const { return m_acquisitions[Idx(id)].load(std::memory_order_relaxed); }
    uint64_t Contended(DomainId id) const { return m_contended[Idx(id)].load(std::memory_order_relaxed); }
    uint64_t WaitUs(DomainId id) const { return m_waitUs[Idx(id)].load(std::memory_order_relaxed); }

    //Production default: diagnose and terminate. Tests install a recorder.
    void SetTripwireCallback(std::function<DomainId(DomainId)> cb) { m_tripwire = std::move(cb); }

    void AcquireChecked(DomainId id) const {
        //Holding any higher-ordered domain while acquiring a lower one is
        //a reverse acquisition — deadlock-shaped, trip the wire.
        const uint32_t held = s_heldMask;
        if (held >> (Idx(id) + 1)) {
            if (m_tripwire) {
                m_tripwire(id);   //test recorder: report and continue
            } else {
                std::fprintf(stderr, "domain lock order violated: acquiring %u with held mask %x\n",
                             static_cast<unsigned>(id), held);
                std::terminate();
            }
        }
        const auto start = std::chrono::steady_clock::now();
        if (!m_mutexes[Idx(id)].try_lock()) {
            m_contended[Idx(id)].fetch_add(1, std::memory_order_relaxed);
            m_mutexes[Idx(id)].lock();
            const auto waited = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start);
            m_waitUs[Idx(id)].fetch_add(static_cast<uint64_t>(waited.count()),
                                        std::memory_order_relaxed);
        }
        m_acquisitions[Idx(id)].fetch_add(1, std::memory_order_relaxed);
        s_heldMask |= (1u << Idx(id));
    }

    void Release(DomainId id) const {
        s_heldMask &= ~(1u << Idx(id));
        m_mutexes[Idx(id)].unlock();
    }

private:
    static size_t Idx(DomainId id) { return static_cast<size_t>(id); }
    //Per-thread currently-held mask (outermost acquisitions only — the
    //guard bumps depth before nesting skips AcquireChecked).
    inline static thread_local uint32_t s_heldMask = 0;

    mutable std::mutex m_mutexes[static_cast<size_t>(DomainId::Count)];
    mutable std::atomic<uint64_t> m_acquisitions[static_cast<size_t>(DomainId::Count)] = {};
    mutable std::atomic<uint64_t> m_contended[static_cast<size_t>(DomainId::Count)] = {};
    mutable std::atomic<uint64_t> m_waitUs[static_cast<size_t>(DomainId::Count)] = {};
    mutable std::function<DomainId(DomainId)> m_tripwire;
};

//RAII guard with per-domain reentrancy marker: nesting the same domain on
//the owning thread is a no-op, so composite sites can guard leaf work
//inside an already-held domain.
class DomainGuard {
public:
    DomainGuard(const DomainLockSet& set, DomainId id)
        : m_set(set), m_id(id) {
        uint32_t& depth = ThreadDepth(id);
        if (depth > 0) {
            m_acquired = false;
        } else {
            set.AcquireChecked(id);   //tripwire + telemetry + lock
            m_acquired = true;
        }
        ++depth;
    }
    ~DomainGuard() {
        --ThreadDepth(m_id);
        if (m_acquired) m_set.Release(m_id);
    }
    DomainGuard(const DomainGuard&) = delete;
    DomainGuard& operator=(const DomainGuard&) = delete;

private:
    static uint32_t& ThreadDepth(DomainId id) {
        static thread_local uint32_t depths[static_cast<size_t>(DomainId::Count)] = {};
        return depths[static_cast<size_t>(id)];
    }
    const DomainLockSet& m_set;
    DomainId m_id;
    bool m_acquired;
};

}   //namespace nlang
