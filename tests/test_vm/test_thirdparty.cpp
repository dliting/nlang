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
#include <sstream>
#include <string>
#include <string_view>

#ifndef STDLIB_DIR
#define STDLIB_DIR ""
#endif
#ifndef FIXTURE_DIR
#define FIXTURE_DIR ""
#endif
#ifndef SOURCE_FIXTURE_DIR
#define SOURCE_FIXTURE_DIR ""
#endif
#ifndef MIX_SOURCE_FIXTURE_DIR
#define MIX_SOURCE_FIXTURE_DIR ""
#endif

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) \
    do { if (cond) ++g_pass; else { ++g_fail; std::fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

namespace {

namespace fs = std::filesystem;

//Built names of the native fixtures (nlang_mylib.dll / .so / .dylib).
#ifndef _WIN32
#define MYLIB_DLL_NAME "libnlang_mylib.so"
#define MIXLIB_DLL_NAME "libnlang_mixlib.so"
#endif
constexpr const char* kNativeDllName = MYLIB_DLL_NAME;
constexpr const char* kMixDllName = MIXLIB_DLL_NAME;

fs::path packageDir() {
    static const auto dir = fs::temp_directory_path() / "nlang_test_thirdparty";
    //Start from a clean slate: a stale prog.nmod left by an earlier run
    //would collide with the module name and fake a build failure.
    fs::remove_all(dir);
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
// with the package dir as the only extra native search path. The module
// name defaults to the scenario directory name: ModuleManager registers
// modules process-wide (no unload API), so names must be unique per
// scenario within one test process.
int buildAndRun(const fs::path& pkg, CapturingIo& ioCapture) {
    const std::string moduleName = pkg.filename().string();
    BuildParams params;
    params.m_SourceFiles.push_back((pkg / "prog.n").string());
    params.m_sOutputModule = moduleName;
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
    CompiledModule mod = ModuleLoader::Load(
        (pkg / (moduleName + ".nmod")).string());
    VmExecutor exec;
    exec.AddNativeSearchDir(pkg.string());
    exec.SetHostIo(&ioCapture);
    return exec.Execute(mod);
}

void TestThirdPartyNativeLibrary() {
    //One package dir per scenario: the native loader maps
    //nlang_mylib.dll in-process, and Windows refuses writes to a loaded
    //image, so scenarios must not share (or overwrite) a DLL directory.
    const fs::path pkg = packageDir() / "native_only";
    fs::create_directories(pkg);
    {
        std::ofstream out(pkg / "mylib.n", std::ios::binary);
        out << kMylibSource;
    }
    {
        std::ofstream out(pkg / "prog.n", std::ios::binary);
        out << kProgram;
    }
    // Ship the native DLL next to the library source, like a real package.
    const fs::path builtDll = fs::path(FIXTURE_DIR) / kNativeDllName;
    std::error_code copyErr;
    fs::copy_file(builtDll, pkg / kNativeDllName,
                  fs::copy_options::overwrite_existing, copyErr);
    CHECK(!copyErr, "nlang_mylib.dll copied into package dir");

    CapturingIo ioCapture;
    int rc = buildAndRun(pkg, ioCapture);
    CHECK(rc == 0, "third-party native library program self-checks (rc)");
    CHECK(ioCapture.text == "hello, world\n",
          "third-party greet output routed through host IO");
}

std::string ReadFixtureFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// Mixed library (Phase 4b): the fixture mixlib.n declares a native `dbl`
// AND carries an ordinary NLang body (`quad` = dbl(dbl(x))). The body is
// inlined and compiled; only dbl dispatches to the DLL at run time. A
// distinct namespace/DLL base name lets both native packages load in one
// process. This is the in-process counterpart of the use_mylib.n e2e
// script, asserting each layer of the mixed library really ran.
void TestMixedLibraryFromFixture() {
    const fs::path pkg = packageDir() / "mixed";
    fs::create_directories(pkg);
    {
        std::ofstream out(pkg / "mixlib.n", std::ios::binary);
        out << ReadFixtureFile(fs::path(MIX_SOURCE_FIXTURE_DIR) / "mixlib.n");
    }
    {
        std::ofstream out(pkg / "prog.n", std::ios::binary);
        out << "import io;\n"
               "import mixlib;\n"
               "int main() {\n"
               "  if (mixlib.quad(3) != 12) return 1;\n"
               "  if (mixlib.dbl(5) != 10) return 2;\n"
               "  io.print(\"mixed ok\");\n"
               "  return 0;\n"
               "}\n";
    }
    std::error_code copyErr;
    fs::copy_file(fs::path(FIXTURE_DIR) / kMixDllName,
                  pkg / kMixDllName,
                  fs::copy_options::overwrite_existing, copyErr);
    CHECK(!copyErr, "DLL copied for the mixed-library scenario");

    CapturingIo ioCapture;
    int rc = buildAndRun(pkg, ioCapture);
    CHECK(rc == 0, "mixed library program self-checks (rc)");
    CHECK(ioCapture.text == "mixed ok\n",
          "NLang library body compiled from source and run");
}

} // namespace

int main() {
    Runtime::StaticInit();
    std::fprintf(stderr, "=== Third-party Library Integration Tests ===\n");
    TestThirdPartyNativeLibrary();
    TestMixedLibraryFromFixture();
    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
