// Stage-2 task 2: comma index lists and ND creation resolution.
// Resolution reaches the creation/subscript branches and the emit-side
// temporary gates fire named diagnostics (ND emission lands in task 4).
// In-process pipeline per test_native_array_abi.cpp (static-init contract).

#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include <nlang/compiler/CastInfo.h>
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "nlang/vm/NativeHost.h"
#include "VmExecutor.h"
#include "NcuLoader.h"
#include "NcuLinker.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifndef STDLIB_DIR
#define STDLIB_DIR ""
#endif

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define TEST(name) \
    do { std::cerr << "  " << #name << " ... "; } while(0)
#define PASS() \
    do { ++g_pass; std::cerr << "OK\n"; } while(0)
#define FAIL(msg) \
    do { ++g_fail; std::cerr << "FAIL: " << msg << "\n"; } while(0)
#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)

static std::filesystem::path scratchDir()
{
    static const auto dir = std::filesystem::temp_directory_path()
        / "nlang_test_nd_resolve";
    std::filesystem::create_directories(dir);
    return dir;
}

struct BuildOutcome
{
    bool ok = false;
    std::string diagnostics;
};

static BuildOutcome buildSource(const std::string& tag,
    const std::string& source)
{
    const auto dir = scratchDir();
    const auto nPath = dir / (tag + ".n");
    const auto modPath = dir / (tag + ".ncu");
    std::filesystem::remove(modPath);

    {
        std::ofstream out(nPath, std::ios::binary);
        out << "import io;\n" << source;
    }

    BuildParams params;
    params.m_SourceFiles.push_back(nPath.string());
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;

    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);

    BuildOutcome outcome;
    bool built = false;
    try {
        built = builder.BuildArtifacts();
    } catch (const std::exception& e) {
        outcome.diagnostics += std::string("internal error: ") + e.what();
        return outcome;
    }

    outcome.ok = built && std::filesystem::exists(modPath);
    for (auto it = logger.cbegin(); it != logger.cend(); ++it)
        outcome.diagnostics += (*it)->Message() + "\n";
    return outcome;
}

static bool DiagHas(const BuildOutcome& o, const char* text)
{
    return o.diagnostics.find(text) != std::string::npos;
}

//ND creation resolves and emits (the Task-2 temporary gate is
//gone; runtime behavior is covered by the ndarray e2e suite).
void test_nd_creation_resolves_to_gate()
{
    TEST(nd_creation_emits);
    const BuildOutcome o = buildSource("nd_create",
        "int main() {\n"
        "    int[,] a = int[2,3];\n"
        "    return 0;\n"
        "}\n");
    CHECK(o.ok, "comma creation must compile (gate removed in task 4)");
    CHECK(!DiagHas(o, "syntax error"),
        "comma creation must PARSE (no syntax error)");
    PASS();
}

//Subscript arity: index count must equal the rank.
void test_subscript_arity_diagnostics()
{
    TEST(subscript_arity_diagnostics);
    const BuildOutcome few = buildSource("nd_arity_few",
        "int main() {\n"
        "    int[,] a = int[2,3];\n"
        "    int v = a[1];\n"
        "    return v;\n"
        "}\n");
    CHECK(!few.ok && DiagHas(few, "indices") && !DiagHas(few, "syntax error"),
        "one index on rank-2 must name the arity mismatch");
    const BuildOutcome many = buildSource("nd_arity_many",
        "int main() {\n"
        "    int[,] a = int[2,3];\n"
        "    int v = a[1,2,3];\n"
        "    return v;\n"
        "}\n");
    CHECK(!many.ok && DiagHas(many, "indices"),
        "three indices on rank-2 must name the arity mismatch");
    PASS();
}

//Jagged chains stay rejected; the rank does not confuse the depth walk.
void test_jagged_still_rejected()
{
    TEST(jagged_still_rejected);
    const BuildOutcome o = buildSource("nd_jagged2",
        "int main() {\n"
        "    double[,][] b = null;\n"
        "    return 0;\n"
        "}\n");
    CHECK(!o.ok && DiagHas(o, "jagged"),
        "mixed ND/jagged nesting must stay rejected");
    PASS();
}

//Every ND value path emits (the Task-2 gates are gone; task 5
//landed the rectangular init-list lowering).
void test_emit_gates_cover_nd_paths()
{
    TEST(nd_paths_emit_gates_gone);
    //subscript read + compound assign (+= on an element)
    const BuildOutcome read = buildSource("nd_gate_read",
        "int main() {\n"
        "    int[,] a = int[2,3];\n"
        "    int v = a[0,0];\n"
        "    return v;\n"
        "}\n");
    CHECK(read.ok, "subscript read must emit");

    const BuildOutcome comp = buildSource("nd_gate_comp",
        "int main() {\n"
        "    int[,] a = int[2,3];\n"
        "    a[0,0] += 1;\n"
        "    return 0;\n"
        "}\n");
    CHECK(comp.ok, "compound element assign must emit");

    //subscript store (statement form) and field store of an element
    const BuildOutcome store = buildSource("nd_gate_store",
        "int main() {\n"
        "    int[,] a = int[2,3];\n"
        "    a[0,0] = 1;\n"
        "    return 0;\n"
        "}\n");
    CHECK(store.ok, "subscript store must emit");

    //foreach over an ND source
    const BuildOutcome fe = buildSource("nd_gate_foreach",
        "int main() {\n"
        "    int[,] a = int[2,3];\n"
        "    int s = 0;\n"
        "    foreach (int v in a) s = s + v;\n"
        "    return s;\n"
        "}\n");
    CHECK(fe.ok, "foreach over ND must emit");

    //init-list target with ND type: parses, but ND init-list resolution
    const BuildOutcome il = buildSource("nd_gate_initlist",
        "int main() {\n"
        "    int[,] a = [[1,2],[3,4]];\n"
        "    return 0;\n"
        "}\n");
    CHECK(!DiagHas(il, "syntax error"),
        "nested bracket init list must parse");
    CHECK(il.ok, "ND init-list emits (task 5 lowering)");
    PASS();
}

//Rank cap: the grammar and the creation twin reject ranks above the
//language maximum with a named diagnostic (rank-9 tokens previously
//compiled and died at runtime in an uncatchable VM range throw).
void test_nd_rank_cap_diagnostics()
{
    TEST(nd_rank_cap_diagnostics);
    const BuildOutcome decl = buildSource("nd_rank9_decl",
        "int main() {\n"
        "    int[,,,,,,,,] a;\n"
        "    return 0;\n"
        "}\n");
    CHECK(!decl.ok && DiagHas(decl, "exceeds the maximum")
        && !DiagHas(decl, "syntax error"),
        "rank-9 declaration must name the rank cap");
    const BuildOutcome make = buildSource("nd_rank9_create",
        "int main() {\n"
        "    int[,] a = int[1,2,3,4,5,6,7,8,9];\n"
        "    return 0;\n"
        "}\n");
    CHECK(!make.ok && DiagHas(make, "exceeds the maximum"),
        "rank-9 creation must name the rank cap");
    const BuildOutcome edge = buildSource("nd_rank8_ok",
        "int main() {\n"
        "    int[,,,,,,,] a;\n"
        "    return 0;\n"
        "}\n");
    CHECK(edge.ok, "rank-8 declaration stays legal");
    PASS();
}

//Brace rows above the leaf level are depth mismatches, not rows: the
//shape gate and the emitter share one row predicate (bracket form).
void test_brace_row_is_depth_mismatch()
{
    TEST(brace_row_is_depth_mismatch);
    const BuildOutcome o = buildSource("nd_brace_row",
        "int main() {\n"
        "    int[,] m = [{{1, 2}, {3, 4}}];\n"
        "    return 0;\n"
        "}\n");
    CHECK(!o.ok && DiagHas(o, "depth does not match")
        && !DiagHas(o, "syntax error"),
        "brace rows must name the depth mismatch");
    PASS();
}
//Comma sizes in declaration position parse so the parser can name
//the C-style form and point at the creation twin (0.10.7 D3:
//parse, then reject with guidance).
void test_decl_comma_size_named_reject()
{
    TEST(decl_comma_size_named_reject);
    const char *msg = "array declarations take one size per bracket";
    const BuildOutcome bare = buildSource("nd_declsize_bare",
        "int main() {\n"
        "    int a[2, 3];\n"
        "    return 0;\n"
        "}\n");
    CHECK(!bare.ok && DiagHas(bare, msg)
        && !DiagHas(bare, "syntax error"),
        "bare comma-size declaration must name the creation twin");
    const BuildOutcome init = buildSource("nd_declsize_init",
        "int main() {\n"
        "    int a[2, 3] = 1;\n"
        "    return 0;\n"
        "}\n");
    CHECK(!init.ok && DiagHas(init, msg),
        "comma-size with initializer must name the creation twin");
    const BuildOutcome cont = buildSource("nd_declsize_cont",
        "int main() {\n"
        "    int x = 1, a[2, 3];\n"
        "    return 0;\n"
        "}\n");
    CHECK(!cont.ok && DiagHas(cont, msg),
        "continuation declarator must name the creation twin");
    PASS();
}

int main()
{
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();

    std::cerr << "=== ND resolve tests ===\n";
    try {
        test_nd_creation_resolves_to_gate();
        test_subscript_arity_diagnostics();
        test_jagged_still_rejected();
        test_emit_gates_cover_nd_paths();
        test_nd_rank_cap_diagnostics();
        test_brace_row_is_depth_mismatch();
        test_decl_comma_size_named_reject();
    } catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << "\n";
        g_fail++;
    }

    std::error_code ec;
    std::filesystem::remove_all(scratchDir(), ec);

    std::cerr << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
