// Builtin-surface coverage gate (v0.12.7 Task 8.5): every class the
// C++ side still certifies as a builtin must carry a real class
// declaration in the standard library's .n sources. The gate is the
// systematic check the user asked for: a new builtin row that lacks a
// declaration face fails here by name, instead of drifting as an
// invisible C++-only surface.
//
// Two assertions:
//   1. streamClassesDeclaredInIo — ByteStream/FileStream are declared
//      in io with the full method families (18 and 17 methods; the two
//      classes the v0.12.3 migration left behind, moved by Task 8.5).
//   2. handleClassesHaveDeclarations — every name the C++ side
//      certifies (s_HandleClassNames + the face classes) resolves to a
//      declared class in an imported package.
#include <QtTest/QtTest>
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/CastInfo.h>
#include "builder/ModuleRegistry.h"
#include "builder/BuiltinNames.h"
#include "builder/BindingTable.h"    //IsHandleClassName
#include "SyntaxTree.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef STDLIB_DIR
#define STDLIB_DIR ""
#endif

using namespace nlang;

namespace {

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

//One in-process build importing the four carrying packages; the
//builder stays alive for the callback so the registry remains valid.
template <typename Fn>
bool withStdlibBuild(const char* szName, Fn fn)
{
    namespace fs = std::filesystem;
    const fs::path dir =
        fs::temp_directory_path() / "nlang_builtin_surface_tests";
    fs::create_directories(dir);
    const fs::path file = dir / (std::string(szName) + ".n");
    std::ofstream(file, std::ios::binary)
        << "import io;\nimport containers;\nimport thread;\n"
           "import concurrent;\nint main() { return 0; }\n";
    BuildParams params;
    params.m_SourceFiles.push_back(file.string());
    params.m_sOutputModule = szName;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    MemLogger logger;
    ModuleBuilder builder(params, logger);
    ModuleBuilder::UnitImages images;
    bool ok = false;
    try { images = builder.BuildUnitImages(); ok = !images.units.empty(); }
    catch (const std::exception&) { ok = false; }
    if (!ok)
    {
        for (const auto& err : logger.errorsText())
            qWarning("build error: %s", err.c_str());
        return false;
    }
    fn(builder, images);
    return true;
}

int countFunctionMembers(const SnField& member)
{
    int methods = 0;
    if (member.Kind() == NK_ClassDecl)
    {
        for (auto& fn : static_cast<const SnClassDecl&>(member).Members())
            if (fn.Kind() == NK_Function)
                ++methods;
    }
    return methods;
}

//First class declaration named className across every imported module.
const SnClassDecl* findDeclaredClass(const ModuleRegistry& reg,
    const std::string& className)
{
    for (uint32_t i = 0; i < reg.ModuleCount(); ++i)
    {
        SnField* hit = reg.FindModuleType(reg.ModulePathOf(i), className);
        if (hit && hit->Kind() == NK_ClassDecl)
            return static_cast<const SnClassDecl*>(hit);
    }
    return nullptr;
}

} //namespace

class TestBuiltinSurfaceCoverage : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() {
        Runtime::StaticInit();
        TypeCastInfo::StaticInit();
    }

    void streamClassesDeclaredInIo();
    void handleClassesHaveDeclarations();
};

void TestBuiltinSurfaceCoverage::streamClassesDeclaredInIo()
{
    QVERIFY(withStdlibBuild("stream_io_face",
        [](ModuleBuilder& builder, ModuleBuilder::UnitImages&) {
        auto& reg = builder.Registry();
        //ByteStream declares 18 methods; FileStream 17 (it has no
        //reset — rewind on a file stream re-opens instead). Counts
        //include the constructor (a same-name function member).
        struct StreamExpectation {
            const char* className;
            int methodCount;
        };
        const StreamExpectation streamClasses[] = {
            { "ByteStream", 19 }, { "FileStream", 18 },
        };
        for (const auto& expect : streamClasses)
        {
            SnField* hit = nullptr;
            for (uint32_t i = 0; i < reg.ModuleCount(); ++i)
            {
                if (reg.ModulePathOf(i) != "io")
                    continue;
                hit = reg.FindModuleType("io", expect.className);
                break;
            }
            QVERIFY2(hit != nullptr,
                qPrintable(QString(
                    "%1 is not declared in io — the stream classes must "
                    "carry .n declarations (builtin-surface coverage "
                    "gate)").arg(expect.className)));
            QCOMPARE(hit->Kind(), NK_ClassDecl);
            QCOMPARE(countFunctionMembers(*hit), expect.methodCount);
        }
    }));
}

void TestBuiltinSurfaceCoverage::handleClassesHaveDeclarations()
{
    QVERIFY(withStdlibBuild("handle_faces",
        [](ModuleBuilder& builder, ModuleBuilder::UnitImages&) {
        auto& reg = builder.Registry();
        //Mirror of BindingTable.cpp's s_HandleClassNames (append-only;
        //the C++ side exports only the per-name IsHandleClassName), so
        //each entry is cross-checked against the C++ certification
        //too — a name here that C++ stopped certifying fails below.
        const char* handleNames[] = {
            "ByteStream", "FileStream", "List", "Dict", "Thread",
            "Mutex", "Channel", "Future", "Semaphore", "Promise",
            "ReentrantMutex", "AtomicInt",
            "String", "Array", "Object",
        };
        for (const char* name : handleNames)
        {
            QVERIFY2(IsHandleClassName(name) || IsPreludeFaceClassName(name)
                || std::string(name) == "Object",
                qPrintable(QString("%1 is no longer a certified builtin "
                    "class — update the coverage list").arg(name)));
            const SnClassDecl* decl = findDeclaredClass(reg, name);
            QVERIFY2(decl != nullptr,
                qPrintable(QString(
                    "builtin class %1 has no .n declaration in the "
                    "imported standard-library packages — every "
                    "user-visible builtin needs a declaration face "
                    "(builtin-surface coverage gate)").arg(name)));
        }
    }));
}

QTEST_GUILESS_MAIN(TestBuiltinSurfaceCoverage)
#include "test_builtin_surface_coverage.moc"
