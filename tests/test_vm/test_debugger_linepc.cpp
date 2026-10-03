// --- ndb debugger tests: line/pc map ---
#include "test_debugger_common.h"

using namespace nlang;

// --- Task 3: line/pc map ---

void test_linepc_map_first_pc()
{
    //Each line maps to its first statement pc; map ascends by pc.
    TEST(linepc_map_first_pc);
    BuildOutcome b = buildSource("linepc_map",
        "int main() {\n"        //1
        "    int a = 1;\n"      //2
        "    a = a + 1;\n"      //3
        "    return a;\n"       //4
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("linepc_map");
    int mainIdx = mod.FindFunction("linepc_map.main");
    REQUIRE(mainIdx >= 0);
    const auto& mainf = mod.functions[static_cast<size_t>(mainIdx)];
    auto map = BuildLinePcMap(mainf);
    CHECK(map.size() == 3, "3 statement lines (2,3,4)");
    CHECK(map[0].line == 2 && map[1].line == 3 && map[2].line == 4,
        "lines ascend in order");
    CHECK(map[0].pc < map[1].pc && map[1].pc < map[2].pc, "pcs ascend");
    //Each mapped pc must actually sit on an OP_DebugInfo whose
    //operand round-trips the line (self-consistency of the decode).
    const auto& bc = mainf.bytecode;
    for (const auto& e : map) {
        REQUIRE(e.pc + 2 < bc.size());
        uint16_t dec = static_cast<uint16_t>(bc[e.pc + 1] | (bc[e.pc + 2] << 8));
        CHECK(dec == e.line, "pc points at its own line operand");
    }
    PASS();
}

void test_linepc_map_same_line_multi_anchor()
{
    //EVERY statement anchor is an entry (gdb-style one line, multiple
    //locations): the multi-declarator line keeps both declarator
    //anchors, the one-line if/else keeps cond + both arms — `b LINE`
    //can cover each statement on the line, not just the first.
    TEST(linepc_map_same_line_multi_anchor);
    BuildOutcome b = buildSource("linepc_multi_anchor",
        "int main() {\n"                               //1
        "    int a = 1, b = 2;\n"                      //2 (two anchors)
        "    if (a > b) { a = 3; } else { b = 4; }\n"  //3 (three anchors)
        "    return a + b;\n"                          //4
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("linepc_multi_anchor");
    int mainIdx = mod.FindFunction("linepc_multi_anchor.main");
    REQUIRE(mainIdx >= 0);
    const auto& mainf = mod.functions[static_cast<size_t>(mainIdx)];
    auto map = BuildLinePcMap(mainf);
    //2 (multidecl) + 3 (cond + both if/else arms) + 1 (return) = 6.
    CHECK(map.size() == 6, "one entry per statement anchor");
    const uint16_t want[] = {2, 2, 3, 3, 3, 4};
    for (size_t i = 0; i < map.size(); ++i)
        CHECK(map[i].line == want[i],
            "entry " + std::to_string(i) + " line");
    for (size_t i = 1; i < map.size(); ++i)
        CHECK(map[i - 1].pc < map[i].pc, "pcs ascend");
    PASS();
}

void test_linepc_map_finally_duplicates()
{
    //try/finally emits each finally-body statement's anchor TWICE: the
    //exception-path copy in the handler region FIRST, then the
    //normal-path copy. Every anchor is an entry, so BOTH copies appear
    //for every finally-body statement (review probe
    //temp/linepc_probe.cpp; the single-statement body is pinned by
    //test_linepc_map_finally_single_two_copies below).
    TEST(linepc_map_finally_duplicates);
    BuildOutcome b = buildSource("linepc_finally_dup",
        "int main() {\n"          //1
        "    int a = 0;\n"        //2
        "    try {\n"             //3
        "        a = 1;\n"        //4
        "    } finally {\n"       //5
        "        a = a + 1;\n"    //6
        "        a = a + 2;\n"    //7
        "    }\n"                 //8
        "    return a;\n"         //9
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("linepc_finally_dup");
    int mainIdx = mod.FindFunction("linepc_finally_dup.main");
    REQUIRE(mainIdx >= 0);
    const auto& mainf = mod.functions[static_cast<size_t>(mainIdx)];
    auto map = BuildLinePcMap(mainf);
    //Observed marker stream: 2,3,4,6,7,6,7,9 — finally lines 6 and 7
    //each appear twice, exception-path copies first.
    REQUIRE(map.size() == 8);
    const uint16_t want[] = {2, 3, 4, 6, 7, 6, 7, 9};
    for (size_t i = 0; i < map.size(); ++i)
        CHECK(map[i].line == want[i],
            "entry " + std::to_string(i) + " line");
    CHECK(map[3].pc < map[5].pc, "line 6 exception copy precedes normal");
    CHECK(map[4].pc < map[6].pc, "line 7 exception copy precedes normal");
    for (size_t i = 1; i < map.size(); ++i)
        CHECK(map[i - 1].pc < map[i].pc, "pcs ascend");
    //Every entry's pc sits on an OP_DebugInfo round-tripping its line.
    const auto& bc = mainf.bytecode;
    for (const auto& e : map) {
        REQUIRE(e.pc + 2 < bc.size());
        uint16_t dec = static_cast<uint16_t>(bc[e.pc + 1] | (bc[e.pc + 2] << 8));
        CHECK(dec == e.line, "pc points at its own line operand");
    }
    PASS();
}

void test_linepc_map_finally_single_two_copies()
{
    //try/finally compiles a single-statement finally body TWICE: the
    //exception-path copy (handler region) and the normal-path copy.
    //Both are entries — under the old adjacent-collapse the normal-path
    //pc was dropped, so `b LINE` could never fire on the normal path.
    TEST(linepc_map_finally_single_two_copies);
    BuildOutcome b = buildSource("linepc_finally_single",
        "int main() {\n"          //1
        "    int a = 0;\n"        //2
        "    try {\n"             //3
        "        a = 1;\n"        //4
        "    } finally {\n"       //5
        "        a = a + 1;\n"    //6
        "    }\n"                 //7
        "    return a;\n"         //8
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("linepc_finally_single");
    int mainIdx = mod.FindFunction("linepc_finally_single.main");
    REQUIRE(mainIdx >= 0);
    const auto& mainf = mod.functions[static_cast<size_t>(mainIdx)];
    auto map = BuildLinePcMap(mainf);
    //Observed marker stream: 2,3,4,6,6,8 — the finally line twice.
    REQUIRE(map.size() == 6);
    const uint16_t want[] = {2, 3, 4, 6, 6, 8};
    for (size_t i = 0; i < map.size(); ++i)
        CHECK(map[i].line == want[i],
            "entry " + std::to_string(i) + " line");
    CHECK(map[3].line == 6 && map[4].line == 6 && map[3].pc < map[4].pc,
        "finally line twice, exception-path copy first");
    //Both copies' pcs sit on OP_DebugInfo round-tripping the line.
    const auto& bc = mainf.bytecode;
    for (size_t i = 3; i <= 4; ++i) {
        REQUIRE(map[i].pc + 2 < bc.size());
        uint16_t dec = static_cast<uint16_t>(
            bc[map[i].pc + 1] | (bc[map[i].pc + 2] << 8));
        CHECK(dec == 6, "finally copy pc points at line 6");
    }
    PASS();
}

void test_instruction_stride_exact_landing()
{
    //Equivalence guard: the read-side stride table must agree with the
    //compiler's emission — walking every function's bytecode must land
    //exactly on its end without hitting the unknown-opcode throw.
    TEST(instruction_stride_exact_landing);
    BuildOutcome b = buildSource("stride_landing",
        "int twice(int v) { return v * 2; }\n"  //1
        "int main() {\n"                        //2
        "    int sum = 0;\n"                    //3
        "    int[] xs = new int[3];\n"          //4
        "    xs[0] = 1;\n"                      //5
        "    foreach (int x in xs) {\n"         //6
        "        sum = sum + x;\n"              //7
        "    }\n"                               //8
        "    int i = 0;\n"                      //9
        "    while (i < 2) {\n"                 //10
        "        i = i + 1;\n"                  //11
        "    }\n"                               //12
        "    try {\n"                           //13
        "        sum = twice(sum);\n"           //14
        "    } finally {\n"                     //15
        "        sum = sum + 1;\n"              //16
        "    }\n"                               //17
        "    string t = \"v\";\n"               //18
        "    string u = t + \"w\";\n"           //19
        "    if (u == \"vw\") {\n"              //20
        "        sum = sum + 10;\n"             //21
        "    }\n"                               //22
        "    return sum;\n"                     //23
        "}\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("stride_landing");
    REQUIRE(mod.functions.size() >= 2);
    bool walked = false;
    for (const auto& f : mod.functions) {
        if (f.bytecode.empty()) continue;
        size_t pc = 0;
        try {
            while (pc < f.bytecode.size())
                pc += InstructionStride(
                    static_cast<OpCode>(f.bytecode[pc]));
        } catch (const std::exception&) {
            FAIL("unknown opcode at pc " + std::to_string(pc)
                + " in " + f.name);
            return;
        }
        CHECK(pc == f.bytecode.size(),
            "walk must land exactly on the end of " + f.name);
        walked = true;
    }
    CHECK(walked, "at least one function walked");
    PASS();
}

void run_debugger_linepc_tests()
{
    test_linepc_map_first_pc();
    test_linepc_map_same_line_multi_anchor();
    test_linepc_map_finally_duplicates();
    test_linepc_map_finally_single_two_copies();
    test_instruction_stride_exact_landing();
}
