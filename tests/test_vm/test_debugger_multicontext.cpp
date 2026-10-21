// --- K4 Task 2: multi-context debug hooks + context-aware inspection ---
// Derived contexts hit breakpoints/throw-freezes, stepping is gated to
// the step focus (K5: the front end's context selection arms it — a
// parked selection steps on wake), and parked contexts are enumerable
// with readable frames/locals through the unified view face.
// All scenarios run on a single carrier (the determinism pin ndb uses);
// SetSchedulerTripForTests(1) forces window alternation where a worker
// must have dispatched before main reaches the probe line.
#include "test_debugger_common.h"
#include <algorithm>
#include <chrono>
#include <thread>

using namespace nlang;

//Local scripted front end (same contract as the controller TU's): records
//stops; the per-stop handler queues the resume WaitUntilResume issues,
//and may hold the frozen window for a host-side delay (timer-freeze pin).
struct MultiFrontEnd : IDebugFrontEnd
{
    enum class Resume { Continue, Into, Over, Out };
    std::vector<StopInfo> stops;
    std::function<void(DebugSessionController&)> onStopped;
    Resume resume = Resume::Continue;
    int holdWindowMs = 0;   //sleep inside the frozen window before resuming
    DebugSessionController* controller = nullptr;
    void OnStopped(const StopInfo& s) override {
        stops.push_back(s);
        resume = Resume::Continue;
        if (onStopped) onStopped(*controller);
        if (holdWindowMs > 0)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(holdWindowMs));
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

//K4 D2: the breakpoint table is global — an anchor inside a spawned
//function stops the run with the derived context identified. Before K4
//the checkpoint never fired off-main (silent pass-through).
void test_derived_breakpoint_hits()
{
    TEST(derived_breakpoint_hits);
    BuildOutcome b = buildSource("mc_derived_bp",
        "import thread;\n"               //1
        "void worker() {\n"              //2
        "    int a = 5;\n"               //3
        "    int b = a + 1;\n"           //4  <- breakpoint
        "}\n"                            //5
        "int main() {\n"                 //6
        "    Thread t = thread.spawn(worker);\n"  //7
        "    thread.join(t);\n"          //8
        "    return 0;\n"                //9
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadLinked("mc_derived_bp");
    MultiFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    REQUIRE(controller.AddBreakpoint("mc_derived_bp.n", 4) != 0);
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 0, "program result");
    bool derivedHit = false;
    for (const StopInfo& s : fe.stops)
        if (s.reason == StopInfo::Reason::Breakpoint && s.contextId == 1) {
            derivedHit = true;
            CHECK(s.line == 4, "derived hit is on the breakpoint line");
        }
    CHECK(derivedHit,
        "a breakpoint inside a spawned function stops the run with the "
        "derived context identified");
    PASS();
}

//K4: break-on-throw covers every context — a worker's throw site freezes
//before unwinding, full stack and locals alive.
void test_derived_throw_break_on_throw()
{
    TEST(derived_throw_break_on_throw);
    BuildOutcome b = buildSource("mc_derived_throw",
        "import thread;\n"                        //1
        "void worker() {\n"                       //2
        "    throw Exception(\"worker boom\");\n" //3  <- throw site
        "}\n"                                     //4
        "int main() {\n"                          //5
        "    Thread t = thread.spawn(worker);\n"  //6
        "    try { thread.join(t); }\n"           //7
        "    catch (Exception e) { }\n"           //8
        "    return 0;\n"                         //9
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadLinked("mc_derived_throw");
    MultiFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    controller.SetBreakOnThrow(true);
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 0, "program result");
    bool derivedThrow = false;
    for (const StopInfo& s : fe.stops)
        if (s.reason == StopInfo::Reason::Throw && s.contextId == 1) {
            derivedThrow = true;
            CHECK(s.line == 3, "derived throw stop is at the throw site");
        }
    CHECK(derivedThrow,
        "break-on-throw fires in a spawned context with it identified");
    PASS();
}

//K4 D2: stepping is exclusive to the step focus — Step* issued at a
//worker stop stops only at the worker's next statements while main keeps
//running (no stops at main's anchors), then Continue releases the gate
//and the program completes.
void test_step_stops_only_at_focus()
{
    TEST(step_stops_only_at_focus);
    BuildOutcome b = buildSource("mc_step_focus",
        "import thread;\n"                 //1
        "void worker() {\n"                //2
        "    int i = 0;\n"                 //3
        "    while (i < 50) {\n"           //4
        "        i = i + 1;\n"             //5  <- breakpoint
        "    }\n"                          //6
        "}\n"                              //7
        "int main() {\n"                   //8
        "    Thread t = thread.spawn(worker);\n"  //9
        "    int m = 0;\n"                 //10
        "    while (m < 50) {\n"           //11
        "        m = m + 1;\n"             //12
        "    }\n"                          //13
        "    thread.join(t);\n"            //14
        "    return 0;\n"                  //15
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadLinked("mc_step_focus");
    MultiFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    REQUIRE(controller.AddBreakpoint("mc_step_focus.n", 5) != 0);
    int stepStops = 0;
    fe.onStopped = [&](DebugSessionController&) {
        if (fe.stops.back().reason == StopInfo::Reason::Breakpoint
            && stepStops < 3) {
            fe.resume = MultiFrontEnd::Resume::Over;
            if (stepStops == 0)
                stepStops = 1;   //stepping starts at the breakpoint stop
            else
                ++stepStops;
        } else if (stepStops >= 1 && stepStops < 3) {
            fe.resume = MultiFrontEnd::Resume::Over;
            ++stepStops;
        }
    };
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    exec.SetSchedulerTripForTests(1);   //interleave main/worker windows
    CHECK(exec.Execute(mod) == 0, "program result");
    for (const StopInfo& s : fe.stops)
        if (s.reason == StopInfo::Reason::Step)
            CHECK(s.contextId == 1,
                "step stops belong to the focus context only (saw main "
                "context " + std::to_string(s.contextId) + ")");
    CHECK(stepStops == 3, "exactly three step-overs ran (script shape)");
    PASS();
}

//K4: unified inspection face — at a freeze, every context of the run is
//enumerable (state + carrier-held annotation) and a PARKED context's
//frames and locals are readable through the same query path as the
//running one.
void test_parked_context_enumeration_and_locals()
{
    TEST(parked_context_enumeration_and_locals);
    //Channel shared through a class field + bound-method spawn (spawn
    //takes zero-argument funcs; the test_threads channel family uses
    //this shape).
    BuildOutcome b = buildSource("mc_parked_view",
        "import thread;\n"                          //1
        "class Receiver {\n"                        //2
        "    public Channel<int> ch;\n"             //3
        "    public void run() {\n"                 //4
        "        int k = 7;\n"                      //5
        "        int v = ch.recv();\n"              //6  <- parks (empty)
        "    }\n"                                   //7
        "}\n"                                       //8
        "int main() {\n"                            //9
        "    Receiver r = Receiver();\n"            //10
        "    r.ch = {};\n"                          //11
        "    Thread t = thread.spawn(r.run);\n"     //12
        "    int spin = 0;\n"                       //13
        "    while (spin < 50) {\n"                 //14
        "        spin = spin + 1;\n"                //15
        "    }\n"                                   //16
        "    int after = 1;\n"                      //17  <- breakpoint
        "    r.ch.send(42);\n"                      //18
        "    thread.join(t);\n"                     //19
        "    return 0;\n"                           //20
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadLinked("mc_parked_view");
    MultiFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    REQUIRE(controller.AddBreakpoint("mc_parked_view.n", 17) != 0);
    bool inspected = false;
    fe.onStopped = [&](DebugSessionController& c) {
        if (fe.stops.back().reason != StopInfo::Reason::Breakpoint)
            return;
        const IVmDebugView& view = c.View();
        REQUIRE(view.ContextCount() == 2);
        const DebugContextInfo worker = view.ContextInfo(1);
        CHECK(worker.stateName == "parked-chan-recv",
            "the recv waiter reports its park state (got: "
            + worker.stateName + ")");
        CHECK(worker.heldByCarrier == -1,
            "a parked context is held by no carrier window");
        CHECK(worker.frameCount == 1, "the parked frame is enumerable");
        bool sawK = false;
        for (const DebugLocalValue& lv : view.FrameLocals(1, 0))
            if (lv.name == "k") {
                sawK = true;
                CHECK(lv.display == "7",
                    "parked local value readable (got: " + lv.display
                    + ")");
            }
        CHECK(sawK, "the parked frame's locals include k");
        inspected = true;
    };
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    exec.SetSchedulerTripForTests(1);   //worker parks before main's probe
    CHECK(exec.Execute(mod) == 0, "program result");
    CHECK(inspected, "the breakpoint freeze ran the inspection script");
    PASS();
}

//K5: selection is the step target. At a worker bp stop the front end
//selects the PARKED main (empty-channel recv); StepOver releases the
//worker (it never stops), and main — woken by the worker's send — stops
//on the statement after the recv. The bp sits after the 50-iteration
//loop so main is deterministically parked at the freeze: main parks
//within ~5 statements while the worker needs ~52 to reach the anchor.
void test_selected_parked_context_steps_on_wake()
{
    TEST(selected_parked_context_steps_on_wake);
    BuildOutcome b = buildSource("mc_sel_parked",
        "import thread;\n"                          //1
        "class Receiver {\n"                        //2
        "    public Channel<int> ch;\n"             //3
        "    public void run() {\n"                 //4
        "        int k = 0;\n"                      //5
        "        while (k < 50) {\n"                //6
        "            k = k + 1;\n"                  //7
        "        }\n"                               //8
        "        int ready = 1;\n"                  //9  <- bp
        "        ch.send(99);\n"                    //10 wakes main
        "        int after = 2;\n"                  //11
        "    }\n"                                   //12
        "}\n"                                       //13
        "int main() {\n"                            //14
        "    Receiver r = Receiver();\n"            //15
        "    r.ch = {};\n"                          //16
        "    Thread t = thread.spawn(r.run);\n"     //17
        "    int v = r.ch.recv();\n"                //18 <- parks
        "    int done = v + 1;\n"                   //19 <- step lands
        "    thread.join(t);\n"                     //20
        "    return 0;\n"                           //21
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadLinked("mc_sel_parked");
    MultiFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    REQUIRE(controller.AddBreakpoint("mc_sel_parked.n", 9) != 0);
    std::string mainStateAtBp;
    fe.onStopped = [&](DebugSessionController& c) {
        if (fe.stops.back().reason != StopInfo::Reason::Breakpoint)
            return;
        mainStateAtBp = c.View().ContextInfo(0).stateName;
        c.SetStepTargetContext(0);   //select the parked main
        fe.resume = MultiFrontEnd::Resume::Over;
    };
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    exec.SetSchedulerTripForTests(1);
    CHECK(exec.Execute(mod) == 0, "program result");
    CHECK(mainStateAtBp == "parked-chan-recv",
        "precondition: main is parked at the bp freeze (got: "
        + mainStateAtBp + ")");
    CHECK(fe.stops.size() == 3, "initial + bp + step shape");
    CHECK(fe.stops[2].reason == StopInfo::Reason::Step
        && fe.stops[2].contextId == 0,
        "the step stops in the SELECTED parked context after it wakes");
    CHECK(fe.stops[2].line == 19,
        "the step lands on the statement after the parking recv");
    PASS();
}

//K5: the selection also retargets stepping to a RUNNABLE context. At a
//worker bp stop the front end selects main (spinning, runnable-queued);
//StepInto then stops at MAIN's next statement — the stopped worker is
//frozen out by the focus gate and never produces a step stop.
void test_selected_runnable_context_steps_exclusively()
{
    TEST(selected_runnable_context_steps_exclusively);
    BuildOutcome b = buildSource("mc_sel_runnable",
        "import thread;\n"                       //1
        "void worker() {\n"                      //2
        "    int a = 5;\n"                       //3  <- bp (hits once)
        "    int b = a + 1;\n"                   //4
        "}\n"                                    //5
        "int main() {\n"                         //6
        "    Thread t = thread.spawn(worker);\n" //7
        "    int m = 0;\n"                       //8
        "    while (m < 50) {\n"                 //9
        "        m = m + 1;\n"                   //10
        "    }\n"                                //11
        "    thread.join(t);\n"                  //12
        "    return 0;\n"                        //13
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadLinked("mc_sel_runnable");
    MultiFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    REQUIRE(controller.AddBreakpoint("mc_sel_runnable.n", 3) != 0);
    fe.onStopped = [&](DebugSessionController& c) {
        if (fe.stops.back().reason != StopInfo::Reason::Breakpoint)
            return;
        c.SetStepTargetContext(0);   //select the spinning main
        fe.resume = MultiFrontEnd::Resume::Into;
    };
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    exec.SetSchedulerTripForTests(1);
    CHECK(exec.Execute(mod) == 0, "program result");
    CHECK(fe.stops.size() == 3, "initial + bp + step shape");
    CHECK(fe.stops[2].reason == StopInfo::Reason::Step
        && fe.stops[2].contextId == 0,
        "StepInto stops in the selected runnable context, not the "
        "stopped one");
    PASS();
}

//K5 deadlock interaction (regression pin): selecting a context that can
//never wake and stepping it must not mask the deadlock guard — the
//released others run out, nothing remains runnable, and the guard's
//report still surfaces out of Execute instead of a hang.
void test_selected_never_waking_context_keeps_deadlock_report()
{
    TEST(selected_never_waking_context_keeps_deadlock_report);
    BuildOutcome b = buildSource("mc_sel_deadlock",
        "import thread;\n"                          //1
        "class Receiver {\n"                        //2
        "    public Channel<int> ch;\n"             //3
        "    public void run() {\n"                 //4
        "        int k = 0;\n"                      //5
        "        while (k < 50) {\n"                //6
        "            k = k + 1;\n"                  //7
        "        }\n"                               //8
        "        int ready = 1;\n"                  //9  <- bp
        "    }\n"                                   //10
        "}\n"                                       //11
        "int main() {\n"                            //12
        "    Receiver r = Receiver();\n"            //13
        "    r.ch = {};\n"                          //14
        "    Thread t = thread.spawn(r.run);\n"     //15
        "    int v = r.ch.recv();\n"                //16 <- parks forever
        "    thread.join(t);\n"                     //17
        "    return 0;\n"                           //18
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadLinked("mc_sel_deadlock");
    MultiFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    REQUIRE(controller.AddBreakpoint("mc_sel_deadlock.n", 9) != 0);
    fe.onStopped = [&](DebugSessionController& c) {
        if (fe.stops.back().reason != StopInfo::Reason::Breakpoint)
            return;
        c.SetStepTargetContext(0);   //select the never-waking main
        fe.resume = MultiFrontEnd::Resume::Over;
    };
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    exec.SetSchedulerTripForTests(1);
    bool threw = false;
    std::string what;
    try {
        exec.Execute(mod);
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(threw, "stepping a never-waking selection surfaces the "
        "deadlock guard instead of hanging");
    CHECK(what.find("deadlock") != std::string::npos,
        "message names the deadlock guard (got: " + what + ")");
    CHECK(fe.stops.size() == 2,
        "initial + bp only: the parked selection never produces a step "
        "stop");
    PASS();
}

//K4 time-freeze semantics (regression pin, passes by design): the freeze
//holds the GIL, so the timer thread cannot migrate expiries while
//stopped — a sleeper whose deadline passes during a freeze still wakes
//after resume (no lost wake).
void test_timer_freeze_semantics()
{
    TEST(timer_freeze_semantics);
    BuildOutcome b = buildSource("mc_timer_freeze",
        "import thread;\n"                        //1
        "void sleeper() {\n"                      //2
        "    thread.sleep(50);\n"                 //3
        "}\n"                                    //4
        "int main() {\n"                         //5
        "    Thread t = thread.spawn(sleeper);\n"//6
        "    int m = 0;\n"                       //7
        "    while (m < 50) {\n"                 //8
        "        m = m + 1;\n"                   //9
        "    }\n"                                //10
        "    thread.join(t);\n"                  //11  <- breakpoint
        "    return 0;\n"                        //12
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadLinked("mc_timer_freeze");
    MultiFrontEnd fe;
    fe.holdWindowMs = 150;   //deadline (50ms) expires inside the freeze
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    REQUIRE(controller.AddBreakpoint("mc_timer_freeze.n", 11) != 0);
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    exec.SetSchedulerTripForTests(1);   //sleeper parks before the probe
    CHECK(exec.Execute(mod) == 0,
        "a deadline expiring during a freeze still wakes after resume");
    PASS();
}

void run_debugger_multicontext_tests()
{
    test_derived_breakpoint_hits();
    test_derived_throw_break_on_throw();
    test_step_stops_only_at_focus();
    test_parked_context_enumeration_and_locals();
    test_selected_parked_context_steps_on_wake();
    test_selected_runnable_context_steps_exclusively();
    test_selected_never_waking_context_keeps_deadlock_report();
    test_timer_freeze_semantics();
}
