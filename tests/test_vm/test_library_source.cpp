// Source-library integration tests. A third-party library shipped as a
// single <name>.n file with ORDINARY NLang function bodies is compiled
// inline into the consumer and really run by VmExecutor.
//
// Coverage (Phase 4a):
//   * happy path  - ordinary bodies, same-library bare calls, recursion;
//   * visibility  - an unimported library call is rejected;
//   * no bare names - import never injects the callee name unqualified;
//   * no sibling auto-visibility - a library TU does not see other files
//     in its directory unless it imports them (the D7 rule is project-only);
//   * owner isolation - a project function and a library function sharing
//     a name resolve independently.
//
// The library is really compiled by ModuleBuilder and really executed by
// VmExecutor (no VM internals mocked); it resolves through the same
// library-index + inline path as the standard library.

#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "VmExecutor.h"
#include "IHostIo.h"
#include "ModuleLoader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
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
using Files = std::vector<std::pair<std::string, std::string>>;

// A fresh, isolated scenario directory under the temp tree.
fs::path scenarioDir(const char* name) {
    auto d = fs::temp_directory_path() / "nlang_test_source_lib" / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void writeFiles(const fs::path& dir, const Files& files) {
    for (const auto& entry : files) {
        std::ofstream out(dir / entry.first, std::ios::binary);
        out << entry.second;
    }
}

// Compile main.n in dir with dir + the stdlib dir on the import path.
// Returns the builder result (false = compile errors diagnosed).
bool compileDir(const fs::path& dir) {
    BuildParams params;
    params.m_SourceFiles.push_back((dir / "main.n").string());
    //Unique output module per scenario: several Build() runs share the
    //process-wide module registry, so a repeated name would collide.
    params.m_sOutputModule = dir.filename().string();
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    params.m_ImportDirs = { dir.string(), STDLIB_DIR };
    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    try { return builder.Build(); }
    catch (const std::exception&) { return false; }
}

struct CapturingIo : IHostIo {
    std::string text;
    void OnOutput(std::string_view v) override { text += v; }
};

// Compile then run main.n; returns the program code (-1 on build failure).
int compileRun(const fs::path& dir, CapturingIo& cap) {
    if (!compileDir(dir))
        return -1;
    const std::string modName = dir.filename().string();
    CompiledModule mod = ModuleLoader::Load(
        (dir / (modName + ".nmod")).string());
    VmExecutor exec;
    exec.AddNativeSearchDir(dir.string());
    exec.SetHostIo(&cap);
    return exec.Execute(mod);
}

// Ordinary NLang functions: same-library bare calls (quad -> twice) and
// recursion (fact). No native keyword, no DLL - every body is compiled.
const char* kLibSource =
    "namespace alib {\n"
    "int twice(int x) {\n"
    "  return x * 2;\n"
    "}\n"
    "int quad(int x) {\n"
    "  return twice(twice(x));\n"
    "}\n"
    "int fact(int n) {\n"
    "  if (n <= 1) return 1;\n"
    "  return n * fact(n - 1);\n"
    "}\n"
    "}\n";

const char* kHappyProgram =
    "import io;\n"
    "import alib;\n"
    "int main() {\n"
    "  if (alib.twice(5) != 10) return 1;\n"
    "  if (alib.quad(3) != 12) return 2;\n"
    "  if (alib.fact(5) != 120) return 3;\n"
    "  io.print(\"ok\");\n"
    "  return 0;\n"
    "}\n";

void TestHappyPath() {
    const auto dir = scenarioDir("happy");
    writeFiles(dir, { { "alib.n", kLibSource }, { "main.n", kHappyProgram } });
    CapturingIo cap;
    int rc = compileRun(dir, cap);
    CHECK(rc == 0, "source-library program self-checks (rc)");
    CHECK(cap.text == "ok\n", "source-library program output");
}

// A library the program never imports cannot be reached by qualification.
void TestUnimportedRejected() {
    const auto dir = scenarioDir("unimported");
    const char* lib = "namespace alib {\nint f() { return 1; }\n}\n";
    const char* prog = "int main() {\n  return alib.f();\n}\n";
    writeFiles(dir, { { "alib.n", lib }, { "main.n", prog } });
    CHECK(!compileDir(dir), "unimported library call is rejected");
}

// import exposes only the qualified name (alib.f), never a bare f().
void TestNoBareNames() {
    const auto dir = scenarioDir("bare");
    const char* lib = "namespace alib {\nint f() { return 1; }\n}\n";
    const char* prog = "import alib;\nint main() {\n  return f();\n}\n";
    writeFiles(dir, { { "alib.n", lib }, { "main.n", prog } });
    CHECK(!compileDir(dir), "import does not inject bare names");
}

// A library TU has no same-directory auto-visibility (the project-only D7
// rule): alib.g may not call blib.h unless alib itself imports blib, even
// though the consumer imports both (so both namespaces are inlined).
void TestNoSiblingAutoVisibility() {
    const auto dir = scenarioDir("sibling");
    const char* blib = "namespace blib {\nint h() { return 5; }\n}\n";
    const char* alib =
        "namespace alib {\nint g() { return blib.h(); }\n}\n";
    const char* prog =
        "import alib;\nimport blib;\n"
        "int main() {\n  return alib.g();\n}\n";
    writeFiles(dir, { { "blib.n", blib }, { "alib.n", alib },
                      { "main.n", prog } });
    CHECK(!compileDir(dir),
          "library TU has no same-directory auto-visibility");
}

// A project free function and a library function with the same name bind
// independently (owner tagging keeps the candidates apart).
void TestOwnerIsolation() {
    const auto dir = scenarioDir("owner");
    const char* lib = "namespace alib {\nint val() { return 7; }\n}\n";
    const char* prog =
        "import io;\nimport alib;\n"
        "int val() { return 9; }\n"
        "int main() {\n"
        "  if (val() != 9) return 1;\n"
        "  if (alib.val() != 7) return 2;\n"
        "  io.print(\"ok\");\n"
        "  return 0;\n"
        "}\n";
    writeFiles(dir, { { "alib.n", lib }, { "main.n", prog } });
    CapturingIo cap;
    int rc = compileRun(dir, cap);
    CHECK(rc == 0, "project/library same-named function (rc)");
    CHECK(cap.text == "ok\n", "project/library same-named function output");
}

} // namespace

int main() {
    Runtime::StaticInit();
    std::fprintf(stderr, "=== Source Library Integration Tests ===\n");
    TestHappyPath();
    TestUnimportedRejected();
    TestNoBareNames();
    TestNoSiblingAutoVisibility();
    TestOwnerIsolation();
    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
