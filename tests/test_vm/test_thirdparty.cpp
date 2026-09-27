// Third-party library integration tests. A package directory holds a
// library source (mylib.n) and its native DLL (nlang_mylib.dll); a program
// imports the library and is really compiled by ModuleBuilder and really
// run by VmExecutor. No VM internals are mocked. The third-party namespace
// must resolve through the same library-index + native-loader path as the
// standard library.

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

#ifndef STDLIB_DIR
#define STDLIB_DIR ""
#endif
#ifndef FIXTURE_DIR
#define FIXTURE_DIR ""
#endif

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) \
    do { if (cond) ++g_pass; else { ++g_fail; std::fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

namespace {

namespace fs = std::filesystem;

fs::path packageDir() {
    static const auto dir = fs::temp_directory_path() / "nlang_test_thirdparty";
    fs::create_directories(dir);
    return dir;
}

const char* kMylibSource =
    "namespace mylib {\n"
    "native int add(int a, int b);\n"
    "native int mul(int a, int b);\n"
    "native string greet(string who);\n"
    "}\n";

const char* kProgram =
    "import io;\n"
    "import mylib;\n"
    "int main() {\n"
    "  if (mylib.add(2, 3) != 5) return 1;\n"
    "  if (mylib.mul(4, 5) != 20) return 2;\n"
    "  if (mylib.greet(\"NLang\") != \"hello, NLang\") return 3;\n"
    "  io.print(mylib.greet(\"world\"));\n"
    "  return 0;\n"
    "}\n";

struct CapturingIo : IHostIo {
    std::string text;
    void OnOutput(std::string_view v) override { text += v; }
};

// Compile the program with the package dir on the import path, then run it
// with the package dir as the only extra native search path.
int buildAndRun(const fs::path& pkg, CapturingIo& ioCapture) {
    BuildParams params;
    params.m_SourceFiles.push_back((pkg / "prog.n").string());
    params.m_sOutputModule = "prog";
    params.m_sOutputDir = pkg.string();
    params.m_sTempDir = pkg.string();
    params.m_sStdLibDir = STDLIB_DIR;
    params.m_ImportDirs.clear();
    params.m_ImportDirs.push_back(pkg.string());

    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    bool built = false;
    try { built = builder.Build(); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "internal error: %s\n", e.what());
        return -1;
    }
    if (!built) {
        for (auto it = logger.cbegin(); it != logger.cend(); ++it)
            std::fprintf(stderr, "diag: %s\n", (*it)->Message().c_str());
        return -1;
    }
    CompiledModule mod = ModuleLoader::Load((pkg / "prog.nmod").string());
    VmExecutor exec;
    exec.AddNativeSearchDir(pkg.string());
    exec.SetHostIo(&ioCapture);
    return exec.Execute(mod);
}

void TestThirdPartyNativeLibrary() {
    const fs::path pkg = packageDir();
    {
        std::ofstream out(pkg / "mylib.n", std::ios::binary);
        out << kMylibSource;
    }
    {
        std::ofstream out(pkg / "prog.n", std::ios::binary);
        out << kProgram;
    }
    // Ship the native DLL next to the library source, like a real package.
    const fs::path builtDll = fs::path(FIXTURE_DIR) /
        (std::string("nlang_mylib.dll"));
    std::error_code copyErr;
    fs::copy_file(builtDll, pkg / "nlang_mylib.dll",
                  fs::copy_options::overwrite_existing, copyErr);
    CHECK(!copyErr, "nlang_mylib.dll copied into package dir");

    CapturingIo ioCapture;
    int rc = buildAndRun(pkg, ioCapture);
    CHECK(rc == 0, "third-party native library program self-checks (rc)");
    CHECK(ioCapture.text == "hello, world\n",
          "third-party greet output routed through host IO");
}

} // namespace

int main() {
    Runtime::StaticInit();
    std::fprintf(stderr, "=== Third-party Library Integration Tests ===\n");
    TestThirdPartyNativeLibrary();
    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
