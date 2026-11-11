/*---
test_net_icmp.cpp - the ping helper's bounded-slice contract (external
review I3).

Pins the three pure verdicts behind the ping loop: the helper timeout is
always bounded by the echo slice (one job never holds a pool worker
longer than one slice, so teardown and the priority-0 DNS queue are
never hostage to a black-hole host), a slice timeout re-arms the job
while the script deadline still allows (retransmission), and the queue
lane follows the deadline (D6: a bounded wait gets the priority lane,
the unbounded wait the ordinary one). The loop itself drives these
through the pool; the verdicts are pure and unit-testable on their own
(I5/I6 precedent). Console-style suite (same shape as test_net_platform).
---*/
#include "net/net_icmp.h"
#include "net/net_state.h"

#include <cstdio>

using namespace nlang;

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

//The helper timeout is bounded by the slice: an unlimited deadline
//(0) gets one slice, a longer remainder is capped to one slice, a
//shorter remainder is exact, a passed deadline is 0.
static void TestEchoTimeoutSlice()
{
    const long long now = 1000000;
    CHECK(IcmpEchoTimeout(0, now) == kIcmpEchoSliceMs);
    CHECK(IcmpEchoTimeout(now + 3000, now) == 3000);
    CHECK(IcmpEchoTimeout(now + kIcmpEchoSliceMs, now) == kIcmpEchoSliceMs);
    CHECK(IcmpEchoTimeout(now + kIcmpEchoSliceMs + 4000, now) == kIcmpEchoSliceMs);
    CHECK(IcmpEchoTimeout(now, now) == 0);
    CHECK(IcmpEchoTimeout(now - 1, now) == 0);
}

//A slice timeout re-arms while the deadline allows (an unlimited
//deadline always allows); every other verdict — reply, unreachable,
//passed deadline — is terminal.
static void TestResubmitVerdict()
{
    const long long now = 1000000;
    CHECK(IcmpShouldResubmit(kNetErrTimeout, 0, now));          //unlimited
    CHECK(IcmpShouldResubmit(kNetErrTimeout, now + 1, now));    //deadline ahead
    CHECK(!IcmpShouldResubmit(kNetErrTimeout, now, now));       //deadline passed
    CHECK(!IcmpShouldResubmit(kNetErrTimeout, now - 1, now));   //deadline passed
    CHECK(!IcmpShouldResubmit(0, 0, now));                      //reply
    CHECK(!IcmpShouldResubmit(kNetErrPlatform, 0, now));        //unreachable
}

//D6: a ping with no deadline waits forever, so it has no latency of its own to
//protect and belongs in the ordinary lane; a deadline-bound ping keeps the
//high-priority lane it needs to answer inside its own budget.
static void TestJobPriority()
{
    CHECK(IcmpJobPriority(0) == 0);      //the only "no deadline" net.n sends
    CHECK(IcmpJobPriority(1) == 1);      //absolute deadline already passed
    CHECK(IcmpJobPriority(5000) == 1);   //a deadline, however small
}

int main()
{
    TestEchoTimeoutSlice();
    TestResubmitVerdict();
    TestJobPriority();
    if (g_failures > 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
