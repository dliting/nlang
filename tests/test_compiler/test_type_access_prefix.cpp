/*---
test_type_access_prefix.cpp - access prefixes on top-level type declarations.

In-process ModuleBuilder coverage of the v0.13.8 item-5 declaration forms:
`private class` / `protected interface` / `private struct` / `private enum`
record the FA_* byte on the declaration node, while `public` and the bare
form keep the FA_Public namespace default.
---*/
#include "test_support/test_scratch.h"
#include <QtTest/QtTest>
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/CastInfo.h>
#include "builder/ModuleRegistry.h"
#include <nlang/compiler/SyntaxNode.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>
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
        nlang::test::ScratchDir("nlang_type_access_tests");
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name.toStdString();
    std::ofstream(file, std::ios::binary) << body;
    return file.string();
}

//Compiles one source in-process and reports the declared access byte of
//the top-level declaration of the given kind and name (-1 when the build
//threw or no such declaration is on the tree). The AST dies with the
//builder, so the access byte is copied out here — never the node pointer.
int buildAndDeclAccess(const std::string& tag, const char* body,
    NodeKind kind, const char* szName, std::vector<std::string>& errorsText)
{
    const auto dir = nlang::test::ScratchDir("nlang_type_access_tests");
    BuildParams params;
    params.m_SourceFiles.push_back(writeTempSource(QString::fromStdString(tag),
        body));
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    MemLogger logger;
    ModuleBuilder builder(params, logger);
    //Exception boundary (same contract as ncc main): codegen internal
    //errors throw; without the catch the abort would kill the test run.
    try { builder.BuildArtifacts(); }
    catch (const std::exception&) { }
    errorsText = logger.errorsText();

    const SnNamespace& root = builder.TreeRootView();
    const auto& children = root.Children();
    for (auto it = children.cbegin(); it != children.cend(); ++it)
    {
        if (it->Kind() != kind)
            continue;
        //Top-level members are the container's own fields (the Kind()
        //check above narrows the cast; the house tree-walk shape).
        const auto& decl = static_cast<const SnField&>(*it);
        if (decl.Name() == szName)
            return static_cast<int>(decl.AccessType());
    }
    return -1;
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

class TestTypeAccessPrefix : public QObject
{
    Q_OBJECT
private slots:
    //Runtime tables (IdString etc.) must exist before any Build().
    void initTestCase() {
        Runtime::StaticInit();
        TypeCastInfo::StaticInit();
    }

    //`private class` parses and records FA_Private on the declaration
    //node.
    void privateClassRecordsPrivate()
    {
        std::vector<std::string> errors;
        const int access = buildAndDeclAccess("acc_private_class",
            "private class SecretTool { int f() { return 1; } }\n"
            "int main() { return 0; }\n",
            NK_ClassDecl, "SecretTool", errors);
        QVERIFY2(!anyErrorMentions(errors, "syntax error"),
            "private class must parse");
        QVERIFY2(access == static_cast<int>(FA_Private),
            "private class must record FA_Private on the declaration");
    }

    //`protected interface` parses and records FA_Protected on the
    //declaration node.
    void protectedInterfaceRecordsProtected()
    {
        std::vector<std::string> errors;
        const int access = buildAndDeclAccess("acc_protected_iface",
            "protected interface Sealed { int m(); }\n"
            "int main() { return 0; }\n",
            NK_InterfaceDecl, "Sealed", errors);
        QVERIFY2(!anyErrorMentions(errors, "syntax error"),
            "protected interface must parse");
        QVERIFY2(access == static_cast<int>(FA_Protected),
            "protected interface must record FA_Protected");
    }

    //`private struct` parses and records FA_Private on the declaration
    //node.
    void privateStructRecordsPrivate()
    {
        std::vector<std::string> errors;
        const int access = buildAndDeclAccess("acc_private_struct",
            "private struct Point { int x; }\n"
            "int main() { return 0; }\n",
            NK_StructDecl, "Point", errors);
        QVERIFY2(!anyErrorMentions(errors, "syntax error"),
            "private struct must parse");
        QVERIFY2(access == static_cast<int>(FA_Private),
            "private struct must record FA_Private on the declaration");
    }

    //`private enum` parses and records FA_Private on the declaration
    //node.
    void privateEnumRecordsPrivate()
    {
        std::vector<std::string> errors;
        const int access = buildAndDeclAccess("acc_private_enum",
            "private enum Tag { A, B }\n"
            "int main() { return 0; }\n",
            NK_EnumDecl, "Tag", errors);
        QVERIFY2(!anyErrorMentions(errors, "syntax error"),
            "private enum must parse");
        QVERIFY2(access == static_cast<int>(FA_Private),
            "private enum must record FA_Private on the declaration");
    }

    //The explicit `public` spelling parses and records FA_Public (the
    //declaration-contract shape Task 4's diagnostics read).
    void publicClassRecordsPublic()
    {
        std::vector<std::string> errors;
        const int access = buildAndDeclAccess("acc_public_class",
            "public class Open { int f() { return 1; } }\n"
            "int main() { return 0; }\n",
            NK_ClassDecl, "Open", errors);
        QVERIFY2(!anyErrorMentions(errors, "syntax error"),
            "public class must parse");
        QVERIFY2(access == static_cast<int>(FA_Public),
            "public class must record FA_Public on the declaration");
    }

    //Access composes with the const-aggregate spelling (the D5.4 family
    //lists const class): `private const class` takes the KT_Const
    //continuation in the access-keyword state onto the declaration core.
    void privateConstClassComposes()
    {
        std::vector<std::string> errors;
        const int access = buildAndDeclAccess("acc_private_const_class",
            "private const class Cell {\n"
            "    public int x;\n"
            "}\n"
            "int main() { return 0; }\n",
            NK_ClassDecl, "Cell", errors);
        QVERIFY2(!anyErrorMentions(errors, "syntax error"),
            "private const class must parse");
        QVERIFY2(access == static_cast<int>(FA_Private),
            "private const class must record FA_Private on the declaration");
    }

    //The bare form keeps the FA_Public namespace default.
    void bareClassKeepsPublicDefault()
    {
        std::vector<std::string> errors;
        const int access = buildAndDeclAccess("acc_bare_class",
            "class Bare { int f() { return 1; } }\n"
            "int main() { return 0; }\n",
            NK_ClassDecl, "Bare", errors);
        QVERIFY2(!anyErrorMentions(errors, "syntax error"),
            "bare class must still parse");
        QVERIFY2(access == static_cast<int>(FA_Public),
            "bare class must keep the FA_Public default");
    }
};

QTEST_GUILESS_MAIN(TestTypeAccessPrefix)
#include "test_type_access_prefix.moc"
