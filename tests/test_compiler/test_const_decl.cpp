/*---
test_const_decl.cpp - const-aggregate declaration marker tests.

In-process ModuleBuilder coverage of the `const class` / `const struct`
declaration prefix (v0.12.4 D1): the grammar accepts the prefix, the
marker lands on the declaration node, and the class form propagates
through the inheritance chain (a subclass of a const class is sealed
too). Structs stand alone — no inheritance to walk.
---*/
#include <QtTest/QtTest>
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/CastInfo.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/ScriptLocation.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#ifndef STDLIB_DIR
#define STDLIB_DIR ""
#endif

using namespace nlang;

namespace {

//Collects error diagnostics in memory; errors() drives the assertions.
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

std::string writeTempSource(const QString& name, const char* body)
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "nlang_const_decl_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name.toStdString();
    std::ofstream(file, std::ios::binary) << body;
    return file.string();
}

//One built module plus its diagnostics; the decl finder walks the
//merged root (TreeRootView) so the tests never guess module paths.
struct BuildOutcome
{
    std::unique_ptr<ModuleBuilder> upBuilder;
    std::vector<std::string> errors;
    bool built = false;
};

BuildOutcome compileSource(const QString& tag, const char* body)
{
    const auto dir = std::filesystem::temp_directory_path()
        / "nlang_const_decl_tests";
    const std::string path = writeTempSource(tag + ".n", body);
    BuildParams params;
    params.m_SourceFiles.push_back(path);
    params.m_sOutputModule = tag.toStdString();
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    BuildOutcome outcome;
    MemLogger logger;
    outcome.upBuilder =
        std::make_unique<ModuleBuilder>(params, logger);
    //Exception boundary (same contract as ncc main): codegen internal
    //errors throw; without the catch the abort would kill the test run.
    try { outcome.built = outcome.upBuilder->BuildArtifacts(); }
    catch (const std::exception&) { outcome.built = false; }
    outcome.errors = logger.errorsText();
    return outcome;
}

//First class/struct declaration with the given name on the merged root
//(read-only view; marker queries are const).
const SnField* findRootDecl(const ModuleBuilder& builder,
    const char* szName)
{
    for (auto& member : builder.TreeRootView().Members())
        if ((member.Kind() == NK_ClassDecl || member.Kind() == NK_StructDecl)
            && member.Name() == szName)
            return &member;
    return nullptr;
}

bool anyErrorMentions(const std::vector<std::string>& errors,
    const char* szNeedle)
{
    for (const auto& error : errors)
        if (error.find(szNeedle) != std::string::npos)
            return true;
    return false;
}

} //namespace

class TestConstDecl : public QObject
{
    Q_OBJECT
private slots:
    //Runtime tables (IdString etc.) must exist before any Build().
    void initTestCase() {
        Runtime::StaticInit();
        TypeCastInfo::StaticInit();
    }

    //`const class C { ... }` parses and the resolved SnClassDecl carries
    //the const-aggregate marker.
    void parseSetsMarkerOnClass()
    {
        auto outcome = compileSource("const_class_mark",
            "const class C {\n"
            "    public int x;\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY2(!anyErrorMentions(outcome.errors, "syntax error"),
            "const class must parse");
        const SnField* pDecl = outcome.built
            ? findRootDecl(*outcome.upBuilder, "C") : nullptr;
        QVERIFY2(pDecl != nullptr, "the C declaration must be on the root");
        QVERIFY2(pDecl->Kind() == NK_ClassDecl, "C must be a class decl");
        QVERIFY2(static_cast<const SnClassDecl*>(pDecl)->IsConstAggregate(),
            "the const prefix must set the class marker");
    }

    //`const struct P { ... }` parses and the resolved SnStructDecl
    //carries the marker (struct form is standalone — no chain walk).
    void parseSetsMarkerOnStruct()
    {
        auto outcome = compileSource("const_struct_mark",
            "const struct P {\n"
            "    int x;\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY2(!anyErrorMentions(outcome.errors, "syntax error"),
            "const struct must parse");
        const SnField* pDecl = outcome.built
            ? findRootDecl(*outcome.upBuilder, "P") : nullptr;
        QVERIFY2(pDecl != nullptr, "the P declaration must be on the root");
        QVERIFY2(pDecl->Kind() == NK_StructDecl, "P must be a struct decl");
        QVERIFY2(static_cast<const SnStructDecl*>(pDecl)->IsConstAggregate(),
            "the const prefix must set the struct marker");
    }

    //Inheritance keeps the seal closed: a const subclass of a const
    //base reads as const (own marker; the chain walk is the defensive
    //query behind it).
    void markerPropagatesThroughInheritance()
    {
        auto outcome = compileSource("const_inherit",
            "const class B {\n"
            "    public int b;\n"
            "}\n"
            "const class D : B {\n"
            "    public int d;\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY2(outcome.built, "the inheritance combo must build clean");
        const SnField* pD = findRootDecl(*outcome.upBuilder, "D");
        QVERIFY2(pD != nullptr && pD->Kind() == NK_ClassDecl,
            "the D declaration must be found");
        QVERIFY2(static_cast<const SnClassDecl*>(pD)->IsConstAggregate(),
            "a const subclass must read as const");
    }

    //User ruling (2026-10-31): a const class may only be extended by a
    //const class — mixed hierarchies (sealed base, mutable subclass)
    //are a declaration error.
    void mixedInheritanceRejected()
    {
        auto outcome = compileSource("const_inherit_mixed",
            "const class B {\n"
            "    public int b;\n"
            "}\n"
            "class D : B {\n"
            "    public int d;\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY2(!outcome.built,
            "a mutable subclass of a const class must be rejected");
        QVERIFY2(anyErrorMentions(outcome.errors, "must be const"),
            "the diagnostic must name the const-subclass rule");
    }

    //Plain declarations carry no marker (the negative control for both
    //forms).
    void plainDeclCarriesNoMarker()
    {
        auto outcome = compileSource("plain_no_mark",
            "class C {\n"
            "    public int x;\n"
            "}\n"
            "struct P {\n"
            "    int y;\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY2(outcome.built, "plain declarations must build clean");
        const SnField* pC = findRootDecl(*outcome.upBuilder, "C");
        const SnField* pP = findRootDecl(*outcome.upBuilder, "P");
        QVERIFY2(pC && pC->Kind() == NK_ClassDecl, "C must be found");
        QVERIFY2(pP && pP->Kind() == NK_StructDecl, "P must be found");
        QVERIFY2(!static_cast<const SnClassDecl*>(pC)->IsConstAggregate(),
            "a plain class must not carry the marker");
        QVERIFY2(!static_cast<const SnStructDecl*>(pP)->IsConstAggregate(),
            "a plain struct must not carry the marker");
    }

    //The const prefix composes with a type-parameter list: the prefix
    //arm keeps TypeParamsOpt, so `const class Box<T>` parses and the
    //template row carries the marker.
    void constGenericDeclCarriesMarker()
    {
        auto outcome = compileSource("const_generic_mark",
            "const class Box<T> {\n"
            "    public native Box();\n"
            "    public native T get();\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY2(!anyErrorMentions(outcome.errors, "syntax error"),
            "const class with type parameters must parse");
        const SnField* pDecl = outcome.built
            ? findRootDecl(*outcome.upBuilder, "Box") : nullptr;
        QVERIFY2(pDecl != nullptr && pDecl->Kind() == NK_ClassDecl,
            "the Box declaration must be found");
        QVERIFY2(static_cast<const SnClassDecl*>(pDecl)->IsConstAggregate(),
            "the const prefix must set the template row's marker");
    }
};

QTEST_GUILESS_MAIN(TestConstDecl)
#include "test_const_decl.moc"
