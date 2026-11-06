/*---
    test_net_jobs.cpp — the job table's adoption rule (final review
    I4): FindOwn must skip records the requesting context already
    released. A released record with the worker still in flight stays
    in use (the worker keeps it alive until it fills), and pre-fix
    FindOwn returned it — a fresh ping adopted the timed-out job's
    record instead of submitting its own, reporting the stale job's
    result.
---*/
#include "net/net_jobs.h"

#include <cstdio>
#include <utility>
#include <vector>

using namespace nlang;   //entries live in nlang:: (Task 1 ruling)

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

//A live owned record is adopted; other kinds/owners are not.
static void TestFindOwnAdoptsLiveRecord() {
    NetJobTable table;
    const int32_t j = table.Mint(kNetJobIcmp, /*owner*/1);
    CHECK(table.FindOwn(kNetJobIcmp, 1) == j);
    CHECK(table.FindOwn(kNetJobResolve, 1) == 0);
    CHECK(table.FindOwn(kNetJobIcmp, 2) == 0);
}

//The released record (worker still in flight) is never adopted again.
static void TestFindOwnSkipsReleased() {
    NetJobTable table;
    const int32_t j = table.Mint(kNetJobIcmp, /*owner*/1);
    table.Release(j);                       //timeout path: not done yet
    CHECK(table.Lookup(j) != nullptr);      //worker keeps it alive
    CHECK(table.FindOwn(kNetJobIcmp, 1) == 0);   //must not re-adopt
}

//Release + done frees the record; a later Mint of the same kind and
//owner starts clean and is adopted.
static void TestReleasedFreesOnDone() {
    NetJobTable table;
    const int32_t j = table.Mint(kNetJobIcmp, /*owner*/1);
    table.Release(j);
    table.MarkDoneIcmp(j, /*rttMs*/5, /*err*/0);
    CHECK(table.Lookup(j) == nullptr);      //both flags: freed
    CHECK(table.FindOwn(kNetJobIcmp, 1) == 0);
    const int32_t j2 = table.Mint(kNetJobIcmp, /*owner*/1);
    CHECK(table.FindOwn(kNetJobIcmp, 1) == j2);
}

int main() {
    TestFindOwnAdoptsLiveRecord();
    TestFindOwnSkipsReleased();
    TestReleasedFreesOnDone();
    std::printf(g_failures ? "FAILED %d\n" : "ok\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
