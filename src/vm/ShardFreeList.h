/*---
    ShardFreeList.h - K8: per-carrier free-index stacks with round-robin
    block mapping. The lock-free path pops the calling carrier's own
    stack only; growth appends one block (DEFAULT_SHARD_BLOCK indices)
    and pushes it into the shard that exhausted — never into another
    shard's stack, so no cross-shard writes race the lock-free pops.
    Sweep (quiescent at the GC rendezvous) is the one place allowed to
    touch every stack: it clears all shards and redistributes free
    indices by the fixed block->shard mapping.
---*/
#pragma once

#include <cstdint>
#include <vector>

namespace nlang {

inline constexpr size_t DEFAULT_SHARD_BLOCK = 1024;   //indices per growth block

class ShardFreeList {
public:
    void Configure(size_t shardCount) { m_stacks.assign(shardCount, {}); }
    size_t ShardCount() const { return m_stacks.size(); }
    bool Empty(size_t shard) const { return m_stacks[shard].empty(); }
    int32_t Pop(size_t shard) {
        std::vector<int32_t>& stack = m_stacks[shard];
        const int32_t v = stack.back();
        stack.pop_back();
        return v;
    }
    //Sweep/growth feed; single owner (own shard on the growth path, the
    //collector on the sweep path).
    void Push(size_t shard, int32_t v) { m_stacks[shard].push_back(v); }
    //Fresh block [base, base+count): pushed descending so pops yield
    //ascending indices — the fresh-allocation order of the unsharded
    //store this design must reproduce on a single carrier.
    void PushBlockDescending(size_t shard, int32_t base, size_t count) {
        std::vector<int32_t>& stack = m_stacks[shard];
        stack.reserve(stack.size() + count);
        for (size_t k = count; k > 0; --k)
            stack.push_back(static_cast<int32_t>(base + k - 1));
    }
    void ClearAll() { for (auto& s : m_stacks) s.clear(); }
    //Fixed mapping for sweep redistribution: block round-robin.
    size_t SweepShardOf(int32_t idx) const {
        return (static_cast<size_t>(idx) / DEFAULT_SHARD_BLOCK) % m_stacks.size();
    }

private:
    std::vector<std::vector<int32_t>> m_stacks;
};

}   //namespace nlang
