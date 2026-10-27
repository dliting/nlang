/*---
    ChunkedArray.h - K8: block-backed array with stable element addresses.

    Replaces the vector-based heap parallel arrays so growth (which
    appends chunks) never moves existing elements. Thread contract:
    Emplace()/clear() run under external growth serialization (the store
    lock today, the Growth domain lock from K8 Task 3); size()/At() are
    safe concurrently with Emplace — the element is constructed before
    the size store publishes it, and tier/chunk pointers are published
    before that size store via release/acquire on the atomic size.
---*/
#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>

namespace nlang {

inline constexpr size_t DEFAULT_CHUNK_ELEMS = 1024;         //elements per chunk
inline constexpr size_t DEFAULT_CHUNKS_PER_TIER = 64;       //chunks per tier
inline constexpr size_t DEFAULT_CHUNK_ROOT_ENTRIES = 64;    //tiers per array

template <typename T,
          size_t kChunkElems = DEFAULT_CHUNK_ELEMS,
          size_t kChunksPerTier = DEFAULT_CHUNKS_PER_TIER,
          size_t kRootEntries = DEFAULT_CHUNK_ROOT_ENTRIES>
class ChunkedArray {
public:
    ChunkedArray() = default;
    ~ChunkedArray() { clear(); }
    ChunkedArray(const ChunkedArray&) = delete;
    ChunkedArray& operator=(const ChunkedArray&) = delete;

    size_t size() const { return m_size.load(std::memory_order_acquire); }

    T& At(size_t idx) {
        assert(idx < size());
        return *Slot(idx);
    }
    const T& At(size_t idx) const {
        assert(idx < size());
        return *Slot(idx);
    }
    T& operator[](size_t idx) { return At(idx); }
    const T& operator[](size_t idx) const { return At(idx); }

    //Appends one element constructed from args; returns its index.
    //Caller holds the external growth serialization.
    template <typename... Args>
    size_t Emplace(Args&&... args) {
        const size_t idx = m_size.load(std::memory_order_relaxed);
        T* slot = EnsureSlot(idx);
        slot->~T();   //chunks are value-initialized; destroy before replace
        new (slot) T(std::forward<Args>(args)...);
        m_size.store(idx + 1, std::memory_order_release);
        return idx;
    }

    void clear() {
        const size_t n = size();
        for (size_t i = 0; i < n; ++i) At(i).~T();
        for (size_t t = 0; t < kRootEntries; ++t) {
            Tier* tier = m_tiers[t].load(std::memory_order_relaxed);
            if (!tier) continue;
            m_tiers[t].store(nullptr, std::memory_order_relaxed);
            delete tier;
        }
        m_size.store(0, std::memory_order_release);
    }

    static constexpr size_t capacity() {
        return kRootEntries * kChunksPerTier * kChunkElems;
    }

private:
    struct Tier {
        std::array<std::unique_ptr<T[]>, kChunksPerTier> chunks;
    };

    T* Slot(size_t idx) const {
        const size_t chunkNo = idx / kChunkElems;
        Tier* tier = m_tiers[chunkNo / kChunksPerTier].load(std::memory_order_acquire);
        //tier and chunk exist for every idx below the published size
        T* elems = tier->chunks[chunkNo % kChunksPerTier].get();
        return elems + (idx % kChunkElems);
    }

    T* EnsureSlot(size_t idx) {
        if (idx >= capacity())
            throw std::runtime_error("NLang VM: chunked store capacity exhausted");
        const size_t chunkNo = idx / kChunkElems;
        const size_t tierNo = chunkNo / kChunksPerTier;
        Tier* tier = m_tiers[tierNo].load(std::memory_order_relaxed);
        if (!tier) {
            tier = new Tier;
            m_tiers[tierNo].store(tier, std::memory_order_release);
        }
        auto& chunk = tier->chunks[chunkNo % kChunksPerTier];
        if (!chunk) chunk = std::make_unique<T[]>(kChunkElems);   //value-initialized
        return chunk.get() + (idx % kChunkElems);
    }

    std::atomic<Tier*> m_tiers[kRootEntries] = {};
    std::atomic<size_t> m_size{0};
};

}   //namespace nlang
