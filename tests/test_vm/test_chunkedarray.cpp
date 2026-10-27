/*---
    test_chunkedarray.cpp - K8 Task 2: chunked block storage tests.

    Real threads verify the reader/writer contract: Emplace runs under
    external growth serialization, Size()/At() are safe concurrently with
    it (element addresses are stable, size publishes after construction).
---*/
#include "ChunkedArray.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
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

static void AppendAcrossBoundary() {
    TEST(append_across_boundary);
    ChunkedArray<int32_t> arr;   //1024-elem chunks by default
    for (int32_t i = 0; i < 2500; ++i) {
        const size_t idx = arr.Emplace(i * 3);
        CHECK(idx == static_cast<size_t>(i), "index mismatch");
    }
    CHECK(arr.size() == 2500, "size mismatch");
    for (int32_t i = 0; i < 2500; ++i)
        CHECK(arr.At(static_cast<size_t>(i)) == i * 3, "value lost across chunks");
    PASS();
}

static void VariableLengthElements() {
    TEST(variable_length_elements);
    using Slot = std::vector<int32_t>;
    ChunkedArray<Slot> arr;
    const size_t a = arr.Emplace(7, 0);      //7 zeros
    const size_t b = arr.Emplace();          //default: empty
    Slot src{1, 2, 3};
    const size_t c = arr.Emplace(src);       //copy
    CHECK(arr.At(a).size() == 7 && arr.At(a)[6] == 0, "sized element wrong");
    CHECK(arr.At(b).empty(), "default element wrong");
    CHECK(arr.At(c) == src, "copied element wrong");
    arr.At(a)[0] = 42;
    CHECK(arr.At(a)[0] == 42, "element reference not stable");
    PASS();
}

static void ClearResets() {
    TEST(clear_resets);
    ChunkedArray<int32_t> arr;
    for (int i = 0; i < 3000; ++i) arr.Emplace(i);
    arr.clear();
    CHECK(arr.size() == 0, "size not reset");
    const size_t idx = arr.Emplace(7);
    CHECK(idx == 0 && arr.At(0) == 7, "reuse after clear broken");
    PASS();
}

static void CapacityCeiling() {
    TEST(capacity_ceiling);
    //small instantiation: 8 elems x 2 chunks x 2 tiers = 32
    ChunkedArray<int32_t, 8, 2, 2> arr;
    bool threw = false;
    try {
        for (int i = 0; i < 40; ++i) arr.Emplace(i);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw, "capacity exhaustion did not raise a clean error");
    CHECK(arr.size() == 32, "size beyond capacity");
    PASS();
}

static void ConcurrentReadWithGrowth() {
    TEST(concurrent_read_with_growth);
    //1024 elems x 4 chunks x 4 tiers = 16384 capacity; grow 12000
    ChunkedArray<uint64_t, 1024, 4, 4> arr;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> readCount{0}, readErrors{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t) {
        readers.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                const size_t n = arr.size();   //snapshot
                for (size_t j = 0; j < n; ++j) {
                    if (arr.At(j) != j) ++readErrors;   //each slot holds its index
                }
                readCount.fetch_add(n);
            }
        });
    }
    for (uint64_t i = 0; i < 12000; ++i)
        arr.Emplace(i);   //grower: serialized by contract (single writer here)
    //let the readers actually run before tearing down (thread startup can
    //outrace the whole growth loop)
    for (int spin = 0; spin < 2000 && readCount.load() == 0; ++spin)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    stop = true;
    for (auto& r : readers) r.join();
    CHECK(readErrors.load() == 0, "torn or lost values during concurrent growth");
    CHECK(readCount.load() > 0, "readers never observed data");
    PASS();
}

int main() {
    std::cerr << "test_chunkedarray:\n";
    AppendAcrossBoundary();
    VariableLengthElements();
    ClearResets();
    CapacityCeiling();
    ConcurrentReadWithGrowth();
    std::cerr << "  " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
