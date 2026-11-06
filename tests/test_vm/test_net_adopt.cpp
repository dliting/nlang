/*---
    test_net_adopt.cpp — the claim-adopt critical section (final review
    I2): a fresh call must never adopt a foreign context's claim on a
    direction. Pre-fix NetAdoptClaim accepted any waiter state as its
    own, so a second write reset the parked writer's writeSent
    progress and the stream carried duplicated bytes. The e2e writer
    test (stdlib_net_tcp_concurrent_writer.n) cannot pin this on
    Windows loopback — the auto-tuned send buffer absorbs successive
    writes unpredictably, so the parked state the bug needs is not
    deterministic there; this unit test is the deterministic proof.
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

//A fresh call on an unclaimed direction: live slot, no claim yet.
static void TestAdoptFreshCallUnclaimed() {
    NetExecState st;
    int32_t h = 0;
    {
        std::lock_guard<std::mutex> g(st.m_mutex);
        h = st.m_slots.Mint(/*fd*/9, kNetKindTcpStream);
    }
    int fd = -1;
    bool claimed = true;
    CHECK(NetAdoptClaim(&st, h, NetDir::Read, /*self*/4, &fd, &claimed)
        == kNetAdoptOk);
    CHECK(!claimed);
    CHECK(fd == 9);
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

int main() {
    TestAdoptRejectsForeignWaiter();
    TestAdoptAdoptsOwnClaim();
    TestAdoptFreshCallUnclaimed();
    TestAdoptGoneDrainsWaiter();
    std::printf(g_failures ? "FAILED %d\n" : "ok\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
