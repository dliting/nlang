// --- ndb reference-slot display tests (v0.13.9 phase 3 Task 11) ---
// The locals view over the byref slot family: & formals/locals render
// base->cell->dereferenced value, promoted scalars/strings render the
// plain value (never a handle), and the LocalDescriptor kind that
// separates the two families rides the .ncu locals table (validated by
// the cross-module stop inside a linked library frame).
#include "test_debugger_common.h"

using namespace nlang;

namespace {

//Captures name|kindName|display rows for the target function's frame-0
//at every stop on stopLine (later stops overwrite — the single-stop
//tests; the foreach test subclasses the accumulation).
class RefDisplayHooks : public IDebugHooks {
public:
    std::string target;
    uint16_t stopLine = 0;
    bool captured = false;
    std::vector<std::string> rows;
    void OnStatement(const DebugStopInfo& s, IVmDebugView& view) override {
        if (s.line != stopLine
            || view.FrameInfo(s.contextId, 0).funcName != target) return;
        captured = true;
        Capture(view, s.contextId);
    }
    void OnThrow(const DebugStopInfo&, IVmDebugView&) override {}
protected:
    virtual void Capture(IVmDebugView& view, int contextId) {
        rows.clear();
        for (const auto& l : view.FrameLocals(contextId, 0)) {
            if (DebugSessionController::IsHiddenLocalName(l.name)) continue;
            rows.push_back(l.name + "|" + l.kindName + "|"
                + l.display);
        }
    }
};

const std::string* RowOf(const std::vector<std::string>& rows,
                         const std::string& name) {
    for (const auto& r : rows)
        if (r.rfind(name + "|", 0) == 0) return &r;
    return nullptr;
}

//A & slot's display contract: "&#<handle>[<cell>] -> <value>". The
//handle number is allocation-order dependent, so tests pin the prefix,
//the cell and the dereferenced value.
bool RefDisplayMatches(const std::string& row, const std::string& cell,
                       const std::string& value) {
    const size_t kindEnd = row.find('|');
    const size_t dispStart = row.find('|', kindEnd + 1) + 1;
    const std::string disp = row.substr(dispStart);
    return disp.rfind("&#", 0) == 0
        && disp.find("[" + cell + "] -> " + value)
            != std::string::npos;
}

} //namespace

//& formals: the pair renders base->cell->value, kindName names the
//element kind with the & suffix. `a` binds the caller's promoted local
//(box payload cell), `b` materializes its rvalue into a fresh box.
static void test_ref_formal_display()
{
    TEST(ref_formal_display);
    BuildOutcome b = buildSource("refdisp_formal",
        "int add(int& a, const int& b) {\n"  //1
        "    int s = a + b;\n"               //2
        "    return s;\n"                    //3  <- stop: a,b,s live
        "}\n"                                //4
        "int main() {\n"                     //5
        "    int x = 5;\n"                   //6
        "    return add(x, 40);\n"           //7
        "}\n");                              //8
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("refdisp_formal");
    VmExecutor exec;
    RefDisplayHooks hooks;
    hooks.target = "refdisp_formal.add";
    hooks.stopLine = 3;
    exec.SetDebugHooks(&hooks);
    CHECK(exec.Execute(mod) == 45, "program result");
    CHECK(hooks.captured, "should stop inside add");
    const std::string* a = RowOf(hooks.rows, "a");
    const std::string* bb = RowOf(hooks.rows, "b");
    REQUIRE(a && bb);
    CHECK(RefDisplayMatches(*a, "1", "5"),
        "the & formal renders base->cell->value (got: " + *a + ")");
    CHECK(RefDisplayMatches(*bb, "1", "40"),
        "the const & formal's materialized pair renders (got: " + *bb + ")");
    CHECK(a->find("|int&|") != std::string::npos,
        "the & formal's kindName is int& (got: " + *a + ")");
    CHECK(bb->find("|int&|") != std::string::npos,
        "the const & formal's kindName is int& (got: " + *bb + ")");
    const std::string* s = RowOf(hooks.rows, "s");
    REQUIRE(s);
    CHECK(s->find("|45") != std::string::npos,
        "the plain local keeps its value display (got: " + *s + ")");
    PASS();
}

//Promoted scalar: `x`'s storage is the stable box, but the display
//keeps showing the VALUE (never a handle); the & binding to it shows
//the pair. String promotion mirrors the shape.
static void test_promoted_scalar_and_ref_display()
{
    TEST(promoted_scalar_and_ref_display);
    BuildOutcome b = buildSource("refdisp_promote",
        "int main() {\n"                  //1
        "    int x = 1;\n"                //2
        "    int& r = x;\n"               //3
        "    r = 7;\n"                    //4
        "    return x;\n"                 //5  <- stop
        "}\n");                           //6
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("refdisp_promote");
    VmExecutor exec;
    RefDisplayHooks hooks;
    hooks.target = "refdisp_promote.main";
    hooks.stopLine = 5;
    exec.SetDebugHooks(&hooks);
    CHECK(exec.Execute(mod) == 7, "program result");
    CHECK(hooks.captured, "should stop in main");
    const std::string* x = RowOf(hooks.rows, "x");
    const std::string* r = RowOf(hooks.rows, "r");
    REQUIRE(x && r);
    CHECK(x->find("x|int|7") == 0,
        "the promoted scalar shows the plain value, not a handle (got: "
        + *x + ")");
    CHECK(RefDisplayMatches(*r, "1", "7"),
        "the & binding shows the pair into the promoted box (got: "
        + *r + ")");
    CHECK(r->find("|int&|") != std::string::npos,
        "the & binding's kindName is int& (got: " + *r + ")");
    PASS();
}

static void test_promoted_string_and_ref_display()
{
    TEST(promoted_string_and_ref_display);
    BuildOutcome b = buildSource("refdisp_promote_str",
        "int main() {\n"                          //1
        "    string name = \"a\";\n"              //2
        "    string& s = name;\n"                 //3
        "    s = \"other\";\n"                    //4
        "    return 0;\n"                         //5  <- stop
        "}\n");                                   //6
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("refdisp_promote_str");
    VmExecutor exec;
    RefDisplayHooks hooks;
    hooks.target = "refdisp_promote_str.main";
    hooks.stopLine = 5;
    exec.SetDebugHooks(&hooks);
    CHECK(exec.Execute(mod) == 0, "program result");
    CHECK(hooks.captured, "should stop in main");
    const std::string* name = RowOf(hooks.rows, "name");
    const std::string* s = RowOf(hooks.rows, "s");
    REQUIRE(name && s);
    CHECK(name->find("name|string|\"other\"") == 0,
        "the promoted string shows the quoted value (got: " + *name
        + ")");
    CHECK(RefDisplayMatches(*s, "1", "\"other\""),
        "the & string binding shows the pair (got: " + *s + ")");
    CHECK(s->find("|string&|") != std::string::npos,
        "the & string binding's kindName is string& (got: " + *s + ")");
    PASS();
}

//The bindable target families: a whole-struct & (offset-0 pair renders
//the record), a struct-field & (the field's own cell), and an array
//element & (header + index cells).
static void test_ref_target_families_display()
{
    TEST(ref_target_families_display);
    BuildOutcome b = buildSource("refdisp_targets",
        "struct Pt {\n"                    //1
        "    int x;\n"                     //2
        "    int y;\n"                     //3
        "}\n"                              //4
        "int main() {\n"                   //5
        "    Pt q;\n"                      //6
        "    q.x = 3;\n"                   //7
        "    q.y = 4;\n"                   //8
        "    Pt& whole = q;\n"             //9
        "    int& fx = q.x;\n"             //10
        "    int[] nums = int[2];\n"       //11
        "    nums[0] = 9;\n"               //12
        "    nums[1] = 8;\n"               //13
        "    int& el = nums[1];\n"         //14
        "    int t = 0;\n"                 //15  <- stop
        "    return whole.y + fx + el;\n"  //16  = 4+3+8
        "}\n");                            //17
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("refdisp_targets");
    VmExecutor exec;
    RefDisplayHooks hooks;
    hooks.target = "refdisp_targets.main";
    hooks.stopLine = 15;
    exec.SetDebugHooks(&hooks);
    CHECK(exec.Execute(mod) == 15, "program result");
    CHECK(hooks.captured, "should stop in main");
    const std::string* whole = RowOf(hooks.rows, "whole");
    const std::string* fx = RowOf(hooks.rows, "fx");
    const std::string* el = RowOf(hooks.rows, "el");
    REQUIRE(whole && fx && el);
    CHECK(RefDisplayMatches(*whole, "0", "Pt{x=3, y=4}"),
        "the whole-struct & renders the record at offset 0 (got: "
        + *whole + ")");
    CHECK(RefDisplayMatches(*fx, "0", "3"),
        "the field & names the field's own cell (got: " + *fx + ")");
    CHECK(RefDisplayMatches(*el, "7", "8"),
        "the element & names the header+index cell (got: " + *el + ")");
    CHECK(whole->find("|struct&|") != std::string::npos,
        "the whole-struct & kindName is struct& (got: " + *whole + ")");
    PASS();
}

//foreach reference loop variable: each iteration's pair names the
//current element cell — the display follows the loop.
static void test_foreach_ref_var_display()
{
    TEST(foreach_ref_var_display);
    BuildOutcome b = buildSource("refdisp_foreach",
        "int main() {\n"                       //1
        "    int[] nums = int[2];\n"           //2
        "    nums[0] = 5;\n"                   //3
        "    nums[1] = 6;\n"                   //4
        "    foreach (int& v in nums) {\n"     //5
        "        int t = v;\n"                 //6  <- stop per iteration
        "    }\n"                              //7
        "    return 0;\n"                      //8
        "}\n");                                //9
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("refdisp_foreach");
    VmExecutor exec;
    class ForeachHooks : public RefDisplayHooks {
    public:
        std::vector<std::string> vRows;  //one row per stop, in order
        void Capture(IVmDebugView& view, int contextId) override {
            RefDisplayHooks::Capture(view, contextId);
            if (const std::string* v = RowOf(rows, "v"))
                vRows.push_back(*v);
        }
    } hooks;
    hooks.target = "refdisp_foreach.main";
    hooks.stopLine = 6;
    exec.SetDebugHooks(&hooks);
    CHECK(exec.Execute(mod) == 0, "program result");
    CHECK(hooks.vRows.size() == 2,
        "two loop iterations stopped (got "
        + std::to_string(hooks.vRows.size()) + ")");
    REQUIRE(hooks.vRows.size() == 2);
    CHECK(RefDisplayMatches(hooks.vRows[0], "6", "5"),
        "iteration 0 pairs the first element cell (got: "
        + hooks.vRows[0] + ")");
    CHECK(RefDisplayMatches(hooks.vRows[1], "7", "6"),
        "iteration 1 pairs the second element cell (got: "
        + hooks.vRows[1] + ")");
    PASS();
}

//Cross-module: the LocalDescriptor kind byte rides the .ncu locals
//table, so a & formal of a LINKED library function displays the same
//pair form (the wire path, not the in-memory builder path).
static void test_cross_module_ref_formal_display()
{
    TEST(cross_module_ref_formal_display);
    BuildOutcome lib = buildSource("refdisp_xmod_lib",
        "int bump(int& v) {\n"       //1
        "    v = v + 1;\n"           //2
        "    return v;\n"            //3  <- stop
        "}\n");                      //4
    CHECK(lib.ok, "lib build should succeed: " + lib.diagnostics);
    //Remove the producer source or the consumer's import resolves the
    //library as an inlined source TU (the discovery layer prefers a
    //sibling .n) — the frame would then run builder-side locals and the
    //wire path would go untested.
    std::filesystem::remove(scratchDir() / "refdisp_xmod_lib.n");
    BuildOutcome main = buildConsumer("refdisp_xmod_main",
        "import refdisp_xmod_lib;\n"     //1
        "int main() {\n"                 //2
        "    int x = 5;\n"               //3
        "    return bump(x);\n"          //4
        "}\n");                          //5
    CHECK(main.ok, "consumer build should succeed: " + main.diagnostics);
    CompiledModule mod = loadLinked("refdisp_xmod_main");
    VmExecutor exec;
    RefDisplayHooks hooks;
    hooks.target = "refdisp_xmod_lib.bump";
    hooks.stopLine = 3;
    exec.SetDebugHooks(&hooks);
    CHECK(exec.Execute(mod) == 6, "program result");
    CHECK(hooks.captured, "should stop inside the linked bump");
    const std::string* v = RowOf(hooks.rows, "v");
    REQUIRE(v);
    CHECK(RefDisplayMatches(*v, "1", "6"),
        "the linked & formal renders the pair (got: " + *v + ")");
    CHECK(v->find("|int&|") != std::string::npos,
        "the linked & formal's kindName is int& (got: " + *v + ")");
    PASS();
}

void run_debugger_refs_tests()
{
    test_ref_formal_display();
    test_promoted_scalar_and_ref_display();
    test_promoted_string_and_ref_display();
    test_ref_target_families_display();
    test_foreach_ref_var_display();
    test_cross_module_ref_formal_display();
}
