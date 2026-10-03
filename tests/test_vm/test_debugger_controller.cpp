// --- ndb debugger tests: DebugSessionController (unified API) ---
#include "test_debugger_common.h"

using namespace nlang;

// --- Task 3 (unified API): DebugSessionController ---

//Scripted front end: records stops; the per-stop handler observes and
//queues the resume, which WaitUntilResume issues (default: continue) —
//controller semantics without a CLI.
struct ScriptedFrontEnd : IDebugFrontEnd
{
    enum class Resume { Continue, Into, Over, Out };
    std::vector<StopInfo> stops;
    std::function<void(DebugSessionController&)> onStopped;  //observe/mutate
    Resume resume = Resume::Continue;                        //queued resume
    DebugSessionController* controller = nullptr;
    void OnStopped(const StopInfo& s) override {
        stops.push_back(s);
        resume = Resume::Continue;   //per-stop default
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

void test_controller_initial_stop()
{
    //gdb `start` behavior: the first statement freezes with reason
    //Initial, before anything has run.
    TEST(controller_initial_stop);
    BuildOutcome b = buildSource("ctrl_init",
        "int main() {\n"      //1
        "    int a = 1;\n"    //2
        "    return a;\n"     //3
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_init");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 1, "program result");
    REQUIRE(fe.stops.size() == 1);
    CHECK(fe.stops[0].reason == StopInfo::Reason::Initial,
        "the first stop is the initial stop");
    CHECK(fe.stops[0].line == 2, "first statement line");
    CHECK(fe.stops[0].depth == 1, "main is depth 1");
    CHECK(fe.stops[0].funcIdx == mod.FindFunction("ctrl_init.main"),
        "funcIdx indexes the module's function table");
    PASS();
}

void test_controller_step_semantics()
{
    //s: next statement any depth (descends into calls); f: skips the
    //callee's remaining statements, lands at depth < recorded. Same
    //script the CLI test and the dbg_step_semantics e2e pin: s s f c.
    TEST(controller_step_semantics);
    BuildOutcome b = buildSource("ctrl_step",
        "int inner(int v) {\n"   //1
        "    int r = v + 1;\n"   //2
        "    return r;\n"        //3
        "}\n"                    //4
        "int main() {\n"         //5
        "    int a = 4;\n"       //6
        "    int b = inner(a);\n"//7
        "    return 0;\n"        //8
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_step");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    size_t visit = 0;
    fe.onStopped = [&](DebugSessionController&) {
        if (visit == 0 || visit == 1)
            fe.resume = ScriptedFrontEnd::Resume::Into;
        else if (visit == 2)
            fe.resume = ScriptedFrontEnd::Resume::Out;
        ++visit;   //afterwards: default Continue
    };
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 0, "program result");
    REQUIRE(fe.stops.size() == 4);
    CHECK(fe.stops[0].line == 6
        && fe.stops[0].reason == StopInfo::Reason::Initial,
        "initial stop at main's first statement");
    CHECK(fe.stops[1].line == 7 && fe.stops[1].depth == 1
        && fe.stops[1].reason == StopInfo::Reason::Step,
        "s advances one statement in main");
    CHECK(fe.stops[2].line == 2 && fe.stops[2].depth == 2
        && fe.stops[2].reason == StopInfo::Reason::Step,
        "s descends into the call");
    CHECK(fe.stops[3].line == 8 && fe.stops[3].depth == 1
        && fe.stops[3].reason == StopInfo::Reason::Step,
        "f lands back in main past the callee's remaining statements");
    PASS();
}

void test_controller_step_over()
{
    //n: crosses the callee wholesale — depth <= recorded stops, so the
    //callee's statements never freeze and execution lands on the next
    //same-depth statement.
    TEST(controller_step_over);
    BuildOutcome b = buildSource("ctrl_over",
        "int inner(int v) {\n"   //1
        "    int r = v + 1;\n"   //2
        "    return r;\n"        //3
        "}\n"                    //4
        "int main() {\n"         //5
        "    int a = 4;\n"       //6
        "    int b = inner(a);\n"//7
        "    return 0;\n"        //8
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_over");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    size_t visit = 0;
    fe.onStopped = [&](DebugSessionController&) {
        if (visit <= 1)
            fe.resume = ScriptedFrontEnd::Resume::Over;
        ++visit;   //afterwards: default Continue
    };
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 0, "program result");
    REQUIRE(fe.stops.size() == 3);
    CHECK(fe.stops[0].line == 6
        && fe.stops[0].reason == StopInfo::Reason::Initial,
        "initial stop");
    CHECK(fe.stops[1].line == 7 && fe.stops[1].depth == 1,
        "n advances within main");
    CHECK(fe.stops[2].line == 8 && fe.stops[2].depth == 1,
        "n crosses the call, landing on the next same-depth statement");
    for (const auto& s : fe.stops)
        CHECK(s.line != 2 && s.line != 3, "no stop inside inner");
    PASS();
}

void test_controller_breakpoint_hit_and_count()
{
    //One breakpoint id, one hit per stop: the loop body line fires
    //three times, every hit reported under the same id and counted
    //once.
    TEST(controller_breakpoint_hit_and_count);
    BuildOutcome b = buildSource("ctrl_bp",
        "int main() {\n"        //1
        "    int t = 0;\n"      //2
        "    int i = 1;\n"      //3
        "    while (i <= 3) {\n"//4
        "        t = t + i;\n"  //5
        "        i = i + 1;\n"  //6
        "    }\n"               //7
        "    return t;\n"       //8
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_bp");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    //Suffix spec, as the CLI's `b file.n:LINE` form passes through.
    int id = controller.AddBreakpoint("ctrl_bp.n", 5);
    CHECK(id == 1, "first breakpoint gets id 1");
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 6, "program result (1+2+3)");
    REQUIRE(fe.stops.size() == 4);
    CHECK(fe.stops[0].reason == StopInfo::Reason::Initial,
        "initial stop first");
    for (size_t i = 1; i < fe.stops.size(); ++i) {
        CHECK(fe.stops[i].reason == StopInfo::Reason::Breakpoint,
            "hit " + std::to_string(i) + " reports as Breakpoint");
        CHECK(fe.stops[i].breakpointId == id, "same id every hit");
        CHECK(fe.stops[i].line == 5, "loop body line");
    }
    const auto rows = controller.BreakpointRows();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == id && rows[0].hits == 3,
        "one row, one hit per stop");
    PASS();
}

void test_controller_break_by_func()
{
    //b funcName: binds the function's FIRST statement under one id; the
    //hit lands there and counts once.
    TEST(controller_break_by_func);
    BuildOutcome b = buildSource("ctrl_bpfunc",
        "int inner(int v) {\n"   //1
        "    int r = v + 1;\n"   //2
        "    return r;\n"        //3
        "}\n"                    //4
        "int main() {\n"         //5
        "    int a = 3;\n"       //6
        "    int b = inner(a);\n"//7
        "    return 0;\n"        //8
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_bpfunc");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    int id = controller.AddFunctionBreakpoint("ctrl_bpfunc.inner");
    CHECK(id == 1, "function breakpoint gets id 1");
    const auto rows = controller.BreakpointRows();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].label == "ctrl_bpfunc.inner (ctrl_bpfunc.n:2)",
        "label names the resolved first statement, got: "
        + rows[0].label);
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 0, "program result");
    REQUIRE(fe.stops.size() == 2);
    CHECK(fe.stops[0].reason == StopInfo::Reason::Initial
        && fe.stops[0].line == 6, "initial stop in main");
    CHECK(fe.stops[1].reason == StopInfo::Reason::Breakpoint
        && fe.stops[1].breakpointId == id && fe.stops[1].line == 2,
        "hit lands at inner's first statement");
    CHECK(controller.BreakpointRows()[0].hits == 1, "counted once");
    PASS();
}

void test_controller_funcbp_all_same_name()
{
    //One id covers every same-named function's first statement (methods
    //on different classes share the bare-name pool): both methods hit,
    //both hits report the same id.
    TEST(controller_funcbp_all_same_name);
    BuildOutcome b = buildSource("ctrl_funcbp",
        "class Left {\n"                        //1
        "    public int val() { return 4; }\n"  //2
        "}\n"                                   //3
        "class Right {\n"                       //4
        "    public int val() { return 5; }\n"  //5
        "}\n"                                   //6
        "int main() {\n"                        //7
        "    Left l = new Left();\n"            //8
        "    Right r = new Right();\n"          //9
        "    return l.val() + r.val();\n"       //10
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_funcbp");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    int id = controller.AddFunctionBreakpoint("val");   //methods stay bare
    CHECK(id == 1, "one id for the whole name pool");
    CHECK(controller.BreakpointRows().size() == 1, "one row");
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 9, "program result");
    REQUIRE(fe.stops.size() == 3);
    CHECK(fe.stops[0].reason == StopInfo::Reason::Initial,
        "initial stop in main");
    CHECK(fe.stops[1].reason == StopInfo::Reason::Breakpoint
        && fe.stops[1].breakpointId == id,
        "first same-named method hits the shared id");
    CHECK(fe.stops[2].reason == StopInfo::Reason::Breakpoint
        && fe.stops[2].breakpointId == id,
        "second same-named method hits the same id");
    CHECK(fe.stops[1].funcIdx != fe.stops[2].funcIdx,
        "the two hits are different functions");
    CHECK(controller.BreakpointRows()[0].hits == 2, "both counted once");
    PASS();
}

void test_controller_funcbp_idempotent()
{
    //A repeated request for the same name returns the existing id — the
    //table key is the function name, mirroring (file, line) idempotency.
    //Without it the duplicate would be a zero-hit twin: OnStatement
    //credits the first matching id only.
    TEST(controller_funcbp_idempotent);
    BuildOutcome b = buildSource("ctrl_funcbp_dup",
        "int inner(int v) {\n"   //1
        "    int r = v + 1;\n"   //2
        "    return r;\n"        //3
        "}\n"                    //4
        "int main() {\n"         //5
        "    int a = 3;\n"       //6
        "    int b = inner(a);\n"//7
        "    return 0;\n"        //8
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_funcbp_dup");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    int id = controller.AddFunctionBreakpoint("ctrl_funcbp_dup.inner");
    CHECK(id != 0, "first request binds");
    CHECK(controller.AddFunctionBreakpoint("ctrl_funcbp_dup.inner") == id,
        "repeated request returns the existing id");
    CHECK(controller.BreakpointRows().size() == 1,
        "the duplicate did not create a second row");
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 0, "program result");
    REQUIRE(fe.stops.size() == 2);
    CHECK(fe.stops[1].reason == StopInfo::Reason::Breakpoint
        && fe.stops[1].breakpointId == id,
        "the hit reports under the one existing id");
    CHECK(controller.BreakpointRows()[0].hits == 1,
        "hit count lands on the single row (no zero-hit twin)");
    PASS();
}

void test_controller_multianchor_finally_one_id()
{
    //try/finally compiles each finally-body statement TWICE (exception-
    //path copy, then normal-path copy) — two anchors on one line. Both
    //live under ONE breakpoint id: every executed copy reports that id
    //and counts one hit; deleting the id once removes both anchors.
    TEST(controller_multianchor_finally_one_id);
    BuildOutcome b = buildSource("ctrl_multianchor",
        "int main() {\n"             //1
        "    int a = 0;\n"           //2
        "    int i = 0;\n"           //3
        "    while (i < 2) {\n"      //4
        "        i = i + 1;\n"       //5
        "        try {\n"            //6
        "            a = a + 1;\n"   //7
        "        } finally {\n"      //8
        "            a = a + 10;\n"  //9  two anchors
        "        }\n"                //10
        "    }\n"                    //11
        "    return a;\n"            //12
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_multianchor");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    int id = controller.AddBreakpoint(
        (scratchDir() / "ctrl_multianchor.n").string(), 9);
    CHECK(id == 1, "one id for the whole line");
    CHECK(controller.BreakpointRows().size() == 1, "one row");
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 22, "program result (2 iterations)");
    REQUIRE(fe.stops.size() == 3);
    CHECK(fe.stops[0].reason == StopInfo::Reason::Initial,
        "initial stop first");
    for (size_t i = 1; i < fe.stops.size(); ++i) {
        CHECK(fe.stops[i].reason == StopInfo::Reason::Breakpoint
            && fe.stops[i].breakpointId == id && fe.stops[i].line == 9,
            "finally line hit " + std::to_string(i)
            + " reports the shared id");
    }
    CHECK(controller.BreakpointRows()[0].hits == 2,
        "one hit per executed anchor copy");
    //Deleting the id once removes the one row that owns BOTH anchors.
    //Directly observable only for the normal-path copy (the re-run
    //executes just that one); the exception copy's release is pinned
    //jointly by controller_multianchor_exception_copy, which proves both
    //copies share this single id.
    CHECK(controller.DeleteBreakpoint(id), "delete succeeds");
    CHECK(!controller.DeleteBreakpoint(id), "second delete fails");
    fe.stops.clear();
    CHECK(exec.Execute(mod) == 22, "re-run result");
    CHECK(fe.stops.empty(), "both anchors are gone after one delete");
    PASS();
}

void test_controller_multianchor_exception_copy()
{
    //The exception-path anchor of a finally line belongs to the SAME
    //breakpoint: when an exception unwinds through the finally, the
    //handler-region copy of the line hits the id (the normal-path copy
    //never runs in this program, so the single Breakpoint stop can only
    //come from the exception copy).
    TEST(controller_multianchor_exception_copy);
    BuildOutcome b = buildSource("ctrl_multianchor_exc",
        "int main() {\n"                            //1
        "    int a = 0;\n"                          //2
        "    try {\n"                               //3
        "        try {\n"                           //4
        "            throw new Exception(\"x\");\n" //5
        "        } finally {\n"                     //6
        "            a = a + 10;\n"                 //7
        "        }\n"                               //8
        "    } catch (Exception e) {\n"             //9
        "        a = a + 100;\n"                    //10
        "    }\n"                                   //11
        "    return a;\n"                           //12
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_multianchor_exc");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    int id = controller.AddBreakpoint(
        (scratchDir() / "ctrl_multianchor_exc.n").string(), 7);
    CHECK(id == 1, "bound");
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 110, "program result");
    REQUIRE(fe.stops.size() == 2);
    CHECK(fe.stops[0].reason == StopInfo::Reason::Initial,
        "initial stop first");
    CHECK(fe.stops[1].reason == StopInfo::Reason::Breakpoint
        && fe.stops[1].breakpointId == id && fe.stops[1].line == 7,
        "exception-path copy hits the same line's id");
    PASS();
}

void test_controller_unbound_lines()
{
    //Lines without a statement anchor (signatures, braces, past EOF)
    //are unbound: id 0, nothing stored. A repeated request returns the
    //same id — the table key is the normalized (file, line) pair.
    TEST(controller_unbound_lines);
    BuildOutcome b = buildSource("ctrl_unbound",
        "int main() {\n"      //1
        "    int a = 1;\n"    //2
        "    return a;\n"     //3
        "}\n");               //4
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_unbound");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    const std::string src = (scratchDir() / "ctrl_unbound.n").string();
    CHECK(controller.AddBreakpoint(src, 1) == 0,
        "function signature line has no anchor");
    CHECK(controller.AddBreakpoint(src, 4) == 0,
        "closing brace line has no anchor");
    CHECK(controller.AddBreakpoint(src, 99) == 0,
        "line past EOF has no anchor");
    CHECK(controller.BreakpointRows().empty(),
        "unbound requests store nothing");
    int id = controller.AddBreakpoint(src, 2);
    CHECK(id != 0, "a statement line binds");
    CHECK(controller.AddBreakpoint(src, 2) == id,
        "re-adding the line returns the same id");
    CHECK(controller.BreakpointRows().size() == 1,
        "the duplicate did not create a second row");
    PASS();
}

void test_controller_window_discipline()
{
    //Resume commands and View() outside the frozen window are front-end
    //programming errors — refused loudly, and the session keeps working
    //afterwards (no resume state leaked out of the window).
    TEST(controller_window_discipline);
    BuildOutcome b = buildSource("ctrl_window",
        "int main() {\n"      //1
        "    int a = 1;\n"    //2
        "    return a;\n"     //3
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("ctrl_window");
    ScriptedFrontEnd fe;
    DebugSessionController controller(mod, fe);
    fe.controller = &controller;
    bool threw = false;
    try { controller.Continue(); }
    catch (const std::logic_error&) { threw = true; }
    CHECK(threw, "Continue outside the frozen window throws");
    threw = false;
    try { controller.StepOver(); }
    catch (const std::logic_error&) { threw = true; }
    CHECK(threw, "StepOver outside the frozen window throws");
    threw = false;
    try { (void)controller.View(); }
    catch (const std::logic_error&) { threw = true; }
    CHECK(threw, "View outside the frozen window throws");
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    CHECK(exec.Execute(mod) == 1, "program result after refused calls");
    REQUIRE(fe.stops.size() == 1);
    CHECK(fe.stops[0].reason == StopInfo::Reason::Initial,
        "initial stop intact (the refused resumes queued nothing)");
    PASS();
}

void test_controller_break_on_throw()
{
    //SetBreakOnThrow freezes at the throw site (reason Throw) before
    //the unwind; default off never stops there.
    TEST(controller_break_on_throw);
    const std::string source =
        "int main() {\n"                             //1
        "    int a = 1;\n"                           //2
        "    try {\n"                                //3
        "        throw new Exception(\"boom\");\n"   //4
        "    } catch (Exception e) {\n"              //5
        "        return 7;\n"                        //6
        "    }\n"                                    //7
        "}\n";                                       //8
    BuildOutcome b = buildSource("ctrl_throw", source);
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    {
        CompiledModule mod = loadBuilt("ctrl_throw");
        ScriptedFrontEnd fe;
        DebugSessionController controller(mod, fe);
        fe.controller = &controller;
        VmExecutor exec;
        exec.SetDebugHooks(&controller);
        CHECK(exec.Execute(mod) == 7, "program result");
        REQUIRE(fe.stops.size() == 1);
        CHECK(fe.stops[0].reason == StopInfo::Reason::Initial,
            "default off: no stop at the throw");
    }
    {
        CompiledModule mod = loadBuilt("ctrl_throw");
        ScriptedFrontEnd fe;
        DebugSessionController controller(mod, fe);
        fe.controller = &controller;
        controller.SetBreakOnThrow(true);
        CHECK(controller.BreakOnThrow(), "getter echoes the state");
        VmExecutor exec;
        exec.SetDebugHooks(&controller);
        CHECK(exec.Execute(mod) == 7, "catch still completes the program");
        REQUIRE(fe.stops.size() == 2);
        CHECK(fe.stops[1].reason == StopInfo::Reason::Throw
            && fe.stops[1].line == 4 && fe.stops[1].depth == 1,
            "frozen at the throw site with the stack alive");
    }
    PASS();
}

void run_debugger_controller_tests()
{
    test_controller_initial_stop();
    test_controller_step_semantics();
    test_controller_step_over();
    test_controller_breakpoint_hit_and_count();
    test_controller_break_by_func();
    test_controller_funcbp_all_same_name();
    test_controller_funcbp_idempotent();
    test_controller_multianchor_finally_one_id();
    test_controller_multianchor_exception_copy();
    test_controller_unbound_lines();
    test_controller_window_discipline();
    test_controller_break_on_throw();
}
