/*---
    test_shardalloc.cpp - K8 Task 3: sharded free-list tests.

    Real threads drive the production pop-or-grow choreography (own-shard
    pop, Growth domain lock, block push) and verify: ascending fresh
    allocation on a single shard (the byte-identity contract), uniqueness
    across concurrent shards, sweep redistribution by the block mapping.
---*/
#include "ShardFreeList.h"
#include "VmDomainLocks.h"
#include <atomic>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define TEST(name) \
    do { std::cerr << "  " << #name << " ... "; } while(0)
#define PASS() \
    do { ++g_pass; std::cerr << "OK\n"; } while(0)
#define FAIL(msg) \
    do { ++g_fail; std::cerr << "FAIL: " << msg << "\n"; } while(0)
#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)

//Production choreography: pop own shard; on exhaustion take the Growth
//domain lock (double-checked), append a block to the backing size, push
//it into the requesting shard, pop.
struct GrowOnDemand {
    ShardFreeList& shards;
    DomainLockSet& locks;
    std::atomic<size_t>& storeSize;
    size_t PopOrGrow(size_t shard) {
        if (shards.Empty(shard)) {
            DomainGuard g(locks, DomainId::Growth);
            if (shards.Empty(shard)) {
                const int32_t base = static_cast<int32_t>(storeSize.load());
                storeSize.fetch_add(DEFAULT_SHARD_BLOCK);
                shards.PushBlockDescending(shard, base, DEFAULT_SHARD_BLOCK);
            }
        }
        return static_cast<size_t>(shards.Pop(shard));
    }
};

static void SingleShardAscendingFresh() {
    TEST(single_shard_ascending_fresh);
    ShardFreeList shards;
    shards.Configure(1);
    DomainLockSet locks;
    std::atomic<size_t> storeSize{1};   //index 0 is the sentinel
    GrowOnDemand alloc{shards, locks, storeSize};
    for (int32_t expect = 1; expect <= 3000; ++expect) {
        const int32_t got = static_cast<int32_t>(alloc.PopOrGrow(0));
        CHECK(got == expect, "fresh allocation broke ascending order");
    }
    PASS();
}

static void ConcurrentPopUniqueness() {
    TEST(concurrent_pop_uniqueness);
    const size_t kShards = 4;
    ShardFreeList shards;
    shards.Configure(kShards);
    DomainLockSet locks;
    std::atomic<size_t> storeSize{1};
    GrowOnDemand alloc{shards, locks, storeSize};

    std::atomic<bool> go{false};
    std::vector<std::thread> workers;
    std::vector<std::vector<int32_t>> taken(kShards);
    for (size_t t = 0; t < kShards; ++t) {
        workers.emplace_back([&, t]() {
            while (!go.load()) std::this_thread::yield();
            for (int i = 0; i < 5000; ++i)
                taken[t].push_back(static_cast<int32_t>(alloc.PopOrGrow(t)));
        });
    }
    go = true;
    for (auto& w : workers) w.join();

    std::set<int32_t> all;
    for (size_t t = 0; t < kShards; ++t) {
        CHECK(taken[t].size() == 5000, "a worker lost indices");
        for (int32_t v : taken[t]) {
            CHECK(v > 0, "index below the sentinel handed out");
            CHECK(all.insert(v).second, "duplicate index across shards");
        }
    }
    CHECK(storeSize.load() >= 20001, "store did not grow enough");
    //blocks pushed while the lock was held must never exceed capacity
    CHECK(storeSize.load() % DEFAULT_SHARD_BLOCK == 1, "growth broke block alignment");
    PASS();
}

static void SweepRedistribution() {
    TEST(sweep_redistribution);
    ShardFreeList shards;
    shards.Configure(2);
    shards.PushBlockDescending(0, 1, DEFAULT_SHARD_BLOCK * 2 + 16);
    //simulate the sweep: clear everything, redistribute a free set
    shards.ClearAll();
    std::vector<int32_t> freeSet;
    for (int32_t i = 1; i <= 2500; ++i) freeSet.push_back(i);
    for (int32_t v : freeSet)
        shards.Push(shards.SweepShardOf(v), v);
    //each shard holds exactly its mapped indices, ascending in the stack
    //so pops come out highest-mapped-first within the shard
    for (size_t s = 0; s < 2; ++s) {
        int32_t prev = -1;
        size_t count = 0;
        while (!shards.Empty(s)) {
            const int32_t v = shards.Pop(s);
            CHECK(v < prev || prev < 0, "shard pops out of scan order");
            CHECK(shards.SweepShardOf(v) == s, "index redistributed to wrong shard");
            prev = v;
            ++count;
        }
        //block mapping, not element parity: shard 0 owns blocks 0 and 2
        //(indices 1..1023 plus 2048..2500), shard 1 owns block 1
        const size_t expected = (s == 0) ? (1023 + 453) : 1024;
        CHECK(count == expected, "redistribution uneven");
    }
    PASS();
}

static void GrowthNeverTouchesForeignShards() {
    TEST(growth_never_touches_foreign_shards);
    ShardFreeList shards;
    shards.Configure(3);
    DomainLockSet locks;
    std::atomic<size_t> storeSize{1};
    GrowOnDemand alloc{shards, locks, storeSize};

    //shard 1 grows; shards 0 and 2 must remain empty (no cross-push)
    (void)alloc.PopOrGrow(1);
    CHECK(shards.Empty(0) && shards.Empty(2),
          "growth pushed into a foreign shard");
    CHECK(!shards.Empty(1), "growing shard did not receive its block");
    //drain shard 1 fully — one block minus the index already popped
    size_t drained = 0;
    while (!shards.Empty(1)) { shards.Pop(1); ++drained; }
    CHECK(drained == DEFAULT_SHARD_BLOCK - 1, "shard 1 holds more than one block");
    PASS();
}

int main() {
    std::cerr << "test_shardalloc:\n";
    SingleShardAscendingFresh();
    ConcurrentPopUniqueness();
    SweepRedistribution();
    GrowthNeverTouchesForeignShards();
    std::cerr << "  " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
