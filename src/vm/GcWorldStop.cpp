/*---
    GcWorldStop.cpp — GC stop-the-world rendezvous (concurrency K6)
---*/
#include "GcWorldStop.h"

namespace nlang {
//Per-thread evaluation-window depth. Static (not per-instance): nested
//windows of DIFFERENT executors on one thread would double-count into
//either instance's total; the codebase never stacks two executors'
//RunFrames on one thread (the embedding host drives one executor at a
//time per thread), and the count the protocol needs is per-scheduler
//anyway — noted here so the assumption is visible, not accidental.
thread_local int GcWorldStop::t_evalDepth = 0;
//Parked-window depth saves (LIFO — see SuspendEvalWindow's contract).
thread_local std::vector<int> GcWorldStop::t_suspendedDepths;
} // namespace nlang
