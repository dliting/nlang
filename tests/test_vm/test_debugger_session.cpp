// --- ndb debugger tests: DebugSession CLI adapter ---
// Covers the CLI adapter session semantics and the SourceCache
// l/x/catch behaviors driven through it.
#include "test_debugger_common.h"

using namespace nlang;

// --- Task 4: DebugSession (CLI adapter over the controller) ---

//Drive the ndb CLI adapter (wired to a DebugSessionController) with a
//command script; returns everything the session wrote. Scripts MUST run
//the program to completion (final `c`) — hitting EOF while frozen would
//quit the whole test binary (EOF = q by contract; dbg_quit_eof e2e
//pins that in a subprocess instead).
static std::string RunSession(const std::string& tag,
    const std::string& source, const std::string& commands)
{
    BuildOutcome b = buildSource(tag, source);
    if (!b.ok) return "BUILD FAILED: " + b.diagnostics;
    CompiledModule mod = loadBuilt(tag);
    std::ostringstream out;
    std::istringstream in(commands);
    DebugSession session(mod,
        (scratchDir() / (tag + ".ncu")).string(), in, out);
    DebugSessionController controller(mod, session);
    session.SetController(&controller);
    VmExecutor exec;
    exec.SetDebugHooks(&controller);
    exec.Execute(mod);
    return out.str();
}

void test_session_initial_stop_and_continue()
{
    TEST(session_initial_stop_and_continue);
    std::string out = RunSession("sess_init",
        "int main() {\n"      //1
        "    int a = 1;\n"    //2
        "    return a;\n"     //3
        "}\n",
        "c\n");
    CHECK(out.find("Stopped: sess_init.main (sess_init.n:2)") != std::string::npos,
        "initial stop at main's first statement");
    CHECK(out.find("(ndb) ") != std::string::npos, "prompt shown");
    PASS();
}

void test_session_step_semantics()
{
    TEST(session_step_semantics);
    std::string out = RunSession("sess_step",
        "int inner(int v) {\n"   //1
        "    int r = v + 1;\n"   //2
        "    return r;\n"        //3
        "}\n"                    //4
        "int main() {\n"         //5
        "    int a = 4;\n"       //6
        "    int b = inner(a);\n"//7
        "    return 0;\n"        //8
        "}\n",
        "s\ns\nf\nc\n");
    //s: main:6 -> main:7; s: into inner:2; f: skips inner:3, lands
    //back in main:8 (depth 1 < recorded 2).
    CHECK(out.find("Stopped: sess_step.main (sess_step.n:6)") != std::string::npos,
        "initial stop line 6");
    CHECK(out.find("Stopped: sess_step.main (sess_step.n:7)") != std::string::npos,
        "step-into advances one statement");
    CHECK(out.find("Stopped: sess_step.inner (sess_step.n:2)") != std::string::npos,
        "step-into descends into the call");
    CHECK(out.find("Stopped: sess_step.main (sess_step.n:8)") != std::string::npos,
        "step-out returns past remaining callee statements");
    CHECK(out.find("Stopped: sess_step.inner (sess_step.n:3)") == std::string::npos,
        "inner:3 is passed over by finish");
    PASS();
}

void test_session_breakpoint_hit()
{
    TEST(session_breakpoint_hit);
    std::string out = RunSession("sess_bp",
        "int main() {\n"        //1
        "    int t = 0;\n"      //2
        "    int i = 1;\n"      //3
        "    while (i <= 3) {\n"//4
        "        t = t + i;\n"  //5
        "        i = i + 1;\n"  //6
        "    }\n"               //7
        "    return t;\n"       //8
        "}\n",
        "b 5\nc\nc\nc\ni b\nc\n");
    //Loop body line 5 executes 3 times; the script continues past all
    //three stops, checks the hit counter, then runs to completion
    //(final `c` — never end a script frozen, EOF would quit the test
    //binary).
    CHECK(out.find("Breakpoint 1 at sess_bp.main (sess_bp.n:5)") != std::string::npos,
        "set-time report names the resolved location");
    CHECK(out.find("Breakpoint 1, sess_bp.main (sess_bp.n:5)") != std::string::npos,
        "hit-time report");
    CHECK(out.find("hits=3") != std::string::npos,
        "breakpoint counts every hit");
    PASS();
}

void test_session_bt_and_locals()
{
    TEST(session_bt_and_locals);
    std::string out = RunSession("sess_bt",
        "int scale(int v, int k) {\n" //1
        "    int s = v * k;\n"        //2
        "    return s;\n"             //3
        "}\n"                         //4
        "int main() {\n"              //5
        "    int r = scale(5, 3);\n"  //6
        "    return 0;\n"             //7
        "}\n",
        "b 2\nc\nbt\ninfo locals\nc\n");
    CHECK(out.find("#0  sess_bt.scale (sess_bt.n:2)") != std::string::npos,
        "bt frame 0 format");
    CHECK(out.find("#1  sess_bt.main (sess_bt.n:6)") != std::string::npos,
        "bt frame 1 carries the CALLING statement anchor");
    CHECK(out.find("v = 5") != std::string::npos, "param local shown");
    CHECK(out.find("k = 3") != std::string::npos, "param local shown");
    //Hidden-name filter: synthesized locals stay out of the display.
    bool leaked = out.find("__foreach") != std::string::npos
        || out.find("$finally") != std::string::npos;
    CHECK(!leaked, "hidden local names stay internal");
    PASS();
}

//info locals honors declaration scope at the session level. The
//session's initial stop lands on main's first statement (line 2,
//before late's declaration): the first query — bounded by the line-4
//breakpoint reports — must not mention late, while the query after
//continuing past the declaration shows the computed value.
void test_session_locals_decl_scope()
{
    TEST(session_locals_decl_scope);
    std::string out = RunSession("sess_scope",
        "int main() {\n"               //1
        "    int early = 1;\n"         //2
        "    int late = early + 1;\n"  //3
        "    return late;\n"           //4
        "}\n",
        "info locals\nb 4\nc\ninfo locals\nc\n");
    const size_t pos2 = out.find("Stopped: sess_scope.main (sess_scope.n:2)");
    const size_t pos4 = out.find("sess_scope.n:4");  // b 4 set report
    REQUIRE(pos2 != std::string::npos && pos4 != std::string::npos
        && pos2 < pos4);
    const std::string firstQuery = out.substr(pos2, pos4 - pos2);
    CHECK(firstQuery.find("early = 0") != std::string::npos,
        "declared local shows its zero value on the decl line");
    CHECK(firstQuery.find("late = ") == std::string::npos,
        "not-yet-declared local stays out of info locals");
    CHECK(out.find("early = 1") != std::string::npos,
        "computed value shows once the initializer has run");
    CHECK(out.find("late = 2") != std::string::npos,
        "late joins the display after execution passes its decl");
    PASS();
}

void test_session_frame_select()
{
    TEST(session_frame_select);
    std::string out = RunSession("sess_frame",
        "int callee(int v) {\n"  //1
        "    int r = v + 1;\n"   //2
        "    return r;\n"        //3
        "}\n"                    //4
        "int main() {\n"         //5
        "    int a = 9;\n"       //6
        "    int b = callee(a);\n"//7
        "    return 0;\n"        //8
        "}\n",
        "b 2\nc\ninfo locals\nframe 1\ninfo locals\nc\n");
    //First `info locals` runs on frame 0 (callee: v = 9), the second
    //after `frame 1` (main: a = 9) — selection routes the query.
    CHECK(out.find("v = 9") != std::string::npos,
        "frame 0 (default) shows callee's param");
    CHECK(out.find("a = 9") != std::string::npos,
        "frame 1 locals belong to main");
    PASS();
}

void test_session_break_by_func()
{
    TEST(session_break_by_func);
    std::string out = RunSession("sess_bpfunc",
        "int inner(int v) {\n"  //1
        "    int r = v + 1;\n"  //2
        "    return r;\n"       //3
        "}\n"                   //4
        "int main() {\n"        //5
        "    int a = 3;\n"      //6
        "    int b = inner(a);\n"//7
        "    return 0;\n"       //8
        "}\n",
        "b sess_bpfunc.inner\nc\nc\n");
    //b funcName resolves to the function's FIRST statement (line 2)
    //and reports the resolved location at set time (spec §8).
    CHECK(out.find("Breakpoint 1 at sess_bpfunc.inner (sess_bpfunc.n:2)")
            != std::string::npos,
        "set-time report names the first statement");
    CHECK(out.find("Breakpoint 1, sess_bpfunc.inner (sess_bpfunc.n:2)")
            != std::string::npos,
        "hit-time report at the same location");
    PASS();
}
// --- Task 5: SourceCache / l / x / catch ---

void test_sourcecache_resolution()
{
    //As-recorded path resolves; stale recorded path falls back to the
    //.ncu's directory by basename; unresolvable degrades to empty
    //(and the negative result is cached).
    TEST(sourcecache_resolution);
    BuildOutcome b = buildSource("src_cache",
        "int main() {\n"      //1
        "    return 0;\n"     //2
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("src_cache");
    int idx = mod.FindFunction("src_cache.main");
    REQUIRE(idx >= 0);
    const auto& mainf = mod.functions[static_cast<size_t>(idx)];
    SourceCache cache((scratchDir() / "src_cache.ncu").string());
    const auto& lines = cache.Lines(mainf.sourceFile);
    REQUIRE(lines.size() >= 3);
    CHECK(lines[0].find("int main()") != std::string::npos,
        "as-recorded path resolves to the real source");
    {
        std::ofstream out(scratchDir() / "sibling.n", std::ios::binary);
        out << "line one\nline two\n";
    }
    const auto& sib = cache.Lines("D:\\gone\\away\\sibling.n");
    CHECK(sib.size() == 2 && sib[1] == "line two",
        "stale recorded path falls back to the .ncu directory");
    const auto& none = cache.Lines("Z:\\no\\such\\file.n");
    CHECK(none.empty(), "unresolvable file degrades to empty");
    CHECK(&cache.Lines("Z:\\no\\such\\file.n") == &none,
        "negative result is cached (stable reference)");
    PASS();
}

void test_session_source_list()
{
    TEST(session_source_list);
    std::string out = RunSession("sess_list",
        "int fib(int n) {\n"                  //1
        "    if (n < 2) {\n"                  //2
        "        return n;\n"                 //3
        "    }\n"                             //4
        "    return fib(n - 1) + fib(n - 2);\n"//5
        "}\n"                                 //6
        "int main() {\n"                      //7
        "    int r = fib(10);\n"              //8
        "    return r - 55;\n"                //9
        "}\n",                                //10
        "b 9\nc\nl\nc\n");
    //Initial stop is main:8 (the first statement) — a breakpoint on 8
    //can never hit again, so the script breaks on 9 instead. Window is
    //centered on the stop line: marker on 9, neighbors 4..13 unmarked.
    //Line format = 2-char marker + 5-char right-aligned number + TAB
    //(unmarked line 4 = 6 spaces + '4').
    CHECK(out.find("->    9\t") != std::string::npos,
        "stop line carries the -> marker");
    CHECK(out.find("      4\t") != std::string::npos,
        "window shows unmarked lines (4 = 9 - 5)");
    CHECK(out.find("     10\t") != std::string::npos,
        "window spans past the stop line (10 = 9 + 1 < 9 + 5)");
    PASS();
}

void test_session_disasm_marker()
{
    TEST(session_disasm_marker);
    std::string out = RunSession("sess_x",
        "int main() {\n"       //1
        "    int a = 6;\n"     //2
        "    int b = a * 7;\n" //3
        "    return b - 42;\n" //4
        "}\n",
        "x\nc\n");
    //`x` at the initial stop: the first statement's pc carries the
    //>> marker; the rest are blank-marked disassembly lines.
    CHECK(out.find(">>  00") != std::string::npos,
        "current pc is marked with >>");
    CHECK(out.find("    00") != std::string::npos,
        "other instructions are blank-marked");
    PASS();
}

//0.7.5 Task 11: the descriptor surfaces (ndisasm struct/class field
//types, function return kinds) name every scalar registry row in wire
//style; the legacy kinds keep their existing names, and a corrupt wide
//kind must not alias a real RTK through its low byte.
void test_disasm_type_kind_names()
{
    TEST(disasm_type_kind_names);
    struct Row { uint16_t kind; const char* name; };
    const Row rows[] = {
        {RTK_Int32, "i32"},   {RTK_Float, "f32"},
        {RTK_Byte, "i8"},     {RTK_UByte, "u8"},
        {RTK_Short, "i16"},   {RTK_UShort, "u16"},
        {RTK_UInt32, "u32"},
        {RTK_Long, "i64"},    {RTK_ULong, "u64"},
        {RTK_Double, "f64"},
        {RTK_Bool, "bool"},   {RTK_Char, "char"},
        {RTK_String, "str"},  {RTK_Struct, "struct"},
        {RTK_Class, "class"}, {RTK_Array, "array"},
        {RTK_Boxed, "boxed"}, {RTK_Func, "func"},
        {300, "unknown"},     {0x102, "unknown"},
    };
    for (const auto& r : rows)
        CHECK(std::string(DisasmTypeKindName(r.kind)) == r.name,
            std::string("wire name for kind ") + r.name);
    PASS();
}

void test_session_catch_throw()
{
    TEST(session_catch_throw);
    std::string out = RunSession("sess_catch",
        "int main() {\n"                             //1
        "    int a = 1;\n"                           //2
        "    try {\n"                                //3
        "        throw new Exception(\"boom\");\n"   //4
        "    } catch (Exception e) {\n"              //5
        "        return 7;\n"                        //6
        "    }\n"                                    //7
        "}\n",
        "catch on\nc\nc\n");
    //The throw statement's own OP_DebugInfo sets currentLine=4 before
    //OP_Throw runs, so the Throw: report anchors at line 4 with the
    //stack still alive; the second c completes the unwind into catch.
    CHECK(out.find("Break on throw: on") != std::string::npos,
        "catch on echoes the new state");
    CHECK(out.find("Throw: sess_catch.main (sess_catch.n:4)") != std::string::npos,
        "throw site freeze report");
    PASS();
}

void run_debugger_session_tests()
{
    test_session_initial_stop_and_continue();
    test_session_step_semantics();
    test_session_breakpoint_hit();
    test_session_bt_and_locals();
    test_session_locals_decl_scope();
    test_session_frame_select();
    test_session_break_by_func();
}

void run_debugger_sourcecache_tests()
{
    test_sourcecache_resolution();
    test_session_source_list();
    test_session_disasm_marker();
    test_disasm_type_kind_names();
    test_session_catch_throw();
}
