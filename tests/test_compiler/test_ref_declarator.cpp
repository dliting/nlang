/*---
test_ref_declarator.cpp - the & declarator mark on declarations (v0.13.9
phase 3, value_type_reference_design.md 3.1/3.5).

In-process ModuleBuilder coverage of the four & positions at the parse
layer: local declarations, formal parameters, return types and foreach
heads record NF_Ref on their nodes; func-type args carry NF_Ref on the
type-arg node; & outside the func-type arms (List<int&>) stays a syntax
error; and the bitwise-and statement readings survive untouched.
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
#include <nlang/compiler/SyntaxNodeConsts.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>
#include <filesystem>
#include <fstream>
#include <string>
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
        nlang::test::ScratchDir("nlang_ref_tests");
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name.toStdString();
    std::ofstream(file, std::ios::binary) << body;
    return file.string();
}

//Depth-first count of nodes carrying NF_Ref. The AST dies with the
//builder, so only the count crosses the boundary — never node pointers.
int countRefMarked(const SyntaxNode& node)
{
    int count = node.ContainFlags(NF_Ref) ? 1 : 0;
    const auto& children = node.Children();
    for (auto it = children.cbegin(); it != children.cend(); ++it)
        count += countRefMarked(static_cast<const SyntaxNode&>(*it));
    return count;
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

//Compiles one source and reports how many nodes carry NF_Ref (-1 when
//the build threw); diagnostics are copied out for the assertions.
int buildAndCountRefMarks(const std::string& tag, const char* body,
    std::vector<std::string>& errorsText)
{
    const auto dir = nlang::test::ScratchDir("nlang_ref_tests");
    BuildParams params;
    params.m_SourceFiles.push_back(writeTempSource(QString::fromStdString(tag),
        body));
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    MemLogger logger;
    ModuleBuilder builder(params, logger);
    int count = -1;
    //Exception boundary (same contract as ncc main): codegen internal
    //errors throw; without the catch the abort would kill the test run.
    try
    {
        builder.BuildArtifacts();
        count = countRefMarked(builder.TreeRootView());
    }
    catch (const std::exception&) { }
    errorsText = logger.errorsText();
    return count;
}

//Reports whether the return type of the root-level function of the given
//name carries NF_Ref (the & mark rides the TYPE node it decorates — the
//same convention as the out mark on type-arg nodes).
bool buildAndReturnTypeRef(const std::string& tag, const char* body,
    const char* szName, std::vector<std::string>& errorsText)
{
    const auto dir = nlang::test::ScratchDir("nlang_ref_tests");
    BuildParams params;
    params.m_SourceFiles.push_back(writeTempSource(QString::fromStdString(tag),
        body));
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    MemLogger logger;
    ModuleBuilder builder(params, logger);
    bool marked = false;
    try
    {
        builder.BuildArtifacts();
        const SnNamespace& root = builder.TreeRootView();
        const auto& children = root.Children();
        for (auto it = children.cbegin(); it != children.cend(); ++it)
        {
            const auto& field = static_cast<const SnField&>(*it);
            if (field.Name() == szName)
            {
                const auto& func = static_cast<const SnFunction&>(field);
                marked = func.ReturnType() != nullptr
                    && func.ReturnType()->ContainFlags(NF_Ref);
                break;
            }
        }
    }
    catch (const std::exception&) { }
    errorsText = logger.errorsText();
    return marked;
}

} //namespace

class TestRefDeclarator : public QObject
{
    Q_OBJECT
private slots:
    //Runtime tables (IdString etc.) must exist before any Build().
    void initTestCase() {
        Runtime::StaticInit();
        TypeCastInfo::StaticInit();
    }

    //`Point& r = p;` parses as a reference declaration and records NF_Ref
    //on the local-declaration statement.
    void refbindLocalDeclRecordsRef()
    {
        std::vector<std::string> errors;
        const int count = buildAndCountRefMarks("ref_local_decl",
            "struct Point { int x; }\n"
            "int main() {\n"
            "    Point p;\n"
            "    Point& r = p;\n"
            "    return 0;\n"
            "}\n",
            errors);
        QVERIFY2(!anyErrorMentions(errors, "syntax error"),
            "Point& r = p must parse as a reference declaration");
        QVERIFY2(count >= 1,
            "the reference local declaration must record NF_Ref");
    }

    //`int& pick(...)` records the return-type & mark on the function
    //node across every header shape the '&' arms serve: bare, static,
    //access-led, access+static, and their generic twins (mirror of
    //NF_ExplicitVoid).
    void refbindReturnRecordsRef()
    {
        struct ReturnShape
        {
            const char* szTag;
            const char* szHeader;
        };
        const ReturnShape shapes[] = {
            {"ref_return_bare",             "int& pick(int[] xs)"},
            {"ref_return_access",           "private int& pick(int[] xs)"},
            {"ref_return_access_static",    "private static int& pick(int[] xs)"},
            {"ref_return_generic_bare",     "int& pick<T>(T[] xs)"},
            {"ref_return_generic_access",   "private int& pick<T>(T[] xs)"},
            {"ref_return_generic_access_static",
                "private static int& pick<T>(T[] xs)"},
            {"ref_return_access_public",    "public int& pick(int[] xs)"},
        };
        for (const ReturnShape& shape : shapes)
        {
            const std::string body = std::string(shape.szHeader)
                + " {\n    return xs[0];\n}\n"
                + "int main() { return 0; }\n";
            std::vector<std::string> errors;
            const bool marked = buildAndReturnTypeRef(shape.szTag,
                body.c_str(), "pick", errors);
            QVERIFY2(!anyErrorMentions(errors, "syntax error"),
                (std::string(shape.szTag) + " must parse").c_str());
            QVERIFY2(marked,
                (std::string(shape.szTag) + " must record NF_Ref").c_str());
        }
    }

    //`void bump(int& v)` records NF_Ref on the formal parameter.
    void refbindParamRecordsRef()
    {
        std::vector<std::string> errors;
        //The param mark is observed through the whole-tree count: the
        //declaration itself is the only & spelling in this source.
        const int count = buildAndCountRefMarks("ref_param",
            "void bump(int& v) {\n"
            "    v = v + 1;\n"
            "}\n"
            "int main() {\n"
            "    int x = 1;\n"
            "    bump(x);\n"
            "    return x;\n"
            "}\n",
            errors);
        QVERIFY2(!anyErrorMentions(errors, "syntax error"),
            "int& formal parameter must parse");
        QVERIFY2(count >= 1, "the & formal parameter must record NF_Ref");
    }

    //`foreach (int& v in xs)` records NF_Ref on the foreach statement.
    void refbindForeachRecordsRef()
    {
        std::vector<std::string> errors;
        const int count = buildAndCountRefMarks("ref_foreach",
            "int main() {\n"
            "    int[] xs = int[2];\n"
            "    foreach (int& v in xs) {\n"
            "        v = 1;\n"
            "    }\n"
            "    return 0;\n"
            "}\n",
            errors);
        QVERIFY2(!anyErrorMentions(errors, "syntax error"),
            "foreach & head must parse");
        QVERIFY2(count >= 1, "the & foreach head must record NF_Ref");
    }

    //`func<int&>` carries NF_Ref on the type-arg node inside the
    //func-type arms (mirror of the out mark on type args).
    void refbindFuncTypeArgRecordsRef()
    {
        std::vector<std::string> errors;
        const int count = buildAndCountRefMarks("ref_func_type_arg",
            "void h(func<int&> cb) {\n"
            "}\n"
            "int main() { return 0; }\n",
            errors);
        QVERIFY2(!anyErrorMentions(errors, "syntax error"),
            "func<int&> must parse");
        QVERIFY2(count >= 1,
            "the func-type & arg must record NF_Ref on the type node");
    }

    //& outside the func-type arms stays a syntax error: List<int&> is
    //not a legal type spelling (the func arms own the & TypeArg form).
    void refbindGenericTypeRefArgRejected()
    {
        std::vector<std::string> errors;
        buildAndCountRefMarks("ref_generic_reject",
            "int main() {\n"
            "    List<int&> xs;\n"
            "    return 0;\n"
            "}\n",
            errors);
        QVERIFY2(anyErrorMentions(errors, "syntax error"),
            "List<int&> must stay a syntax error");
    }

    //The bitwise-and readings survive the new declarator arms in every
    //expression context: an init context holds no HeadType items, so
    //`a & b` reduces through Expression and binds no reference. (The
    //bare statement form `a & b;` is a different story: at the statement
    //head HeadType wins the '&' reduce/reduce, so the shape reads as a
    //byref-declaration attempt — the accepted-conflict ledger in
    //nlang.y documents that trade.)
    void refbindBitwiseStatementSurvives()
    {
        std::vector<std::string> errors;
        const int count = buildAndCountRefMarks("ref_bitwise_stmt",
            "int main() {\n"
            "    int a = 6;\n"
            "    int b = 3;\n"
            "    int c = a & b;\n"
            "    return c;\n"
            "}\n",
            errors);
        QVERIFY2(!anyErrorMentions(errors, "syntax error"),
            "a & b must keep parsing as bitwise and");
        QVERIFY2(count == 0, "bitwise and must record no NF_Ref");
    }

    //`a & b[i]` keeps the bitwise reading in an init context (the `[`
    //continuation inside an expression is not a byref-declarator form).
    void refbindBitwiseSubscriptSurvives()
    {
        std::vector<std::string> errors;
        const int count = buildAndCountRefMarks("ref_bitwise_sub",
            "int main() {\n"
            "    int a = 6;\n"
            "    int[] b = int[2];\n"
            "    b[0] = 3;\n"
            "    int c = a & b[0];\n"
            "    return c;\n"
            "}\n",
            errors);
        QVERIFY2(!anyErrorMentions(errors, "syntax error"),
            "a & b[0] must keep parsing as bitwise and");
        QVERIFY2(count == 0, "bitwise and with subscript records no NF_Ref");
    }

    //`x & mask(y)` keeps the bitwise reading in an init context (the
    //`(` continuation inside an expression is not a byref-declarator
    //form).
    void refbindBitwiseCallSurvives()
    {
        std::vector<std::string> errors;
        const int count = buildAndCountRefMarks("ref_bitwise_call",
            "int mask(int m) {\n"
            "    return m;\n"
            "}\n"
            "int main() {\n"
            "    int x = 12;\n"
            "    int c = x & mask(10);\n"
            "    return c;\n"
            "}\n",
            errors);
        QVERIFY2(!anyErrorMentions(errors, "syntax error"),
            "x & mask(y) must keep parsing as bitwise and");
        QVERIFY2(count == 0, "bitwise and with call records no NF_Ref");
    }
};

QTEST_GUILESS_MAIN(TestRefDeclarator)
#include "test_ref_declarator.moc"
