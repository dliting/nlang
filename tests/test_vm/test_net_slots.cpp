/*---
    test_net_slots.cpp — the per-executor socket slot table: handle
    minting with a generation tag (ABA), closed/lookup rules, and the
    close-waits-for-waiters fd lifetime rule (plan ruling R5).
---*/
#include "net/net_slots.h"
#include <cstdio>

using namespace nlang;   //entries live in nlang:: (Task 1 ruling)

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

static void TestMintLookupGeneration() {
    NetSocketTable table;
    const int32_t h = table.Mint(/*fd*/5, kNetKindTcpStream);
    CHECK(h != 0);
    NetSocketSlot* s = table.Lookup(h);
    CHECK(s != nullptr && s->fd == 5 && !s->closed);
    table.MarkClosed(h);
    CHECK(table.Lookup(h) == nullptr);          //closed slots reject
    //MarkClosed on a waiter-free slot also recycles it (M10): remint
    //reuses the index, the generation differs, the stale handle rejects.
    const int32_t indexMask = (1 << kNetHandleIndexBits) - 1;
    const int32_t h2 = table.Mint(/*fd*/9, kNetKindTcpStream);
    CHECK((h2 & indexMask) == (h & indexMask)); //index came back from the
                                                //free list, not a fresh slot
    CHECK((h2 >> kNetHandleIndexBits) != (h >> kNetHandleIndexBits));
    CHECK(table.Lookup(h) == nullptr);          //stale generation
    CHECK(table.Lookup(h2) != nullptr);
}

static void TestCloseWaitsForWaiters() {
    NetSocketTable table;
    const int32_t h = table.Mint(7, kNetKindUdp);
    table.SetWaiter(h, NetDir::Read, /*ctxId*/42);
    table.MarkClosed(h);
    CHECK(table.FdOf(h) == 7);                  //fd kept while parked
    table.ReleaseWaiter(h, NetDir::Read);
    CHECK(table.FdOf(h) == -1);                 //last waiter drained:
                                                //the real close ran
    CHECK(table.OpenCount() == 0);
}

int main() {
    TestMintLookupGeneration();
    TestCloseWaitsForWaiters();
    std::printf(g_failures ? "FAILED %d\n" : "ok\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
