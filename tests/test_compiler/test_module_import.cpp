/*---
test_module_import.cpp - module registry unit tests.

In-process ModuleBuilder coverage of the compile-time module registry
(module import visibility plan): per-TU module path computation
relative to BuildParams::m_sProjectDir, the single-file stem fallback,
the reserved path-segment gate, the per-TU import gates with their
external .nmod stub tables, and the owner tags MergeTransUnits stamps
on every merged TU member (top-level and nested-namespace members).
---*/
#include <QtTest/QtTest>
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
//Internal header: the registry is an opaque type in the public API, so
//the test adds src/compiler to its include path (same pattern as
//test_vm reaching into src/vm).
#include "builder/ModuleRegistry.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace nlang;

namespace {

//Collects error diagnostics in memory; the overridable hook of
//CompileLogger is WriteLog (same shape as test_import_parse.cpp).
class MemLogger : public CompileLogger
{
public:
    const std::vector<std::string>& errorsText() const
    {
        return m_errors;
    }

protected:
    void WriteLog(CompileLogLevel level, const ISourceLocation*,
        const char* szMessage) override
    {
        if (level == CLL_Error || level == CLL_Fatal)
            m_errors.emplace_back(szMessage);
    }

private:
    std::vector<std::string> m_errors;
};

//Write one source file; returns an empty string when the file could not
//be created (the caller fails the test instead of testing nothing).
std::string writeTempSource(const char* szName, const char* szBody)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path()
        / "nlang_module_import_tests";
    std::error_code fsError;
    std::filesystem::create_directories(dir, fsError);
    const std::filesystem::path file = dir / szName;
    std::ofstream stream(file, std::ios::binary);
    if (fsError || !stream)
        return std::string();
    stream << szBody;
    return stream.good() ? file.string() : std::string();
}

//Outcome of one in-process compile: build ok, collected error texts,
//and the registry's per-TU view, so every test drives this same helper
//instead of hand-rolling BuildParams/MemLogger/ModuleBuilder boilerplate.
struct CompileOutcome
{
    bool ok = false;
    std::vector<std::string> errors;
    std::vector<std::string> modulePaths;   //ModulePathOf, TU order
    std::vector<std::string> directories;   //DirectoryOf, TU order
};

//Compile the given sources with an optional project root.
CompileOutcome compile(const std::string& tag,
    const std::vector<std::string>& sources,
    const std::string& projectDir)
{
    const auto outDir = std::filesystem::temp_directory_path()
        / "nlang_module_import_tests";
    BuildParams params;
    for (const auto& source : sources)
        params.m_SourceFiles.push_back(source);
    params.m_sProjectDir = projectDir;
    params.m_sOutputModule = tag;
    params.m_sOutputDir = outDir.string();
    params.m_sTempDir = outDir.string();
    MemLogger logger;
    ModuleBuilder builder(params, logger);
    CompileOutcome outcome;
    //Exception boundary (same contract as ncc main): codegen internal
    //errors throw; without the catch the abort would kill the test run.
    try { outcome.ok = builder.Build(); }
    catch (const std::exception&) { outcome.ok = false; }
    outcome.errors = logger.errorsText();
    const ModuleRegistry& reg = builder.Registry();
    for (uint32_t i = 0; i < reg.ModuleCount(); ++i)
    {
        outcome.modulePaths.push_back(reg.ModulePathOf(i));
        outcome.directories.push_back(reg.DirectoryOf(i));
    }
    return outcome;
}

bool containsError(const std::vector<std::string>& errors,
    const char* szNeedle)
{
    for (const auto& error : errors)
    {
        if (error.find(szNeedle) != std::string::npos)
            return true;
    }
    return false;
}

//Registry index of the module with the given path; NO_OWNER when the
//path is not registered.
uint32_t moduleIndexOfPath(const ModuleRegistry& reg,
    const std::string& path)
{
    for (uint32_t i = 0; i < reg.ModuleCount(); ++i)
    {
        if (reg.ModulePathOf(i) == path)
            return i;
    }
    return ModuleRegistry::NO_OWNER;
}

//First member of the given kind and name in a namespace member list;
//null when absent.
const SnField* findMember(
    const SnFunctionParentField::MemberList& members,
    NodeKind kind, const char* szName)
{
    for (const auto& member : members)
    {
        if (member.Kind() == kind && member.Name() == szName)
            return &member;
    }
    return nullptr;
}

//Shared scaffold of the import-gate tests: a two-stage build. Stage 1
//compiles the external lib(s) into out/; stage 2 compiles the main
//project that reaches them through m_ImportDirs. An infrastructure
//failure (source could not be written, stage 1 failed) returns a
//result with a null builder — callers assert ok before touching it.
//
//Directory layout (<tmp>/nlang_import_gate):
//  libsrc/lib.n           external lib source (int add(int,int))
//  libsrc/lib2.n          twin lib exporting the same name (opt-in)
//  out/lib.nmod           external lib artifact (stage 1)
//  proj/main.n            main project (content injected)
//  proj/utils/helper.n    cross-directory module
//  proj/utils/sub/deep.n  recursive wildcard target
//  proj/extra.n           root-directory peer (same-dir auto-import)
//  proj/utils.n           root module named "utils" (D11 opt-in)
//  proj/utils2/MyClass.n  module sharing a class name (Task 5 opt-in)
struct GateResult
{
    bool ok = false;
    std::vector<std::string> errors;
    //Own the inputs: ModuleBuilder/BuildEnvironment hold REFERENCES to
    //the params & logger, so they must outlive the builder — declared
    //before it, destroyed after it.
    std::unique_ptr<BuildParams> params;
    std::unique_ptr<MemLogger> logger;
    std::unique_ptr<ModuleBuilder> builder;
};

//Variants of the gate scaffold: a named struct instead of a bool
//parameter wall, so every new variant lands here with its default
//visible at the declaration.
struct GateProjectOptions
{
    const char* szMainBody;
    bool withTwinLib = false;        //also build libsrc/lib2.n
    bool withRootUtils = false;      //also register root utils.n
    bool helperImportsLib = false;   //helper.n imports the external lib
    const char* szHelperBody = nullptr;   //explicit helper.n body
    const char* szExtraBody = nullptr;    //explicit extra.n body
    bool withUtils2MyClass = false;  //also register utils2/MyClass.n
};

GateResult buildGateProject(const GateProjectOptions& opts)
{
    namespace fs = std::filesystem;
    GateResult failure;
    const fs::path root = fs::temp_directory_path() / "nlang_import_gate";
    const fs::path libSrc = root / "libsrc";
    const fs::path out = root / "out";
    const fs::path proj = root / "proj";
    std::error_code fsError;
    //ModuleManager is process-global and refuses a second Create of the
    //same module name, so the external libs are built once per process
    //and later calls reuse their .nmod artifacts on disk. Only the
    //project subtree is refreshed per call; a fresh process starts
    //from a clean root.
    static bool firstCall = true;
    if (firstCall)
    {
        fs::remove_all(root, fsError);
        firstCall = false;
    }
    fs::remove_all(proj, fsError);
    fs::create_directories(libSrc, fsError);
    fs::create_directories(out, fsError);
    fs::create_directories(proj / "utils" / "sub", fsError);
    fs::create_directories(proj / "utils2", fsError);
    auto writeFile = [](const fs::path& file, const char* szBody)
    {
        std::ofstream stream(file, std::ios::binary);
        if (!stream)
            return false;
        stream << szBody;
        return stream.good();
    };
    //Explicit helper body wins (owner-tag tests need a custom one);
    //otherwise the lib-importing variant or the default body.
    const char* szHelper = opts.szHelperBody;
    if (szHelper == nullptr)
        szHelper = opts.helperImportsLib
            ? "import lib;\nint help() { return 3; }\n"
            : "int help() { return 3; }\n";
    const bool written =
        writeFile(libSrc / "lib.n",
            "int add(int a, int b) { return a + b; }\n") &&
        (!opts.withTwinLib ||
            writeFile(libSrc / "lib2.n",
                "int add(int a, int b) { return a + b; }\n")) &&
        writeFile(proj / "utils" / "helper.n", szHelper) &&
        writeFile(proj / "utils" / "sub" / "deep.n",
            "int deep() { return 4; }\n") &&
        writeFile(proj / "extra.n", opts.szExtraBody
            ? opts.szExtraBody : "int extra() { return 9; }\n") &&
        (!opts.withRootUtils ||
            writeFile(proj / "utils.n",
                "int rootUtil() { return 6; }\n")) &&
        (!opts.withUtils2MyClass ||
            writeFile(proj / "utils2" / "MyClass.n",
                "int m() { return 8; }\n")) &&
        writeFile(proj / "main.n", opts.szMainBody);
    if (!written)
    {
        failure.errors.push_back(
            "gate scaffold: temp source could not be written");
        return failure;
    }

    //Stage 1: compile the external .nmod(s) into out/ (skip the ones an
    //earlier call of this process already produced).
    const char* libNames[] = {"lib", "lib2"};
    const int libCount = opts.withTwinLib ? 2 : 1;
    for (int i = 0; i < libCount; ++i)
    {
        const fs::path nmod =
            out / (std::string(libNames[i]) + ".nmod");
        if (fs::exists(nmod))
            continue;
        BuildParams libParams;
        libParams.m_SourceFiles.push_back(
            (libSrc / (std::string(libNames[i]) + ".n")).string());
        libParams.m_sOutputModule = libNames[i];
        libParams.m_sOutputDir = out.string();
        libParams.m_sTempDir = out.string();
        MemLogger libLogger;
        ModuleBuilder libBuilder(libParams, libLogger);
        bool libOk = false;
        try { libOk = libBuilder.Build(); }
        catch (const std::exception&) { libOk = false; }
        if (!libOk)
        {
            failure.errors = libLogger.errorsText();
            return failure;
        }
    }

    //Stage 2: compile the main project with out/ importable. The output
    //module name must be unique per call (ModuleManager, see above).
    static uint32_t gateRunCount = 0;
    GateResult res;
    res.params = std::make_unique<BuildParams>();
    res.params->m_sProjectDir = proj.string();
    for (const char* szRel : {"main.n", "utils/helper.n",
            "utils/sub/deep.n", "extra.n"})
        res.params->m_SourceFiles.push_back((proj / szRel).string());
    //Appended last so module 0 (main) and the TU order of every other
    //test stay unchanged; utils.n is registered after extra.n.
    if (opts.withRootUtils)
        res.params->m_SourceFiles.push_back((proj / "utils.n").string());
    if (opts.withUtils2MyClass)
        res.params->m_SourceFiles.push_back(
            (proj / "utils2" / "MyClass.n").string());
    res.params->m_ImportDirs.push_back(out.string());
    res.params->m_sOutputModule =
        "gate_test_" + std::to_string(++gateRunCount);
    res.params->m_sOutputDir = out.string();
    res.params->m_sTempDir = out.string();
    res.logger = std::make_unique<MemLogger>();
    res.builder = std::make_unique<ModuleBuilder>(*res.params, *res.logger);
    try { res.ok = res.builder->Build(); }
    catch (const std::exception&) { res.ok = false; }
    res.errors = res.logger->errorsText();
    return res;
}

} //namespace

class TestModuleImport : public QObject
{
    Q_OBJECT
private slots:
    //Runtime tables (IdString etc.) must exist before any Build().
    void initTestCase() { Runtime::StaticInit(); }

    //nproj layout: root/main.n, root/utils/helper.n,
    //root/utils/sub/deep.n -> module paths main / utils.helper /
    //utils.sub.deep, directory strings "" / "utils" / "utils.sub".
    void projectModulePaths()
    {
        auto dir = std::filesystem::temp_directory_path()
            / "nlang_proj_paths";
        std::filesystem::create_directories(dir / "utils" / "sub");
        auto write = [&dir](const char* szRel, const char* szBody)
        {
            std::ofstream stream(dir / szRel, std::ios::binary);
            QVERIFY2(bool(stream), "temp source could not be written");
            stream << szBody;
        };
        write("main.n", "int main() { return 0; }\n");
        write("utils/helper.n", "int help() { return 1; }\n");
        write("utils/sub/deep.n", "int deep() { return 2; }\n");

        const auto outcome = compile("proj_paths_test",
            {(dir / "main.n").string(),
             (dir / "utils/helper.n").string(),
             (dir / "utils/sub/deep.n").string()},
            dir.string());
        QVERIFY2(outcome.ok,
            "three-file project layout must build clean");

        QCOMPARE(outcome.modulePaths[0], std::string("main"));
        QCOMPARE(outcome.modulePaths[1], std::string("utils.helper"));
        QCOMPARE(outcome.modulePaths[2], std::string("utils.sub.deep"));
        QCOMPARE(outcome.directories[0], std::string(""));
        QCOMPARE(outcome.directories[1], std::string("utils"));
        QCOMPARE(outcome.directories[2], std::string("utils.sub"));
    }

    //Single-file mode has no project root: the module path degenerates
    //to the file stem.
    void singleFileModeUsesStem()
    {
        auto path = writeTempSource("solo.n",
            "int main() { return 0; }\n");
        QVERIFY2(!path.empty(), "temp source could not be written");
        const auto outcome = compile("solo_test", {path}, std::string());
        QVERIFY2(outcome.ok, "single-file build must succeed");
        QCOMPARE(outcome.modulePaths[0], std::string("solo"));
    }

    //A source path segment colliding with a built-in namespace name
    //(io/math/fs) is a compile error, not a silently unreachable module.
    void reservedPathSegmentRejected()
    {
        auto dir = std::filesystem::temp_directory_path()
            / "nlang_proj_reserved";
        std::filesystem::create_directories(dir / "io");
        std::ofstream(dir / "main.n", std::ios::binary)
            << "int main() { return 0; }\n";
        std::ofstream(dir / "io/ops.n", std::ios::binary)
            << "int f() { return 1; }\n";
        const auto outcome = compile("reserved_test",
            {(dir / "main.n").string(), (dir / "io/ops.n").string()},
            dir.string());
        QVERIFY2(!outcome.ok, "reserved segment must fail the build");
        QVERIFY2(containsError(outcome.errors,
            "Module path segment 'io' collides with a built-in "
            "namespace."),
            "reserved segment must get the collision diagnostic");
    }

    //The shared helper keeps the single-file contract: an empty project
    //dir yields the stem-only module path.
    void compileHelperKeepsStemContract()
    {
        auto path = writeTempSource("helper_solo.n",
            "int main() { return 0; }\n");
        QVERIFY2(!path.empty(), "temp source could not be written");
        const auto outcome = compile("helper_solo_test", {path},
            std::string());
        QVERIFY2(outcome.ok, "helper compile must build clean");
        QCOMPARE(outcome.modulePaths[0], std::string("helper_solo"));
        QVERIFY2(!containsError(outcome.errors, "syntax error"),
            "helper compile must stay syntax clean");
    }

    //One gate per TU (main.n = module 0): builtin / project / external
    //imports each land in the gate; an unimported project module stays
    //out.
    void importGateBuiltinAndProjectAndExternal()
    {
        auto res = buildGateProject({
            "import io;\n"
            "import utils.helper;\n"
            "import lib;\n"
            "int main() { return 0; }\n"});
        QVERIFY2(res.ok, "gated project with valid imports must build");
        const ModuleRegistry& reg = res.builder->Registry();
        QVERIFY(reg.IsBuiltinImported(0, "io"));
        QVERIFY(reg.IsModuleImported(0, "utils.helper"));
        QVERIFY(reg.IsModuleImported(0, "lib"));
        QVERIFY(!reg.IsModuleImported(0, "utils.sub.deep"));
    }

    //Gates are per TU: helper.n imports the external lib, main.n does
    //not — 'lib' must be inside helper's gate only. helperIdx comes
    //from the registry path table (never a hardcoded position), which
    //also pins the gateModuleIndex <-> TU order alignment that
    //LoadImports' two same-order loops rely on constructively.
    void perTUGateIsolation()
    {
        GateProjectOptions opts;
        opts.szMainBody = "int main() { return 0; }\n";
        opts.helperImportsLib = true;
        auto res = buildGateProject(opts);
        QVERIFY2(res.ok, "helper-only external import must build");
        const ModuleRegistry& reg = res.builder->Registry();
        const uint32_t helperIdx = moduleIndexOfPath(reg, "utils.helper");
        QVERIFY(helperIdx != ModuleRegistry::NO_OWNER);
        QVERIFY(reg.IsModuleImported(helperIdx, "lib"));
        QVERIFY(!reg.IsModuleImported(0, "lib"));
    }

    //D5: the wildcard is a recursive prefix match — nested
    //subdirectory modules are inside the gate too.
    void wildcardIsRecursivePrefix()
    {
        auto res = buildGateProject({
            "import utils.*;\n"
            "int main() { return 0; }\n"});
        QVERIFY2(res.ok, "wildcard-only project must build");
        const ModuleRegistry& reg = res.builder->Registry();
        QVERIFY(reg.IsModuleImported(0, "utils.helper"));
        QVERIFY(reg.IsModuleImported(0, "utils.sub.deep"));
    }

    //D10: a wildcard on a builtin name is rejected — builtins are
    //namespaces, not module trees (the '*' would be silently eaten).
    void builtinWildcardRejected()
    {
        auto res = buildGateProject({
            "import io.*;\n"
            "int main() { return 0; }\n"});
        QVERIFY2(!res.ok, "builtin wildcard must fail the build");
        QVERIFY2(containsError(res.errors,
            "Wildcard import cannot target builtin namespace 'io'."),
            "builtin wildcard must get the dedicated diagnostic");
    }

    //D10: a wildcard matching no project TU module path is almost
    //certainly a typo — external .nmod names are single-segment (§3.3)
    //and never count as matches.
    void wildcardZeroMatchRejected()
    {
        auto res = buildGateProject({
            "import nosuch.*;\n"
            "int main() { return 0; }\n"});
        QVERIFY2(!res.ok, "zero-match wildcard must fail the build");
        QVERIFY2(containsError(res.errors,
            "No project modules matched import 'nosuch.*'."),
            "zero-match wildcard must get the dedicated diagnostic");
    }

    //D11 regression: a wildcard never reaches an external .nmod name.
    //'lib' exists only as an external module, so the D11 union surface
    //(exact project module OR 'lib.'-prefixed project module) is empty
    //and the import fails loud instead of building with a closed gate.
    void wildcardNeverMatchesExternalRejected()
    {
        auto res = buildGateProject({
            "import lib.*;\n"
            "int main() { return 0; }\n"});
        QVERIFY2(res.builder != nullptr,
            "gate scaffold failed before the gate stage");
        QVERIFY2(!res.ok,
            "wildcard on an external-only name must fail the build");
        QVERIFY2(containsError(res.errors,
            "No project modules matched import 'lib.*'."),
            "external-only wildcard must get the zero-match diagnostic");
    }

    //D11: 'import utils.*;' is a union — the exact module "utils"
    //(root utils.n) AND the recursive "utils." prefix both join the
    //gate. The prefix side is the discriminating assertion: before D11
    //the exact hit silently degraded the wildcard to exact-only. The
    //exact side is additionally (and unavoidably) covered by D7 here,
    //since main.n and utils.n share the root directory.
    void wildcardIncludesExactModule()
    {
        GateProjectOptions opts;
        opts.szMainBody =
            "import utils.*;\n"
            "int main() { return 0; }\n";
        opts.withRootUtils = true;
        auto res = buildGateProject(opts);
        QVERIFY2(res.ok, "wildcard over exact module + tree must build");
        const ModuleRegistry& reg = res.builder->Registry();
        QVERIFY(reg.IsModuleImported(0, "utils"));
        QVERIFY(reg.IsModuleImported(0, "utils.helper"));
        QVERIFY(reg.IsModuleImported(0, "utils.sub.deep"));
    }

    //D7: files of the TU's own directory are implicitly imported.
    void sameDirectoryAutoImported()
    {
        auto res = buildGateProject({
            "int main() { return 0; }\n"});
        QVERIFY2(res.ok, "import-free project must build");
        QVERIFY(res.builder->Registry().IsModuleImported(0, "extra"));
    }

    //§5.3: single-file mode has no project context, so a dotted import
    //can never resolve — spec §7 module-not-found wording.
    void dottedImportSingleFileModeFails()
    {
        auto path = writeTempSource("t_dotted.n",
            "import utils.helper;\n"
            "int main() { return 0; }\n");
        QVERIFY2(!path.empty(), "temp source could not be written");
        const auto outcome = compile("dotted_solo_test", {path},
            std::string());
        QVERIFY2(!outcome.ok, "dotted import must fail in single-file mode");
        QVERIFY2(containsError(outcome.errors,
            "Module 'utils.helper' not found. Check the project "
            "Sources list or -I import path."),
            "dotted import must get the module-not-found diagnostic");
    }

    //A single-segment import that is neither builtin, project module,
    //nor a loadable .nmod gets the spec §7 module-not-found wording.
    void unknownImportFails()
    {
        auto res = buildGateProject({
            "import nosuch;\n"
            "int main() { return 0; }\n"});
        //Rule out a scaffold infrastructure failure first, so the
        //needle check below cannot mask it.
        QVERIFY2(res.builder != nullptr,
            "gate scaffold failed before the gate stage");
        QVERIFY2(!res.ok, "unknown import must fail the build");
        QVERIFY2(containsError(res.errors,
            "Module 'nosuch' not found. Check the project "
            "Sources list or -I import path."),
            "unknown import must get the module-not-found diagnostic");
    }

    //Duplicate and overlapping imports are idempotent: exact, wildcard
    //and repeated forms union into one gate.
    void duplicateImportIdempotent()
    {
        auto res = buildGateProject({
            "import lib;\n"
            "import lib;\n"
            "import utils.*;\n"
            "import utils.helper;\n"
            "int main() { return 0; }\n"});
        QVERIFY2(res.ok, "duplicate imports must build");
        const ModuleRegistry& reg = res.builder->Registry();
        QVERIFY(reg.IsModuleImported(0, "lib"));
        QVERIFY(reg.IsModuleImported(0, "utils.helper"));
        //Registry-level idempotence: exactly ONE external entry exists
        //for 'lib' no matter how often the import is repeated.
        size_t libEntryCount = 0;
        for (uint32_t i = 0; i < reg.ModuleCount(); ++i)
        {
            if (reg.ModulePathOf(i) == "lib")
                ++libEntryCount;
        }
        QCOMPARE(libEntryCount, size_t(1));
    }

    //IsKnownModule covers both kinds of registry entries — project
    //modules and external .nmod names — and nothing else.
    void knownModuleTruthTable()
    {
        auto res = buildGateProject({
            "import utils.helper;\n"
            "import lib;\n"
            "int main() { return 0; }\n"});
        QVERIFY2(res.ok, "project + external imports must build");
        const ModuleRegistry& reg = res.builder->Registry();
        QVERIFY(reg.IsKnownModule("utils.helper"));
        QVERIFY(reg.IsKnownModule("lib"));
        QVERIFY(!reg.IsKnownModule("nosuch"));
    }

    //C1 regression: the imported add() stub must be owned by an
    //EXTERNAL registry entry (never a TU directory).
    void externalStubOwnersTagged()
    {
        auto res = buildGateProject({
            "import lib;\n"
            "int main() { return 0; }\n"});
        QVERIFY2(res.ok, "external-import project must build");
        const ModuleRegistry& reg = res.builder->Registry();
        bool found = false;
        for (const auto& member : res.builder->TreeRootView().Members())
        {
            if (member.Name() == "add" && member.Kind() == NK_Function)
            {
                const uint32_t owner = reg.OwnerOf(member);
                QVERIFY(owner != ModuleRegistry::NO_OWNER);
                QVERIFY(reg.IsExternal(owner));
                found = true;
            }
        }
        QVERIFY2(found, "the imported add() stub must reach the root");
    }

    //Two external .nmod modules exporting the same function name: the
    //second module's stub must not gain a second root entry (one 'add'
    //stays) but must still land in its own module's stub table —
    //qualified calls resolve through the stub table, never through the
    //shared root.
    void externalStubTableCompleteOnNameClash()
    {
        GateProjectOptions opts;
        opts.szMainBody =
            "import lib;\n"
            "import lib2;\n"
            "int main() { return 0; }\n";
        opts.withTwinLib = true;
        auto res = buildGateProject(opts);
        QVERIFY2(res.ok, "twin-lib project must build");
        const ModuleRegistry& reg = res.builder->Registry();
        size_t rootAddCount = 0;
        for (const auto& member : res.builder->TreeRootView().Members())
        {
            if (member.Name() == "add" && member.Kind() == NK_Function)
                ++rootAddCount;
        }
        QCOMPARE(rootAddCount, size_t(1));

        const uint32_t libIdx = moduleIndexOfPath(reg, "lib");
        const uint32_t lib2Idx = moduleIndexOfPath(reg, "lib2");
        QVERIFY(libIdx != ModuleRegistry::NO_OWNER);
        QVERIFY(lib2Idx != ModuleRegistry::NO_OWNER);
        QVERIFY(reg.IsExternal(libIdx));
        QVERIFY(reg.IsExternal(lib2Idx));
        const std::vector<SnFunction*> libStubs =
            reg.ModuleFunctions("lib", "add");
        const std::vector<SnFunction*> lib2Stubs =
            reg.ModuleFunctions("lib2", "add");
        QVERIFY2(!libStubs.empty(), "lib must expose its own add stub");
        QVERIFY2(!lib2Stubs.empty(), "lib2 stub table must be complete");
        QVERIFY(libStubs.front() != lib2Stubs.front());
        QCOMPARE(reg.OwnerOf(*lib2Stubs.front()), lib2Idx);
    }

    //F18: MergeTransUnits tags every merged TU member with its owning
    //module BEFORE the merge erases the unit boundary — top-level
    //functions/classes and nested namespace members alike (a namespace
    //can span TUs, so the tag must land at member level, not on the
    //namespace alone). The resolver-context view follows the parent
    //chain: any descendant of main resolves to main's index.
    void ownerTagsTopLevelAndNamespaceMembers()
    {
        //main.n: int main + class Cfg; helper.n: int help +
        //namespace NS { int inner }.
        GateProjectOptions opts;
        opts.szMainBody =
            "class Cfg {\n"
            "    public int size;\n"
            "}\n"
            "int main() { return 0; }\n";
        opts.szHelperBody =
            "int help() { return 3; }\n"
            "namespace NS {\n"
            "    int inner() { return 5; }\n"
            "}\n";
        auto res = buildGateProject(opts);
        QVERIFY2(res.ok, "project with class + namespace must build");
        const ModuleRegistry& reg = res.builder->Registry();
        const uint32_t mainIdx = moduleIndexOfPath(reg, "main");
        const uint32_t helperIdx = moduleIndexOfPath(reg, "utils.helper");
        QVERIFY(mainIdx != ModuleRegistry::NO_OWNER);
        QVERIFY(helperIdx != ModuleRegistry::NO_OWNER);

        const SnNamespace& rootView = res.builder->TreeRootView();
        const SnField* pMainFunc =
            findMember(rootView.Members(), NK_Function, "main");
        const SnField* pCfg =
            findMember(rootView.Members(), NK_ClassDecl, "Cfg");
        const SnField* pMergedNS =
            findMember(rootView.Members(), NK_Namespace, "NS");
        const SnField* pHelp =
            findMember(rootView.Members(), NK_Function, "help");
        QVERIFY2(pMainFunc != nullptr, "main must reach the merged root");
        QVERIFY2(pCfg != nullptr, "Cfg must reach the merged root");
        QVERIFY2(pMergedNS != nullptr, "NS must reach the merged root");
        QVERIFY2(pHelp != nullptr, "help must reach the merged root");
        QCOMPARE(reg.OwnerOf(*pMainFunc), mainIdx);
        QCOMPARE(reg.OwnerOf(*pCfg), mainIdx);
        QCOMPARE(reg.OwnerOf(*pMergedNS), helperIdx);
        QCOMPARE(reg.OwnerOf(*pHelp), helperIdx);

        //Member-level tag inside a merged namespace (the F18 point).
        const auto& mergedNS = static_cast<const SnNamespace&>(*pMergedNS);
        const SnField* pInner =
            findMember(mergedNS.Members(), NK_Function, "inner");
        QVERIFY2(pInner != nullptr, "inner must reach the merged NS");
        QCOMPARE(reg.OwnerOf(*pInner), helperIdx);

        //Any node of main's body carries main's owner through the
        //ancestor chain.
        const auto& mainFunc = static_cast<const SnFunction&>(*pMainFunc);
        QVERIFY2(mainFunc.Body() != nullptr, "main must have a body");
        QCOMPARE(reg.OwnerOfContext(*mainFunc.Body()), mainIdx);
    }

    //A namespace declared by several TUs merges member-wise into ONE
    //root entry: every contributing TU's members keep their OWN module
    //as owner inside the merged NS. (The losing NS shell dies with its
    //unit root; which TU's shell survives follows TU order — here
    //main.n parses first.)
    void namespaceCrossTUTagsEachSide()
    {
        //main.n declares NS { outer }; helper.n declares NS { inner }.
        GateProjectOptions opts;
        opts.szMainBody =
            "namespace NS {\n"
            "    int outer() { return 7; }\n"
            "}\n"
            "int main() { return 0; }\n";
        opts.szHelperBody =
            "int help() { return 3; }\n"
            "namespace NS {\n"
            "    int inner() { return 5; }\n"
            "}\n";
        auto res = buildGateProject(opts);
        QVERIFY2(res.ok, "cross-TU namespace project must build");
        const ModuleRegistry& reg = res.builder->Registry();
        const uint32_t mainIdx = moduleIndexOfPath(reg, "main");
        const uint32_t helperIdx = moduleIndexOfPath(reg, "utils.helper");
        QVERIFY(mainIdx != ModuleRegistry::NO_OWNER);
        QVERIFY(helperIdx != ModuleRegistry::NO_OWNER);

        const SnNamespace& rootView = res.builder->TreeRootView();
        const SnField* pMergedNS =
            findMember(rootView.Members(), NK_Namespace, "NS");
        QVERIFY2(pMergedNS != nullptr, "the merged NS must reach the root");
        QCOMPARE(reg.OwnerOf(*pMergedNS), mainIdx);
        const auto& mergedNS = static_cast<const SnNamespace&>(*pMergedNS);

        const SnField* pOuter =
            findMember(mergedNS.Members(), NK_Function, "outer");
        const SnField* pInner =
            findMember(mergedNS.Members(), NK_Function, "inner");
        QVERIFY2(pOuter != nullptr, "outer must reach the merged NS");
        QVERIFY2(pInner != nullptr, "inner must reach the merged NS");
        QCOMPARE(reg.OwnerOf(*pOuter), mainIdx);
        QCOMPARE(reg.OwnerOf(*pInner), helperIdx);
    }

    //Task 3 review follow-up: the project branch of ModuleFunctions
    //(dead production code until the owner tags landed) returns the
    //same-name functions OWNED by the module — a same-name function in
    //another module is excluded. helper's help() takes no parameters,
    //main's help(int) takes one (distinct signatures keep the duplicate
    //checker out of the picture; the param count tells them apart).
    void moduleFunctionsProjectBranch()
    {
        auto res = buildGateProject({
            "int help(int x) { return x; }\n"
            "int main() { return 0; }\n"});
        QVERIFY2(res.ok, "two-module overload project must build");
        const ModuleRegistry& reg = res.builder->Registry();
        const uint32_t mainIdx = moduleIndexOfPath(reg, "main");
        const uint32_t helperIdx = moduleIndexOfPath(reg, "utils.helper");
        QVERIFY(mainIdx != ModuleRegistry::NO_OWNER);
        QVERIFY(helperIdx != ModuleRegistry::NO_OWNER);

        const std::vector<SnFunction*> helperHelp =
            reg.ModuleFunctions("utils.helper", "help");
        QVERIFY2(helperHelp.size() == 1,
            "utils.helper must expose exactly its own help");
        QVERIFY(helperHelp.front()->Params().size() == 0);
        QCOMPARE(reg.OwnerOf(*helperHelp.front()), helperIdx);

        //Same name, other module: main gets its own overload only.
        const std::vector<SnFunction*> mainHelp =
            reg.ModuleFunctions("main", "help");
        QVERIFY2(mainHelp.size() == 1,
            "main must expose exactly its own help");
        QVERIFY(mainHelp.front()->Params().size() == 1);
        QCOMPARE(reg.OwnerOf(*mainHelp.front()), mainIdx);
    }

    //Task 4 review follow-up: the owner recursion descends through
    //NESTED namespaces declared by different TUs - A{B{f}} in main and
    //A{B{g}} in helper must merge into one A.B holding both functions,
    //each keeping its own module as owner (recursive tagging + the
    //nested MAK_Merge path).
    void namespaceNestedCrossTUTagsEachSide()
    {
        GateProjectOptions opts;
        opts.szMainBody =
            "namespace A {\n"
            "    namespace B {\n"
            "        int f() { return 1; }\n"
            "    }\n"
            "}\n"
            "int main() { return 0; }\n";
        opts.szHelperBody =
            "namespace A {\n"
            "    namespace B {\n"
            "        int g() { return 2; }\n"
            "    }\n"
            "}\n";
        auto res = buildGateProject(opts);
        QVERIFY2(res.ok, "nested cross-TU namespace project must build");
        const ModuleRegistry& reg = res.builder->Registry();
        const uint32_t mainIdx = moduleIndexOfPath(reg, "main");
        const uint32_t helperIdx = moduleIndexOfPath(reg, "utils.helper");
        QVERIFY(mainIdx != ModuleRegistry::NO_OWNER);
        QVERIFY(helperIdx != ModuleRegistry::NO_OWNER);

        const SnNamespace& rootView = res.builder->TreeRootView();
        const SnField* pA =
            findMember(rootView.Members(), NK_Namespace, "A");
        QVERIFY2(pA != nullptr, "A must reach the merged root");
        const auto& nsA = static_cast<const SnNamespace&>(*pA);
        const SnField* pB = findMember(nsA.Members(), NK_Namespace, "B");
        QVERIFY2(pB != nullptr, "B must reach the merged A");
        const auto& nsB = static_cast<const SnNamespace&>(*pB);

        const SnField* pF = findMember(nsB.Members(), NK_Function, "f");
        const SnField* pG = findMember(nsB.Members(), NK_Function, "g");
        QVERIFY2(pF != nullptr, "f must reach the merged A.B");
        QVERIFY2(pG != nullptr, "g must reach the merged A.B");
        QCOMPARE(reg.OwnerOf(*pF), mainIdx);
        QCOMPARE(reg.OwnerOf(*pG), helperIdx);
    }

    //Task 4 review follow-up: the NO_OWNER contract has a positive side -
    //a node nobody tagged (the root namespace itself is never a merge
    //member) reports NO_OWNER for both OwnerOf and OwnerOfContext instead
    //of an index that would silently pass a gate.
    void noOwnerForUntaggedNodes()
    {
        auto res = buildGateProject(
            {"int main() { return 0; }\n"});
        QVERIFY2(res.ok, "import-free project must build");
        const ModuleRegistry& reg = res.builder->Registry();
        const SnNamespace& rootView = res.builder->TreeRootView();
        QCOMPARE(reg.OwnerOf(rootView), ModuleRegistry::NO_OWNER);
        QCOMPARE(reg.OwnerOfContext(rootView), ModuleRegistry::NO_OWNER);
    }

    //Task 3 review follow-up: a source stem containing a dot ("my.lib.n")
    //would register as module path "my.lib" while DirectoryOf reports "my"
    //- the same-directory set and the `import my.*;` wildcard surface
    //would both silently mis-include it. Reject the file name up front.
    void dottedStemSourceRejected()
    {
        auto dir = std::filesystem::temp_directory_path()
            / "nlang_proj_dotted_stem";
        std::filesystem::create_directories(dir);
        std::ofstream(dir / "main.n", std::ios::binary)
            << "int main() { return 0; }\n";
        std::ofstream(dir / "my.lib.n", std::ios::binary)
            << "int f() { return 1; }\n";
        const auto outcome = compile("dotted_stem_test",
            {(dir / "main.n").string(), (dir / "my.lib.n").string()},
            dir.string());
        QVERIFY2(!outcome.ok, "dotted stem must fail the build");
        QVERIFY2(containsError(outcome.errors, "my.lib"),
            "the diagnostic must name the offending stem");
    }

    //--- Task 5: module-qualified call resolution (spec section 6.2) ---

    //import utils.helper; makes utils.helper.help() resolve to helper's
    //own help() through the module table.
    void qualifiedCrossDirectoryCall()
    {
        auto res = buildGateProject({
            "import utils.helper;\n"
            "int main() { return utils.helper.help(); }\n"});
        QVERIFY2(res.ok, "qualified cross-directory call must build");
    }

    //import lib; makes lib.add(2,3) bind the external stub - the same
    //qualified form as project modules.
    void qualifiedExternalCall()
    {
        auto res = buildGateProject({
            "import lib;\n"
            "int main() { return lib.add(2, 3); }\n"});
        QVERIFY2(res.ok, "qualified external call must build");
    }

    //D7: the own directory is implicitly imported, so extra.f() resolves
    //without any import statement.
    void qualifiedSameDirectoryWithoutImport()
    {
        GateProjectOptions opts;
        opts.szMainBody = "int main() { return extra.f(); }\n";
        opts.szExtraBody = "int f() { return 7; }\n";
        auto res = buildGateProject(opts);
        QVERIFY2(res.ok, "same-directory qualified call must build");
    }

    //D5 recursion carries over to qualified calls: the wildcard covers
    //the nested module path, so utils.sub.deep.deep() resolves.
    void qualifiedWildcardReachable()
    {
        auto res = buildGateProject({
            "import utils.*;\n"
            "int main() { return utils.sub.deep.deep(); }\n"});
        QVERIFY2(res.ok, "wildcard-imported qualified call must build");
    }

    //Spec section 7 row 1: a known module path that this TU did not
    //import gets the not-imported wording - and nothing else, in
    //particular no spurious "Cannot resolve the field" for the path.
    void qualifiedWithoutImportRejected()
    {
        auto res = buildGateProject({
            "int main() { return utils.helper.help(); }\n"});
        QVERIFY2(res.builder != nullptr,
            "gate scaffold failed before the gate stage");
        QVERIFY2(!res.ok, "unimported qualified call must fail the build");
        QVERIFY2(containsError(res.errors,
            "Module 'utils.helper' is not imported. Add 'import "
            "utils.helper;' (or 'import utils.*;') at the top of this "
            "file."),
            "must get the spec section 7 not-imported diagnostic");
        QVERIFY2(!containsError(res.errors, "Cannot resolve the field"),
            "the module gate must consume the chain without a spurious "
            "identifier diagnostic");
    }

    //Spec section 6.2: a leftmost identifier resolving as a class keeps
    //the normal (class) path - the module fallback must not fire, even
    //with a module literally named after the class (utils2/MyClass.n,
    //same method name m) inside the build. As-built limit: NLang has no
    //static dispatch in the VM (see e2e void_multi_flag.n), so codegen
    //rejects the class-name receiver AFTER resolution; the discriminating
    //assertion is that no resolution diagnostic is logged (a fallback
    //capture would either log one or bind the other module's m()).
    void classStaticWinsOverModule()
    {
        GateProjectOptions opts;
        opts.szMainBody =
            "class MyClass {\n"
            "    public static int m() { return 5; }\n"
            "}\n"
            "int main() { return MyClass.m(); }\n";
        opts.withUtils2MyClass = true;
        auto res = buildGateProject(opts);
        QVERIFY2(res.builder != nullptr,
            "gate scaffold failed before the gate stage");
        QVERIFY2(!res.ok,
            "static-flagged calls stay rejected (no static dispatch in "
            "the VM)");
        QVERIFY2(res.errors.empty(),
            "the class path must own the chain at resolve time: no "
            "module-table diagnostic may appear");
    }

    //Spec section 6.2 last line: a class shadowing a module path's first
    //segment owns the chain, and when the class path then fails the error
    //notes the conflicting module path. The class lacks the chained member
    //so the class path fails outright (a member that RESOLVES would let
    //the invoke fall through to the still-global bare pool - Task 6
    //narrows it - and the build would succeed).
    void classModuleConflictHint()
    {
        auto res = buildGateProject({
            "import utils.helper;\n"
            "class utils {\n"
            "    public int size;\n"
            "}\n"
            "int main() { return utils.helper.help(); }\n"});
        QVERIFY2(res.builder != nullptr,
            "gate scaffold failed before the gate stage");
        QVERIFY2(!res.ok, "class-shadowed chain must fail to resolve");
        QVERIFY2(containsError(res.errors,
            "(note: a module path 'utils.helper' exists; module access "
            "requires an import and qualification)"),
            "the class/module conflict note must be appended");
    }
};

QTEST_GUILESS_MAIN(TestModuleImport)
#include "test_module_import.moc"
