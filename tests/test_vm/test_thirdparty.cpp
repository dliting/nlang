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
#include "nlang/vm/NcuPackage.h"
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
    //Start from a clean slate: a stale prog.ncu left by an earlier run
    //would collide with the module name and fake a build failure.
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

const char* kMylibSource =
    "native int add(int a, int b);\n"
    "native int mul(int a, int b);\n"
    "native string greet(string who);\n";

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

// One compile per scenario dir, returning the outcome AND the log: a
// second Build() on the same dir name would be refused by
// ModuleManager::Create (process-wide, no unload API), and its log would
// be about that collision instead of about the test. Negative cases share
// this entry so they never compile the same scenario twice.
struct ScenarioBuild {
    bool ok = false;
    std::string log;   //every CLL_* message, concatenated
};

ScenarioBuild compileScenario(const fs::path& pkg,
                              const std::vector<std::string>& importDirs) {
    const std::string moduleName = pkg.filename().string();
    BuildParams params;
    params.m_SourceFiles.push_back((pkg / "prog.n").string());
    params.m_sOutputModule = moduleName;
    params.m_sOutputDir = pkg.string();
    params.m_sTempDir = pkg.string();
    params.m_sStdLibDir = STDLIB_DIR;
    params.m_ImportDirs.clear();
    for (const auto& dir : importDirs)
        params.m_ImportDirs.push_back(dir);
    if (params.m_ImportDirs.empty())
        params.m_ImportDirs.push_back(pkg.string());

    ScenarioBuild out;
    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    try { out.ok = builder.Build(); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "internal error: %s\n", e.what());
        out.ok = false;
    }
    for (auto it = logger.cbegin(); it != logger.cend(); ++it)
        out.log += (*it)->Message() + "\n";
    return out;
}

// Compile the program with the package dir on the import path, then run it
// with the package dir as the only extra native search path. The module
// name defaults to the scenario directory name: ModuleManager registers
// modules process-wide (no unload API), so names must be unique per
// scenario within one test process. importDirs empty = the scenario dir
// itself; multi-root scenarios pass their roots explicitly.
int buildAndRun(const fs::path& pkg, CapturingIo& ioCapture,
                const std::vector<std::string>& importDirs = {}) {
    ScenarioBuild build = compileScenario(pkg, importDirs);
    if (!build.ok) {
        std::fprintf(stderr, "%s", build.log.c_str());
        return -1;
    }
    const std::string moduleName = pkg.filename().string();
    CompiledModule mod = ModuleLoader::Load(
        (pkg / (moduleName + ".ncu")).string());
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

//(1) R3 positive: `-I <root>` + `<root>/vendor/graphics.n` makes
//`import vendor.graphics;` and `vendor.graphics.hue()` work. No shell can
//be written (Task 5 removed the syntax), so the package comes from the
//path relative to the matched root.
static void TestDottedLibraryImport() {
    const fs::path pkg = packageDir() / "dotted_import";
    fs::create_directories(pkg / "vendor");
    std::ofstream(pkg / "vendor" / "graphics.n", std::ios::binary)
        << "struct Sprite { int id; }\n"
           "int hue() { return 3; }\n";
    std::ofstream(pkg / "prog.n", std::ios::binary)
        << "import vendor.graphics;\n"
           "int main() {\n"
           "  vendor.graphics.Sprite sp;\n"
           "  sp.id = vendor.graphics.hue();\n"
           "  return sp.id - 3;\n"
           "}\n";
    CapturingIo io;
    CHECK(buildAndRun(pkg, io, { pkg.string() }) == 0,
          "a dotted import resolves through <root>/vendor/graphics.n");
}

//(2) The last segment alone is not a qualified spelling, in either form.
//(2a) `import graphics;` is rejected by that exact name today already —
//a regression guard, expected green now and after Step 5's table swap.
//(2b) the real dotted import succeeds, and the shorthand `graphics.hue()`
//still does not resolve. Both checks eat the shared `Module 'graphics'`
//prefix of ModuleNotFoundText and the not-imported wording, so the pin
//survives either emitter owning the rejection.
static void TestLastSegmentShorthandRejected() {
    const fs::path root = packageDir();     //exactly ONE call: see the note below
    const fs::path imp = root / "dotted_short_imp";
    const fs::path use = root / "dotted_short_use";
    for (const fs::path& pkg : { imp, use }) {
        fs::create_directories(pkg / "vendor");
        std::ofstream(pkg / "vendor" / "graphics.n", std::ios::binary)
            << "int hue() { return 3; }\n";
    }
    std::ofstream(imp / "prog.n", std::ios::binary)
        << "import graphics;\n"
           "int main() { return graphics.hue(); }\n";
    std::ofstream(use / "prog.n", std::ios::binary)
        << "import vendor.graphics;\n"
           "int main() { return graphics.hue(); }\n";
    //(2a) shorthand import
    ScenarioBuild a = compileScenario(imp, { imp.string() });
    CHECK(!a.ok, "importing the last segment alone fails the build");
    CHECK(a.log.find("Module 'graphics'") != std::string::npos,
          "the shorthand import is rejected by that exact name");
    //(2b) real dotted import, shorthand use. The shorthand resolves as
    //nothing — it is nobody's identity (the package is vendor.graphics;
    //no index, no module path, no bare pool carries the bare last
    //segment), so the generic not-found names it. Same wording shape as
    //the not-a-package rejection in Task 5's TestPathBeatsShellName.
    ScenarioBuild b = compileScenario(use, { use.string() });
    CHECK(!b.ok, "a bare last segment is not callable after the dotted import");
    CHECK(b.log.find("Cannot resolve the field: graphics") != std::string::npos,
          "the shorthand resolves as nothing, named exactly");
}

//(3) D4: two search roots each offering a package named `graphics` is a
//build error naming both source paths, not a silent first-wins.
static void TestDuplicatePackageAcrossRoots() {
    const fs::path pkg = packageDir() / "dup_roots";
    fs::create_directories(pkg / "r1");
    fs::create_directories(pkg / "r2");
    std::ofstream(pkg / "r1" / "graphics.n", std::ios::binary)
        << "int hue() { return 1; }\n";
    std::ofstream(pkg / "r2" / "graphics.n", std::ios::binary)
        << "int hue() { return 2; }\n";
    std::ofstream(pkg / "prog.n", std::ios::binary)
        << "import graphics;\nint main() { return graphics.hue(); }\n";
    ScenarioBuild r = compileScenario(pkg, { (pkg/"r1").string(), (pkg/"r2").string() });
    CHECK(!r.ok, "two roots offering the same package name fail the build");
    CHECK(r.log.find("graphics") != std::string::npos, "the diagnostic names the package");
    CHECK(r.log.find("r1") != std::string::npos && r.log.find("r2") != std::string::npos,
          "both source paths appear (Step 5's new duplicate-package check)");
}

//(4) Design §4: a three-segment library path is addressable, and a parent
//import does not open the child package.
static void TestThreeSegmentPackageAndSiblingInvisibility() {
    //(4a) positive
    const fs::path ok = packageDir() / "three_seg";
    fs::create_directories(ok / "gfx" / "color");
    std::ofstream(ok / "gfx" / "color" / "deep.n", std::ios::binary)
        << "struct Shade { int v; }\n"
           "int tone() { return 4; }\n";
    std::ofstream(ok / "prog.n", std::ios::binary)
        << "import gfx.color.deep;\n"
           "int main() {\n"
           "  gfx.color.deep.Shade s;\n"
           "  s.v = gfx.color.deep.tone();\n"
           "  return s.v - 4;\n"
           "}\n";
    CapturingIo io;
    CHECK(buildAndRun(ok, io, { ok.string() }) == 0,
          "a three-segment library path is addressable for types and functions");
    //(4b) the parent package does not open the child: a REAL parent
    //package `gfx/color.n` exists, so the failure comes from the
    //`gfx.color.deep` visibility layer, not from a nonexistent module.
    const fs::path ng = packageDir() / "three_seg_neg";
    fs::create_directories(ng / "gfx" / "color");
    std::ofstream(ng / "gfx" / "color.n", std::ios::binary)
        << "int base() { return 1; }\n";
    std::ofstream(ng / "gfx" / "color" / "deep.n", std::ios::binary)
        << "int tone() { return 4; }\n";
    std::ofstream(ng / "prog.n", std::ios::binary)
        << "import gfx.color;\n"
           "int main() { return gfx.color.base() + gfx.color.deep.tone(); }\n";
    ScenarioBuild neg = compileScenario(ng, { ng.string() });
    CHECK(!neg.ok, "the parent package does not open the child package");
    CHECK(neg.log.find("gfx.color.deep") != std::string::npos,
          "the missing-import diagnostic names the exact child package");

    //(4c) design §11's other half: the package exists, the type does not.
    const fs::path miss = packageDir() / "three_seg_missing_type";
    fs::create_directories(miss / "gfx" / "color");
    std::ofstream(miss / "gfx" / "color" / "deep.n", std::ios::binary)
        << "struct Shade { int v; }\n";
    std::ofstream(miss / "prog.n", std::ios::binary)
        << "import gfx.color.deep;\n"
           "int main() { gfx.color.deep.NoSuch t; return 0; }\n";
    ScenarioBuild missOut = compileScenario(miss, { miss.string() });
    CHECK(!missOut.ok, "a package without that type is not silently accepted");
    CHECK(missOut.log.find("is not a member of") != std::string::npos
          && missOut.log.find("gfx.color.deep") != std::string::npos,
          "an imported package without that type says so, naming the package");
    //Only the stable `is not a member of` prefix is pinned: the noun in
    //the full sentence changed in Task 5 Step 6b (namespace -> package).
}

//(5) D5 / design §9: `native` in a multi-segment package is a compile-time
//diagnostic (the host DLL name splits at the first dot). Task 4 landed
//the diagnostic; Task 4's single-segment scenarios could not even mint a
//multi-segment package, so this is its only possible pin.
static void TestNativeInMultiSegmentPackageRejected() {
    const fs::path pkg = packageDir() / "native_multi";
    fs::create_directories(pkg / "gfx" / "color");
    std::ofstream(pkg / "gfx" / "color" / "deep.n", std::ios::binary)
        << "native int tone();\n";
    std::ofstream(pkg / "prog.n", std::ios::binary)
        << "import gfx.color.deep;\nint main() { return gfx.color.deep.tone(); }\n";
    ScenarioBuild nat = compileScenario(pkg, { pkg.string() });
    CHECK(!nat.ok,
          "the build actually fails - a message alone would also match a green run");
    CHECK(nat.log.find("native") != std::string::npos,
          "the diagnostic mentions native");
    CHECK(nat.log.find("gfx.color.deep") != std::string::npos,
          "and it names the package that cannot carry it");
}

//(6) Design §2: two packages sharing a LAST segment (`a.io`, `b.io`)
//coexist — functions and types stay separate. Both functions are named
//`f()` (same-name, different values), so a key collision makes the sums
//wrong; the Rec layouts differ on purpose too. `io` as the last segment is
//deliberate: it also pins that a two-segment path and the builtin
//single-segment `io` are not the same identity.
static void TestSameLastSegmentPackagesCoexist() {
    const fs::path pkg = packageDir() / "same_last_seg";
    fs::create_directories(pkg / "a");
    fs::create_directories(pkg / "b");
    std::ofstream(pkg / "a" / "io.n", std::ios::binary)
        << "struct Rec { int x; }\n"
           "int f() { return 10; }\n";
    std::ofstream(pkg / "b" / "io.n", std::ios::binary)
        << "struct Rec { int y; int z; }\n"   //same type name, different layout
           "int f() { return 20; }\n";
    std::ofstream(pkg / "prog.n", std::ios::binary)
        << "import a.io;\n"
           "import b.io;\n"
           "int main() {\n"
           "  a.io.Rec p; p.x = 7;\n"
           "  b.io.Rec q; q.y = 1; q.z = 2;\n"
           "  if (p.x != 7 || q.y + q.z != 3) return 1;\n"
           "  if (a.io.f() + b.io.f() != 30) return 2;\n"
           "  return 0;\n"
           "}\n";
    CapturingIo io;
    CHECK(buildAndRun(pkg, io, { pkg.string() }) == 0,
          "two packages sharing a last segment coexist; functions and types stay separate");
}

//(7) Phase 6 Task 3 Step 1: a third-party package ships as .npkg ONLY
//(no source, no loose .ncu). The consumer's build consumes the package's
//embedded compile unit for signatures, and the built program runs
//self-contained. Negative: removing the package makes the consumer build
//fail with the module-not-found diagnostic.
static void TestProgramPackageWithoutSources() {
    //One packageDir() call: every later call remove_all's the whole
    //shared root (established rule), so all scenario dirs derive from a
    //single root and no scenario touches another's directory.
    const fs::path root = packageDir();
    const fs::path pkg = root / "pkg_only";
    fs::create_directories(pkg);

    //The third-party package: lib.ncu built in place, then packed and
    //every other artifact (source, loose .ncu) removed.
    {
        std::ofstream out(pkg / "lib.n", std::ios::binary);
        out << "int f() { return 5; }\n";
    }
    {
        BuildParams libParams;
        libParams.m_SourceFiles.push_back((pkg / "lib.n").string());
        libParams.m_sOutputModule = "lib";
        libParams.m_sOutputDir = pkg.string();
        libParams.m_sTempDir = pkg.string();
        libParams.m_sStdLibDir = STDLIB_DIR;
        ListCompileLogger libLogger;
        ModuleBuilder libBuilder(libParams, libLogger);
        bool libOk = false;
        try { libOk = libBuilder.Build(); }
        catch (const std::exception&) { libOk = false; }
        CHECK(libOk, "library package member builds");
    }
    std::string libBytes;
    {
        std::ifstream in(pkg / "lib.ncu", std::ios::binary);
        CHECK(in.good(), "lib.ncu written by the library build");
        libBytes.assign(std::istreambuf_iterator<char>(in),
                        std::istreambuf_iterator<char>());
    }
    {
        NcuPackageWriter packer;
        CHECK(packer.AddMember({"lib", libBytes}), "member packed");
        NcuEntryRecord entry;
        entry.modulePath = "lib";
        entry.functionName = "main";
        std::string error;
        CHECK(packer.Write((pkg / "lib.npkg").string(), "lib", &entry,
                           &error),
              "lib.npkg written");
    }
    std::error_code ec;
    fs::remove(pkg / "lib.ncu", ec);
    fs::remove(pkg / "lib.n", ec);
    CHECK(!fs::exists(pkg / "lib.ncu") && !fs::exists(pkg / "lib.n"),
          "only lib.npkg remains - no sources, no loose unit");

    //Consumer: imports lib; built with only the package on the import
    //path, then run in-process.
    const fs::path app = root / "pkg_app";
    fs::create_directories(app);
    {
        std::ofstream out(app / "prog.n", std::ios::binary);
        out << "import lib;\n"
               "int main() { return lib.f(); }\n";
    }
    ScenarioBuild build = compileScenario(app, { pkg.string() });
    if (!build.ok)
        std::fprintf(stderr, "consumer build log: %s\n", build.log.c_str());
    CHECK(build.ok, "consumer builds against the .npkg signature surface");
    if (build.ok) {
        CapturingIo io;
        const std::string moduleName = app.filename().string();
        CompiledModule mod = ModuleLoader::Load(
            (app / (moduleName + ".ncu")).string());
        VmExecutor exec;
        exec.AddNativeSearchDir(pkg.string());
        exec.SetHostIo(&io);
        //lib.f() returns 5; main returns it - the value IS the pin.
        CHECK(exec.Execute(mod) == 5, "program runs self-contained (rc 5)");
    }

    //Negative: without the package the consumer build fails, naming the
    //module (same in-process harness; distinct dir for a fresh build).
    const fs::path gone = root / "pkg_gone";
    fs::create_directories(gone);
    {
        std::ofstream out(gone / "prog.n", std::ios::binary);
        out << "import lib;\n"
               "int main() { return lib.f(); }\n";
    }
    ScenarioBuild missing = compileScenario(gone, { gone.string() });
    CHECK(!missing.ok, "without the package the build fails");
    CHECK(missing.log.find("'lib'") != std::string::npos,
          "the diagnostic names the missing module");
}

int main() {
    Runtime::StaticInit();
    std::fprintf(stderr, "=== Third-party Library Integration Tests ===\n");
    TestThirdPartyNativeLibrary();
    TestMixedLibraryFromFixture();
    TestDottedLibraryImport();
    TestLastSegmentShorthandRejected();
    TestDuplicatePackageAcrossRoots();
    TestThreeSegmentPackageAndSiblingInvisibility();
    TestNativeInMultiSegmentPackageRejected();
    TestSameLastSegmentPackagesCoexist();
    TestProgramPackageWithoutSources();
    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
