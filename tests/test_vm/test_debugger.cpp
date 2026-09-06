// --- ndb debugger unit tests ---
// In-process compile+run via the public ModuleBuilder API (see
// test_stdlib.cpp for the harness rationale). Covers the .nmod v1.9
// sourceFile field, the B.1 imported-locals fix, debug hooks, the
// read-only view, and DebugSession stepping semantics (Task 4).
// MUST call Runtime::StaticInit() before any Build() (IdString tables).

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
#include <iterator>
#include <string>
#include <vector>

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

//Scratch directory for temp sources and modules.
static std::filesystem::path scratchDir()
{
    static const auto dir = std::filesystem::temp_directory_path()
        / "nlang_test_debugger";
    std::filesystem::create_directories(dir);
    return dir;
}

struct BuildOutcome
{
    bool ok = false;
    std::string diagnostics;
};

//Write <tag>.n with the raw source (no stdlib imports prepended —
//debugger tests target plain programs), build <tag>.nmod.
static BuildOutcome buildSource(const std::string& tag,
    const std::string& source)
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

    BuildOutcome outcome;
    try {
        outcome.ok = builder.Build()
            && std::filesystem::exists(modPath);
    } catch (const std::exception& e) {
        outcome.diagnostics += std::string("internal error: ") + e.what();
        return outcome;
    }
    for (auto it = logger.cbegin(); it != logger.cend(); ++it)
        outcome.diagnostics += (*it)->Message() + "\n";
    return outcome;
}

//加载刚构建的 <tag>.nmod。
static CompiledModule loadBuilt(const std::string& tag)
{
    return ModuleLoader::Load(
        (scratchDir() / (tag + ".nmod")).string());
}

void test_v19_sourcefile_single_tu()
{
    TEST(v19_sourcefile_single_tu);
    BuildOutcome b = buildSource("srcfile_single",
        "int helper(int v) { return v * 2; }\n"
        "int main() { return helper(21); }\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("srcfile_single");
    const std::string* mainSrc = nullptr;
    const std::string* helperSrc = nullptr;
    for (const auto& f : mod.functions) {
        if (f.name == "main") mainSrc = &f.sourceFile;
        if (f.name == "helper") helperSrc = &f.sourceFile;
    }
    CHECK(mainSrc != nullptr, "main should be in the module");
    CHECK(helperSrc != nullptr, "helper should be in the module");
    CHECK(mainSrc->find("srcfile_single.n") != std::string::npos,
        "main.sourceFile should name the source file, got: " + *mainSrc);
    CHECK(*mainSrc == *helperSrc,
        "same-TU functions should share the source file path");
    PASS();
}

void test_v19_import_roundtrip()
{
    TEST(v19_import_roundtrip);
    const auto dir = scratchDir();
    //The import target must exist as a compiled .nmod — ModuleBuilder's
    //FindModuleFile searches m_ImportDirs for <name>.nmod, never a .n
    //source, so build the lib module first, then the consumer.
    BuildOutcome lib = buildSource("dbgutil_lib",
        "int triple(int v) { int t = v * 3; return t; }\n");
    CHECK(lib.ok, "lib build should succeed: " + lib.diagnostics);
    {
        std::ofstream out(dir / "dbgutil_main.n", std::ios::binary);
        out << "import dbgutil_lib;\n"
               "int main() { return dbgutil_lib.triple(14); }\n";
    }
    BuildParams params;
    params.m_SourceFiles.push_back((dir / "dbgutil_main.n").string());
    params.m_sOutputModule = "dbgutil_main";
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_ImportDirs.push_back(dir.string());
    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    CHECK(builder.Build(), "import build should succeed");

    CompiledModule mod = loadBuilt("dbgutil_main");
    int tripleIdx = mod.FindFunction("triple");
    CHECK(tripleIdx >= 0, "triple should be merged into the consumer");
    const auto& triple = mod.functions[static_cast<size_t>(tripleIdx)];
    CHECK(triple.sourceFile.find("dbgutil_lib.n") != std::string::npos,
        "imported function keeps producer source file, got: "
        + triple.sourceFile);
    CHECK(!triple.locals.empty(),
        "B.1 merge must copy locals (GC roots + debugger visibility)");
    PASS();
}

void test_v19_loader_rejects_v1_8()
{
    TEST(v19_loader_rejects_v1_8);
    BuildOutcome b = buildSource("oldver",
        "int main() { return 0; }\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    const auto modPath = scratchDir() / "oldver.nmod";

    //Patch the minorVer field (header offset 10, little-endian u16:
    //magic[8] + major(u16) + minor(u16)) down to 8 — the loader must
    //refuse v1.8 modules outright (floor bump discipline).
    std::vector<char> bytes;
    {
        std::ifstream in(modPath, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    CHECK(bytes.size() >= 12, "module file should have a full header");
    bytes[10] = 0x08;
    bytes[11] = 0x00;
    const auto oldPath = scratchDir() / "oldver_v18.nmod";
    {
        std::ofstream out(oldPath, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    bool threw = false;
    try {
        ModuleLoader::Load(oldPath.string());
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw, "loader must reject a v1.8 module (floor is 9)");
    PASS();
}

void test_v19_import_gc_roots()
{
    TEST(v19_import_gc_roots);
    //Pins the B.1 locals fix: before it, imported frames had no local
    //descriptors, so MarkPhase marked nothing and every imported-frame
    //heap value was swept. The array must be referenced ONLY from the
    //imported frame (a caller-frame local would root it through the
    //caller's own locals table).
    //
    //The scenario works around the sweeper's reuse dynamics: a swept
    //slot re-enters the free list only at the GC that sweeps it (the
    //rebuild skips kind==0 slots), and vector::clear() keeps the old
    //buffer, so a swept-but-never-reallocated slot still reads its old
    //value. To make the bug observable, the program drives the heap
    //past the GC threshold (1024 slots), allocates `keep`, then runs
    //exactly one more loop iteration — its back-edge GC sweeps keep
    //(no roots under the bug) together with that iteration's single
    //scratch, leaving exactly two slots in the free list. Two
    //straight-line allocations then drain the free list: the second
    //pop recycles keep's slot and assign() zeroes its data, so the
    //final read observes 0 instead of 7. With the fix, keep is marked
    //at that GC and the drain cannot touch its slot.
    const auto dir = scratchDir();
    //Lib must be a compiled .nmod before the consumer can import it
    //(same FindModuleFile contract as the roundtrip test above).
    BuildOutcome lib = buildSource("gcutil_lib",
        "int churn() {\n"
        "    int i = 0;\n"
        "    while (i < 1100) {\n"
        "        int[] warm = new int[8];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    int[] keep = new int[1];\n"
        "    keep[0] = 7;\n"
        "    i = 0;\n"
        "    while (i < 1) {\n"
        "        int[] scratch = new int[64];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    int[] drainA = new int[8];\n"
        "    int[] drainB = new int[8];\n"
        "    return keep[0] + drainA[0] + drainB[0];\n"
        "}\n");
    CHECK(lib.ok, "lib build should succeed: " + lib.diagnostics);
    {
        std::ofstream out(dir / "gcutil_main.n", std::ios::binary);
        out << "import gcutil_lib;\n"
               "int main() { return gcutil_lib.churn(); }\n";
    }
    BuildParams params;
    params.m_SourceFiles.push_back((dir / "gcutil_main.n").string());
    params.m_sOutputModule = "gcutil_main";
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_ImportDirs.push_back(dir.string());
    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    CHECK(builder.Build(), "build should succeed");
    CompiledModule mod = loadBuilt("gcutil_main");
    VmExecutor exec;
    CHECK(exec.Execute(mod) == 7,
        "imported-frame local 'keep' must survive GC (B.1 locals fix)");
    PASS();
}

int main()
{
    //In-process host init: ModuleBuilder's Build() dereferences the
    //IdString static tables — StaticInit must run first or Build()
    //segfaults (a crash try/catch cannot intercept).
    Runtime::StaticInit();

    test_v19_sourcefile_single_tu();
    test_v19_import_roundtrip();
    test_v19_loader_rejects_v1_8();
    test_v19_import_gc_roots();

    std::cerr << "\ndebugger_tests: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail > 0 ? 1 : 0;
}
