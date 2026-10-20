// Script-mode wrap unit tests (v0.9.1): the entry synthesis in
// ModuleBuilder::SynthesizeScriptEntries, driven in process through the
// real build -> load -> link pipeline (no mocks). The synthesized main()
// is asserted at the artifact level (the linked function table): entry
// selection, signature shape, and the two rejection paths (duplicate
// main, statements in an imported library unit). The AST-level
// indistinguishability (FA_Default|NF_NONE) has no artifact surface —
// flags are not serialized and the AST stays inside the builder — its
// observable behavior is pinned by the e2e fixtures (script_pub_fn:
// a handwritten main is the entry; err_script_dup_main: synthesized and
// handwritten mains collide with the same diagnostic).

#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/CastInfo.h"
#include "nlang/compiler/Logger.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/runtime/PrimitiveTypes.h"
#include "nlang/vm/CompiledModule.h"
#include "NcuLoader.h"
#include "NcuLinker.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef STDLIB_DIR
#define STDLIB_DIR ""
#endif

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) \
    do { if (cond) ++g_pass; else { ++g_fail; std::fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

namespace {

namespace fs = std::filesystem;

//One directory per scenario (module name = directory name): ModuleManager
//registers modules process-wide with no unload API, so names must stay
//unique within one test process. A stale .ncu from an earlier run would
//collide with the module name and fake a build result — start clean.
fs::path scenarioDir(const char* name) {
    const fs::path dir =
        fs::temp_directory_path() / "nlang_test_script_wrap" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void writeFile(const fs::path& path, const char* content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

//One compile per scenario, returning the outcome AND the log (negative
//cases assert on the diagnostic text). Mirrors the thirdparty driver.
struct ScenarioBuild {
    bool ok = false;
    std::string log;   //every CLL_* message, concatenated
    size_t errors = 0; //CLL_Error count (D8: pin "exactly one error")
};

ScenarioBuild compileScenario(const fs::path& dir,
                              const std::vector<std::string>& importDirs) {
    const std::string moduleName = dir.filename().string();
    BuildParams params;
    params.m_SourceFiles.push_back((dir / "main.n").string());
    params.m_sProjectDir = dir.string();
    params.m_sOutputModule = moduleName;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    for (const auto& importDir : importDirs)
        params.m_ImportDirs.push_back(importDir);

    ScenarioBuild out;
    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    try { out.ok = builder.BuildArtifacts(); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "internal error: %s\n", e.what());
        out.ok = false;
    }
    for (auto it = logger.cbegin(); it != logger.cend(); ++it)
        out.log += (*it)->Message() + "\n";
    out.errors = logger.Errors();
    return out;
}

//Load and link a successfully built scenario's entry unit — the same
//seam every execution surface uses. Throws on a broken closure; callers
//only link scenarios they expect to build.
CompiledModule linkScenario(const fs::path& dir) {
    const std::string moduleName = dir.filename().string();
    NcuLoader::Options loaderOpts;
    loaderOpts.searchDirs = {dir.string(), STDLIB_DIR};
    NcuLoader::Result loaded = NcuLoader::LoadClosure(
        (dir / (moduleName + ".ncu")).string(), loaderOpts);
    return NcuLinker::Link(std::move(loaded.units), loaded.entryKey);
}

bool hasFunctionLeaf(const CompiledModule& mod, const char* leaf) {
    for (const auto& fn : mod.functions)
        if (LeafNameOfKey(fn.name) == leaf)
            return true;
    return false;
}

//Top-level statements wrap into a synthesized int main(): the linked
//module selects it as the entry point, and the function-table record has
//the bare handwritten signature (zero params, int32 return, real
//bytecode, not a native stub).
void TestEntrySynthesizedFromStatements() {
    const fs::path dir = scenarioDir("entry_stmts");
    writeFile(dir / "main.n", "int x = 6;\nreturn x * 7;\n");
    ScenarioBuild build = compileScenario(dir, {dir.string()});
    CHECK(build.ok, "statements-only scenario builds");
    if (!build.ok) return;
    try {
        CompiledModule mod = linkScenario(dir);
        CHECK(mod.entryPoint >= 0, "synthesized main is the entry point");
        if (mod.entryPoint >= 0) {
            const CompiledFunction& entry = mod.functions[mod.entryPoint];
            CHECK(LeafNameOfKey(entry.name) == "main",
                "entry function is named main");
            CHECK(entry.paramCount == 0, "synthesized main takes no params");
            CHECK(entry.returnTypeKind == RTK_Int32,
                "synthesized main returns int32");
            CHECK(!entry.isNative, "synthesized main is not a native stub");
            CHECK(!entry.bytecode.empty(), "synthesized main has a body");
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "link failed: %s\n", e.what());
        ++g_fail;
    }
}

//A declarations-only file synthesizes nothing: the unit links with no
//entry point and no main in the table at all (the second check keeps a
//synthesized-but-unselected main from passing silently).
void TestDeclarationsOnlyNoEntry() {
    const fs::path dir = scenarioDir("decls_only");
    writeFile(dir / "main.n", "int helper(int v) { return v * 2; }\n");
    ScenarioBuild build = compileScenario(dir, {dir.string()});
    CHECK(build.ok, "declarations-only scenario builds");
    if (!build.ok) return;
    try {
        CompiledModule mod = linkScenario(dir);
        CHECK(mod.entryPoint < 0, "no entry point is synthesized");
        CHECK(!hasFunctionLeaf(mod, "main"), "no main in the function table");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "link failed: %s\n", e.what());
        ++g_fail;
    }
}

//D8: top-level statements plus an explicit main() is a hard error.
//Synthesis is unconditional by design (silently skipping it would drop
//every statement), so the synthesized script entry and the user main land
//in the same bare pool. The duplicate-field checker gives them a dedicated
//diagnostic (not the generic redefinition text), reports it at the explicit
//main's location, and emits exactly one error: the builder aborts before
//statement resolution, so no further errors cascade.
void TestDuplicateMainRejected() {
    const fs::path dir = scenarioDir("dup_main");
    writeFile(dir / "main.n",
        "int x = 1;\nint main() { return 5; }\n");
    ScenarioBuild build = compileScenario(dir, {dir.string()});
    CHECK(!build.ok, "statements + explicit main fails to build");
    CHECK(build.errors == 1, "exactly one error is reported");
    CHECK(build.log.find(
        "top-level statements cannot be combined with an explicit "
        "main() function") != std::string::npos,
        "D8 dedicated diagnostic");
    CHECK(build.log.find("is conflicted") == std::string::npos,
        "no generic redefinition diagnostic");
}

//D8 scope: a main nested inside a class is a method of that class, not a
//top-level function. It lives in the class's member table, so it does not
//share a bare pool with the synthesized top-level main and must not trigger
//the duplicate-main diagnostic. The file still synthesizes its entry from
//the top-level statements and builds cleanly.
void TestNestedMainNotRejected() {
    const fs::path dir = scenarioDir("nested_main");
    writeFile(dir / "main.n",
        "class A {\n    int main() { return 1; }\n}\n"
        "int x = 2;\nreturn x;\n");
    ScenarioBuild build = compileScenario(dir, {dir.string()});
    CHECK(build.ok, "nested main does not conflict with the entry");
    CHECK(build.log.find("top-level statements cannot be combined")
            == std::string::npos,
        "no D8 diagnostic for a nested main");
}

//D8 regression guard: the duplicate-field checker's generic redefinition
//diagnostic must survive for ordinary same-name collisions that are NOT
//the script-entry case (here two top-level functions named "foo"; neither
//is the synthesized entry, so the legacy "is conflicted" text applies).
void TestGenericDuplicateStillRejected() {
    const fs::path dir = scenarioDir("dup_generic");
    writeFile(dir / "main.n",
        "int foo() { return 1; }\nint foo() { return 2; }\n");
    ScenarioBuild build = compileScenario(dir, {dir.string()});
    CHECK(!build.ok, "duplicate top-level function fails to build");
    CHECK(build.log.find("is conflicted") != std::string::npos,
        "generic redefinition diagnostic preserved");
}

//D14: an imported unit's declarations load, its statements may not
//execute on import — the compile rejects the importing file instead.
void TestLibraryStatementsRejected() {
    const fs::path dir = scenarioDir("lib_stmts");
    writeFile(dir / "lib.n", "int leak = 1;\nreturn leak;\n");
    writeFile(dir / "main.n", "import lib;\nreturn 0;\n");
    ScenarioBuild build = compileScenario(dir, {dir.string()});
    CHECK(!build.ok, "importing a statement-carrying library fails");
    CHECK(build.log.find("contains top-level statements") != std::string::npos,
        "D14 diagnostic");
    //The diagnostic must name the IMPORTED unit (lib.n), not the consumer:
    //a regression rejecting any statement-carrying file would otherwise
    //pass this scenario in isolation.
    CHECK(build.log.find("lib.n") != std::string::npos,
        "D14 diagnostic names the imported unit");
}

} //namespace

int main() {
    //Static tables before any compiler/VM work: Runtime::StaticInit owns
    //the IdString intern tables (building any module dereferences them),
    //then the compiler-side cast table.
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();

    TestEntrySynthesizedFromStatements();
    TestDeclarationsOnlyNoEntry();
    TestDuplicateMainRejected();
    TestNestedMainNotRejected();
    TestGenericDuplicateStillRejected();
    TestLibraryStatementsRejected();

    std::printf("script_wrap_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
