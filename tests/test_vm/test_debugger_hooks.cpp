// --- ndb debugger tests: debug hooks + read-only view ---
#include "test_debugger_common.h"

using namespace nlang;

// --- Task 2: hooks + read-only view ---

//Records every checkpoint as (line, depth) pairs.
class RecordingHooks : public IDebugHooks {
public:
    std::vector<std::pair<uint16_t, size_t>> statementStops;
    std::vector<uint16_t> throwLines;
    void OnStatement(const DebugStopInfo& s, IVmDebugView&) override {
        statementStops.push_back({s.line, s.depth});
    }
    void OnThrow(const DebugStopInfo& s, IVmDebugView&) override {
        throwLines.push_back(s.line);
    }
};

void test_hooks_line_sequence()
{
    //Straight-line program: statement checkpoints arrive in source
    //order, one per statement (same-line statements each fire).
    TEST(hooks_line_sequence);
    BuildOutcome b = buildSource("hook_lines",
        "int main() {\n"            //1
        "    int a = 1;\n"          //2
        "    int c = 2;\n"          //3
        "    return a + c;\n"       //4
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("hook_lines");
    VmExecutor exec;
    RecordingHooks hooks;
    exec.SetDebugHooks(&hooks);
    CHECK(exec.Execute(mod) == 3, "program result");
    CHECK(hooks.statementStops.size() == 3, "3 statements = 3 stops");
    CHECK(hooks.statementStops[0].first == 2, "first stop at line 2");
    CHECK(hooks.statementStops[1].first == 3, "second stop at line 3");
    CHECK(hooks.statementStops[2].first == 4, "third stop at line 4");
    for (auto& s : hooks.statementStops)
        CHECK(s.second == 1, "main-only run has depth 1");
    PASS();
}

void test_hooks_call_depths()
{
    //main -> f -> g is C++ recursion; depth tracks the NLang stack.
    TEST(hooks_call_depths);
    BuildOutcome b = buildSource("hook_depths",
        "int g() { return 5; }\n"   //1
        "int f() { return g(); }\n" //2
        "int main() { return f(); }\n"); //3
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("hook_depths");
    VmExecutor exec;
    RecordingHooks hooks;
    exec.SetDebugHooks(&hooks);
    CHECK(exec.Execute(mod) == 5, "program result");
    //Statements: main:3, f:2, g:1 — depth 1, 2, 3.
    REQUIRE(hooks.statementStops.size() == 3);
    CHECK(hooks.statementStops[0] == std::make_pair(uint16_t(3), size_t(1)),
        "main statement at depth 1");
    CHECK(hooks.statementStops[1] == std::make_pair(uint16_t(2), size_t(2)),
        "f statement at depth 2");
    CHECK(hooks.statementStops[2] == std::make_pair(uint16_t(1), size_t(3)),
        "g statement at depth 3");
    PASS();
}

//Captures view state at the stop on a chosen line of a target function.
//Stops fire BEFORE the statement runs — captured values reflect every
//statement up to but excluding stopLine, so tests pick the line
//accordingly (e.g. the return line to see computed locals).
class InspectHooks : public IDebugHooks {
public:
    std::string target;               //capture when frame-0 is this func
    uint16_t stopLine = 0;            //...on this source line
    bool captured = false;
    size_t frameCount = 0;
    uint16_t line = 0;
    std::vector<std::string> frameNames;
    std::vector<std::string> localLines;  //"name=display"
    void OnStatement(const DebugStopInfo& s, IVmDebugView& view) override {
        if (captured || s.line != stopLine
            || view.FrameInfo(0).funcName != target) return;
        captured = true;
        line = s.line;
        frameCount = view.FrameCount();
        for (size_t d = 0; d < frameCount; ++d)
            frameNames.push_back(view.FrameInfo(d).funcName);
        for (const auto& l : view.FrameLocals(0))
            localLines.push_back(l.name + "=" + l.display);
    }
    void OnThrow(const DebugStopInfo&, IVmDebugView&) override {}
};

void test_view_frames_and_locals()
{
    TEST(view_frames_and_locals);
    BuildOutcome b = buildSource("view_locals",
        "class Point { int x; int y; }\n"       //1
        "int inner(Point p) {\n"                //2
        "    int s = p.x + p.y;\n"              //3
        "    return s;\n"                       //4
        "}\n"                                   //5
        "int main() {\n"                        //6
        "    Point p = new Point{x: 6, y: 7};\n"//7
        "    return inner(p);\n"                //8
        "}\n");                                 //9
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("view_locals");
    VmExecutor exec;
    InspectHooks hooks;
    hooks.target = "view_locals.inner";
    hooks.stopLine = 4;  //return s; — line 3 has run, so s is computed
    exec.SetDebugHooks(&hooks);
    CHECK(exec.Execute(mod) == 13, "program result");
    CHECK(hooks.captured, "should stop inside inner");
    CHECK(hooks.frameCount == 2, "inner + main frames");
    CHECK(hooks.frameNames.size() == 2
        && hooks.frameNames[0] == "view_locals.inner"
        && hooks.frameNames[1] == "view_locals.main", "innermost-first ordering");
    //Exact visible-locals pin: filter hidden names (the ndb info-locals
    //contract), then the visible set must be exactly {p, s} — no extra
    //descriptors leak into the display. (Frame temps never appear here:
    //tempSlot1-4/returnSlot/evalArea are raw offsets outside func->locals.)
    std::vector<std::string> visible;
    for (const auto& l : hooks.localLines) {
        size_t eq = l.find('=');
        REQUIRE(eq != std::string::npos);
        std::string name = l.substr(0, eq);
        if (name.size() >= 2 && name[0] == '_' && name[1] == '_') continue;
        if (!name.empty() && name[0] == '$') continue;
        visible.push_back(l);
    }
    CHECK(visible.size() == 2, "exactly two visible locals (param p, s)");
    bool sawP = false, sawS = false;
    for (const auto& l : visible) {
        if (l == "p=Point{x=6, y=7}") sawP = true;
        if (l == "s=13") sawS = true;
    }
    CHECK(sawP, "class local renders one-level fields (got lines mismatch)");
    CHECK(sawS, "int local renders value");
    PASS();
}

//Scope visibility of locals in the debug view. The frame is flat and
//static — every slot exists from function entry — but a local must join
//the display only once execution reaches its declaration line. On the
//decl line itself it shows the zero slot (gdb/Visual Studio convention:
//in scope at the declaration, uninitialized until the initializer
//runs); while paused on earlier lines it stays hidden. This pins the
//reported bug: pausing on line N showed line N+2's not-yet-declared
//string local as "".
void test_view_locals_decl_scope()
{
    TEST(view_locals_decl_scope);
    BuildOutcome b = buildSource("decl_scope",
        "int main() {\n"                     //1
        "    int early = 1;\n"               //2
        "    int mid = early + 1;\n"         //3
        "    string late = \"v\";\n"         //4
        "    return mid;\n"                  //5
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("decl_scope");
    //Fresh run per stop line; stops fire BEFORE the statement runs, so
    //a stop on line N has executed exactly lines < N.
    auto visibleAt = [&](uint16_t stopLine) {
        VmExecutor exec;
        InspectHooks hooks;
        hooks.target = "decl_scope.main";
        hooks.stopLine = stopLine;
        exec.SetDebugHooks(&hooks);
        exec.Execute(mod);
        if (!hooks.captured)
            return std::vector<std::string>{"<no capture>"};
        return hooks.localLines;
    };
    auto hasRow = [](const std::vector<std::string>& rows,
                     const char* row) {
        for (const auto& r : rows)
            if (r == row) return true;
        return false;
    };
    auto hasName = [](const std::vector<std::string>& rows,
                      const char* name) {
        const std::string prefix = std::string(name) + "=";
        for (const auto& r : rows)
            if (r.compare(0, prefix.size(), prefix) == 0) return true;
        return false;
    };
    const auto at2 = visibleAt(2);
    CHECK(hasRow(at2, "early=0"),
        "paused on its own decl line: early in scope, zero value");
    CHECK(!hasName(at2, "mid"), "mid hidden while paused on line 2");
    CHECK(!hasName(at2, "late"), "late hidden while paused on line 2");
    const auto at3 = visibleAt(3);
    CHECK(hasRow(at3, "early=1"), "early keeps its computed value");
    CHECK(hasRow(at3, "mid=0"), "mid joins on its own decl line (zero)");
    CHECK(!hasName(at3, "late"), "late hidden while paused on line 3");
    const auto at4 = visibleAt(4);
    CHECK(hasRow(at4, "late=\"\""),
        "string local on its decl line renders the zero slot as \"\"");
    const auto at5 = visibleAt(5);
    CHECK(hasRow(at5, "mid=2") && hasRow(at5, "late=\"v\""),
        "computed values show after their initializers run");
    PASS();
}

//Captures "name:kindName=display" rows for main() at the return line —
//the 0.7.5 scalar-family display contract (one row per declared local,
//kindName straight from the registry).
class FamilyHooks : public IDebugHooks {
public:
    std::vector<std::string> rows;
    uint16_t stopLine = 0;
    bool captured = false;
    void OnStatement(const DebugStopInfo& s, IVmDebugView& view) override {
        if (captured || s.line != stopLine
            || view.FrameInfo(0).funcName != "family_locals.main") return;
        captured = true;
        for (const auto& l : view.FrameLocals(0))
            rows.push_back(l.name + ":" + l.kindName + "=" + l.display);
    }
    void OnThrow(const DebugStopInfo&, IVmDebugView&) override {}
};

static std::string FamilyRow(const std::vector<std::string>& rows,
                             const char* name) {
    const std::string prefix = std::string(name) + ":";
    for (const auto& r : rows)
        if (r.compare(0, prefix.size(), prefix) == 0)
            return r.substr(prefix.size());
    return "<missing " + std::string(name) + ">";
}

void test_view_scalar_family_display()
{
    TEST(view_scalar_family_display);
    BuildOutcome b = buildSource("family_locals",
        "int main() {\n"                          //1
        "    bool flag = 1 < 2;\n"                //2
        "    bool off = 2 < 1;\n"                 //3
        "    char c = '中';\n"                    //4
        "    char nl = '\\n';\n"                  //5
        "    char a = '\\u0041';\n"               //6
        "    byte b = 42;\n"                      //7
        "    ushort us = 65535;\n"                //8
        "    uint u = 4000000000;\n"              //9
        "    long l = 5000000000;\n"              //10
        "    double d = 0.5;\n"                   //11
        "    float f = 1.25f;\n"                  //12
        "    ulong ul = 18446744073709551615;\n"  //13
        "    return 0;\n"                         //14
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("family_locals");
    VmExecutor exec;
    FamilyHooks hooks;
    hooks.stopLine = 14;
    exec.SetDebugHooks(&hooks);
    CHECK(exec.Execute(mod) == 0, "program result");
    CHECK(hooks.captured, "should stop at main's return");
    CHECK(hooks.rows.size() == 12, "one row per declared local");
    CHECK(FamilyRow(hooks.rows, "flag") == "bool=true", "bool true");
    CHECK(FamilyRow(hooks.rows, "off") == "bool=false", "bool false");
    CHECK(FamilyRow(hooks.rows, "c") == "char='中' (U+4E2D)",
        "char: quoted code point + identity tag");
    CHECK(FamilyRow(hooks.rows, "nl") == "char=(U+000A)",
        "non-printable char keeps the tag, drops the quotes");
    CHECK(FamilyRow(hooks.rows, "a") == "char='A' (U+0041)",
        "\\uXXXX literal round-trips");
    CHECK(FamilyRow(hooks.rows, "b") == "byte=42", "byte decimal");
    CHECK(FamilyRow(hooks.rows, "us") == "ushort=65535", "ushort decimal");
    CHECK(FamilyRow(hooks.rows, "u") == "uint=4000000000", "uint decimal");
    CHECK(FamilyRow(hooks.rows, "l") == "long=5000000000", "long direct");
    CHECK(FamilyRow(hooks.rows, "d") == "double=0.5", "double direct");
    CHECK(FamilyRow(hooks.rows, "f") == "float=1.25", "float direct");
    CHECK(FamilyRow(hooks.rows, "ul") == "ulong=18446744073709551615",
        "ulong direct");
    PASS();
}

void test_hooks_on_throw()
{
    //OnThrow fires at the throw site with the stack still alive; the
    //program then completes the unwind into the catch handler.
    TEST(hooks_on_throw);
    BuildOutcome b = buildSource("hook_throw",
        "int main() {\n"                             //1
        "    try {\n"                                //2
        "        throw new Exception(\"boom\");\n"   //3
        "    } catch (Exception e) {\n"              //4
        "        return 7;\n"                        //5
        "    }\n"                                   //6
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("hook_throw");
    VmExecutor exec;
    RecordingHooks hooks;
    exec.SetDebugHooks(&hooks);
    CHECK(exec.Execute(mod) == 7, "catch handler runs after OnThrow");
    CHECK(hooks.throwLines.size() == 1, "one OnThrow");
    CHECK(hooks.throwLines[0] == 3, "throw anchored at line 3");
    PASS();
}

void test_view_value_kinds()
{
    //Shallow formatters across value kinds: string, struct, array —
    //int/class are covered by view_frames_and_locals above.
    TEST(view_value_kinds);
    BuildOutcome b = buildSource("view_kinds",
        "struct P { int x; int y; }\n"               //1
        "int main() {\n"                             //2
        "    string s = \"hi\";\n"                   //3
        "    P p = new P{x: 1, y: 2};\n"             //4
        "    int[] a = new int[2];\n"                //5
        "    a[0] = 7;\n"                            //6
        "    a[1] = 8;\n"                            //7
        "    return 0;\n"                            //8
        "}\n");                                      //9
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("view_kinds");
    VmExecutor exec;
    InspectHooks hooks;
    hooks.target = "view_kinds.main";
    hooks.stopLine = 8;  //return 0; — s/p/a all assigned by then
    exec.SetDebugHooks(&hooks);
    CHECK(exec.Execute(mod) == 0, "program result");
    REQUIRE(hooks.captured);
    //Exact full-line pins (no substring slop — the render format is
    //the e2e assertion contract).
    bool sawS = false, sawP = false, sawA = false;
    for (const auto& l : hooks.localLines) {
        if (l == "s=\"hi\"") sawS = true;
        if (l == "p=P{x=1, y=2}") sawP = true;
        if (l == "a=int[2]{7, 8}") sawA = true;
    }
    CHECK(sawS, "string local renders quoted");
    CHECK(sawP, "struct local renders one-level fields");
    CHECK(sawA, "array local renders element kind + values");
    PASS();
}

void test_view_struct_array_field_local()
{
    //Array redesign B Task 5 pin: an array-typed struct field renders
    //through FormatDebugField's declared RTK_Array case (the array
    //formatter). Before the declared-kind fix, `int[]` was filed as
    //RTK_Int32 and the field rendered as a raw integer.
    TEST(view_struct_array_field_local);
    BuildOutcome b = buildSource("view_struct_arr_field",
        "struct Box { int[] a; }\n"                  //1
        "int main() {\n"                             //2
        "    Box b;\n"                               //3
        "    b.a = new int[2];\n"                    //4
        "    b.a[0] = 7;\n"                          //5
        "    b.a[1] = 8;\n"                          //6
        "    return 0;\n"                            //7
        "}\n");                                      //8
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("view_struct_arr_field");
    VmExecutor exec;
    InspectHooks hooks;
    hooks.target = "view_struct_arr_field.main";
    hooks.stopLine = 7;  //return 0; — b.a fully assigned by then
    exec.SetDebugHooks(&hooks);
    CHECK(exec.Execute(mod) == 0, "program result");
    REQUIRE(hooks.captured);
    bool sawBox = false;
    for (const auto& l : hooks.localLines)
        if (l == "b=Box{a=int[2]{7, 8}}") sawBox = true;
    CHECK(sawBox, "struct local's array field renders via array formatter");
    PASS();
}

void test_hooks_cpp_exception_propagates()
{
    //A front end that breaks the contract and throws from a
    //checkpoint: the C++ exception does not match the VM's
    //NLangThrow handler, so it propagates out of Execute as a normal
    //C++ exception the embedder can catch — no crash, no silent
    //swallow. (The compliant front end catches its own exceptions;
    //DebugSession's command loop does.)
    TEST(hooks_cpp_exception_propagates);
    BuildOutcome b = buildSource("hook_throw_cpp",
        "int main() {\n"      //1
        "    int a = 1;\n"    //2
        "    return a;\n"     //3
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("hook_throw_cpp");
    VmExecutor exec;
    class ThrowingHooks : public IDebugHooks {
    public:
        void OnStatement(const DebugStopInfo&, IVmDebugView&) override {
            throw std::runtime_error("front end broke the contract");
        }
        void OnThrow(const DebugStopInfo&, IVmDebugView&) override {}
    };
    ThrowingHooks hooks;
    exec.SetDebugHooks(&hooks);
    bool caught = false;
    try {
        exec.Execute(mod);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    CHECK(caught, "C++ exception from a checkpoint escapes Execute "
          "cleanly instead of crashing or vanishing");
    PASS();
}

void run_debugger_hooks_tests()
{
    test_hooks_line_sequence();
    test_hooks_call_depths();
    test_view_frames_and_locals();
    test_view_locals_decl_scope();
    test_view_scalar_family_display();
    test_hooks_on_throw();
    test_view_value_kinds();
    test_view_struct_array_field_local();
    test_hooks_cpp_exception_propagates();
}
