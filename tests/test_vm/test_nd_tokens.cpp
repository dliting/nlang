// Rectangular array type tokens, stage 2 task 1: rank rides the interned
// token (key = element type + rank), comma-suffix grammar parses, the
// jagged gate still rejects nested chains, and generic arguments accept a
// single ND token. Mirrors test_native_array_abi.cpp's in-process pipeline
// (Runtime::StaticInit + TypeCastInfo::StaticInit before any build).

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
        / "nlang_test_nd_tokens";
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

//--- source-level tests ---------------------------------------------------------

void test_cross_rank_assignment_rejected()
{
    TEST(cross_rank_assignment_rejected);
    //Distinct ranks are distinct types: double[,] and double[] must not
    //assign to each other (pointer-identity through the interned tokens).
    const BuildOutcome a2b1 = buildSource("nd_xrank_a",
        "int main() {\n"
        "    double[] b = null;\n"
        "    double[,] a = b;\n"
        "    return 0;\n"
        "}\n");
    CHECK(!a2b1.ok, "double[] must not assign to double[,] (rank identity)");
    const BuildOutcome b1a2 = buildSource("nd_xrank_b",
        "int main() {\n"
        "    double[,] a = null;\n"
        "    double[] b = a;\n"
        "    return 0;\n"
        "}\n");
    CHECK(!b1a2.ok, "double[,] must not assign to double[] (rank identity)");
    PASS();
}

//--- source-level tests ---------------------------------------------------------

void test_comma_type_declaration_compiles()
{
    TEST(comma_type_declaration_compiles);
    const BuildOutcome outcome = buildSource("nd_decl",
        "int main() {\n"
        "    double[,] a = null;\n"
        "    double[,,] b = null;\n"
        "    if (a == null && b == null) return 0;\n"
        "    return 1;\n"
        "}\n");
    CHECK(outcome.ok, "T[,] declarations must compile (rank-2/3 tokens)");
    CHECK(outcome.diagnostics.find("not supported") == std::string::npos,
        "declaration itself must not be gated");
    PASS();
}

void test_jagged_still_rejected()
{
    TEST(jagged_still_rejected);
    const BuildOutcome outcome = buildSource("nd_jagged",
        "int main() {\n"
        "    double[][] b = null;\n"
        "    return 0;\n"
        "}\n");
    CHECK(!outcome.ok, "jagged T[][] must stay rejected");
    CHECK(outcome.diagnostics.find("jagged") != std::string::npos,
        "diagnostic should name the jagged rejection");
    PASS();
}

void test_generic_arg_accepts_nd_token()
{
    TEST(generic_arg_accepts_nd_token);
    //The list creation idiom is the collection initializer (the anchored
    //generic literal); `new List<...>()` is not NLang syntax.
    const BuildOutcome outcome = buildSource("nd_generic",
        "int main() {\n"
        "    List<double[,]> grid = {};\n"
        "    if (grid.length() == 0) return 0;\n"
        "    return 1;\n"
        "}\n");
    CHECK(outcome.ok, "List<double[,]> generic argument must compile");
    PASS();
}

int main()
{
    //Static tables before any compiler work (in-process test contract).
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();

    std::cerr << "=== ND token tests ===\n";
    try {
        test_cross_rank_assignment_rejected();
        test_comma_type_declaration_compiles();
        test_jagged_still_rejected();
        test_generic_arg_accepts_nd_token();
    } catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }

    std::error_code ec;
    std::filesystem::remove_all(scratchDir(), ec);

    std::cerr << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
