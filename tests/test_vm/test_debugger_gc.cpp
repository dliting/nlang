// --- ndb debugger tests: GC pins + element-opcode kind checks ---
#include "test_debugger_common.h"

using namespace nlang;

// --- Task 6 GC stress pins (array redesign B, spec §5.3#1) ---
//Regression pins, not red->green: both drive the heap past
//GC_THRESHOLD_DEFAULT (1024 records) AFTER the kept references are
//established, then verify end-to-end value integrity. Real allocation,
//real collection — same in-process compile+execute shape as
//test_v19_import_gc_roots in test_debugger_format.cpp.

void test_gc_tracks_class_elements_through_lists()
{
    //Container form: List<C> churn stress. Class-typed elements are NOT
    //boxed (BoxingTagFor returns no-box for class T) — the list store
    //holds raw heap idxs and MarkPhase's list-element loop marks and
    //pushes them, so the kept instances are genuinely traced: they are
    //reachable ONLY through the container here. ASan-clean by design.
    //The List<int[]> variant of this shape is NOT a pin today: erasure
    //boxes array elements under an RTK_Int32 tag, the wrapped records go
    //untraced and are swept, and reads hit cleared slots (ASan
    //container-overflow, measured 2026-09-12; Release passes only via
    //stale bits) — ledgered under the generic-erasure entry (roadmap.md
    //已知遗留), fix belongs to the C phase.
    TEST(gc_tracks_class_elements_through_lists);
    BuildOutcome b = buildSource("gc_container_pin",
        "class C {\n"
        "    public int v;\n"
        "}\n"
        "int main() {\n"
        "    List<C> keep = new List<C>();\n"
        "    int i = 0;\n"
        "    while (i < 32) {\n"
        "        C a = new C();\n"
        "        a.v = i * 7 + 1;\n"
        "        keep.add(a);\n"
        "        i += 1;\n"
        "    }\n"
        "    int churn = 0;\n"
        "    while (churn < 20000) {\n"
        "        C t = new C();\n"
        "        churn += 1;\n"
        "    }\n"
        "    int bad = 0;\n"
        "    i = 0;\n"
        "    while (i < 32) {\n"
        "        C a = keep.get(i);\n"
        "        if (a.v != i * 7 + 1) bad += 1;\n"
        "        i += 1;\n"
        "    }\n"
        "    return bad;\n"
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("gc_container_pin");
    VmExecutor exec;
    CHECK(exec.Execute(mod) == 0,
        "List<C> elements must survive GC churn");
    PASS();
}

void test_gc_tracks_class_elements_through_arrays()
{
    //Direct form: C[] element slots hold class references — pins the
    //array branch's Class element arm: the kept instances are reachable
    //ONLY through the array record, so every collection between the
    //population and verify loops must trace through that arm.
    TEST(gc_tracks_class_elements_through_arrays);
    BuildOutcome b = buildSource("gc_classarr_pin",
        "class C {\n"
        "    public int v;\n"
        "}\n"
        "int main() {\n"
        "    C[] cs = new C[3];\n"
        "    int i = 0;\n"
        "    while (i < 3) {\n"
        "        C c = new C();\n"
        "        c.v = 100 + i;\n"
        "        cs[i] = c;\n"
        "        i += 1;\n"
        "    }\n"
        "    int churn = 0;\n"
        "    while (churn < 20000) {\n"
        "        C t = new C();\n"
        "        churn += 1;\n"
        "    }\n"
        "    int bad = 0;\n"
        "    i = 0;\n"
        "    while (i < 3) {\n"
        "        if (cs[i].v != 100 + i) bad += 1;\n"
        "        i += 1;\n"
        "    }\n"
        "    return bad;\n"
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("gc_classarr_pin");
    VmExecutor exec;
    CHECK(exec.Execute(mod) == 0,
        "C[] elements must survive GC churn");
    PASS();
}

// --- C-period hole 3: element-opcode slot kind checks ---
//The three element opcodes are only emitted for array-typed bases, so
//a non-array slot kind at any of them is a compiler invariant
//violation. Born as the hole-3 red: List<int[]> boxed elements under
//an RTK_Int32 tag, MarkPhase traced the box but never the wrapped
//RTK_Array record, the sweep cleared its kind, and the kept element's
//read reached OP_LoadElement on a non-array slot (named throw).
//C-period Task 5 landed raw-handle flow + GC RTK_Array tracing, so the
//same program is now the positive lifetime pin: the array survives GC
//pressure through the List slot and main returns 40. The kind checks
//stay in place — any regression back to dangling handles turns this
//green value assertion into the named throw again.
void test_array_opcode_kind_check()
{
    TEST(array_opcode_kind_check);
    BuildOutcome b = buildSource("arr_kind_check",
        "int[] mkArr(int v) {\n"
        "    int[] a = new int[2];\n"
        "    a[0] = v;\n"
        "    a[1] = v + 1;\n"
        "    return a;\n"
        "}\n"
        "int churn() {\n"
        "    List<int[]> keep = new List<int[]>();\n"
        "    keep.add(mkArr(40));\n"
        "    for (int i = 0; i < 3000; i = i + 1) {\n"
        "        List<int[]> tmp = new List<int[]>();\n"
        "        tmp.add(mkArr(i));\n"
        "    }\n"
        "    return keep.get(0)[0];\n"
        "}\n"
        "int main() { return churn(); }\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("arr_kind_check");
    VmExecutor exec;
    int rc = -1;
    try {
        rc = exec.Execute(mod);
    } catch (const std::runtime_error& e) {
        CHECK(false, std::string("raw-handle array must stay GC-traced, ")
            + "got: " + e.what());
    }
    CHECK(rc == 40, "element survives GC pressure with its value, "
        "main returned " + std::to_string(rc));
    PASS();
}

void run_debugger_gc_tests()
{
    //Task 6 GC stress pins (real allocation + real collection).
    test_gc_tracks_class_elements_through_lists();
    test_gc_tracks_class_elements_through_arrays();

    //C-period hole 3: element-opcode slot kind checks.
    test_array_opcode_kind_check();

}
