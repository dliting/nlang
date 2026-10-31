// --- K7 Task 5: freeze parking without the global lock (debugger) ---
// Four scenarios for the K7 debugger rework:
//  (a) a freeze parks the frozen carrier and releases the global lock:
//      another carrier's in-flight segment completes its output write
//      and parks on its channel receive while the window stays open;
//      queued contexts get no new dispatch until resume.
//  (b) regression pin: a breakpoint inside a toString override (nested
//      evaluation window) stops on the override's own line and depth —
//      the freeze protocol must stay window-depth-agnostic.
//  (c) breakpoint-table edits hammered from the frozen window must stay
//      consistent against other carriers' concurrent checkpoints (the
//      controller state mutex; re-run under the ASan leg).
//  (d) two near-simultaneous breakpoint hits: the second freezer queues;
//      its hit is decided at its own checkpoint, so withdrawing the
//      second breakpoint during the first window does not cancel it —
//      after the first window closes, the queued stop opens its own.
// (a) and (d) are RED against the pre-K7 freeze (the whole freeze held
// the global lock): in (a) the peer carrier stalls at its next store op
// so neither the write nor the receive park is observable during the
// window; in (d) the peer's checkpoint stalls on the lock and its hit
// decision happens only after the resume — a breakpoint withdrawn during
// the first window silences it.
#include "test_debugger_common.h"
#include <chrono>
#include <mutex>
#include <thread>

using namespace nlang;

//Thread-safe output capturer: workers append from carrier threads while
//the frozen window's pump thread polls (the plain string capturer in
//test_threads.cpp is single-threaded only).
class MutexIo : public IHostIo
{
public:
    void OnOutput(std::string_view text) override
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_out += text;
    }
    bool contains(const char* needle)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_out.find(needle) != std::string::npos;
    }
private:
    std::mutex m_mutex;
    std::string m_out;
};

//Local scripted front end (same contract as the multicontext TU's):
//records stops; the per-stop handler queues the resume WaitUntilResume
//issues.
struct MultiFrontEnd : IDebugFrontEnd
{
    enum class Resume { Continue, Into, Over, Out };
    std::vector<StopInfo> stops;
    std::function<void(DebugSessionController&)> onStopped;
    Resume resume = Resume::Continue;
    DebugSessionController* controller = nullptr;
    void OnStopped(const StopInfo& s) override {
        stops.push_back(s);
        resume = Resume::Continue;
        if (onStopped) onStopped(*controller);
    }
    void OnExited(int) override {}
    void OnRuntimeError(const std::string&) override {}
    void WaitUntilResume() override {
        switch (resume) {
        case Resume::Continue: controller->Continue(); break;
        case Resume::Into:     controller->StepInto(); break;
        case Resume::Over:     controller->StepOver(); break;
        case Resume::Out:      controller->StepOut(); break;
        }
    }
};

//(a) The worker's whole body is ONE dispatch segment (the trip budget
//exceeds its backedge count), so main's breakpoint freezes while that
//segment is in flight on the other carrier.
void test_freeze_releases_inflight_segment()
{
    TEST(freeze_releases_inflight_segment);
    BuildOutcome b = buildSource("k7_freeze_release",
        "import concurrent;\n"                    //1
        "import thread;\n"                        //2
        "import io;\n"                            //3
        "class Runner {\n"                        //4
        "    public Channel<int> ch2;\n"          //5
        "    public int go = 0;\n"                //6
        "    public void run() {\n"               //7
        "        go = 1;\n"                       //8
        "        int i = 0;\n"                    //9
        "        while (i < 2000000) {\n"         //10
        "            i = i + 1;\n"                //11
        "        }\n"                             //12
        "        io.write(\"b1\");\n"             //13
        "        int v = ch2.recv();\n"           //14
        "        io.write(\"b2\");\n"             //15
        "    }\n"                                 //16
        "    public void tail() {\n"              //17
        "        io.write(\"c1\");\n"             //18
        "    }\n"                                 //19
        "}\n"                                     //20
        "int main() {\n"                          //21
        "    Runner r = Runner();\n"              //22
        "    r.ch2 = {};\n"                       //23
        "    Thread t1 = thread.spawn(r.run);\n"  //24
        "    int spin = 0;\n"                     //25
        "    while (r.go == 0) { spin = spin + 1; }\n"  //26
        "    Thread t2 = thread.spawn(r.tail);\n" //27
        "    r.ch2.send(9);\n"                    //28  <- breakpoint
        "    thread.join(t1);\n"                  //29
        "    thread.join(t2);\n"                  //30
        "    return 0;\n"                         //31
        "}\n");                                   //32
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadLinked("k7_freeze_release");
    MutexIo io;
    MultiFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    REQUIRE(controller.AddBreakpoint("k7_freeze_release.n", 28) != 0);
    bool sawB1 = false;
    bool sawC1DuringFreeze = false;
    std::string workerState;
    fe.onStopped = [&](DebugSessionController& c) {
        if (fe.stops.back().reason != StopInfo::Reason::Breakpoint)
            return;
        //With the lock released by the freeze park, the in-flight
        //segment's output write lands and the following receive parks —
        //both observable before the window closes. The deadline is
        //generous (the pre-K7 regression stalls the write FOREVER, not
        //merely slowly): the ASan leg's 2M-iteration segment alone
        //runs ~2s, so a tight bound would misfire there.
        auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(10000);
        while (!io.contains("b1")
               && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        sawB1 = io.contains("b1");
        deadline += std::chrono::milliseconds(10000);
        do {
            workerState = c.View().ContextInfo(1).stateName;
            if (workerState == "parked-chan-recv")
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < deadline);
        sawC1DuringFreeze = io.contains("c1");
    };
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    exec.SetHostIo(&io);
    exec.SetCarrierThreadCount(2);
    exec.SetSchedulerTripForTests(3000000);
    //The run must complete without the deadlock guard firing: a frozen
    //parked context is not dead work — the guard must not see a drained
    //queue as fatal while the freeze gate is closed.
    std::string thrown;
    int rc = -1;
    try {
        rc = exec.Execute(mod);
    } catch (const std::exception& e) {
        thrown = e.what();
    }
    CHECK(thrown.empty(),
        "the frozen run completes without the deadlock guard (got: "
        + thrown + ")");
    CHECK(rc == 0, "program result");
    CHECK(sawB1,
        "the in-flight segment's output write completes during the "
        "freeze (the parked freeze releases the global lock)");
    CHECK(workerState == "parked-chan-recv",
        "the worker parks on its channel receive during the freeze "
        "(got: " + workerState + ")");
    CHECK(!sawC1DuringFreeze,
        "a queued context gets no new dispatch while the freeze gate "
        "is closed");
    CHECK(io.contains("b2") && io.contains("c1"),
        "both workers finish after the resume");
    PASS();
}

//(b) The collection formatter invokes the user toString override in a
//nested evaluation window mid-instruction; a breakpoint on the
//override's own line must stop there with the nested frame depth.
void test_nested_window_breakpoint_stops_in_override()
{
    TEST(nested_window_breakpoint_stops_in_override);
    BuildOutcome b = buildSource("k7_nested_bp",
        "class W {\n"                              //1
        "    public int tag = 0;\n"                //2
        "    public string toString() {\n"         //3
        "        int k = tag + 1;\n"               //4  <- breakpoint
        "        return \"w\" + k;\n"              //5
        "    }\n"                                  //6
        "}\n"                                     //7
        "int main() {\n"                           //8
        "    W item = W();\n"                      //9
        "    item.tag = 8;\n"                      //10
        "    List<W> xs = {};\n"                   //11
        "    xs.add(item);\n"                      //12
        "    string s = xs.toString();\n"          //13
        "    if (s == \"[w9]\") { return 1; }\n"   //14
        "    return 0;\n"                          //15
        "}\n");                                    //16
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("k7_nested_bp");
    MultiFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    REQUIRE(controller.AddBreakpoint("k7_nested_bp.n", 4) != 0);
    bool stoppedInOverride = false;
    fe.onStopped = [&](DebugSessionController& c) {
        if (fe.stops.back().reason != StopInfo::Reason::Breakpoint)
            return;
        const StopInfo& s = fe.stops.back();
        CHECK(s.line == 4,
            "the breakpoint stops inside the override, not at the outer "
            "formatting statement");
        CHECK(s.contextId == 0,
            "the nested window runs on main's context");
        CHECK(s.depth == 2,
            "the checkpoint sees main + override frames (got depth "
            + std::to_string(s.depth) + ")");
        REQUIRE(c.View().FrameCount(0) == 2);
        CHECK(c.View().FrameInfo(0, 0).funcName == "toString",
            "the innermost frame is the override");
        stoppedInOverride = true;
    };
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 1,
        "program result (the override produced \"[w9]\")");
    CHECK(stoppedInOverride, "the in-override breakpoint fired");
    PASS();
}

//(c) While the window is open, hammer add/delete/read on the breakpoint
//table. Under K7 the worker carrier keeps firing statement checkpoints
//(each scans the table) concurrently with the pump's edits — the state
//mutex keeps that consistent. Hammer anchors sit on a never-executed
//function so no hit decisions change.
void test_breakpoint_edits_during_freeze_stay_consistent()
{
    TEST(breakpoint_edits_during_freeze_stay_consistent);
    BuildOutcome b = buildSource("k7_bp_mutation",
        "import thread;\n"                        //1
        "void unused() {\n"                       //2
        "    int z = 1;\n"                        //3  <- hammer anchor
        "    z = z + 1;\n"                        //4  <- hammer anchor
        "}\n"                                     //5
        "void worker() {\n"                       //6
        "    int i = 0;\n"                        //7
        "    while (i < 500000) {\n"              //8
        "        i = i + 1;\n"                    //9
        "    }\n"                                 //10
        "}\n"                                     //11
        "int main() {\n"                          //12
        "    Thread t = thread.spawn(worker);\n"  //13
        "    int m = 0;\n"                        //14
        "    while (m < 50) {\n"                  //15
        "        m = m + 1;\n"                    //16
        "    }\n"                                 //17
        "    thread.join(t);\n"                   //18  <- breakpoint
        "    return 0;\n"                         //19
        "}\n");                                   //20
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadLinked("k7_bp_mutation");
    MultiFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    const int preA = controller.AddBreakpoint("k7_bp_mutation.n", 3);
    const int preB = controller.AddBreakpoint("k7_bp_mutation.n", 4);
    REQUIRE(preA != 0);
    REQUIRE(preB != 0);
    controller.DeleteBreakpoint(preA);
    controller.DeleteBreakpoint(preB);
    REQUIRE(controller.AddBreakpoint("k7_bp_mutation.n", 18) != 0);
    int hammerRounds = 0;
    fe.onStopped = [&](DebugSessionController& c) {
        if (fe.stops.back().reason != StopInfo::Reason::Breakpoint)
            return;
        for (int round = 0; round < 500; ++round) {
            const int idA = c.AddBreakpoint("k7_bp_mutation.n", 3);
            const int idB = c.AddBreakpoint("k7_bp_mutation.n", 4);
            (void)c.BreakpointRows();
            c.DeleteBreakpoint(idA);
            c.DeleteBreakpoint(idB);
            ++hammerRounds;
        }
    };
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    exec.SetCarrierThreadCount(2);
    std::string thrown;
    int rc = -1;
    try {
        rc = exec.Execute(mod);
    } catch (const std::exception& e) {
        thrown = e.what();
    }
    CHECK(thrown.empty(),
        "the frozen run completes without the deadlock guard (got: "
        + thrown + ")");
    CHECK(rc == 0, "program result");
    CHECK(hammerRounds == 500, "the hammer ran inside the frozen window");
    int bpStops = 0;
    for (const StopInfo& s : fe.stops)
        if (s.reason == StopInfo::Reason::Breakpoint)
            ++bpStops;
    CHECK(bpStops == 1, "only the join breakpoint stops the run");
    PASS();
}

//(d) main and a worker both hit breakpoints. The worker's pure delay
//loop spans into main's window, so its own checkpoint decides its hit
//(and queues) while main is still frozen; withdrawing the worker's
//breakpoint during main's window must NOT silence the queued stop.
void test_second_freezer_queues_and_keeps_its_hit()
{
    TEST(second_freezer_queues_and_keeps_its_hit);
    BuildOutcome b = buildSource("k7_double_freeze",
        "import thread;\n"                          //1
        "class Pair {\n"                            //2
        "    public int go = 0;\n"                  //3
        "    public void second() {\n"              //4
        "        go = 1;\n"                         //5
        "        int j = 0;\n"                      //6
        "        while (j < 120000) {\n"            //7
        "            j = j + 1;\n"                  //8
        "        }\n"                               //9
        "        int hit = 1;\n"                    //10  <- bp2
        "    }\n"                                   //11
        "}\n"                                       //12
        "int main() {\n"                            //13
        "    Pair p = Pair();\n"                    //14
        "    Thread t = thread.spawn(p.second);\n"  //15
        "    int spin = 0;\n"                       //16
        "    while (p.go == 0) { spin = spin + 1; }\n"  //17
        "    int probe = 2;\n"                      //18  <- bp1
        "    thread.join(t);\n"                     //19
        "    return 0;\n"                           //20
        "}\n");                                     //21
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadLinked("k7_double_freeze");
    MultiFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    REQUIRE(controller.AddBreakpoint("k7_double_freeze.n", 18) != 0);
    const int bp2 = controller.AddBreakpoint("k7_double_freeze.n", 10);
    REQUIRE(bp2 != 0);
    bool bStopped = false;
    fe.onStopped = [&](DebugSessionController& c) {
        const StopInfo& s = fe.stops.back();
        if (s.reason != StopInfo::Reason::Breakpoint)
            return;
        if (s.contextId == 0) {
            //Hold the window past the worker's delay loop (~100ms) so
            //its checkpoint has decided and queued the hit, then
            //withdraw bp2 — an undecided hit would be silenced.
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1500));
            c.DeleteBreakpoint(bp2);
        } else if (s.contextId == 1) {
            bStopped = true;
            CHECK(s.line == 10,
                "the queued freezer stops on its own breakpoint line");
            CHECK(s.breakpointId == bp2,
                "the queued stop still reports the withdrawn "
                "breakpoint's id (the hit was decided at its own "
                "checkpoint)");
        }
    };
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    exec.SetCarrierThreadCount(2);
    std::string thrown;
    int rc = -1;
    try {
        rc = exec.Execute(mod);
    } catch (const std::exception& e) {
        thrown = e.what();
    }
    CHECK(thrown.empty(),
        "the frozen run completes without the deadlock guard (got: "
        + thrown + ")");
    CHECK(rc == 0, "program result");
    CHECK(fe.stops.size() == 3,
        "initial + both breakpoint windows (got "
        + std::to_string(fe.stops.size()) + ")");
    CHECK(bStopped,
        "a breakpoint withdrawn during another context's freeze still "
        "stops the already-decided hitter after that window closes");
    PASS();
}

void run_debugger_k7_tests()
{
    test_freeze_releases_inflight_segment();
    test_nested_window_breakpoint_stops_in_override();
    test_breakpoint_edits_during_freeze_stay_consistent();
    test_second_freezer_queues_and_keeps_its_hit();
}
