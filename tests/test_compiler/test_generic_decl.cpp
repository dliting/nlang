/*---
test_generic_decl.cpp - generic class declaration grammar tests.

In-process ModuleBuilder coverage of the `class Name<T, ...>` declaration
form: acceptance of an all-native generic declaration, and rejection of
a method body inside one (the declaration grammar carries signature
surfaces only — no generic method compilation).
---*/
#include <QtTest/QtTest>
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/CastInfo.h>
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
        std::filesystem::temp_directory_path() / "nlang_generic_decl_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name.toStdString();
    std::ofstream(file, std::ios::binary) << body;
    return file.string();
}

//Compile one source file; returns (build ok, error texts).
std::pair<bool, std::vector<std::string>> compileSource(
    const std::string& tag, const std::string& path)
{
    const auto dir = std::filesystem::temp_directory_path()
        / "nlang_generic_decl_tests";
    BuildParams params;
    params.m_SourceFiles.push_back(path);
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    MemLogger logger;
    ModuleBuilder builder(params, logger);
    bool ok = false;
    //Exception boundary (same contract as ncc main): codegen internal
    //errors throw; without the catch the abort would kill the test run.
    try { ok = builder.BuildArtifacts(); }
    catch (const std::exception&) { ok = false; }
    return {ok, logger.errorsText()};
}

bool anyErrorMentions(const std::vector<std::string>& errors,
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

class TestGenericDecl : public QObject
{
    Q_OBJECT
private slots:
    //Runtime tables (IdString etc.) must exist before any Build().
    void initTestCase() {
        Runtime::StaticInit();
        TypeCastInfo::StaticInit();
    }

    //A generic declaration with all-native members parses (grammar-level
    //acceptance — resolving T inside member signatures is Task 2 of the
    //plan, so build-clean is not asserted here).
    void declGenericAcceptsNativeMembers()
    {
        const auto path = writeTempSource("generic_ok.n",
            "class Box<T> {\n"
            "    public native T get();\n"
            "    public native void set(T v);\n"
            "}\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("generic_decl_ok", path);
        QVERIFY2(!anyErrorMentions(outcome.second, "syntax error"),
            "class Box<T> must parse");
    }

    //A method body inside a generic declaration is rejected: the
    //declaration grammar carries signature surfaces only.
    void declGenericRejectsNonNativeBody()
    {
        const auto path = writeTempSource("generic_body.n",
            "class Box<T> {\n"
            "    public int get() {\n"
            "        return 0;\n"
            "    }\n"
            "}\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("generic_decl_body", path);
        QVERIFY2(!outcome.first || !outcome.second.empty(),
            "non-native body in a generic declaration must be rejected");
        QVERIFY2(anyErrorMentions(outcome.second, "get"),
            "diagnostic must name the offending method");
    }

    //A declared generic instantiates: the member signatures substitute
    //the type arguments (Box<int>.get() reads as int), and the class
    //builds clean end to end.
    void declGenericInstantiatesMembers()
    {
        const auto path = writeTempSource("generic_inst.n",
            "class Box<T> {\n"
            "    public native Box();\n"
            "    public native T get();\n"
            "    public native void set(T v);\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box();\n"
            "    int x = b.get();\n"
            "    b.set(x);\n"
            "    return 0;\n"
            "}\n");
        const auto outcome = compileSource("generic_inst", path);
        QVERIFY2(outcome.first,
            "declared generic instantiation must build clean");
        QVERIFY2(outcome.second.empty(),
            "no diagnostics expected for the instantiated generic");
    }

    //The type-argument count is checked against the declaration's
    //parameter list.
    void declGenericRejectsWrongArity()
    {
        const auto path = writeTempSource("generic_arity.n",
            "class Box<T> {\n"
            "    public native Box();\n"
            "    public native T get();\n"
            "}\n"
            "int main() {\n"
            "    Box<int, string> b = Box();\n"
            "    return 0;\n"
            "}\n");
        const auto outcome = compileSource("generic_arity", path);
        QVERIFY2(!outcome.first,
            "wrong type-argument count must fail the build");
        QVERIFY2(anyErrorMentions(outcome.second, "type argument"),
            "diagnostic must name the type-argument count mismatch");
    }

    //A generic declaration used bare (no type arguments) is not a type:
    //the diagnostic names the instantiation form.
    void declGenericRejectsBareType()
    {
        const auto path = writeTempSource("generic_bare.n",
            "class Box<T> {\n"
            "    public native Box();\n"
            "    public native T get();\n"
            "}\n"
            "int main() {\n"
            "    Box b = Box();\n"
            "    return 0;\n"
            "}\n");
        const auto outcome = compileSource("generic_bare", path);
        QVERIFY2(!outcome.first,
            "a bare generic declaration must not be usable as a type");
        QVERIFY2(anyErrorMentions(outcome.second, "type argument"),
            "diagnostic must point at the missing type arguments");
    }

    //A class FIELD of declared-generic type mints the instantiation in
    //the declaration pass — before the template's own signatures resolve.
    //The mint must resolve the clone's signature surface: an unresolved
    //return type read as void here, and the cache served the broken
    //instance to every later use of the same instantiation.
    void declGenericFieldMintResolvesMembers()
    {
        const auto path = writeTempSource("generic_field_mint.n",
            "class C {\n"
            "    public Promise<int> p;\n"
            "}\n"
            "int main() {\n"
            "    Promise<int> p = Promise();\n"
            "    Future<int> f = p.future();\n"
            "    return 0;\n"
            "}\n");
        const auto outcome = compileSource("generic_field_mint", path);
        QString diag;
        for (const auto& e : outcome.second) diag += QString::fromStdString(e) + " | ";
        QVERIFY2(outcome.first,
            qPrintable("a declared-generic field must mint a complete "
                       "instance — errors: " + diag));
        QVERIFY2(!anyErrorMentions(outcome.second, "void function"),
            "future() must keep its declared return type");
    }
};

QTEST_GUILESS_MAIN(TestGenericDecl)
#include "test_generic_decl.moc"
