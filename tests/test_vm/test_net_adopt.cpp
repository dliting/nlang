/*---
    test_net_adopt.cpp — the claim-adopt critical section (final
    review I2, external review C1): a call claims its direction at
    entry and must never adopt a foreign context's claim. Pre-fix
    NetAdoptClaim left a fresh call unclaimed until it parked (C1:
    two fresh same-direction calls then raced for the bytes, the
    second resetting the first's writeSent and splitting or
    duplicating the stream) and accepted a foreign waiter as its
    own (I2: a fresh write reset the parked writer's progress). The
    e2e writer test (stdlib_net_tcp_concurrent_writer.n) cannot pin
    these on Windows loopback — the auto-tuned send buffer absorbs
    successive writes unpredictably, so the parked state the bugs
    need is not deterministic there; this unit test is the
    deterministic proof.
---*/
#include "net/net_state.h"
#include "net/net_tcp_park.h"

#include <cstdio>
#include <mutex>

using namespace nlang;   //entries live in nlang:: (Task 1 ruling)

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

static int WaiterOf(const NetSocketSlot* s, NetDir dir) {
    return (dir == NetDir::Read) ? s->readWaiter
        : (dir == NetDir::Write) ? s->writeWaiter : s->acceptWaiter;
}

//A foreign context holding the direction must be rejected (-3) with
//the claim untouched — the caller never claims, so it must not touch
//the direction's per-direction state (writeSent).
static void TestAdoptRejectsForeignWaiter() {
    NetExecState st;
    int32_t h = 0;
    {
        std::lock_guard<std::mutex> g(st.m_mutex);
        h = st.m_slots.Mint(/*fd*/5, kNetKindTcpStream);
        st.m_slots.SetWaiter(h, NetDir::Read, /*ctxA*/1);
        st.m_slots.SetWaiter(h, NetDir::Write, /*ctxA*/1);
        st.m_slots.SetWaiter(h, NetDir::Accept, /*ctxA*/1);
    }
    const NetDir dirs[] = {NetDir::Read, NetDir::Write, NetDir::Accept};
    for (NetDir dir : dirs) {
        int fd = -1;
        bool claimed = false;
        CHECK(NetAdoptClaim(&st, h, dir, /*ctxB*/2, &fd, &claimed)
            == kNetAdoptConcurrent);
        CHECK(!claimed);
        std::lock_guard<std::mutex> g(st.m_mutex);
        CHECK(WaiterOf(st.m_slots.Lookup(h), dir) == 1);   //A keeps it
    }
}

//Re-entry after a park: the waiter is this call's own context id, so
//the claim is adopted and the fd handed out.
static void TestAdoptAdoptsOwnClaim() {
    NetExecState st;
    int32_t h = 0;
    {
        std::lock_guard<std::mutex> g(st.m_mutex);
        h = st.m_slots.Mint(/*fd*/7, kNetKindTcpStream);
        st.m_slots.SetWaiter(h, NetDir::Write, /*self*/3);
    }
    int fd = -1;
    bool claimed = false;
    CHECK(NetAdoptClaim(&st, h, NetDir::Write, /*self*/3, &fd, &claimed)
        == kNetAdoptOk);
    CHECK(claimed);
    CHECK(fd == 7);
}

//External review C1: a fresh call claims the direction at entry —
//the claim must not wait for the park, or two fresh same-direction
//calls race for the bytes (the second resets the first's writeSent
//and the stream splits or duplicates). A fresh call claims, a
//second fresh call on the same direction is rejected, and the
//other direction stays free.
static void TestAdoptFreshCallClaims() {
    NetExecState st;
    int32_t h = 0;
    {
        std::lock_guard<std::mutex> g(st.m_mutex);
        h = st.m_slots.Mint(/*fd*/9, kNetKindTcpStream);
    }
    int fd = -1;
    bool claimed = false;
    CHECK(NetAdoptClaim(&st, h, NetDir::Read, /*self*/4, &fd, &claimed)
        == kNetAdoptOk);
    CHECK(claimed);
    CHECK(fd == 9);
    {
        std::lock_guard<std::mutex> g(st.m_mutex);
        CHECK(st.m_slots.Lookup(h)->readWaiter == 4);   //claimed at entry
    }
    //Callers initialize claimed=false; the Concurrent branch leaves
    //it untouched (the caller never held the direction).
    bool claimed2 = false;
    CHECK(NetAdoptClaim(&st, h, NetDir::Read, /*ctxB*/5, &fd, &claimed2)
        == kNetAdoptConcurrent);
    CHECK(!claimed2);
    CHECK(NetAdoptClaim(&st, h, NetDir::Write, /*ctxB*/5, &fd, &claimed2)
        == kNetAdoptOk);
    CHECK(claimed2);
}

//The closer won the window: Gone, and the direction's waiter is
//drained so the R5 deferred close proceeds (OpenCount drops to 0).
static void TestAdoptGoneDrainsWaiter() {
    NetExecState st;
    int32_t h = 0;
    {
        std::lock_guard<std::mutex> g(st.m_mutex);
        h = st.m_slots.Mint(/*fd*/11, kNetKindTcpStream);
        st.m_slots.SetWaiter(h, NetDir::Write, /*ctxA*/1);
        st.m_slots.MarkClosed(h);
        CHECK(st.m_slots.OpenCount() == 1);   //fd kept while parked
    }
    int fd = -1;
    bool claimed = true;
    CHECK(NetAdoptClaim(&st, h, NetDir::Write, /*ctxB*/2, &fd, &claimed)
        == kNetAdoptGone);
    std::lock_guard<std::mutex> g(st.m_mutex);
    CHECK(st.m_slots.OpenCount() == 0);       //waiter drained: closed
}

//0.12.9 net pass I-1: a fresh call must be distinguishable from a
//park re-entry. NetTcpWrite resets writeSent only on fresh entry,
//so the discrimination is the correctness hinge for every exit that
//leaves the stash behind (write timeout, EPIPE): pre-fix the reset
//was dead code — claimed was unconditionally true — and the next
//write resumed at the stale offset (skipped prefix, or a bogus
//return with nothing sent).
static void TestAdoptReportsFreshVersusAdopted() {
    NetExecState st;
    int32_t h = 0;
    {
        std::lock_guard<std::mutex> g(st.m_mutex);
        h = st.m_slots.Mint(/*fd*/13, kNetKindTcpStream);
    }
    int fd = -1;
    bool claimed = false;
    bool fresh = false;
    CHECK(NetAdoptClaim(&st, h, NetDir::Write, /*self*/6, &fd, &claimed,
        &fresh) == kNetAdoptOk);
    CHECK(claimed);
    CHECK(fresh);   //first entry: no prior waiter
    fresh = false;
    CHECK(NetAdoptClaim(&st, h, NetDir::Write, /*self again*/6, &fd,
        &claimed, &fresh) == kNetAdoptOk);   //re-entry after a park
    CHECK(claimed);
    CHECK(!fresh);  //adopted: the stash must survive
    {
        std::lock_guard<std::mutex> g(st.m_mutex);
        st.m_slots.ReleaseWaiter(h, NetDir::Write);
    }
    CHECK(NetAdoptClaim(&st, h, NetDir::Write, 6, &fd, &claimed,
        &fresh) == kNetAdoptOk);
    CHECK(fresh);   //post-timeout release: the next call restarts
    //Rejection writes nothing: a foreign waiter keeps fresh untouched.
    fresh = false;
    CHECK(NetAdoptClaim(&st, h, NetDir::Write, /*ctxB*/7, &fd, &claimed,
        &fresh) == kNetAdoptConcurrent);
    CHECK(!fresh);
}

int main() {
    TestAdoptRejectsForeignWaiter();
    TestAdoptAdoptsOwnClaim();
    TestAdoptFreshCallClaims();
    TestAdoptGoneDrainsWaiter();
    TestAdoptReportsFreshVersusAdopted();
    std::printf(g_failures ? "FAILED %d\n" : "ok\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
