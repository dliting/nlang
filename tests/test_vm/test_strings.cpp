// --- String object store + GC unit tests ---
// In-process compile+run (same harness shape as test_stdlib.cpp), plus
// white-box GC observers: SetGcStressThresholds before Execute and
// LiveStringObjectCount after it. The --gc-stress e2e shapes pin trace
// correctness end to end; these pin store-level behavior (bounded live
// population, immortal constants, handle-0 semantics).

#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "VmExecutor.h"
#include "ModuleLoader.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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
        / "nlang_test_strings";
    std::filesystem::create_directories(dir);
    return dir;
}

//Compile one source into <tag>.nmod; false + diagnostics on failure.
static bool loadSource(const std::string& tag, const std::string& source,
    CompiledModule& mod)
{
    const auto dir = scratchDir();
    const auto nPath = dir / (tag + ".n");
    const auto modPath = dir / (tag + ".nmod");
    std::filesystem::remove(modPath);
    {
        std::ofstream out(nPath, std::ios::binary);
        out << source;
    }
    BuildParams params;
    params.m_SourceFiles.push_back(nPath.string());
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    try {
        if (!builder.Build() || !std::filesystem::exists(modPath)) {
            std::cerr << "build failed:\n";
            for (auto it = logger.cbegin(); it != logger.cend(); ++it)
                std::cerr << "  " << (*it)->Message() << "\n";
            return false;
        }
    } catch (const std::exception& e) {
        std::cerr << "internal error: " << e.what() << "\n";
        return false;
    }
    mod = ModuleLoader::Load(modPath.string());
    return true;
}

//The dropped accumulator must not survive: with the trace faces complete,
//the 10k-iteration history reclaims down to a small residue.
void test_bounded_concat_live_count()
{
    TEST(bounded_concat_live_count);
    CompiledModule mod;
    CHECK(loadSource("bounded_concat",
        "int main() {\n"
        "    string s = \"\";\n"
        "    int i = 0;\n"
        "    while (i < 10000) {\n"
        "        s = s + \"ab\";\n"
        "        i = i + 1;\n"
        "    }\n"
        "    if (s.length() != 20000) return 1;\n"
        "    s = \"\";\n"
        "    i = 0;\n"
        "    while (i < 100) {\n"
        "        string t = \"x\" + i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n", mod), "build failed");
    VmExecutor exec;
    exec.SetGcStressThresholds(8);
    CHECK(exec.Execute(mod) == 0, "program self-checks must pass");
    CHECK(exec.LiveStringObjectCount() < 1000,
        "live string objects should be far below the iteration count");
    PASS();
}

//Every compile-time constant stays materialized and live through stress.
void test_constants_immortal_under_stress()
{
    TEST(constants_immortal_under_stress);
    CompiledModule mod;
    CHECK(loadSource("const_immortal",
        "int main() {\n"
        "    int i = 0;\n"
        "    while (i < 300) {\n"
        "        string t = \"garbage-\" + i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    string p = \"anchor-\";\n"
        "    if (\"anchor-a\" != p + \"a\") return 1;\n"
        "    return 0;\n"
        "}\n", mod), "build failed");
    VmExecutor exec;
    exec.SetGcStressThresholds(8);
    CHECK(exec.Execute(mod) == 0, "constant must read back correct");
    CHECK(exec.LiveStringObjectCount() >= mod.stringConstants.size(),
        "all constants must remain live (immortal)");
    PASS();
}

//Cons chain: appends build a left-leaning chain that flattens iteratively
//(a deep chain must not overflow the stack), stays consistent on re-read
//(in-place flatten), and is reclaimed whole after the root drops (the
//mark phase traces cons children — untraced, the stress sweeps would
//gut the chain mid-build and the first read would come back wrong).
void test_cons_chain_flatten_and_reclaim()
{
    TEST(cons_chain_flatten_and_reclaim);
    CompiledModule mod;
    CHECK(loadSource("cons_chain",
        "int main() {\n"
        "    string s = \"\";\n"
        "    int i = 0;\n"
        "    while (i < 4000) {\n"
        "        s = s + \"ab\";\n"
        "        i = i + 1;\n"
        "    }\n"
        "    if (s.length() != 8000) return 1;\n"
        "    if (s.length() != 8000) return 2;\n"
        "    if (s.substring(7998, 8000) != \"ab\") return 3;\n"
        "    s = \"\";\n"
        "    i = 0;\n"
        "    while (i < 100) {\n"
        "        string t = \"x\" + i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n", mod), "build failed");
    VmExecutor exec;
    exec.SetGcStressThresholds(8);
    CHECK(exec.Execute(mod) == 0, "chain must flatten and read correctly");
    CHECK(exec.LiveStringObjectCount() < 100,
        "dropped chain must be reclaimed whole (cons children traced)");
    PASS();
}

//Handle 0 (uninitialized) reads as "" — the old pool[0] fallback shape.
void test_uninitialized_string_reads_empty()
{
    TEST(uninitialized_string_reads_empty);
    CompiledModule mod;
    CHECK(loadSource("uninit_str",
        "int main() {\n"
        "    string s;\n"
        "    if (s != \"\") return 1;\n"
        "    if (s.length() != 0) return 2;\n"
        "    return 0;\n"
        "}\n", mod), "build failed");
    VmExecutor exec;
    CHECK(exec.Execute(mod) == 0, "null string must read as empty");
    PASS();
}

int main()
{
    Runtime::StaticInit();   //in-process host requirement (IdString tables)
    test_bounded_concat_live_count();
    test_constants_immortal_under_stress();
    test_cons_chain_flatten_and_reclaim();
    test_uninitialized_string_reads_empty();
    std::cerr << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
