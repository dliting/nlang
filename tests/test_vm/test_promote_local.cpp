//v0.13.9 phase 3 Task 4: scalar/string local promotion. A local (or
//value formal) that is &-bound anywhere in its function gets its
//storage compiled into one stable boxed record — the slot holds the
//record handle, every access is indirected through the payload cells,
//and the box is a GC root for the whole activation (the Python-closure
//cell / Lua-upvalue family). Un-bound locals keep their bare slots.
//The modules are REAL-compiled (ModuleBuilder → NcuLoader closure →
//NcuLinker → VmExecutor) with no bytecode surgery: these pins are
//source-level behavior and heap-allocation counts, exactly what an
//ordinary program observes.
#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/core/CompiledModule.h"
#include "VmExecutor.h"
#include "NcuLoader.h"
#include "NcuLinker.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#ifndef STDLIB_DIR
#define STDLIB_DIR ""
#endif

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define PASS() \
    do { ++g_pass; std::cerr << "OK\n"; } while(0)
#define FAIL(msg) \
    do { ++g_fail; std::cerr << "FAIL: " << msg << "\n"; } while(0)
#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)
#define REQUIRE(cond) \
    do { if (!(cond)) { FAIL("precondition: " #cond); return; } } while(0)

static std::filesystem::path scratchDir()
{
    static const auto dir = std::filesystem::temp_directory_path()
        / "nlang_test_promote_local";
    std::filesystem::create_directories(dir);
    return dir;
}

struct ExecResult {
    std::unique_ptr<VmExecutor> exec;
    std::unique_ptr<CompiledModule> mod;
    int exitCode = -1;
    bool built = false;
    bool ran = false;
    std::string error;
};

//Compile <tag>.n, link and execute; keep the executor alive so the
//heap stays inspectable. An executor throw is captured in error.
static ExecResult runProgram(const std::string& tag,
                             const std::string& source)
{
    ExecResult out;
    const auto dir = scratchDir();
    const auto nPath = dir / (tag + ".n");
    const auto modPath = dir / (tag + ".ncu");
    std::filesystem::remove(modPath);
    {
        std::ofstream file(nPath, std::ios::binary);
        file << source;
    }
    BuildParams params;
    params.m_SourceFiles.push_back(nPath.string());
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    if (!builder.BuildArtifacts()) {
        for (auto it = logger.cbegin(); it != logger.cend(); ++it)
            std::cerr << "    " << (*it)->Message() << "\n";
        return out;
    }
    out.built = true;
    NcuLoader::Options loaderOpts;
    loaderOpts.searchDirs = {dir.string(), STDLIB_DIR};
    NcuLoader::Result loaded =
        NcuLoader::LoadClosure(modPath.string(), loaderOpts);
    out.mod = std::make_unique<CompiledModule>(
        NcuLinker::Link(std::move(loaded.units), loaded.entryKey));
    out.exec = std::make_unique<VmExecutor>();
    try {
        out.exitCode = out.exec->Execute(*out.mod);
        out.ran = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

static size_t allocCount(const ExecResult& r)
{
    return r.exec ? r.exec->HeapAllocCountForTests() : 0;
}

//The binding write-through: r = 7 lands in x's box, x reads back 7.
//The alloc pin proves the promotion actually happened (a box exists).
static void promoteLocalWriteThrough()
{
    std::cerr << "  promoteLocalWriteThrough ... ";
    auto r = runProgram("promote_local",
        "int main() {\n"
        "    int x = 1;\n"
        "    int& r = x;\n"
        "    r = 7;\n"
        "    return x;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 7, "write-through (exit=" +
        std::to_string(r.exitCode) + ")");
    CHECK(allocCount(r) >= 1, "promoted box allocated (alloc=" +
        std::to_string(allocCount(r)) + ")");
    PASS();
}

//A value formal &-bound inside the body promotes the same way: the
//callee's copy lives in a box and the binding writes into it.
static void paramPromotionWriteThrough()
{
    std::cerr << "  paramPromotionWriteThrough ... ";
    auto r = runProgram("promote_param",
        "int bump(int v) {\n"
        "    int& r = v;\n"
        "    r = 7;\n"
        "    return v;\n"
        "}\n"
        "int main() {\n"
        "    return bump(1);\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 7, "param write-through (exit=" +
        std::to_string(r.exitCode) + ")");
    CHECK(allocCount(r) >= 1, "promoted param box allocated (alloc=" +
        std::to_string(allocCount(r)) + ")");
    PASS();
}

//A target declared in a loop body re-runs its box allocation per
//iteration; each iteration's binding tracks the fresh storage.
static void loopBodyRedeclWriteThrough()
{
    std::cerr << "  loopBodyRedeclWriteThrough ... ";
    auto r = runProgram("promote_loop",
        "int main() {\n"
        "    int acc = 0;\n"
        "    int i = 0;\n"
        "    while (i < 3) {\n"
        "        int t = i;\n"
        "        int& r = t;\n"
        "        r = r + 10;\n"
        "        acc = acc + t;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return acc;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 33, "per-iteration write-through (exit=" +
        std::to_string(r.exitCode) + ")");
    PASS();
}

//String promotion re-binds WHICH string the target names: the box
//payload carries the string-store handle, and a store through the
//binding replaces that handle.
static void stringBindReassign()
{
    std::cerr << "  stringBindReassign ... ";
    auto r = runProgram("promote_string",
        "int main() {\n"
        "    string name = \"a\";\n"
        "    string& s = name;\n"
        "    s = \"other\";\n"
        "    if (name == \"other\") {\n"
        "        return 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 1, "string re-bind (exit=" +
        std::to_string(r.exitCode) + ")");
    CHECK(allocCount(r) >= 1, "promoted string box allocated (alloc=" +
        std::to_string(allocCount(r)) + ")");
    PASS();
}

//Wide payloads occupy both payload cells; the store/load round-trip
//must preserve all 8 bytes (the box payload pair, not just the low
//half).
static void wideScalarPromotion()
{
    std::cerr << "  wideScalarPromotion ... ";
    auto r = runProgram("promote_wide",
        "int main() {\n"
        "    long v = 5000000000;\n"
        "    long& r = v;\n"
        "    r = 6000000000;\n"
        "    if (v == 6000000000) {\n"
        "        return 4;\n"
        "    }\n"
        "    return 0;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 4, "wide payload round-trip (exit=" +
        std::to_string(r.exitCode) + ")");
    PASS();
}

//A promoted local without an initializer reads the type default from
//the zeroed box payload.
static void zeroInitPromoted()
{
    std::cerr << "  zeroInitPromoted ... ";
    auto r = runProgram("promote_zero",
        "int main() {\n"
        "    int x;\n"
        "    int& r = x;\n"
        "    return x;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 0, "zero payload default (exit=" +
        std::to_string(r.exitCode) + ")");
    CHECK(allocCount(r) >= 1, "zero-init box allocated (alloc=" +
        std::to_string(allocCount(r)) + ")");
    PASS();
}

//The promoted local's box is a GC root for the whole activation:
//3000 discarded boxes cross the struct-heap collection thresholds
//mid-loop and the promoted box must survive every collection. The
//churn allocates on the COUNTED heap (Object boxing — strings live in
//the string store and would not register), allocation-counted, not
//time-counted (deterministic).
static void gcSurvivalUnderChurn()
{
    std::cerr << "  gcSurvivalUnderChurn ... ";
    auto r = runProgram("promote_gc",
        "int main() {\n"
        "    int x = 7;\n"
        "    int& r = x;\n"
        "    Object o = null;\n"
        "    int i = 0;\n"
        "    while (i < 3000) {\n"
        "        o = i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    r = 9;\n"
        "    if (x != 9) {\n"
        "        return 0;\n"
        "    }\n"
        "    return 9;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 9, "box survived mid-loop collections (exit=" +
        std::to_string(r.exitCode) + ")");
    CHECK(allocCount(r) >= 3000, "churn actually allocated (alloc=" +
        std::to_string(allocCount(r)) + ")");
    PASS();
}

//The no-& floor: a function without any binding keeps bare slots —
//zero heap allocations, byte-identical value semantics.
static void noBindZeroAlloc()
{
    std::cerr << "  noBindZeroAlloc ... ";
    auto r = runProgram("promote_none",
        "int main() {\n"
        "    int x = 1;\n"
        "    int y = x + 2;\n"
        "    return y;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 3, "value semantics unchanged (exit=" +
        std::to_string(r.exitCode) + ")");
    CHECK(allocCount(r) == 0, "no box for un-bound locals (alloc=" +
        std::to_string(allocCount(r)) + ")");
    PASS();
}

//Compound assignment through the binding: read-modify-write round-trips
//the payload cells.
static void compoundThroughRef()
{
    std::cerr << "  compoundThroughRef ... ";
    auto r = runProgram("promote_compound",
        "int main() {\n"
        "    int x = 1;\n"
        "    int& r = x;\n"
        "    r += 2;\n"
        "    return x;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 3, "compound through binding (exit=" +
        std::to_string(r.exitCode) + ")");
    PASS();
}

//Guard: direct reads and writes of a promoted local keep working
//through the box — promotion is storage-only, semantics unchanged.
static void directAccessGuard()
{
    std::cerr << "  directAccessGuard ... ";
    auto r = runProgram("promote_direct",
        "int main() {\n"
        "    int x = 1;\n"
        "    int& r = x;\n"
        "    x = 7;\n"
        "    x = x + 1;\n"
        "    return x;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 8, "direct access through box (exit=" +
        std::to_string(r.exitCode) + ")");
    PASS();
}

int main()
{
    //In-process host: the id-string table and cast matrix must exist
    //before any ModuleBuilder touches them.
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();
    promoteLocalWriteThrough();
    paramPromotionWriteThrough();
    loopBodyRedeclWriteThrough();
    stringBindReassign();
    wideScalarPromotion();
    zeroInitPromoted();
    gcSurvivalUnderChurn();
    noBindZeroAlloc();
    compoundThroughRef();
    directAccessGuard();
    std::cerr << "promote_local: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
