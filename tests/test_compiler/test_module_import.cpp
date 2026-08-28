/*---
test_module_import.cpp - module registry unit tests.

In-process ModuleBuilder coverage of the compile-time module registry
(module import visibility Task 2): per-TU module path computation
relative to BuildParams::m_sProjectDir, the single-file stem fallback,
and the reserved path-segment gate. Owner tagging and import gating
are later tasks in the same plan — this suite only pins the
registry's path bookkeeping.
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
};

QTEST_GUILESS_MAIN(TestModuleImport)
#include "test_module_import.moc"
